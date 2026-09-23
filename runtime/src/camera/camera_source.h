#pragma once

#include "frame/frame.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace babble {

enum class SourceState { Closed, Opened, Streaming, Lost, Unavailable };

inline const char *to_string(SourceState s) {
    switch (s) {
    case SourceState::Closed: return "Closed";
    case SourceState::Opened: return "Opened";
    case SourceState::Streaming: return "Streaming";
    case SourceState::Lost: return "Lost";
    case SourceState::Unavailable: return "Unavailable";
    }
    return "?";
}

// Called from the source's capture thread. on_lost fires at most once per
// start(); the owner then calls stop() from its own thread.
struct FrameSink {
    std::function<void(FramePtr)> on_frame;
    std::function<void(std::string_view reason)> on_lost;
};

// A single capture stream. Capture only: hardware controls, where a camera
// has them, are the separate ICameraControls interface.
class ICameraSource {
public:
    virtual ~ICameraSource() = default;
    virtual bool open() = 0;              // acquire the device, no streaming yet
    virtual bool start(FrameSink sink) = 0;
    virtual void stop() = 0;              // stops streaming and releases the device
    virtual SourceState state() const = 0;
    virtual const std::string &last_error() const = 0;
};

struct ControlRange {
    int64_t min = 0, max = 0, step = 1, def = 0;
};

// Internal hardware controls (exposure, gain, LED strobe...). Driven by the
// runtime's own controllers, not exposed to users in normal operation.
// Names follow v4l2-ctl's convention: lowercase, non-alphanumerics as '_'.
class ICameraControls {
public:
    virtual ~ICameraControls() = default;
    virtual bool has(std::string_view name) const = 0;
    virtual std::optional<int64_t> get(std::string_view name) const = 0;
    virtual bool set(std::string_view name, int64_t value) = 0;
    // Applies each pair in order; returns false if any failed (the rest are still applied).
    virtual bool set_many(const std::vector<std::pair<std::string, int64_t>> &values) = 0;
    virtual std::optional<ControlRange> range(std::string_view name) const = 0;
};

} // namespace babble
