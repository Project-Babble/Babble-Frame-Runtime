#include "frame/frame_pool.h"

namespace babble {

std::shared_ptr<FramePool> FramePool::create_owned(size_t count, size_t bytes) {
    std::shared_ptr<FramePool> pool(new FramePool());
    pool->bytes_ = bytes;
    pool->storage_.resize(count * bytes);
    pool->slots_.resize(count);
    for (size_t i = 0; i < count; i++) {
        pool->slots_[i].frame.data = pool->storage_.data() + i * bytes;
        pool->slots_[i].frame.size = bytes;
        pool->slots_[i].frame.pool_index = uint32_t(i);
    }
    return pool;
}

std::shared_ptr<FramePool> FramePool::create_external(std::vector<uint8_t *> buffers, size_t bytes,
                                                      ReleaseFn on_release, DestroyFn on_destroy) {
    std::shared_ptr<FramePool> pool(new FramePool());
    pool->bytes_ = bytes;
    pool->slots_.resize(buffers.size());
    for (size_t i = 0; i < buffers.size(); i++) {
        pool->slots_[i].frame.data = buffers[i];
        pool->slots_[i].frame.size = bytes;
        pool->slots_[i].frame.pool_index = uint32_t(i);
    }
    pool->on_release_ = std::move(on_release);
    pool->on_destroy_ = std::move(on_destroy);
    return pool;
}

FramePool::~FramePool() {
    if (on_destroy_)
        on_destroy_();
}

std::shared_ptr<Frame> FramePool::wrap(uint32_t index) {
    auto self = shared_from_this();
    Frame *f = &slots_[index].frame;
    // Reset per-acquisition fields; data/size/pool_index are fixed per slot.
    f->capture_ns = f->arrival_ns = 0;
    f->seq = 0;
    return std::shared_ptr<Frame>(f, [self, index](Frame *) { self->release(index); });
}

std::shared_ptr<Frame> FramePool::acquire() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < slots_.size(); i++) {
        if (!slots_[i].busy) {
            slots_[i].busy = true;
            outstanding_.fetch_add(1, std::memory_order_relaxed);
            return wrap(uint32_t(i));
        }
    }
    exhausted_.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

std::shared_ptr<Frame> FramePool::acquire(uint32_t index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= slots_.size() || slots_[index].busy) {
        exhausted_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    slots_[index].busy = true;
    outstanding_.fetch_add(1, std::memory_order_relaxed);
    return wrap(index);
}

void FramePool::detach() {
    std::lock_guard<std::mutex> lock(mutex_);
    on_release_ = nullptr;
}

void FramePool::release(uint32_t index) {
    ReleaseFn cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        slots_[index].busy = false;
        outstanding_.fetch_sub(1, std::memory_order_relaxed);
        cb = on_release_;
    }
    if (cb)
        cb(index);
}

} // namespace babble
