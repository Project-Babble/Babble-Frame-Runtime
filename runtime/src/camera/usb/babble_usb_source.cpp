#include "camera/usb/babble_usb_source.h"

#include "camera/usb/uvc_camera.h"
#include "core/clock.h"
#include "core/log.h"
#include "model/image_ops.h"
#include "platform/steamvr_client.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace {
const babble::log::Logger LOG("usb");
}

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

// The largest decoded image that the frame buffers accept, and the size when the mode is not known.
constexpr size_t kMaxPixels = 1920 * 1080, kFallbackPixels = 640 * 480;

// Undoes the JSON pointer escapes (~1 is "/" and ~0 is "~") in one token of a setting id.
std::string unescape_token(const std::string &t) {
    std::string out;
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i] == '~' && i + 1 < t.size() && (t[i + 1] == '0' || t[i + 1] == '1')) {
            out += t[i + 1] == '1' ? '/' : '~';
            i++;
        } else {
            out += t[i];
        }
    }
    return out;
}

// Returns true if this key and value are a valid camera setting.
bool valid_camera_setting(const std::string &key, const nlohmann::json &v) {
    if (key == "format")
        return v.is_string() && uvc::format_from_key(v.get<std::string>()) != uvc::FormatKind::Unknown;
    if (key == "width" || key == "height")
        return v.is_number_integer() && v.get<int>() > 0 && v.get<int>() <= 8192;
    if (key == "fps")
        return v.is_number() && v.get<double>() > 0 && v.get<double>() <= 1000;
    if (key == "rotate")
        return v.is_number_integer() && v.get<int>() >= 0 && v.get<int>() < 360 && v.get<int>() % 90 == 0;
    if (key == "flip_h" || key == "flip_v")
        return v.is_boolean();
    return false;
}

}

