#pragma once

#include "camera/eye/dsp_client.h"
#include "camera/tracking_source.h"
#include "core/metrics.h"
#include "core/threading.h"
#include "frame/frame_pool.h"
#include "frame/mailbox.h"
#include "frame/rate_limiter.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace babble {

// Steam Frame eye cameras via Valve's dsp_service. Frames are copied out of
// the shared ring (so ring slots are held for microseconds) and cropped to the
// configured 400x400 regions. Any validation or runtime fault makes the eye
// source Unavailable; the rest of the runtime keeps going. Config type
// "frame_dsp_eye".
//
// dsp_service has no frame notification, so the ring index is polled. To keep
// wakeups low the poll thread learns the frame interval and sleeps until just
// before the next expected frame, then polls finely until it arrives. Frames
// that were already there when a coarse sleep ended count as late_detect and
// widen the margin.
class DspEyeSource : public ITrackingSource {
public:
    enum class State { Disabled, Connecting, Streaming, Backoff, Unavailable, Paused };

    DspEyeSource(const nlohmann::json &cfg, SourceDeps deps);
    ~DspEyeSource() override;

    void tick(uint64_t now_ns) override;
    void shutdown() override;
    void set_process_fps(double fps) override;
    void set_tracking(bool on) override;
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
    bool tracking_ = true;   // main thread only

    DspClient client_;
    DspClient::Layout layout_;
    std::vector<uint32_t> candidates_;
    Crop crop_l_, crop_r_;
    std::shared_ptr<FramePool> pool_;
    RateLimiter limiter_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> failed_{false};
    std::string thread_error_;

    State state_ = State::Disabled;
    uint64_t retry_at_ns_ = 0, backoff_ns_ = 0, backoff_min_ns_, backoff_max_ns_;
    std::string last_error_;
    std::string reported_layout_error_;
    bool not_ready_ = false;   // last start found the control block not set up yet
    bool logged_gap_ = false;

    RateStat frames_, processed_;
    Counter layout_faults_, restarts_, pool_exhausted_, late_detect_;   // stats: lifetime totals
    // Bad index reads in a row (capture thread while streaming, read by tick
    // after the thread stopped): kMaxLayoutFaults of them disable the source.
    uint32_t consecutive_faults_ = 0;
    LatencyStat copy_time_;
};

} // namespace babble
