#pragma once

#include "camera/mouth/exposure_controller.h"
#include "camera/tracking_source.h"
#include "camera/v4l2/v4l2_capture.h"
#include "camera/v4l2/v4l2_controls.h"
#include "core/metrics.h"
#include "frame/mailbox.h"
#include "frame/rate_limiter.h"
#include "frame/stereo_pairer.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace babble {

// The Babble mouth camera pair on the Frame's lower extension port: two
// OV6211 sensors on separate CAMSS video nodes, controlled through the
// sensor subdev (controls are mirrored to both sensors by the driver; the
// LED strobe path is on the secondary).
//
// Config type "frame_ov6211_stereo". Driven from the main thread by tick(),
// which waits for the camera arbiter to report the cameras routed; capture
// runs on two threads that pair frames by timestamp and publish the latest
// pair to the output slot, subsampled to process_fps.
class FrameMouthSource : public ITrackingSource {
public:
    enum class State { Disabled, WaitingForRoute, Streaming, Backoff, Paused };

    FrameMouthSource(const nlohmann::json &cfg, SourceDeps deps);
    ~FrameMouthSource() override;

    void tick(uint64_t now_ns) override;
    void shutdown() override;
    void set_process_fps(double fps) override;
    void set_tracking(bool on) override;
    bool tracking() const override { return tracking_; }
    void set_frame_tap(IFrameTap *tap) override { tap_.store(tap); }

    bool streaming() const override { return state_ == State::Streaming; }
    SourceActivity activity() const override;
    const char *state_name() const override;
    // While unrouted for a while, the arbiter's explanation (for the UI).
    const std::string &last_error() const override { return route_fault_ ? route_error_ : last_error_; }

private:
    bool start();
    void stop(const char *why);
    void on_frame(FramePtr frame);
    void apply_control_set(const char *label, const nlohmann::json &set);

    nlohmann::json cfg_;
    std::string name_;
    LatestSlot<StereoFrame> &out_;
    StatsRegistry &stats_;
    const CameraArbiterClient &arbiter_;
    const SteamVrClient *steamvr_ = nullptr;
    bool pause_when_absent_ = true;
    bool tracking_ = true;   // main thread only
    uint64_t unrouted_since_ns_ = 0;
    bool route_fault_ = false;   // waiting for the route longer than the grace period
    std::string route_error_;

    std::unique_ptr<V4l2Capture> left_, right_;
    V4l2Controls controls_;
    std::vector<std::pair<std::string, int64_t>> saved_strobe_;
    StereoPairer pairer_;
    RateLimiter limiter_;
    std::atomic<IFrameTap *> tap_{nullptr};
    std::unique_ptr<IExposureController> ae_;
    std::mutex pair_mutex_;   // limiter + AE are fed from both capture threads
    uint32_t ae_interval_ = 8;
    uint32_t ae_counter_ = 0;

    State state_ = State::Disabled;
    std::atomic<bool> lost_{false};
    uint64_t retry_at_ns_ = 0;
    uint64_t backoff_ns_ = 0;
    uint64_t backoff_min_ns_, backoff_max_ns_;
    std::string last_error_;

    RateStat pairs_, processed_;
    Counter unmatched_l_, unmatched_r_, restarts_;
    LatencyStat skew_, pair_latency_;
};

} // namespace babble
