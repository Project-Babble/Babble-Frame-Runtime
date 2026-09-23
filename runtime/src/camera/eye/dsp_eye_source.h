#pragma once

#include "camera/eye/dsp_client.h"
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
// source Unavailable; the rest of the runtime keeps going.
class DspEyeSource {
public:
    enum class State { Disabled, Connecting, Streaming, Backoff, Unavailable };

    DspEyeSource(const nlohmann::json &eye_cfg, const nlohmann::json &threads_cfg, LatestSlot<StereoFrame> &out,
                 StatsRegistry &stats);
    ~DspEyeSource();

    void tick(uint64_t now_ns);
    void shutdown();

    // User-facing subsample of the eye tracking rate; takes effect immediately.
    void set_process_fps(double fps);

    State state() const { return state_; }
    const char *state_name() const;
    const std::string &last_error() const { return last_error_; }

private:
    struct Crop {
        uint32_t y = 0, x = 0, h = 0, w = 0;
    };

    bool start();
    void stop(const char *why);
    void run();
    bool copy_view(bool right, uint32_t idx, uint64_t now, uint32_t seq, FramePtr *out);

    nlohmann::json cfg_;
    ThreadSpec thread_spec_;
    LatestSlot<StereoFrame> &out_;
    StatsRegistry &stats_;

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
    bool logged_gap_ = false;

    RateStat frames_, processed_;
    Counter layout_faults_, restarts_, pool_exhausted_;
    LatencyStat copy_time_;
};

} // namespace babble
