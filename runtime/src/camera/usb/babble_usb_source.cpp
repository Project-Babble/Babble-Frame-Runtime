#include "camera/usb/babble_usb_source.h"

#include "camera/usb/jpeg.h"
#include "camera/usb/uvc_camera.h"
#include "core/clock.h"
#include "core/log.h"
#include "platform/steamvr_client.h"

#include <cstdlib>

namespace {
const babble::log::Logger LOG("usb");
} // namespace

namespace babble {

namespace {

uint16_t hex_id(const nlohmann::json &cfg, const char *key, uint16_t fallback) {
    if (!cfg.contains(key))
        return fallback;
    const auto &v = cfg[key];
    if (v.is_number_unsigned())
        return v.get<uint16_t>();
    if (v.is_string())
        return uint16_t(std::strtoul(v.get<std::string>().c_str(), nullptr, 16));
    return fallback;
}

// Frame buffers: the largest decoded image we accept.
constexpr size_t kMaxPixels = 640 * 480;

} // namespace

BabbleUsbSource::BabbleUsbSource(const nlohmann::json &cfg, SourceDeps deps)
    : name_(deps.name), thread_spec_(thread_spec(deps.threads, cfg.value("thread", "face"), "bbl-face")),
      out_(deps.out), stats_(deps.stats), steamvr_(deps.steamvr),
      pause_when_absent_(cfg.value("pause_when_absent", true)), tracking_(cfg.value("tracking", true)),
      camera_(cfg.value("camera", "")),
      vendor_(hex_id(cfg, "vendor", 0x303a)), product_(hex_id(cfg, "product", 0x8000)),
      width_(cfg.value("width", uint16_t(240))), height_(cfg.value("height", uint16_t(240))),
      fps_(cfg.value("fps", 30.0)), limiter_(cfg.value("process_fps", 30.0)) {
    const auto backoff = cfg.value("reopen_backoff_ms", nlohmann::json::object());
    backoff_min_ns_ = uint64_t(backoff.value("min", 1000)) * 1000000ull;
    backoff_max_ns_ = uint64_t(backoff.value("max", 10000)) * 1000000ull;
    state_ = cfg.value("enabled", true) ? State::Waiting : State::Disabled;

    stats_.add(name_ + ".fps", &frames_);
    stats_.add(name_ + ".process_fps", &processed_);
    stats_.add(name_ + ".restarts", &restarts_);
    stats_.add(name_ + ".pool_exhausted", &pool_exhausted_);
    stats_.add(name_ + ".undecodable", &undecodable_);
    stats_.add(name_ + ".reassembly_dropped", &reassembly_dropped_);
    stats_.add(name_ + ".decode", &decode_time_);
}

BabbleUsbSource::~BabbleUsbSource() {
    shutdown();
    stats_.remove_prefix(name_ + ".");
}

void BabbleUsbSource::set_process_fps(double fps) {
    limiter_.set_fps(fps);
    LOG.info("%s: process rate %.1f fps", name_.c_str(), fps);
}

void BabbleUsbSource::set_tracking(bool on) {
    if (on != tracking_)
        LOG.info("%s: tracking turned %s", name_.c_str(), on ? "on" : "off");
    tracking_ = on;
}

SourceActivity BabbleUsbSource::activity() const {
    switch (state_) {
    case State::Disabled: return SourceActivity::Off;
    case State::Waiting: return SourceActivity::Starting;
    case State::Streaming: return opened_ ? SourceActivity::Streaming : SourceActivity::Starting;
    case State::Backoff: return SourceActivity::Fault;
    case State::Paused: return tracking_ ? SourceActivity::Paused : SourceActivity::Off;
    }
    return SourceActivity::Fault;
}

const char *BabbleUsbSource::state_name() const {
    switch (state_) {
    case State::Disabled: return "Disabled";
    case State::Waiting: return "Waiting";
    case State::Streaming: return "Streaming";
    case State::Backoff: return "Backoff";
    case State::Paused: return "Paused";
    }
    return "?";
}

bool BabbleUsbSource::set_option(const std::string &key, const nlohmann::json &value) {
    if (key != "camera" || !value.is_string())
        return false;
    if (value.get<std::string>() == camera_)
        return true;
    camera_ = value.get<std::string>();
    LOG.info("%s: camera set to %s", name_.c_str(), camera_.empty() ? "automatic" : camera_.c_str());
    if (state_ == State::Streaming && active_id_ != camera_) {
        stop("switching camera");
        state_ = State::Waiting;
        retry_at_ns_ = 0;
    }
    logged_waiting_ = false;
    return true;
}

std::optional<usb::Found> BabbleUsbSource::pick() const {
    const auto cams = usb::list_uvc();
    if (LOG.debug_enabled()) {
        std::string list;
        for (const auto &c : cams)
            list += (list.empty() ? "" : ", ") + usb::device_id(c);
        LOG.debug("%s: cameras [%s]; setting %s", name_.c_str(), list.c_str(),
                  camera_.empty() ? "automatic" : camera_.c_str());
    }
    if (!camera_.empty()) {
        for (const auto &c : cams)
            if (usb::device_id(c) == camera_)
                return c;
        return std::nullopt;
    }
    // Automatic: the Babble tracker if one is plugged in, else the first camera.
    for (const auto &c : cams)
        if (c.vendor == vendor_ && c.product == product_)
            return c;
    if (!cams.empty())
        return cams.front();
    return std::nullopt;
}

bool BabbleUsbSource::start(const usb::Found &dev) {
    pool_ = FramePool::create_owned(6, kMaxPixels);
    stop_ = false;
    failed_ = false;
    opened_ = false;
    limiter_.reset();
    active_id_ = usb::device_id(dev);
    thread_ = start_thread(thread_spec_, [this, dev] { run(dev); });
    return true;
}

void BabbleUsbSource::stop(const char *why) {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
    pool_.reset();
    active_id_.clear();
    LOG.info("%s: stopped (%s)", name_.c_str(), why);
}

void BabbleUsbSource::run(usb::Found dev) {
    uvc::Camera cam;
    std::string err;
    if (!cam.open(dev, width_, height_, fps_, &err)) {
        thread_error_ = "cannot open " + usb::device_label(dev) + ": " + err;
        failed_ = true;
        return;
    }
    opened_ = true;
    const auto &p = cam.negotiated();
    LOG.info("%s: streaming %s (%s, %s): MJPEG %ux%u at %.0f fps", name_.c_str(), usb::device_label(dev).c_str(),
             usb::device_id(dev).c_str(), dev.node.c_str(), cam.frame_size().width, cam.frame_size().height,
             p.frame_interval ? 1e7 / p.frame_interval : 0.0);

    uint32_t seq = 0;
    uint64_t last_dropped = 0;
    // A stream report every 10 s at debug level: what arrived and what was kept.
    uint64_t report_at = mono_ns() + 10'000'000'000ull, frames_in = 0, frames_kept = 0, bytes_in = 0;
    while (!stop_) {
        const bool ok = cam.read(200, [&](const uint8_t *data, size_t size) {
            const uint64_t now = mono_ns();
            frames_.tick();
            frames_in++;
            bytes_in += size;
            // A frame tap (the recorder) gets every frame; tracking only the subsampled ones.
            IFrameTap *tap = tap_.load(std::memory_order_relaxed);
            const bool want_tap = tap && tap->tap_wanted();
            const bool forward = limiter_.accept(now);
            if (!forward && !want_tap)
                return;   // subsampled out: skip the decode
            auto frame = pool_->acquire();
            if (!frame) {
                pool_exhausted_.add();
                return;
            }
            int w = 0, h = 0;
            uint8_t *dst = const_cast<uint8_t *>(frame->data);
            if (!uvc::looks_like_jpeg(data, size) || !jpeg::decode_gray(data, size, dst, kMaxPixels, &w, &h)) {
                undecodable_.add();
                return;
            }
            decode_time_.add(mono_ns() - now);
            frame->origin = FrameOrigin::Face;
            frame->fmt = PixelFormat::Gray8;
            frame->width = uint32_t(w);
            frame->height = uint32_t(h);
            frame->stride = uint32_t(w);
            frame->size = size_t(w) * size_t(h);
            frame->capture_ns = now;   // arrival: UVC bulk payloads carry no reliable timestamp here
            frame->arrival_ns = now;
            frame->seq = ++seq;
            StereoFrame sf;
            sf.left = std::move(frame);
            sf.capture_ns = now;
            if (want_tap)
                tap->tap_frames(name_, sf);
            if (!forward)
                return;
            processed_.tick();
            frames_kept++;
            out_.put(std::move(sf));
        }, &err);
        if (const uint64_t t = mono_ns(); t >= report_at) {
            LOG.debug("%s: %llu frames in 10 s (%.1f KB avg), %llu passed on, %llu dropped in reassembly", name_.c_str(),
                      (unsigned long long)frames_in, frames_in ? double(bytes_in) / 1024.0 / double(frames_in) : 0.0,
                      (unsigned long long)frames_kept, (unsigned long long)cam.dropped());
            report_at = t + 10'000'000'000ull;
            frames_in = frames_kept = bytes_in = 0;
        }
        if (cam.dropped() != last_dropped) {
            reassembly_dropped_.add(cam.dropped() - last_dropped);
            last_dropped = cam.dropped();
        }
        if (!ok) {
            thread_error_ = err;
            failed_ = true;
            return;
        }
    }
}

void BabbleUsbSource::tick(uint64_t now) {
    // Only an explicit "not worn" pauses; unknown presence keeps streaming.
    const bool absent = pause_when_absent_ && steamvr_ && steamvr_->worn() == Worn::No;
    const bool hold = !tracking_ || absent;
    const char *why = !tracking_ ? "tracking turned off" : "headset not worn";
    switch (state_) {
    case State::Disabled: return;
    case State::Paused:
        if (!hold) {
            LOG.info("%s: resuming", name_.c_str());
            state_ = State::Waiting;
            retry_at_ns_ = 0;
        }
        return;
    case State::Streaming:
        if (hold) {
            stop(why);
            state_ = State::Paused;
            return;
        }
        if (opened_ && backoff_ns_) {   // it works again: the next failure starts over at the minimum
            backoff_ns_ = 0;
            last_error_.clear();
        }
        if (failed_) {
            last_error_ = thread_error_;
            stop(last_error_.c_str());
            restarts_.add();
            // Unplugged: wait for it quietly. Anything else: back off and retry.
            if (!pick()) {
                state_ = State::Waiting;
                logged_waiting_ = false;
                retry_at_ns_ = now + 1000000000ull;
            } else {
                backoff_ns_ = backoff_ns_ ? std::min(backoff_ns_ * 2, backoff_max_ns_) : backoff_min_ns_;
                retry_at_ns_ = now + backoff_ns_;
                state_ = State::Backoff;
            }
        }
        return;
    case State::Waiting:
    case State::Backoff:
        if (hold) {
            state_ = State::Paused;
            return;
        }
        if (now < retry_at_ns_)
            return;
        const auto dev = pick();
        if (!dev) {
            if (!logged_waiting_)
                LOG.info("%s: waiting for %s", name_.c_str(),
                         camera_.empty() ? "a USB camera" : ("USB camera " + camera_).c_str());
            logged_waiting_ = true;
            last_error_ = camera_.empty() ? "waiting for a USB camera" : "waiting for the selected USB camera";
            state_ = State::Waiting;
            retry_at_ns_ = now + 1000000000ull;   // a sysfs scan a second
            return;
        }
        logged_waiting_ = false;
        start(*dev);
        state_ = State::Streaming;
        return;
    }
}

void BabbleUsbSource::shutdown() {
    if (state_ == State::Streaming) {
        stop("shutdown");
        state_ = State::Waiting;
    }
}

} // namespace babble
