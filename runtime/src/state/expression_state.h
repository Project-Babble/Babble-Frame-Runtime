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

// Single source of truth for tracked values (blendshapes, eye openness,
// gaze...). Pipelines write, output modules read snapshots at their own rate
// or wait for the next update. Filtering and calibration land here in
// Milestone 2.
class ExpressionState {
public:
    struct Snapshot {
        uint64_t seq = 0;   // increments on every update
        std::vector<std::string> names;
        std::vector<Signal> signals;
    };

    // Registers a signal name (idempotent) and returns its id. Call before streaming starts.
    SignalId add_signal(const std::string &name);

    void update(std::span<const std::pair<SignalId, Signal>> values);
    Snapshot snapshot() const;
    // Current values of a few signals (out[i] for ids[i]; unknown ids read as
    // default); returns the sequence number they belong to.
    uint64_t read(std::span<const SignalId> ids, Signal *out) const;
    size_t count() const;                 // registered signals; ids are 0 .. count-1
    std::string name(SignalId id) const;  // "" for an unknown id

    // Blocks until an update newer than `after_seq`, or the timeout; returns
    // the current sequence number either way.
    uint64_t wait_update(uint64_t after_seq, std::chrono::nanoseconds timeout) const;

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    uint64_t seq_ = 0;
    std::vector<std::string> names_;
    std::vector<Signal> signals_;
};

} // namespace babble
