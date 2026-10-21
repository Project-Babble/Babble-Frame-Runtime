#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace babble {

using SignalId = uint32_t;

struct Signal {
    float value = 0;
    float confidence = 0;
    uint64_t t_ns = 0;
};

// The single source of truth for tracked values, which pipelines write and output modules read.
class ExpressionState {
public:
    struct Snapshot {
        uint64_t seq = 0;   // Increments on each update.
        std::vector<std::string> names;
        std::vector<Signal> signals;
    };

    // Registers a signal name and returns its id, so call it before the stream starts.
    SignalId add_signal(const std::string &name);

    void update(std::span<const std::pair<SignalId, Signal>> values);
    Snapshot snapshot() const;
    void snapshot(Snapshot &out) const;
    // Writes the current value for ids[i] to out[i] and returns the sequence number of the values.
    uint64_t read(std::span<const SignalId> ids, Signal *out) const;
    size_t count() const;                 // The number of registered signals, with ids from 0 to count-1.
    std::string name(SignalId id) const;  // Returns "" for an unknown id.

    // Waits for an update newer than `after_seq` or for the timeout, and returns the current sequence number.
    uint64_t wait_update(uint64_t after_seq, std::chrono::nanoseconds timeout) const;

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    uint64_t seq_ = 0;
    std::vector<std::string> names_;
    std::vector<Signal> signals_;
};

}
