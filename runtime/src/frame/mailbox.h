#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>

namespace babble {

// Single-producer/single-consumer "latest wins" slot. put() never blocks and
// overwrites an unread value (counted as a drop); the consumer takes the
// newest value or waits for one. Tracking wants the freshest frame, never a
// backlog.
template <class T>
class LatestSlot {
public:
    void put(T value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (value_)
                dropped_++;
            value_ = std::move(value);
        }
        cv_.notify_one();
    }

    std::optional<T> take() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::optional<T> v = std::move(value_);
        value_.reset();
        return v;
    }

    // Waits up to `timeout` for a value; returns nullopt on timeout or close.
    std::optional<T> wait_take(std::chrono::nanoseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, timeout, [this] { return value_.has_value() || closed_; });
        std::optional<T> v = std::move(value_);
        value_.reset();
        return v;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    void reopen() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = false;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        value_.reset();
    }

    bool closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    uint64_t dropped() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<T> value_;
    bool closed_ = false;
    uint64_t dropped_ = 0;
};

} // namespace babble
