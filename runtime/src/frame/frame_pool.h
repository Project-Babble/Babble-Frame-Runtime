#pragma once

#include "frame/frame.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace babble {

// A fixed set of frame buffers that owns its memory or wraps external memory, and never allocates on the hot path.
class FramePool : public std::enable_shared_from_this<FramePool> {
public:
    using ReleaseFn = std::function<void(uint32_t index)>;
    using DestroyFn = std::function<void()>;

    static std::shared_ptr<FramePool> create_owned(size_t count, size_t bytes);
    static std::shared_ptr<FramePool> create_external(std::vector<uint8_t *> buffers, size_t bytes,
                                                      ReleaseFn on_release, DestroyFn on_destroy);
    ~FramePool();

    // Gets any free buffer, or returns null and increments the exhausted count when no buffer is free.
    std::shared_ptr<Frame> acquire();
    // Gets the buffer at a known free index, such as the V4L2 DQBUF index of an external pool.
    std::shared_ptr<Frame> acquire(uint32_t index);

    // Stops on_release callbacks, so call it after STREAMOFF to make late releases do nothing.
    void detach();

    size_t capacity() const { return slots_.size(); }
    size_t outstanding() const { return outstanding_.load(std::memory_order_relaxed); }
    uint64_t exhausted() const { return exhausted_.load(std::memory_order_relaxed); }
    size_t buffer_bytes() const { return bytes_; }

private:
    struct Slot {
        Frame frame;
        bool busy = false;
    };
    FramePool() = default;
    std::shared_ptr<Frame> wrap(uint32_t index);
    void release(uint32_t index);

    std::vector<Slot> slots_;
    std::vector<uint8_t> storage_;   // Only owned pools use this storage.
    size_t bytes_ = 0;
    std::mutex mutex_;
    ReleaseFn on_release_;
    DestroyFn on_destroy_;
    std::atomic<size_t> outstanding_{0};
    std::atomic<uint64_t> exhausted_{0};
};

}
