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

// Gets all frames on the capture thread of the source, so it must not block or do I/O.
class IFrameTap {
public:
    virtual ~IFrameTap() = default;
    virtual bool tap_wanted() const = 0;
    virtual void tap_frames(const std::string &source, const StereoFrame &frames) = 0;
};

// The activity of a source, which the dashboard shows to the user.
enum class SourceActivity {
    Off,         // The config or the user turned off the source.
    Starting,    // The source waits for the platform or opens the device.
    Paused,      // The headset is not worn, so the cameras are stopped.
    Streaming,
    Fault,       // The source is unavailable or waits after an error, and last_error() tells why.
};

// A tracking camera module that owns its capture threads and hardware and puts the latest frame into its output slot.
class ITrackingSource {
public:
    virtual ~ITrackingSource() = default;

    // Runs the state machine of the module on the main thread.
    virtual void tick(uint64_t now_ns) = 0;
    virtual void shutdown() = 0;

    // Sets the tracking rate that the user selected, and the change is immediate.
    virtual void set_process_fps(double fps) = 0;
    // Turns tracking on or off, and off stops the cameras on the next tick.
    virtual void set_tracking(bool on) = 0;
    virtual bool tracking() const = 0;
    // Changes an option of the module and returns false if the module does not know the option or the value.
    virtual bool set_option(const std::string &key, const nlohmann::json &value) { return false; }
    // Call this on the main thread and remove the tap before it is destroyed.
    virtual void set_frame_tap(IFrameTap *tap) {}

    // Returns the camera frame rate that the platform set, or 0 if it is not known.
    virtual double camera_fps() const { return 0; }

    virtual bool streaming() const = 0;
    virtual SourceActivity activity() const = 0;
    virtual const char *state_name() const = 0;
    virtual const std::string &last_error() const = 0;
};

// The runtime objects for a module, which must stay alive longer than the module.
struct SourceDeps {
    std::string name;                        // The config key, which is also the stats prefix.
    const nlohmann::json &threads;           // This is config["threads"].
    LatestSlot<StereoFrame> &out;
    StatsRegistry &stats;
    // The camera routing of the platform, which only the Frame mouth cameras use.
    const CameraArbiterClient *arbiter = nullptr;
    // The headset-worn state, which a module uses to stop its cameras when the headset is not worn.
    const SteamVrClient *steamvr = nullptr;
};

// Builds the module for cfg["type"], or returns null for an unknown type.
std::unique_ptr<ITrackingSource> make_tracking_source(const nlohmann::json &cfg, SourceDeps deps);

}
