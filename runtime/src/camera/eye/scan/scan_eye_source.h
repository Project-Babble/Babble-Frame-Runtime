#pragma once

// Config type `frame_dsp_eye`: the eye camera reader, ported from eyecam's reader
// (https://github.com/sasaken1102r/frameeyeosc, tools/eyecam). It is read-only: it does not write to the dsp_service
// buffer, does not use the control block to find frames, and takes the capture time and the camera rate from the
// camera timestamp in each slot header. See docs/eye-tracking.md, "Eye camera source".

#include "camera/eye/dsp_client.h"
#include "camera/eye/scan/dma_arena.h"
#include "camera/eye/scan/eye_ring.h"
#include "camera/eye/scan/slot_watcher.h"
#include "camera/tracking_source.h"
#include "core/metrics.h"
#include "core/threading.h"
#include "frame/frame_pool.h"
#include "frame/mailbox.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace babble {

class ScanEyeSource : public ITrackingSource {
public:
    enum class State { Disabled, Connecting, Streaming, Backoff, Unavailable, Paused };

    ScanEyeSource(const nlohmann::json &cfg, SourceDeps deps);
    ~ScanEyeSource() override;

    void tick(uint64_t now_ns) override;
    void shutdown() override;
    void set_process_fps(double fps) override;
    void set_tracking(bool on) override;
    bool set_option(const std::string &key, const nlohmann::json &value) override;
    void set_frame_tap(IFrameTap *tap) override { tap_.store(tap); }

    double camera_fps() const override { return camera_fps_.load(std::memory_order_relaxed); }
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
    bool search(eyescan::Ring *ring);
    void stream(const eyescan::Ring &ring);
    void update_clock_offset();
    void read_rate_word();
    bool make_frame(int side, const uint8_t *slot, uint64_t capture_ns, uint64_t arrival_ns, uint32_t seq,
                    FramePtr *out);

    nlohmann::json cfg_;
    std::string name_;
    ThreadSpec thread_spec_;
    LatestSlot<StereoFrame> &out_;
    StatsRegistry &stats_;
    const SteamVrClient *steamvr_ = nullptr;
    bool pause_when_absent_ = true;
    bool tracking_ = true;   // Only the main thread uses this.

    Crop crop_[2];                  // [0] = lower-address group ("left"), [1] = higher-address group ("right").
    size_t copy_len_ = 0;           // Bytes copied from the start of each slot header.
    std::vector<uint32_t> candidates_;

    DspClient client_;
    eyescan::DmaArena arena_;
    std::optional<DspClient::Layout> layout_;   // Only to read the rate word, for comparison.
    std::shared_ptr<FramePool> pool_;
    std::atomic<uint32_t> divisor_{1};
    std::atomic<IFrameTap *> tap_{nullptr};
    std::atomic<float> camera_fps_{0};
    std::atomic<bool> searching_{false};
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> failed_{false};
    std::string thread_error_;
    int64_t raw_to_mono_ns_ = 0;   // CLOCK_MONOTONIC minus CLOCK_MONOTONIC_RAW.

    State state_ = State::Disabled;
    uint64_t retry_at_ns_ = 0, backoff_ns_ = 0, backoff_min_ns_, backoff_max_ns_;
    std::string last_error_;

    RateStat frames_, processed_, side_l_, side_r_;
    Counter restarts_, pool_exhausted_, half_pairs_, torn_copies_, missed_frames_, stale_frames_, bad_stamps_;
    Counter searches_, camera_rate_, rate_word_;
    LatencyStat copy_time_, stamp_age_, pair_skew_;
};

}