BabbleUsbSource::BabbleUsbSource(const nlohmann::json &cfg, SourceDeps deps)
    : name_(deps.name), thread_spec_(thread_spec(deps.threads, cfg.value("thread", "face"), "bbl-face")),
      out_(deps.out), stats_(deps.stats), steamvr_(deps.steamvr),
      pause_when_absent_(cfg.value("pause_when_absent", true)), tracking_(cfg.value("tracking", true)),
      camera_(cfg.value("camera", "")),
      vendor_(hex_id(cfg, "vendor", 0x303a)), product_(hex_id(cfg, "product", 0x8000)),
      width_(cfg.value("width", uint16_t(240))), height_(cfg.value("height", uint16_t(240))),
      fps_(cfg.value("fps", 30.0)), tracker_flip_h_(cfg.value("tracker_flip_h", true)),
      cameras_(cfg.contains("cameras") && cfg["cameras"].is_object() ? cfg["cameras"] : nlohmann::json::object()),
      limiter_(cfg.value("process_fps", 30.0)) {
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

BabbleUsbSource::CameraSettings BabbleUsbSource::settings_for(const usb::Found &dev) const {
    CameraSettings s;
    s.mode = {uvc::FormatKind::Mjpeg, width_, height_, fps_};
    s.flip_h = dev.vendor == vendor_ && dev.product == product_ && tracker_flip_h_;
    const auto it = cameras_.find(usb::device_id(dev));
    if (it == cameras_.end() || !it->is_object())
        return s;
    for (const auto &[key, v] : it->items()) {
        if (!valid_camera_setting(key, v))
            continue;   // A bad value keeps the default, and set_option() never stores one.
        if (key == "format")
            s.mode.format = uvc::format_from_key(v.get<std::string>());
        else if (key == "width")
            s.mode.width = uint16_t(v.get<int>());
        else if (key == "height")
            s.mode.height = uint16_t(v.get<int>());
        else if (key == "fps")
            s.mode.fps = v.get<double>();
        else if (key == "rotate")
            s.rotate = v.get<int>();
        else if (key == "flip_h")
            s.flip_h = v.get<bool>();
        else if (key == "flip_v")
            s.flip_v = v.get<bool>();
    }
    return s;
}

nlohmann::json BabbleUsbSource::to_json(const CameraSettings &s) {
    return {{"format", uvc::format_key(s.mode.format)},
            {"width", s.mode.width},
            {"height", s.mode.height},
            {"fps", s.mode.fps},
            {"rotate", s.rotate},
            {"flip_h", s.flip_h},
            {"flip_v", s.flip_v}};
}

bool BabbleUsbSource::set_camera_option(const std::string &key, const nlohmann::json &value) {
    const size_t slash = key.find('/');
    const std::string id = unescape_token(key.substr(0, slash));
    if (id.empty())
        return false;
    nlohmann::json next = cameras_.contains(id) && cameras_[id].is_object() ? cameras_[id] : nlohmann::json::object();
    if (slash == std::string::npos) {   // The value has all the settings of the camera.
        if (!value.is_object())
            return false;
        for (const auto &[k, v] : value.items()) {
            if (!valid_camera_setting(k, v))
                return false;
            next[k] = v;
        }
    } else {
        const std::string field = key.substr(slash + 1);
        if (!valid_camera_setting(field, value))
            return false;
        next[field] = value;
    }
    cameras_[id] = next;
    LOG.info("%s: camera %s settings %s", name_.c_str(), id.c_str(), next.dump().c_str());
    if (state_ == State::Streaming && active_id_ == id) {
        const CameraSettings s = settings_for(active_dev_);
        rotate_ = s.rotate;
        flip_h_ = s.flip_h;
        flip_v_ = s.flip_v;
        if (!(s.mode == active_mode_)) {   // A new format, size or rate needs a new stream.
            stop("camera mode changed");
            state_ = State::Waiting;
            retry_at_ns_ = 0;
        }
    }
    return true;
}

bool BabbleUsbSource::set_option(const std::string &key, const nlohmann::json &value) {
    if (key.starts_with("cameras/"))
        return set_camera_option(key.substr(8), value);
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
    // The automatic choice is the Babble tracker if it is plugged in, or else the first camera.
    for (const auto &c : cams)
        if (c.vendor == vendor_ && c.product == product_)
            return c;
    if (!cams.empty())
        return cams.front();
    return std::nullopt;
}

bool BabbleUsbSource::start(const usb::Found &dev) {
    const CameraSettings s = settings_for(dev);
    active_dev_ = dev;
    active_mode_ = s.mode;
    rotate_ = s.rotate;
    flip_h_ = s.flip_h;
    flip_v_ = s.flip_v;
    // Size the frame buffers for the mode that the camera will stream. The descriptor comes from sysfs.
    pool_pixels_ = kFallbackPixels;
    const auto desc = usb::config_descriptor(dev);
    if (const auto info = uvc::parse_config(desc.data(), desc.size()))
        if (const auto p = uvc::pick_mode(*info, s.mode))
            pool_pixels_ = std::min(kMaxPixels, size_t(p->frame->width) * p->frame->height);
    pool_ = FramePool::create_owned(6, pool_pixels_);
    stop_ = false;
    failed_ = false;
    opened_ = false;
    limiter_.reset();
    active_id_ = usb::device_id(dev);
    thread_ = start_thread(thread_spec_, [this, dev, mode = s.mode] { run(dev, mode); });
    return true;
}

void BabbleUsbSource::stop(const char *why) {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
    clear_preview();
    pool_.reset();
    active_id_.clear();
    LOG.info("%s: stopped (%s)", name_.c_str(), why);
}

void BabbleUsbSource::transform(std::vector<uint8_t> (&scratch)[2], int w, int h, int rotate, bool flip_h, bool flip_v,
                                uint8_t *dst, int *out_w, int *out_h) {
    const uint32_t uw = uint32_t(w), uh = uint32_t(h);
    scratch[1].resize(size_t(w) * h);
    img::View cur{scratch[0].data(), uw, uh, uw};
    if (flip_h) {
        img::flip_horizontal(cur, {scratch[1].data(), uw, uh, uw});
        cur = {scratch[1].data(), uw, uh, uw};
    }
    if (flip_v) {
        uint8_t *o = cur.data == scratch[1].data() ? scratch[0].data() : scratch[1].data();
        img::flip_vertical(cur, {o, uw, uh, uw});
        cur = {o, uw, uh, uw};
    }
    const int ccw = (4 - rotate / 90) % 4;   // The setting turns clockwise, and img::rotate turns counter-clockwise.
    if (!ccw) {
        memcpy(dst, cur.data, size_t(w) * h);
        *out_w = w;
        *out_h = h;
        return;
    }
    const bool swap = ccw % 2 == 1;
    img::rotate(cur, {dst, swap ? uh : uw, swap ? uw : uh, swap ? uh : uw}, ccw);
    *out_w = swap ? h : w;
    *out_h = swap ? w : h;
}

bool BabbleUsbSource::take_preview(Preview &out) {
    preview_until_ns_.store(mono_ns() + 1'000'000'000ull, std::memory_order_relaxed);
    std::lock_guard lock(preview_mutex_);
    if (preview_.seq == out.seq)
        return false;
    out = preview_;
    return true;
}

void BabbleUsbSource::clear_preview() {
    std::lock_guard lock(preview_mutex_);
    preview_.camera.clear();
    preview_.gray.clear();
    preview_.width = preview_.height = 0;
    preview_.seq++;
}

void BabbleUsbSource::make_preview(const std::string &camera, const uint8_t *gray, int w, int h) {
    // Shrink the frame to fit the preview box, and keep its shape.
    const double scale = std::min({1.0, double(kPreviewWidth) / w, double(kPreviewHeight) / h});
    const int pw = std::max(1, int(w * scale)), ph = std::max(1, int(h * scale));
    std::vector<uint8_t> small(size_t(pw) * ph);
    const img::View src{gray, uint32_t(w), uint32_t(h), uint32_t(w)};
    if (pw == w && ph == h)
        memcpy(small.data(), gray, small.size());
    else
        img::resize_area(src, {small.data(), uint32_t(pw), uint32_t(ph), uint32_t(pw)});
    std::lock_guard lock(preview_mutex_);
    preview_.camera = camera;
    preview_.width = pw;
    preview_.height = ph;
    preview_.gray.swap(small);
    preview_.seq++;
}

void BabbleUsbSource::run(usb::Found dev, uvc::Mode mode) {
    uvc::Camera cam;
    std::string err;
    if (!cam.open(dev, mode, &err)) {
        thread_error_ = "cannot open " + usb::device_label(dev) + ": " + err;
        failed_ = true;
        return;
    }
    opened_ = true;
    const auto &p = cam.negotiated();
    const std::string transfer = cam.iso() ? "isochronous alt " + std::to_string(cam.iso_alt()) : "bulk";
    LOG.info("%s: streaming %s (%s, %s): %s %ux%u at %.0f fps, %s", name_.c_str(), usb::device_label(dev).c_str(),
             usb::device_id(dev).c_str(), dev.node.c_str(), uvc::format_label(cam.format()), cam.frame_size().width,
             cam.frame_size().height, p.frame_interval ? 1e7 / p.frame_interval : 0.0, transfer.c_str());
    const uvc::FormatKind format = cam.format();
    const std::string camera_id = usb::device_id(dev);
    const uint16_t fw = cam.frame_size().width, fh = cam.frame_size().height;

    uint32_t seq = 0;
    uint64_t last_dropped = 0;
    // Log a stream report at debug level each 10 s.
    uint64_t report_at = mono_ns() + 10'000'000'000ull, frames_in = 0, frames_kept = 0, bytes_in = 0;
    while (!stop_) {
        const bool ok = cam.read(200, [&](const uint8_t *data, size_t size) {
            const uint64_t now = mono_ns();
            frames_.tick();
            frames_in++;
            bytes_in += size;
            // The frame tap gets all frames, but tracking gets only the frames that the limiter accepts.
            IFrameTap *tap = tap_.load(std::memory_order_relaxed);
            const bool want_tap = tap && tap->tap_wanted();
            const bool forward = limiter_.accept(now);
            const bool want_preview =
                now < preview_until_ns_.load(std::memory_order_relaxed) && now >= preview_next_ns_;
            if (!forward && !want_tap && !want_preview)
                return;   // Skip the decode because nothing uses the frame.
            auto frame = pool_->acquire();
            if (!frame) {
                pool_exhausted_.add();
                return;
            }
            int w = 0, h = 0;
            uint8_t *dst = const_cast<uint8_t *>(frame->data);
            const int rotate = rotate_.load(std::memory_order_relaxed);
            const bool flip_h = flip_h_.load(std::memory_order_relaxed), flip_v = flip_v_.load(std::memory_order_relaxed);
            if (!rotate && !flip_h && !flip_v) {
                if (!uvc::to_gray(format, data, size, fw, fh, dst, pool_pixels_, &w, &h)) {
                    undecodable_.add();
                    return;
                }
            } else {
                scratch_[0].resize(pool_pixels_);
                if (!uvc::to_gray(format, data, size, fw, fh, scratch_[0].data(), pool_pixels_, &w, &h)) {
                    undecodable_.add();
                    return;
                }
                transform(scratch_, w, h, rotate, flip_h, flip_v, dst, &w, &h);
            }
            if (want_preview) {
                make_preview(camera_id, dst, w, h);
                preview_next_ns_ = now + 1'000'000'000ull / kPreviewFps;
            }
            decode_time_.add(mono_ns() - now);
            frame->origin = FrameOrigin::Face;
            frame->fmt = PixelFormat::Gray8;
            frame->width = uint32_t(w);
            frame->height = uint32_t(h);
            frame->stride = uint32_t(w);
            frame->size = size_t(w) * size_t(h);
            frame->capture_ns = now;   // Use the arrival time because the UVC payloads have no reliable timestamp.
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
    // Pause only if SteamVR reports that the headset is not worn.
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
        if (opened_ && backoff_ns_) {   // The camera works again, so the next failure starts at the minimum backoff.
            backoff_ns_ = 0;
            last_error_.clear();
        }
        if (failed_) {
            last_error_ = thread_error_;
            stop(last_error_.c_str());
            restarts_.add();
            // Wait quietly for an unplugged camera, and wait and try again after other errors.
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
            retry_at_ns_ = now + 1000000000ull;   // Scan sysfs one time each second.
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

}
