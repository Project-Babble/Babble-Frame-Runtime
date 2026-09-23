#pragma once

#include "camera/camera_source.h"
#include "core/metrics.h"
#include "core/threading.h"
#include "frame/frame_pool.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace babble {

// One V4L2 multiplanar capture node, mmap streaming, zero-copy: dequeued
// buffers are handed out as FramePtrs and re-queued when the last reference
// drops. Works for the Frame's CAMSS nodes today and for UVC cameras later
// (UVC nodes are single-planar; supporting them means a second buffer type,
// not a new class).
class V4l2Capture : public ICameraSource {
public:
    struct Params {
        std::string node;
        uint32_t width = 0, height = 0;
        uint32_t fourcc = 0;
        uint32_t buffers = 6;
        FrameOrigin origin = FrameOrigin::MouthL;
        ThreadSpec thread;
        uint64_t silence_timeout_ns = 1000000000ull;
    };

    explicit V4l2Capture(Params params);
    ~V4l2Capture() override;

    bool open() override;
    bool start(FrameSink sink) override;
    void stop() override;
    SourceState state() const override { return state_.load(); }
    const std::string &last_error() const override { return error_; }

    const Params &params() const { return params_; }
    uint32_t generation() const { return generation_; }

    Counter driver_drops;    // gaps in the driver's sequence numbers
    Counter error_buffers;   // buffers flagged V4L2_BUF_FLAG_ERROR
    RateStat frames;

private:
    void run();
    void requeue_released();
    bool queue(uint32_t index);
    void fail(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
    void lost(const std::string &reason);
    void release_device();

    Params params_;
    int fd_ = -1;
    uint32_t stride_ = 0;
    size_t frame_bytes_ = 0;
    uint32_t generation_ = 0;
    std::shared_ptr<FramePool> pool_;
    // Shared with the pool's release callback, which may run after stop().
    std::shared_ptr<std::atomic<uint64_t>> released_;
    uint32_t queued_ = 0;
    bool streaming_ = false;
    enum class TsClock { Monotonic, MonotonicRaw, Arrival };
    TsClock ts_clock_ = TsClock::Monotonic;
    uint32_t last_seq_ = 0;
    bool have_seq_ = false;

    FrameSink sink_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<SourceState> state_{SourceState::Closed};
    std::string error_;
};

} // namespace babble
