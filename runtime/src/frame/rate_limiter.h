#pragma once

#include <atomic>
#include <cstdint>

namespace babble {

// Time-based subsampler: accept() is true at most `fps` times per second.
// The camera keeps running at full rate; this only decides which pairs go on
// to processing, so any target rate works, not just integer divisors.
// accept() is called from one thread; set_fps() from any.
class RateLimiter {
public:
    explicit RateLimiter(double fps = 0) { set_fps(fps); }

    void set_fps(double fps) {
        period_ns_.store(fps > 0 ? uint64_t(1e9 / fps) : 0, std::memory_order_relaxed);
    }

    double fps() const {
        uint64_t p = period_ns_.load(std::memory_order_relaxed);
        return p ? 1e9 / double(p) : 0;
    }

    bool accept(uint64_t now_ns) {
        uint64_t p = period_ns_.load(std::memory_order_relaxed);
        if (p == 0)
            return true;
        // Due times advance on an ideal timeline rather than from the last
        // accepted frame, so rates that don't divide the camera rate (45 of
        // 120) average out exactly. 10% slack absorbs timestamp jitter.
        if (next_due_ns_ == 0) {
            next_due_ns_ = now_ns + p;
            return true;
        }
        if (now_ns + p / 10 < next_due_ns_)
            return false;
        next_due_ns_ += p;
        if (next_due_ns_ + p < now_ns)
            next_due_ns_ = now_ns + p;   // fell far behind (stall): resync instead of bursting
        return true;
    }

    void reset() { next_due_ns_ = 0; }

private:
    std::atomic<uint64_t> period_ns_{0};
    uint64_t next_due_ns_ = 0;
};

} // namespace babble
