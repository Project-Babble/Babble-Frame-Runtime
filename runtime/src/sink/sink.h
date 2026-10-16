#pragma once

#include "frame/frame.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>

namespace babble {

class ExpressionState;
class StatsRegistry;

// An output module that reads ExpressionState on its own thread, so a slow output cannot stop the tracking.
class ISink {
public:
    virtual ~ISink() = default;

    virtual bool start() = 0;   // If this returns false, the runtime drops the module.
    virtual void stop() = 0;    // This must return quickly because the full shutdown has about 2 s.

    // offer_frames() runs on pipeline threads, so it must only hand off the frame and never block or do I/O.
    virtual bool wants_frames() const { return false; }
    virtual void offer_frames(const std::string &source, const StereoFrame &frames) {}

    // The main thread calls this for a live setting under /sinks/<name>/, and it returns false for an unknown key.
    virtual bool set_option(const std::string &key, const nlohmann::json &value) { return false; }
};

// The runtime objects for a module, which must all live longer than the module.
struct SinkDeps {
    std::string name;                  // The config key, which is also the stats prefix.
    const nlohmann::json &threads;
    const ExpressionState &state;
    StatsRegistry &stats;
    std::string version = "", profile = "";   // The runtime version and profile for the session manifests.
};

// Makes the module that cfg["type"] names, or returns null for an unknown type.
std::unique_ptr<ISink> make_sink(const nlohmann::json &cfg, SinkDeps deps);

}
