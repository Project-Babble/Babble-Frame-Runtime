#pragma once

#include "core/metrics.h"
#include "frame/frame.h"
#include "frame/mailbox.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace babble {

class CameraArbiterClient;
class SteamVrClient;

// What a source is doing, for user-facing status (the dashboard).
enum class SourceActivity {
    Off,         // disabled in config, or tracking turned off by the user
    Starting,    // waiting for the platform (camera route, DSP service) or opening
    Paused,      // headset not worn; cameras stopped on purpose
    Streaming,
    Fault,       // backing off after an error, or unavailable; see last_error()
};

// A tracking camera module: owns its capture threads and hardware, publishes
// the latest (stereo) frame into its output slot. Each concrete module (the
// Frame's OV6211 mouth pair, the Frame's DSP eye cameras, a future UVC
// camera...) is registered by a config "type" in make_tracking_source().
//
// Only the control plane goes through this interface (construction, the main
// thread's 10 Hz tick, settings). Frames go straight from the module's capture
// threads into the output slot, so the interface adds nothing per frame.
class ITrackingSource {
public:
    virtual ~ITrackingSource() = default;

    // Main thread: drive the module's state machine (open, recover, back off).
    virtual void tick(uint64_t now_ns) = 0;
    virtual void shutdown() = 0;

    // User-facing subsample of the tracking rate; takes effect immediately.
    virtual void set_process_fps(double fps) = 0;
    // User-facing on/off (config "tracking"): off stops the cameras on the
    // next tick, like the headset-off pause; the module stays loaded.
    virtual void set_tracking(bool on) = 0;
    virtual bool tracking() const = 0;
    // A module-specific option changed live (config reload, the debug overlay).
    // False = not an option this module has, or a value it rejects.
    virtual bool set_option(const std::string &key, const nlohmann::json &value) { return false; }

    virtual bool streaming() const = 0;
    virtual SourceActivity activity() const = 0;
    virtual const char *state_name() const = 0;
    virtual const std::string &last_error() const = 0;
};

// Everything a module may need from the runtime. Modules keep references, so
// all of these must outlive the module.
struct SourceDeps {
    std::string name;                        // config key, also the stats prefix
    const nlohmann::json &threads;           // config["threads"]
    LatestSlot<StereoFrame> &out;
    StatsRegistry &stats;
    const CameraArbiterClient &arbiter;
    // Headset-worn state; with pause_when_absent (default) a module stops its
    // cameras while SteamVR reports the headset not worn. Null = never pause.
    const SteamVrClient *steamvr = nullptr;
};

// Builds the module named by cfg["type"]; null (logged) for unknown types.
std::unique_ptr<ITrackingSource> make_tracking_source(const nlohmann::json &cfg, SourceDeps deps);

} // namespace babble
