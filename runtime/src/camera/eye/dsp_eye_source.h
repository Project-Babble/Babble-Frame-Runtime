#pragma once

#include "camera/eye/dsp_client.h"
#include "camera/eye/eye_pairer.h"
#include "camera/tracking_source.h"
#include "core/metrics.h"
#include "core/threading.h"
#include "frame/frame_pool.h"
#include "frame/mailbox.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace babble {

// Gets the Steam Frame eye cameras from Valve's dsp_service and copies the frames out of the shared ring.
class DspEyeSource : public ITrackingSource {
public:
    enum class State { Disabled, Connecting, Streaming, Backoff, Unavailable, Paused };

    DspEyeSource(const nlohmann::json &cfg, SourceDeps deps);
    ~DspEyeSource() override;

    void tick(uint64_t now_ns) override;
    void shutdown() override;
    void set_process_fps(double fps) override;   // The eye rate is a divisor of the camera rate, so this only logs a warning.
    void set_tracking(bool on) override;
    bool set_option(const std::string &key, const nlohmann::json &value) override;
    void set_frame_tap(IFrameTap *tap) override { tap_.store(tap); }

    double camera_fps() const override { return camera_fps_.load(std::memory_order_relaxed); }
    uint32_t rate_divisor() const { return divisor_.load(std::memory_order_relaxed); }
    bool tracking() const override { return tracking_; }

    bool streaming() const override { return state_ == State::Streaming; }
    SourceActivity activity() const override;
    const char *state_name() const override;
    const std::string &last_error() const override { return last_error_; }

private:
    struct Crop {
        uint32_t y = 0, x = 0, h = 0, w = 0;
    };

    bool start();
    void stop(const char *why);
    void run();
    bool copy_view(bool right, uint32_t idx, uint64_t now, uint32_t seq, FramePtr *out);

    nlohmann::json cfg_;
    std::string name_;
    ThreadSpec thread_spec_;
    LatestSlot<StereoFrame> &out_;
    StatsRegistry &stats_;
    const SteamVrClient *steamvr_ = nullptr;
    bool pause_when_absent_ = true;
    bool tracking_ = true;   // Only the main thread uses this.

    DspClient client_;
    DspClient::Layout layout_;
    std::vector<uint32_t> candidates_;
    Crop crop_l_, crop_r_;
    std::shared_ptr<FramePool> pool_;
    EyePairer pairer_;
    std::atomic<uint32_t> divisor_{1};
    std::atomic<IFrameTap *> tap_{nullptr};
    std::atomic<float> camera_fps_{0};
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> failed_{false};
    std::string thread_error_;

    State state_ = State::Disabled;
    uint64_t retry_at_ns_ = 0, backoff_ns_ = 0, backoff_min_ns_, backoff_max_ns_;
    std::string last_error_;
    std::string reported_layout_error_;
    bool not_ready_ = false;   // True if the last start found that the control block was not set up.
    bool logged_gap_ = false;
    bool logged_dsp_messages_ = false;

    RateStat frames_, processed_, side_l_, side_r_;
    Counter layout_faults_, restarts_, pool_exhausted_, late_detect_, half_pairs_;   // These are lifetime totals.
    Counter camera_rate_;
    uint32_t consecutive_faults_ = 0;
    LatencyStat copy_time_;
};

}
