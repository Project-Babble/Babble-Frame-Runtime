#pragma once

#include <atomic>
#include <cstdint>

namespace babble {

// Lets accept() return true at most `fps` times per second, and only one thread can call accept().
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
        // Move the due time on an ideal timeline, so that a rate such as 45 of 120 fps is correct on average.
        if (next_due_ns_ == 0) {
            next_due_ns_ = now_ns + p;
            return true;
        }
        if (now_ns + p / 10 < next_due_ns_)
            return false;
        next_due_ns_ += p;
        if (next_due_ns_ + p < now_ns)
            next_due_ns_ = now_ns + p;   // After a stall, start the timeline again to prevent a burst of frames.
        return true;
    }

    void reset() { next_due_ns_ = 0; }

private:
    std::atomic<uint64_t> period_ns_{0};
    uint64_t next_due_ns_ = 0;
};

}
