#pragma once

#include "frame/frame.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace babble {

// Fixed set of frame buffers, never allocating on the hot path. Two flavours:
//  - owned: the pool allocates `count` buffers of `bytes` (DSP copy-out).
//  - external: wraps memory the pool does not own (mmapped V4L2 planes); the
//    owner is told through on_release(index) when a buffer becomes free again
//    and through on_destroy when the last reference (pool or frame) is gone,
//    which is when it is safe to munmap.
//
// acquire() hands out a mutable Frame for the producer to fill; converting it
// to FramePtr (shared_ptr<const Frame>) is implicit. The deleter keeps the
// pool alive, so frames may outlive the source that produced them.
class FramePool : public std::enable_shared_from_this<FramePool> {
public:
    using ReleaseFn = std::function<void(uint32_t index)>;
    using DestroyFn = std::function<void()>;

    static std::shared_ptr<FramePool> create_owned(size_t count, size_t bytes);
    static std::shared_ptr<FramePool> create_external(std::vector<uint8_t *> buffers, size_t bytes,
                                                      ReleaseFn on_release, DestroyFn on_destroy);
    ~FramePool();

    // Any free buffer (owned pools). Null when exhausted (counted).
    std::shared_ptr<Frame> acquire();
    // A specific buffer that the producer knows is free (external pools, V4L2 DQBUF index).
    std::shared_ptr<Frame> acquire(uint32_t index);

    // Stops on_release callbacks (call after STREAMOFF so late releases are no-ops).
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
    std::vector<uint8_t> storage_;   // owned pools only
    size_t bytes_ = 0;
    std::mutex mutex_;
    ReleaseFn on_release_;
    DestroyFn on_destroy_;
    std::atomic<size_t> outstanding_{0};
    std::atomic<uint64_t> exhausted_{0};
};

} // namespace babble
