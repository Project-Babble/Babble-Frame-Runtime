#include "camera/eye/scan/scan_eye_source.h"

#include "core/clock.h"
#include "core/log.h"
#include "platform/steamvr_client.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
const babble::log::Logger LOG("eye");
}

namespace babble {

using namespace eyescan;

namespace {

constexpr uint32_t kMaxDivisor = 3;
constexpr uint64_t kLostNs = 2000000000ull;   // No slot changed for this long: the frames stopped.

}

ScanEyeSource::ScanEyeSource(const nlohmann::json &cfg, SourceDeps deps)
    : cfg_(cfg), name_(deps.name), thread_spec_(thread_spec(deps.threads, "eye", "bbl-eye")), out_(deps.out),
      stats_(deps.stats), steamvr_(deps.steamvr), pause_when_absent_(cfg.value("pause_when_absent", true)),
      tracking_(cfg.value("tracking", true)) {
    divisor_ = std::clamp(cfg.value("rate_divisor", 1u), 1u, kMaxDivisor);
    for (const auto &c : cfg_.value("ctrl_base_candidates", nlohmann::json::array())) {
        if (c.is_string())
            candidates_.push_back(uint32_t(std::stoul(c.get<std::string>(), nullptr, 0)));
        else if (c.is_number_unsigned())
            candidates_.push_back(c.get<uint32_t>());
    }
    auto backoff = cfg_.value("reopen_backoff_ms", nlohmann::json::object());
    backoff_min_ns_ = uint64_t(backoff.value("min", 5000)) * 1000000ull;
    backoff_max_ns_ = uint64_t(backoff.value("max", 60000)) * 1000000ull;
    state_ = cfg_.value("enabled", true) ? State::Connecting : State::Disabled;

    // Each crop is [y, x, h, w] from the first pixel of the 400x400 image.
    const nlohmann::json full = nlohmann::json::array({0, 0, kHeight, kWidth});
    const auto crops = cfg_.value("crop", nlohmann::json::object());
    const char *keys[2] = {"left", "right"};
    copy_len_ = kHeaderBytes + kFrameBytes;
    for (int s = 0; s < 2; s++) {
        const nlohmann::json a = crops.value(keys[s], full);
        Crop c;
        if (a.is_array() && a.size() == 4)
            c = {a[0].get<uint32_t>(), a[1].get<uint32_t>(), a[2].get<uint32_t>(), a[3].get<uint32_t>()};
        crop_[s] = c;
        const bool ok = c.w > 0 && c.h > 0 && c.x + c.w <= kWidth && c.y + c.h <= kHeight;
        if (!ok && state_ != State::Disabled) {
            last_error_ = std::string("eye crop ") + keys[s] + " is outside the frame";
            LOG.error("%s: %s", name_.c_str(), last_error_.c_str());
            state_ = State::Unavailable;
        }
    }

    stats_.add(name_ + ".fps", &frames_);
    stats_.add(name_ + ".process_fps", &processed_);
    stats_.add(name_ + ".fps_l", &side_l_);
    stats_.add(name_ + ".fps_r", &side_r_);
    stats_.add(name_ + ".restarts", &restarts_);
    stats_.add(name_ + ".pool_exhausted", &pool_exhausted_);
    stats_.add(name_ + ".half_pairs", &half_pairs_);
    stats_.add(name_ + ".torn_copies", &torn_copies_);
    stats_.add(name_ + ".missed_frames", &missed_frames_);
    stats_.add(name_ + ".stale_frames", &stale_frames_);
    stats_.add(name_ + ".bad_stamps", &bad_stamps_);
    stats_.add(name_ + ".searches", &searches_);
    stats_.add(name_ + ".camera_rate", &camera_rate_);
    stats_.add(name_ + ".rate_word", &rate_word_);
    stats_.add(name_ + ".copy", &copy_time_);
    stats_.add(name_ + ".stamp_age", &stamp_age_);
    stats_.add(name_ + ".pair_skew", &pair_skew_);
}

ScanEyeSource::~ScanEyeSource() {
    shutdown();
    stats_.remove_prefix(name_ + ".");
}

void ScanEyeSource::set_process_fps(double) {
    LOG.warn("%s: a fixed process rate isn't supported; set rate_divisor (1, 2 or 3) instead", name_.c_str());
}

bool ScanEyeSource::set_option(const std::string &key, const nlohmann::json &value) {
    if (key != "rate_divisor" || !value.is_number())
        return false;
    const double v = value.get<double>();
    const int64_t d = std::llround(v);
    if (double(d) != v || d < 1 || d > int64_t(kMaxDivisor))
        return false;
    divisor_ = uint32_t(d);
    LOG.info("%s: tracking rate 1:%u of the camera rate", name_.c_str(), unsigned(d));
    return true;
}

void ScanEyeSource::set_tracking(bool on) {
    if (on != tracking_)
        LOG.info("%s: tracking turned %s", name_.c_str(), on ? "on" : "off");
    tracking_ = on;
}

SourceActivity ScanEyeSource::activity() const {
    switch (state_) {
    case State::Disabled: return SourceActivity::Off;
    case State::Connecting: return SourceActivity::Starting;
    case State::Streaming: return searching_ ? SourceActivity::Starting : SourceActivity::Streaming;
    case State::Backoff: return SourceActivity::Fault;
    case State::Unavailable: return SourceActivity::Fault;
    case State::Paused: return tracking_ ? SourceActivity::Paused : SourceActivity::Off;
    }
    return SourceActivity::Fault;
}

const char *ScanEyeSource::state_name() const {
    switch (state_) {
    case State::Disabled: return "Disabled";
    case State::Connecting: return "Connecting";
    case State::Streaming: return searching_ ? "Searching" : "Streaming";
    case State::Backoff: return "Backoff";
    case State::Unavailable: return "Unavailable";
    case State::Paused: return "Paused";
    }
    return "?";
}

bool ScanEyeSource::start() {
    // Read-only: both mappings are PROT_READ, and nothing is written to the control block.
    if (!client_.connect(cfg_.value("dsp_request", 0u))) {
        last_error_ = "cannot connect to dsp_service";
        return false;
    }
    arena_ = DmaArena(client_.data(), DspClient::kRegionBytes);
    // The control block is used only to show the rate word next to the measured camera rate.
    layout_ = client_.probe(candidates_, nullptr);
    size_t bytes = 0;
    for (const Crop &c : crop_)
        bytes = std::max(bytes, size_t(c.w) * c.h);
    pool_ = FramePool::create_owned(12, bytes);
    stop_ = false;
    failed_ = false;
    thread_ = start_thread(thread_spec_, [this] { run(); });
    return true;
}

void ScanEyeSource::stop(const char *why) {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
    searching_ = false;
    client_.disconnect();
    arena_ = DmaArena();
    layout_.reset();
    pool_.reset();
    camera_fps_ = 0;
    LOG.info("eye: stopped (%s)", why);
}

void ScanEyeSource::update_clock_offset() {
    const uint64_t r1 = mono_raw_ns();
    const uint64_t m = mono_ns();
    const uint64_t r2 = mono_raw_ns();
    raw_to_mono_ns_ = int64_t(m) - int64_t(r1 + (r2 - r1) / 2);
}

void ScanEyeSource::read_rate_word() {
    if (!layout_ || !client_.ctrl())
        return;
    invalidate(client_.ctrl() + layout_->ctrl_base + layout_->fps, 4);
    const float fps = client_.read_fps(*layout_);
    const uint64_t r = uint64_t(std::lround(fps));
    if (r != rate_word_.get())
        LOG.info("eye: rate word %.0f fps (set by SteamVR)", fps);
    rate_word_.set(r);
}

bool ScanEyeSource::make_frame(int side, const uint8_t *slot, uint64_t capture_ns, uint64_t arrival_ns, uint32_t seq,
                               FramePtr *out) {
    auto frame = pool_->acquire();
    if (!frame) {
        pool_exhausted_.add();
        return false;
    }
    const Crop &c = crop_[side];
    const uint8_t *base = slot + kHeaderBytes;
    uint8_t *dst = const_cast<uint8_t *>(frame->data);
    for (uint32_t y = 0; y < c.h; y++)
        memcpy(dst + size_t(y) * c.w, base + size_t(c.y + y) * kStride + c.x, c.w);
    frame->origin = side ? FrameOrigin::EyeR : FrameOrigin::EyeL;
    frame->fmt = PixelFormat::Gray8;
    frame->width = c.w;
    frame->height = c.h;
    frame->stride = c.w;
    frame->size = size_t(c.w) * c.h;
    frame->capture_ns = capture_ns;
    frame->arrival_ns = arrival_ns;
    frame->seq = seq;
    *out = std::move(frame);
    return true;
}

namespace {

class RealClock : public ScanClock {
public:
    explicit RealClock(const std::atomic<bool> &stop) : stop_(stop) {}
    double now() override { return double(mono_ns()) * 1e-9; }
    void wait() override { sleep_ns(2500000ull); }
    bool stopped() const override { return stop_.load(); }

private:
    const std::atomic<bool> &stop_;
};

}

bool ScanEyeSource::search(Ring *ring) {
    searching_ = true;
    searches_.add();
    RealClock clock(stop_);
    const uint64_t timeout_ns = uint64_t(cfg_.value("search_timeout_s", 15)) * 1000000000ull;
    const uint64_t deadline = mono_ns() + timeout_ns;
    std::string last_stop = "-";
    auto log_line = [](const std::string &l) { LOG.debug("eye: %s", l.c_str()); };
    arena_.reset_block_changes();
    while (!stop_) {
        // Collect one second of changes, as eyecam does between searches.
        for (int i = 0; i < 10 && !stop_; i++)
            sleep_ns(100000000ull);
        if (stop_)
            break;
        arena_.note_block_changes();
        Look look;
        const bool found = discover(arena_, clock, log_line, &look, ring);
        if (found && ring->both_groups) {
            std::string shifts;
            for (int64_t s : ring->shift)
                shifts += (shifts.empty() ? "" : ",") + std::to_string(s);
            LOG.info("eye: ring found: %zu slots at 0x%zx, spacing %zu, shifts [%s]; %s", ring->off.size(),
                     ring->off[0], ring->pitch, shifts.c_str(), ring->reason.c_str());
            searching_ = false;
            return true;
        }
        const std::string why = found ? "one_eye" : look.stopped_at;
        if (why != last_stop)
            LOG.info("eye: searching for the frame ring (%s, %zu candidates)", why.c_str(), look.candidates);
        last_stop = why;
        arena_.clear_block_changes();
        if (mono_ns() > deadline) {
            thread_error_ = "eye frame ring not found (" + why + ")";
            break;
        }
    }
    searching_ = false;
    return false;
}

void ScanEyeSource::stream(const Ring &ring) {
    for (size_t o : ring.off) {
        if (o < kHeaderBytes || o - kHeaderBytes + copy_len_ > arena_.size()) {
            thread_error_ = "a ring slot is too near the end of the buffer for the crop";
            failed_ = true;
            return;
        }
    }
    const uint64_t poll_ns = uint64_t(cfg_.value("poll_interval_us", 2500)) * 1000ull;
    const uint64_t no_pairs_ns = uint64_t(cfg_.value("no_frames_timeout_ms", 2000)) * 1000000ull;

    SlotWatcher watcher;
    watcher.reset(arena_, ring.off);
    std::vector<uint8_t> tmp(copy_len_), pend[2] = {std::vector<uint8_t>(copy_len_), std::vector<uint8_t>(copy_len_)};
    uint64_t pend_arrival[2] = {0, 0};
    uint64_t last_stamp[2] = {0, 0};
    CamRate rate[2];
    StampPairer pairer;
    uint64_t last_change = mono_ns(), last_pair = last_change, next_second = 0;
    uint32_t pair_count = 0, seq = 0;

    auto to_mono = [&](uint64_t stamp, uint64_t fallback) {
        const uint64_t raw = mono_raw_ns();
        if (stamp == 0 || stamp > raw + 50000000ull || raw - stamp > 1000000000ull) {
            bad_stamps_.add();
            return fallback;
        }
        return uint64_t(int64_t(stamp) + raw_to_mono_ns_);
    };

    while (!stop_) {
        uint64_t now = mono_ns();
        if (now >= next_second) {
            update_clock_offset();
            read_rate_word();
            client_.drain();   // Read all messages from the service so that its socket does not fill.
            const double a = rate[0].rate(), b = rate[1].rate();
            const double fps = a > 0 && b > 0 ? std::min(a, b) : std::max(a, b);
            if (std::lround(fps) != std::lround(camera_fps_.load()))
                LOG.info("eye: camera rate %.1f fps (from the frame timestamps)", fps);
            camera_fps_ = float(fps);
            camera_rate_.set(uint64_t(std::lround(fps)));
            next_second = now + 1000000000ull;
        }
        for (size_t k = 0; k < watcher.size(); k++) {
            uint64_t first = 0;
            const auto ev = watcher.poll(arena_, k, now, tmp.data(), copy_len_, &first);
            if (ev == SlotWatcher::Event::None)
                continue;
            last_change = now;
            if (ev == SlotWatcher::Event::Torn)
                torn_copies_.add();
            if (ev != SlotWatcher::Event::Frame)
                continue;
            const int g = ring.group[k];
            const uint64_t t = header_stamp(tmp.data());
            if (t <= last_stamp[g]) {
                stale_frames_.add();
                continue;
            }
            const double side_fps = rate[g].rate();
            if (last_stamp[g] && side_fps > 0) {
                // A gap of more than 1.5 frame periods means frames that we did not see.
                const double periods = double(t - last_stamp[g]) * side_fps * 1e-9;
                if (periods > 1.5 && periods < 1e4)
                    missed_frames_.add(uint64_t(std::llround(periods)) - 1);
            }
            last_stamp[g] = t;
            rate[g].push(t);
            (g ? side_r_ : side_l_).tick();
            pend[g].swap(tmp);
            pend_arrival[g] = now;
            bool replaced = false;
            pairer.put(g, t, &replaced);
            if (replaced)
                half_pairs_.add();

            // A pair is two timestamps within half a frame period.
            const double a = rate[0].rate(), b = rate[1].rate();
            const double fps = a > 0 && b > 0 ? std::min(a, b) : std::max(a, b);
            const uint64_t tol = fps > 0 ? uint64_t(0.5e9 / fps) : 16000000ull;
            int dropped = -1;
            const auto m = pairer.match(tol, &dropped);
            if (m == StampPairer::Result::DroppedOne) {
                half_pairs_.add();
                continue;
            }
            if (m != StampPairer::Result::Pair)
                continue;
            last_pair = now;
            frames_.tick();
            IFrameTap *tap = tap_.load(std::memory_order_relaxed);
            const bool want_tap = tap && tap->tap_wanted();
            const bool forward = pair_count++ % divisor_.load(std::memory_order_relaxed) == 0;
            if (!forward && !want_tap)
                continue;

            const uint64_t cap_l = to_mono(pairer.stamp(0), pend_arrival[0]);
            const uint64_t cap_r = to_mono(pairer.stamp(1), pend_arrival[1]);
            const uint64_t t0 = mono_ns();
            StereoFrame sf;
            seq++;
            if (!make_frame(0, pend[0].data(), cap_l, pend_arrival[0], seq, &sf.left) ||
                !make_frame(1, pend[1].data(), cap_r, pend_arrival[1], seq, &sf.right))
                continue;
            copy_time_.add(mono_ns() - t0);
            sf.skew_ns = int64_t(cap_l) - int64_t(cap_r);
            sf.capture_ns = cap_r + uint64_t(sf.skew_ns / 2);
            pair_skew_.add(uint64_t(std::llabs(sf.skew_ns)));
            const uint64_t newest = std::max(cap_l, cap_r);
            if (now > newest)
                stamp_age_.add(now - newest);   // The time from the camera timestamp to the copy.
            if (want_tap)
                tap->tap_frames(name_, sf);
            if (forward) {
                processed_.tick();
                out_.put(std::move(sf));
            }
        }
        if (now - last_change > kLostNs) {
            thread_error_ = "eye frames stopped";
            failed_ = true;
            return;
        }
        if (now - last_pair > no_pairs_ns) {
            thread_error_ = "no eye frame pairs";
            failed_ = true;
            return;
        }
        sleep_ns(poll_ns);
    }
}

void ScanEyeSource::run() {
    Ring ring;
    if (!search(&ring)) {
        if (!stop_) {
            if (thread_error_.empty())
                thread_error_ = "eye frame ring not found";
            failed_ = true;
        }
        return;
    }
    if (!stop_)
        stream(ring);
}

void ScanEyeSource::tick(uint64_t now) {
    // Pause only if SteamVR reports that the headset is not worn.
    const bool absent = pause_when_absent_ && steamvr_ && steamvr_->worn() == Worn::No;
    const bool hold = !tracking_ || absent;
    const char *why = !tracking_ ? "tracking turned off" : "headset not worn";
    switch (state_) {
    case State::Paused:
        if (!hold) {
            LOG.info("%s: resuming", name_.c_str());
            state_ = State::Connecting;
            retry_at_ns_ = 0;
            backoff_ns_ = 0;
        }
        return;
    case State::Disabled:
    case State::Unavailable:
        return;
    case State::Streaming:
        if (hold) {
            stop(why);
            state_ = State::Paused;
            return;
        }
        if (failed_) {
            last_error_ = thread_error_;
            stop(last_error_.c_str());
            restarts_.add();
            backoff_ns_ = backoff_ns_ ? std::min(backoff_ns_ * 2, backoff_max_ns_) : backoff_min_ns_;
            retry_at_ns_ = now + backoff_ns_;
            state_ = State::Backoff;
        }
        return;
    case State::Connecting:
    case State::Backoff:
        if (hold) {
            state_ = State::Paused;
            return;
        }
        if (now < retry_at_ns_)
            return;
        if (start()) {
            state_ = State::Streaming;
            backoff_ns_ = 0;
            last_error_.clear();
            LOG.info("eye: reader started (read-only)");
        } else {
            backoff_ns_ = backoff_ns_ ? std::min(backoff_ns_ * 2, backoff_max_ns_) : backoff_min_ns_;
            retry_at_ns_ = now + backoff_ns_;
            state_ = State::Backoff;
            LOG.warn("eye: %s; retrying in %llus", last_error_.c_str(), (unsigned long long)(backoff_ns_ / 1000000000ull));
        }
        return;
    }
}

void ScanEyeSource::shutdown() {
    if (state_ == State::Streaming) {
        stop("shutdown");
        state_ = State::Connecting;
    }
}

}
