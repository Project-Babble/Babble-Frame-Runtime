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

// A USB mouth camera that the source streams through usbfs, because the kernel of the Frame has no uvcvideo
// driver.
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
    void set_frame_tap(IFrameTap *tap) override { tap_.store(tap); }
    // Returns the camera setting, which is empty for the automatic choice.
    const std::string &camera() const { return camera_; }
    const std::string &active_camera() const { return active_id_; }
    SourceActivity activity() const override;
    const char *state_name() const override;
    const std::string &last_error() const override { return last_error_; }

private:
    // Returns the camera for the setting, or nullopt if that camera is not plugged in.
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
    bool tracking_ = true;   // Only the main thread uses this.
    std::string camera_;       // Only the main thread uses this.
    std::string active_id_;    // The main thread sets this on start and clears it on stop.
    uint16_t vendor_ = 0x303a, product_ = 0x8000;
    uint16_t width_ = 240, height_ = 240;
    double fps_ = 30;

    std::shared_ptr<FramePool> pool_;
    RateLimiter limiter_;
    std::atomic<IFrameTap *> tap_{nullptr};
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> opened_{false};   // True after the thread opened the camera.
    std::string thread_error_;   // The thread writes this before it sets failed_.

    State state_ = State::Disabled;
    uint64_t retry_at_ns_ = 0, backoff_ns_ = 0, backoff_min_ns_, backoff_max_ns_;
    std::string last_error_;
    bool logged_waiting_ = false;

    RateStat frames_, processed_;
    Counter restarts_, pool_exhausted_, undecodable_, reassembly_dropped_;
    LatencyStat decode_time_;
};

}
