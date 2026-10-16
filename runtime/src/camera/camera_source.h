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

// The capture thread calls on_lost one time at most for each start(), and the owner then calls stop().
struct FrameSink {
    std::function<void(FramePtr)> on_frame;
    std::function<void(std::string_view reason)> on_lost;
};

// A single capture stream, without the hardware controls that ICameraControls gives.
class ICameraSource {
public:
    virtual ~ICameraSource() = default;
    virtual bool open() = 0;              // Gets the device but does not start the stream.
    virtual bool start(FrameSink sink) = 0;
    virtual void stop() = 0;              // Stops the stream and releases the device.
    virtual SourceState state() const = 0;
    virtual const std::string &last_error() const = 0;
};

struct ControlRange {
    int64_t min = 0, max = 0, step = 1, def = 0;
};

// Internal hardware controls that use the v4l2-ctl names, and only the controllers of the runtime use them.
class ICameraControls {
public:
    virtual ~ICameraControls() = default;
    virtual bool has(std::string_view name) const = 0;
    virtual std::optional<int64_t> get(std::string_view name) const = 0;
    virtual bool set(std::string_view name, int64_t value) = 0;
    // Applies all pairs in order and returns false if one or more failed.
    virtual bool set_many(const std::vector<std::pair<std::string, int64_t>> &values) = 0;
    virtual std::optional<ControlRange> range(std::string_view name) const = 0;
};

}
