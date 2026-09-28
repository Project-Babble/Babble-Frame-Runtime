#pragma once

#include "camera/tracking_source.h"
#include "camera/usb/usb_device.h"
#include "core/metrics.h"
#include "core/threading.h"
#include "frame/frame_pool.h"
#include "frame/mailbox.h"
#include "frame/rate_limiter.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace babble {

// A USB camera for the mouth, normally a Babble face tracker (OpenIris
// firmware, "openiristracker", USB 303a:8000), plugged into the Frame: an
// MJPEG UVC camera, streamed through usbfs because the Frame's kernel has no
// uvcvideo driver (camera/usb/). Frames are decoded to 8-bit grayscale and
// published as the left frame of a StereoFrame (right is empty), origin Face.
// Config type "babble_usb".
//
// Until the camera is plugged in the source waits (Starting, not a fault);
// unplugging goes back to waiting. Pauses with the headset off or tracking off
// like the Frame's own cameras.
//
// Which camera: config "camera" (live: set_option), a usb::device_id() such as
// "303a:8000:28372F18F94C", or "" (automatic: the Babble tracker if one is
// plugged in, else the first UVC camera).
//
// Config: camera (""), vendor ("303a") and product ("8000") preferred by the
// automatic choice, width/height (240), fps (30, asked of the camera; its
// default rate if it refuses), process_fps (30: the rate frames are decoded
// and passed on at; the mono face model costs ~30 ms a frame), tracking,
// pause_when_absent, reopen_backoff_ms {min, max}.
class BabbleUsbSource : public ITrackingSource {
public:
    enum class State { Disabled, Waiting, Streaming, Backoff, Paused };

    BabbleUsbSource(const nlohmann::json &cfg, SourceDeps deps);
    ~BabbleUsbSource() override;

    void tick(uint64_t now_ns) override;
    void shutdown() override;
    void set_process_fps(double fps) override;
    void set_tracking(bool on) override;
    bool tracking() const override { return tracking_; }
    bool streaming() const override { return state_ == State::Streaming; }
    bool set_option(const std::string &key, const nlohmann::json &value) override;
    // The camera setting ("" = automatic) and the id of the one streaming ("" if none).
    const std::string &camera() const { return camera_; }
    const std::string &active_camera() const { return active_id_; }
    SourceActivity activity() const override;
    const char *state_name() const override;
    const std::string &last_error() const override { return last_error_; }

private:
    // The camera to use now, per the setting; nullopt if it isn't plugged in.
    std::optional<usb::Found> pick() const;
    bool start(const usb::Found &dev);
    void stop(const char *why);
    void run(usb::Found dev);

    std::string name_;
    ThreadSpec thread_spec_;
    LatestSlot<StereoFrame> &out_;
    StatsRegistry &stats_;
    const SteamVrClient *steamvr_ = nullptr;
    bool pause_when_absent_ = true;
    bool tracking_ = true;   // main thread only
    std::string camera_;       // main thread
    std::string active_id_;    // main thread: set on start, cleared on stop
    uint16_t vendor_ = 0x303a, product_ = 0x8000;
    uint16_t width_ = 240, height_ = 240;
    double fps_ = 30;

    std::shared_ptr<FramePool> pool_;
    RateLimiter limiter_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> opened_{false};   // the thread opened the camera (Streaming until then means "opening")
    std::string thread_error_;   // written by the thread before failed_

    State state_ = State::Disabled;
    uint64_t retry_at_ns_ = 0, backoff_ns_ = 0, backoff_min_ns_, backoff_max_ns_;
    std::string last_error_;
    bool logged_waiting_ = false;

    RateStat frames_, processed_;
    Counter restarts_, pool_exhausted_, undecodable_, reassembly_dropped_;
    LatencyStat decode_time_;
};

} // namespace babble
