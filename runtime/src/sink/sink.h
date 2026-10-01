#pragma once

#include "frame/frame.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>

namespace babble {

class ExpressionState;
class StatsRegistry;

// An output module: sends tracked values somewhere (OSC, an OpenXR layer
// bridge, the UI, SteamVR gaze...). Each module runs on its own thread and
// reads ExpressionState, either at its own rate or by waiting for updates
// (ExpressionState::wait_update), so a slow or blocked output can never stall
// tracking. Modules are registered by config "type" in make_sink().
class ISink {
public:
    virtual ~ISink() = default;

    virtual bool start() = 0;   // false: the module could not start (logged) and is dropped
    virtual void stop() = 0;    // must return promptly (shutdown budget is ~2 s total)

    // Optional access to camera frames, for preview/recording/calibration
    // modules. offer_frames() runs on pipeline threads: it must only hand the
    // frame off (e.g. into a LatestSlot), never block or do I/O.
    virtual bool wants_frames() const { return false; }
    virtual void offer_frames(const std::string &source, const StereoFrame &frames) {}

    // A live setting under /sinks/<name>/ (key is the rest of the path); false
    // if the module doesn't take it. Called on the main thread.
    virtual bool set_option(const std::string &key, const nlohmann::json &value) { return false; }
};

// Everything a module may need from the runtime; all must outlive the module.
struct SinkDeps {
    std::string name;                  // config key, also the stats prefix
    const nlohmann::json &threads;     // config["threads"]
    const ExpressionState &state;
    StatsRegistry &stats;
};

// Builds the module named by cfg["type"]; null (logged) for unknown types.
std::unique_ptr<ISink> make_sink(const nlohmann::json &cfg, SinkDeps deps);

} // namespace babble
