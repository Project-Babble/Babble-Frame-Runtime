#pragma once

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
// gaze...). Pipelines write, sinks read snapshots at their own rate.
// Filtering and calibration land here in Milestone 2.
class ExpressionState {
public:
    struct Snapshot {
        std::vector<std::string> names;
        std::vector<Signal> signals;
    };

    // Registers a signal name (idempotent) and returns its id. Call before streaming starts.
    SignalId add_signal(const std::string &name);

    void update(std::span<const std::pair<SignalId, Signal>> values);
    Snapshot snapshot() const;

private:
    mutable std::mutex mutex_;
    std::vector<std::string> names_;
    std::vector<Signal> signals_;
};

} // namespace babble
