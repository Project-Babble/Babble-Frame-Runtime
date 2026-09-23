#pragma once

namespace babble {

class ExpressionState;
class StatsRegistry;

// Output of tracked values (OSC, OpenXR layer bridge, UI stream...). Sinks
// read ExpressionState snapshots on their own thread and rate.
class ISink {
public:
    virtual ~ISink() = default;
    virtual void start(const ExpressionState &state, StatsRegistry &stats) = 0;
    virtual void stop() = 0;
};

} // namespace babble
