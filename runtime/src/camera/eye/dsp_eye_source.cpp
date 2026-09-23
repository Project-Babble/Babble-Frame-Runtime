#include "camera/eye/dsp_eye_source.h"

#include "core/clock.h"
#include "core/log.h"

#include <cstring>

namespace babble {

namespace {

constexpr uint32_t kMaxLayoutFaults = 3;

} // namespace

DspEyeSource::DspEyeSource(const nlohmann::json &eye_cfg, const nlohmann::json &threads_cfg,
                           LatestSlot<StereoFrame> &out, StatsRegistry &stats)
    : cfg_(eye_cfg), thread_spec_(thread_spec(threads_cfg, "eye", "bbl-eye")), out_(out), stats_(stats),
      limiter_(eye_cfg.value("process_fps", 120.0)) {
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

    stats_.add("eye.fps", &frames_);
    stats_.add("eye.process_fps", &processed_);
    stats_.add("eye.layout_faults", &layout_faults_);
    stats_.add("eye.restarts", &restarts_);
    stats_.add("eye.pool_exhausted", &pool_exhausted_);
    stats_.add("eye.copy", &copy_time_);
}

DspEyeSource::~DspEyeSource() {
    shutdown();
    stats_.remove_prefix("eye.");
}

void DspEyeSource::set_process_fps(double fps) {
    limiter_.set_fps(fps);
    LOG_INFO("eye: process rate %.1f fps", fps);
}

const char *DspEyeSource::state_name() const {
    switch (state_) {
    case State::Disabled: return "Disabled";
    case State::Connecting: return "Connecting";
    case State::Streaming: return "Streaming";
    case State::Backoff: return "Backoff";
    case State::Unavailable: return "Unavailable";
    }
    return "?";
}

bool DspEyeSource::start() {
    if (!client_.connect(cfg_.value("dsp_version", 17u))) {
        last_error_ = "cannot connect to dsp_service";
        return false;
    }
    std::string why;
    auto layout = client_.probe(candidates_, &why);
    if (!layout) {
        // Unknown layout: never write to it. Needs a runtime update, not a retry.
        last_error_ = "unrecognized dsp_service layout (" + why + ")";
        LOG_ERROR("eye: %s", last_error_.c_str());
        client_.disconnect();
        state_ = State::Unavailable;
        return false;
    }
    layout_ = *layout;
    for (const Crop *c : {&crop_l_, &crop_r_}) {
        if (c->w == 0 || c->h == 0 || c->x + c->w > layout_.width || c->y + c->h > layout_.height) {
            last_error_ = "eye crop outside the 512x512 frame";
            LOG_ERROR("eye: %s", last_error_.c_str());
            client_.disconnect();
            state_ = State::Unavailable;
            return false;
        }
    }
    LOG_INFO("eye: dsp_service layout at ctrl base 0x%x", layout_.ctrl_base);

    // Two frames per stereo pair, a few pairs in flight downstream.
    size_t bytes = size_t(std::max(crop_l_.w * crop_l_.h, crop_r_.w * crop_r_.h));
    pool_ = FramePool::create_owned(12, bytes);

    client_.start_streaming(layout_, float(cfg_.value("request_fps", 0.0)));
    stop_ = false;
    failed_ = false;
    limiter_.reset();
    thread_ = start_thread(thread_spec_, [this] { run(); });
    return true;
}

void DspEyeSource::stop(const char *why) {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
    client_.restore_fps(layout_);
    client_.disconnect();
    pool_.reset();
    LOG_INFO("eye: stopped (%s)", why);
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
    uint32_t last_l = DspClient::kNotReady, last_r = DspClient::kNotReady;
    uint64_t last_frame = mono_ns();
    uint32_t seq = 0;

    if (!logged_gap_) {
        // Research aid: the 64 bytes between frame end and next slot may hold metadata.
        const uint8_t *s = client_.slot_ptr(layout_, false, 0) + layout_.frame_bytes;
        char hex[3 * 64 + 1] = {0};
        for (int i = 0; i < 64; i++)
            snprintf(hex + 3 * i, 4, "%02x ", s[i]);
        LOG_DEBUG("eye: slot trailer bytes: %s", hex);
        logged_gap_ = true;
    }

    while (!stop_) {
        auto idx = client_.read_indices(layout_);
        uint64_t now = mono_ns();
        bool bad_l = idx.l != DspClient::kNotReady && idx.l >= layout_.slots;
        bool bad_r = idx.r != DspClient::kNotReady && idx.r >= layout_.slots;
        if (bad_l || bad_r) {
            layout_faults_.add();
            if (layout_faults_.get() >= kMaxLayoutFaults) {
                thread_error_ = "dsp_service index words out of range; layout changed?";
                failed_ = true;
                return;
            }
            sleep_ns(poll_ns);
            continue;
        }
        if (idx.l == DspClient::kNotReady || idx.r == DspClient::kNotReady || (idx.l == last_l && idx.r == last_r)) {
            if (now - last_frame > no_frames_ns) {
                thread_error_ = "no eye frames";
                failed_ = true;
                return;
            }
            sleep_ns(poll_ns);
            continue;
        }

        frames_.tick();
        if (!limiter_.accept(now)) {
            // Subsampled out: skip the copy, but hand the slots back exactly
            // as a consumed frame would.
            client_.release(layout_, false, idx.l);
            client_.release(layout_, true, idx.r);
            last_l = idx.l;
            last_r = idx.r;
            last_frame = now;
            continue;
        }

        uint64_t t0 = mono_ns();
        StereoFrame sf;
        seq++;
        if (!copy_view(false, idx.l, now, seq, &sf.left) || !copy_view(true, idx.r, now, seq, &sf.right)) {
            last_l = idx.l;
            last_r = idx.r;
            continue;
        }
        copy_time_.add(mono_ns() - t0);
        sf.capture_ns = now;
        sf.skew_ns = 0;
        last_l = idx.l;
        last_r = idx.r;
        last_frame = now;
        processed_.tick();
        out_.put(std::move(sf));
    }
}

void DspEyeSource::tick(uint64_t now) {
    switch (state_) {
    case State::Disabled:
    case State::Unavailable:
        return;
    case State::Streaming:
        if (failed_) {
            last_error_ = thread_error_;
            bool fatal = layout_faults_.get() >= kMaxLayoutFaults;
            stop(last_error_.c_str());
            restarts_.add();
            if (fatal) {
                LOG_ERROR("eye: disabled: %s", last_error_.c_str());
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
        if (now < retry_at_ns_)
            return;
        if (start()) {
            state_ = State::Streaming;
            backoff_ns_ = 0;
            LOG_INFO("eye: streaming");
        } else if (state_ != State::Unavailable) {
            backoff_ns_ = backoff_ns_ ? std::min(backoff_ns_ * 2, backoff_max_ns_) : backoff_min_ns_;
            retry_at_ns_ = now + backoff_ns_;
            state_ = State::Backoff;
            LOG_WARN("eye: %s; retrying in %llus", last_error_.c_str(), (unsigned long long)(backoff_ns_ / 1000000000ull));
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
