#include "camera/eye/dsp_eye_source.h"

#include "core/clock.h"
#include "core/log.h"
#include "platform/steamvr_client.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
const babble::log::Logger LOG("eye");
} // namespace

namespace babble {

namespace {

constexpr uint32_t kMaxLayoutFaults = 3;
constexpr uint32_t kMaxDivisor = 3;
constexpr uint64_t kFpsCheckNs = 500000000ull;   // how often the camera rate word is re-read

} // namespace

DspEyeSource::DspEyeSource(const nlohmann::json &cfg, SourceDeps deps)
    : cfg_(cfg), name_(deps.name), thread_spec_(thread_spec(deps.threads, "eye", "bbl-eye")), out_(deps.out),
      stats_(deps.stats), steamvr_(deps.steamvr), pause_when_absent_(cfg.value("pause_when_absent", true)),
      tracking_(cfg.value("tracking", true)) {
    divisor_ = std::clamp(cfg.value("rate_divisor", 1u), 1u, kMaxDivisor);
    if (cfg.contains("process_fps"))
        LOG.info("%s: process_fps is no longer used (saved from an older version); the tracking rate is rate_divisor",
                 name_.c_str());
    for (const auto &c : cfg_.value("ctrl_base_candidates", nlohmann::json::array())) {
        if (c.is_string())
            candidates_.push_back(uint32_t(std::stoul(c.get<std::string>(), nullptr, 0)));
        else if (c.is_number_unsigned())
            candidates_.push_back(c.get<uint32_t>());
    }
    auto crop = [](const nlohmann::json &a) {
        Crop c;
        if (a.is_array() && a.size() == 4)
            c = {a[0].get<uint32_t>(), a[1].get<uint32_t>(), a[2].get<uint32_t>(), a[3].get<uint32_t>()};
        return c;
    };
    auto crops = cfg_.value("crop", nlohmann::json::object());
    crop_l_ = crop(crops.value("left", nlohmann::json::array()));
    crop_r_ = crop(crops.value("right", nlohmann::json::array()));
    auto backoff = cfg_.value("reopen_backoff_ms", nlohmann::json::object());
    backoff_min_ns_ = uint64_t(backoff.value("min", 5000)) * 1000000ull;
    backoff_max_ns_ = uint64_t(backoff.value("max", 60000)) * 1000000ull;
    state_ = cfg_.value("enabled", true) ? State::Connecting : State::Disabled;

    stats_.add(name_ + ".fps", &frames_);
    stats_.add(name_ + ".process_fps", &processed_);
    stats_.add(name_ + ".layout_faults", &layout_faults_);
    stats_.add(name_ + ".restarts", &restarts_);
    stats_.add(name_ + ".pool_exhausted", &pool_exhausted_);
    stats_.add(name_ + ".copy", &copy_time_);
    stats_.add(name_ + ".late_detect", &late_detect_);
    stats_.add(name_ + ".fps_l", &side_l_);
    stats_.add(name_ + ".fps_r", &side_r_);
    stats_.add(name_ + ".half_pairs", &half_pairs_);
    stats_.add(name_ + ".camera_rate", &camera_rate_);
}

DspEyeSource::~DspEyeSource() {
    shutdown();
    stats_.remove_prefix(name_ + ".");
}

void DspEyeSource::set_process_fps(double fps) {
    LOG.warn("%s: a fixed process rate isn't supported; set rate_divisor (1, 2 or 3) instead", name_.c_str());
}

bool DspEyeSource::set_option(const std::string &key, const nlohmann::json &value) {
    // A whole number; the Babble tab sends numbers as doubles.
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

void DspEyeSource::set_tracking(bool on) {
    if (on != tracking_)
        LOG.info("%s: tracking turned %s", name_.c_str(), on ? "on" : "off");
    tracking_ = on;
}

SourceActivity DspEyeSource::activity() const {
    switch (state_) {
    case State::Disabled: return SourceActivity::Off;
    case State::Connecting: return SourceActivity::Starting;
    case State::Streaming: return SourceActivity::Streaming;
    case State::Backoff: return not_ready_ ? SourceActivity::Starting : SourceActivity::Fault;
    case State::Unavailable: return SourceActivity::Fault;
    case State::Paused: return tracking_ ? SourceActivity::Paused : SourceActivity::Off;
    }
    return SourceActivity::Fault;
}

const char *DspEyeSource::state_name() const {
    switch (state_) {
    case State::Disabled: return "Disabled";
    case State::Connecting: return "Connecting";
    case State::Streaming: return "Streaming";
    case State::Backoff: return "Backoff";
    case State::Unavailable: return "Unavailable";
    case State::Paused: return "Paused";
    }
    return "?";
}

bool DspEyeSource::start() {
    if (!client_.connect(cfg_.value("dsp_request", 0u))) {
        last_error_ = "cannot connect to dsp_service";
        return false;
    }
    std::string why;
    auto layout = client_.probe(candidates_, &why);
    if (!layout) {
        // Never write to a control block we can't validate; look again later
        // (read-only, with backoff). Right after boot the block is still blank
        // until the service sets up the eye cameras: that's "not ready", not
        // an unknown layout.
        not_ready_ = DspClient::fps_unset(client_.ctrl(), DspClient::kRegionBytes, candidates_);
        if (not_ready_) {
            last_error_ = "waiting for dsp_service to set up the eye cameras";
        } else {
            last_error_ = "unrecognized dsp_service layout (" + why + ")";
            if (last_error_ != reported_layout_error_)   // once per distinct reason
                LOG.error("eye: %s", last_error_.c_str());
            reported_layout_error_ = last_error_;
        }
        client_.disconnect();
        return false;
    }
    reported_layout_error_.clear();
    not_ready_ = false;
    layout_ = *layout;
    for (const Crop *c : {&crop_l_, &crop_r_}) {
        if (c->w == 0 || c->h == 0 || c->x + c->w > layout_.width || c->y + c->h > layout_.height) {
            last_error_ = "eye crop outside the 512x512 frame";
            LOG.error("eye: %s", last_error_.c_str());
            client_.disconnect();
            state_ = State::Unavailable;
            return false;
        }
    }
    LOG.info("eye: dsp_service layout at ctrl base 0x%x", layout_.ctrl_base);

    // Two frames per stereo pair, a few pairs in flight downstream.
    size_t bytes = size_t(std::max(crop_l_.w * crop_l_.h, crop_r_.w * crop_r_.h));
    pool_ = FramePool::create_owned(12, bytes);

    client_.start_streaming(layout_);
    stop_ = false;
    failed_ = false;
    consecutive_faults_ = 0;
    pairer_.reset();
    thread_ = start_thread(thread_spec_, [this] { run(); });
    return true;
}

void DspEyeSource::stop(const char *why) {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
    client_.disconnect();
    pool_.reset();
    LOG.info("eye: stopped (%s)", why);
}

bool DspEyeSource::copy_view(bool right, uint32_t idx, uint64_t now, uint32_t seq, FramePtr *out) {
    const uint8_t *src = client_.slot_ptr(layout_, right, idx);
    auto frame = pool_->acquire();
    if (!frame || !src) {
        pool_exhausted_.add();
        client_.release(layout_, right, idx);
        return false;
    }
    const Crop &c = right ? crop_r_ : crop_l_;
    uint8_t *dst = const_cast<uint8_t *>(frame->data);
    for (uint32_t y = 0; y < c.h; y++)
        memcpy(dst + size_t(y) * c.w, src + size_t(c.y + y) * layout_.width + c.x, c.w);
    client_.release(layout_, right, idx);

    frame->origin = right ? FrameOrigin::EyeR : FrameOrigin::EyeL;
    frame->fmt = PixelFormat::Gray8;
    frame->width = c.w;
    frame->height = c.h;
    frame->stride = c.w;
    frame->size = size_t(c.w) * c.h;
    frame->capture_ns = now;   // no DSP timestamp known; detection time
    frame->arrival_ns = now;
    frame->seq = seq;
    *out = std::move(frame);
    return true;
}

void DspEyeSource::run() {
    const uint64_t poll_ns = uint64_t(cfg_.value("poll_interval_us", 400)) * 1000ull;
    const uint64_t no_frames_ns = uint64_t(cfg_.value("no_frames_timeout_ms", 2000)) * 1000000ull;
    const uint64_t margin_min_ns = uint64_t(cfg_.value("predict_margin_us", 1500)) * 1000ull;
    uint64_t last_frame = mono_ns();
    uint32_t seq = 0;

    // Frame-timed polling state (see class comment).
    uint64_t interval_ns = 0;          // smoothed frame interval, 0 = unknown
    uint64_t margin_ns = margin_min_ns;
    uint64_t last_change_ns = 0;
    uint32_t on_time = 0;
    bool after_coarse_sleep = false;
    auto on_new_frame = [&](uint64_t now) {
        if (last_change_ns) {
            uint64_t dt = now - last_change_ns;
            if (dt < 100000000ull)   // ignore gaps (stalls, restarts)
                interval_ns = interval_ns ? (interval_ns * 7 + dt) / 8 : dt;
        }
        last_change_ns = now;
        if (after_coarse_sleep) {
            // The frame landed while we slept: detection was late by up to
            // the margin. Wake earlier from now on.
            late_detect_.add();
            margin_ns = std::min<uint64_t>(margin_ns + 250000ull, interval_ns / 2);
            on_time = 0;
        } else if (++on_time >= 200 && margin_ns > margin_min_ns) {
            margin_ns -= 100000ull;
            on_time = 0;
        }
        after_coarse_sleep = false;
    };
    auto idle_sleep = [&](uint64_t now) {
        uint64_t wake = interval_ns ? last_change_ns + interval_ns - std::min(margin_ns, interval_ns) : 0;
        if (wake > now + poll_ns) {
            sleep_ns(wake - now);
            after_coarse_sleep = true;
        } else {
            sleep_ns(poll_ns);
            after_coarse_sleep = false;
        }
    };

    if (!logged_gap_) {
        // Research aid: the 64 bytes between frame end and next slot may hold metadata.
        const uint8_t *s = client_.slot_ptr(layout_, false, 0) + layout_.frame_bytes;
        char hex[3 * 64 + 1] = {0};
        for (int i = 0; i < 64; i++)
            snprintf(hex + 3 * i, 4, "%02x ", s[i]);
        LOG.debug("eye: slot trailer bytes: %s", hex);
        logged_gap_ = true;
    }

    uint64_t next_fps_check = 0;
    uint64_t wait_ns = 5000000ull;   // how long a lone side may wait for the other: half a frame period
    uint32_t pair_count = 0;
    while (!stop_) {
        auto idx = client_.read_indices(layout_);
        uint64_t now = mono_ns();
        if (now >= next_fps_check) {
            // SteamVR re-sets the camera rate on every headset-on and display mode change.
            const float fps = client_.read_fps(layout_);
            if (fps != camera_fps_.load(std::memory_order_relaxed)) {
                if (fps > 0)
                    LOG.info("eye: camera rate %.0f fps (set by SteamVR)", fps);
                camera_fps_ = fps;
                camera_rate_.set(uint64_t(fps + 0.5f));
            }
            if (fps > 0)
                wait_ns = uint64_t(0.5e9 / fps);
            next_fps_check = now + kFpsCheckNs;
            // With request type 0 the service sends nothing; if it ever does, read it
            // rather than let its socket fill (see DspClient::connect).
            if (client_.drain() && !logged_dsp_messages_) {
                LOG.warn("eye: dsp_service sent messages (request type %u); discarding them",
                         cfg_.value("dsp_request", 0u));
                logged_dsp_messages_ = true;
            }
        }
        bool bad_l = idx.l != DspClient::kNotReady && idx.l >= layout_.slots;
        bool bad_r = idx.r != DspClient::kNotReady && idx.r >= layout_.slots;
        if (bad_l || bad_r) {
            layout_faults_.add();
            if (++consecutive_faults_ >= kMaxLayoutFaults) {
                thread_error_ = "dsp_service index words out of range; layout changed?";
                failed_ = true;
                return;
            }
            sleep_ns(poll_ns);
            continue;
        }
        consecutive_faults_ = 0;   // a sane index read: only a run of bad reads is fatal

        const EyePairer::Step step = pairer_.feed(idx.l, idx.r, now, wait_ns);
        if (step.new_l)
            side_l_.tick();
        if (step.new_r)
            side_r_.tick();
        if (step.result == EyePairer::Result::Half) {
            // One camera published and the other never followed: hand the
            // lone slot back unused rather than pair it with a stale image.
            half_pairs_.add();
            if (step.half_l)
                client_.release(layout_, false, idx.l);
            if (step.half_r)
                client_.release(layout_, true, idx.r);
            last_frame = now;
            continue;
        }
        if (step.result != EyePairer::Result::Pair) {
            if (now - last_frame > no_frames_ns) {
                thread_error_ = "no eye frames";
                failed_ = true;
                return;
            }
            if (pairer_.waiting())
                sleep_ns(poll_ns);   // the other side is due any moment: poll finely
            else
                idle_sleep(now);
            continue;
        }

        on_new_frame(now);
        frames_.tick();
        last_frame = now;
        IFrameTap *tap = tap_.load(std::memory_order_relaxed);
        const bool want_tap = tap && tap->tap_wanted();
        // The tracking rate is every Nth pair, counted rather than timed, so
        // 1:1 never drops a pair to jitter.
        const bool forward = pair_count++ % divisor_.load(std::memory_order_relaxed) == 0;
        if (!forward && !want_tap) {
            // Skip the copy, but hand the slots back exactly as a consumed pair would.
            client_.release(layout_, false, idx.l);
            client_.release(layout_, true, idx.r);
            continue;
        }

        uint64_t t0 = mono_ns();
        StereoFrame sf;
        seq++;
        // Both copies always run: each hands its ring slot back to dsp_service
        // whether it succeeds or not (a short-circuit here leaked the right slot).
        // Each view is stamped when its index was first seen to advance; the
        // pair with the midpoint, as StereoFrame defines it.
        const bool ok_l = copy_view(false, idx.l, step.l_ns, seq, &sf.left);
        const bool ok_r = copy_view(true, idx.r, step.r_ns, seq, &sf.right);
        if (!ok_l || !ok_r)
            continue;
        copy_time_.add(mono_ns() - t0);
        sf.skew_ns = int64_t(step.l_ns) - int64_t(step.r_ns);
        sf.capture_ns = step.r_ns + uint64_t(sf.skew_ns / 2);
        if (want_tap)
            tap->tap_frames(name_, sf);
        if (forward) {
            processed_.tick();
            out_.put(std::move(sf));
        }
    }
}

void DspEyeSource::tick(uint64_t now) {
    // Only an explicit "not worn" pauses; unknown presence keeps streaming.
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
            bool fatal = consecutive_faults_ >= kMaxLayoutFaults;
            stop(last_error_.c_str());
            restarts_.add();
            if (fatal) {
                LOG.error("eye: disabled: %s", last_error_.c_str());
                state_ = State::Unavailable;
                return;
            }
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
            LOG.info("eye: streaming");
        } else if (state_ != State::Unavailable) {
            backoff_ns_ = backoff_ns_ ? std::min(backoff_ns_ * 2, backoff_max_ns_) : backoff_min_ns_;
            retry_at_ns_ = now + backoff_ns_;
            state_ = State::Backoff;
            LOG.warn("eye: %s; retrying in %llus", last_error_.c_str(), (unsigned long long)(backoff_ns_ / 1000000000ull));
        }
        return;
    }
}

void DspEyeSource::shutdown() {
    if (state_ == State::Streaming) {
        stop("shutdown");
        state_ = State::Connecting;
    }
}

} // namespace babble
