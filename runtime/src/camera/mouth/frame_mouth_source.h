#pragma once

#include "camera/mouth/exposure_controller.h"
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
// Driven from the main thread by tick(); capture runs on two threads that
// pair frames by timestamp and publish the latest pair to `out`, subsampled
// to process_fps.
class FrameMouthSource {
public:
    enum class State { Disabled, WaitingForRoute, Streaming, Backoff };

    FrameMouthSource(const nlohmann::json &mouth_cfg, const nlohmann::json &threads_cfg,
                     LatestSlot<StereoFrame> &out, StatsRegistry &stats);
    ~FrameMouthSource();

    // routed: the camera arbiter reports the lower cameras are routed.
    void tick(uint64_t now_ns, bool routed);
    void shutdown();

    // User-facing subsample of the tracking rate; takes effect immediately.
    void set_process_fps(double fps);

    State state() const { return state_; }
    const char *state_name() const;
    const std::string &last_error() const { return last_error_; }

private:
    bool start();
    void stop(const char *why);
    void on_frame(FramePtr frame);
    void apply_control_set(const char *label, const nlohmann::json &set);

    nlohmann::json cfg_;
    LatestSlot<StereoFrame> &out_;
    StatsRegistry &stats_;

    std::unique_ptr<V4l2Capture> left_, right_;
    V4l2Controls controls_;
    std::vector<std::pair<std::string, int64_t>> saved_strobe_;
    StereoPairer pairer_;
    RateLimiter limiter_;
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
