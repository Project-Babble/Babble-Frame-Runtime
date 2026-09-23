#pragma once

#include "core/threading.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>

namespace babble {

// Headset-worn state as SteamVR reports it (the proximity sensor, folded
// into the HMD's activity level; the same signal Valve's proxmicmute uses).
enum class Worn { Unknown, Yes, No };

inline const char *to_string(Worn w) {
    switch (w) {
    case Worn::Unknown: return "unknown";
    case Worn::Yes: return "worn";
    case Worn::No: return "not worn";
    }
    return "?";
}

// The runtime's single connection to SteamVR, as a background OpenVR client
// (never starts SteamVR, never counts as a running app). Owns connecting,
// reconnecting, the event loop, SteamVR quitting and IPD changes; other
// modules make their OpenVR calls through with_vr() so the connection can't
// go away underneath them.
//
// Worn: the headset's raw proximity sensor, read as a boolean input action
// bound to /user/head/proximity (driver_cv registers the /proximity
// component) - instant in both directions. If that action isn't active, the
// HMD activity level is the fallback: UserInteraction = worn, Idle/Standby =
// not worn (SteamVR only reaches Idle ~6 s after removal on the Frame).
// Anything else (not connected, unknown) = Unknown: callers must not pause.
//
// The client owns the process's single SteamVR input manifest: the proximity
// action plus the eye-tracking action the gaze module reads (gaze_action()).
class SteamVrClient {
public:
    ~SteamVrClient();

    struct Options {
        std::string manifest_dir;                              // where the input manifest is written
        std::string controller_type = "frame_hmd";
        std::string proximity_path = "/user/head/proximity";
        std::string eyetracking_path = "/user/head/eyetracking";
    };
    void start(ThreadSpec thread, Options options);
    void stop();

    Worn worn() const { return worn_.load(); }
    bool connected() const { return connected_.load(); }
    // Increments on every (re)connection; users redo per-connection setup when it changes.
    uint64_t connection() const { return connection_.load(); }
    // Headset IPD in metres from SteamVR, 0 until known.
    double ipd() const { return ipd_.load(); }

    // Input handles (valid while connected; 0 if setup failed).
    uint64_t gaze_action() const { return gaze_action_.load(); }
    uint64_t action_set() const { return action_set_.load(); }

    // Runs fn while connected, serialized with all other OpenVR calls; false if not connected.
    bool with_vr(const std::function<void()> &fn) const;

private:
    void run();
    bool connect();
    void disconnect();
    void refresh_activity();
    bool setup_input();
    std::optional<bool> read_proximity();
    void refresh_ipd();

    ThreadSpec thread_spec_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    mutable std::shared_mutex vr_mutex_;
    std::atomic<bool> connected_{false};
    std::atomic<uint64_t> connection_{0};
    std::atomic<Worn> worn_{Worn::Unknown};
    std::atomic<double> ipd_{0.0};
    bool has_prox_ = false;
    Options options_;
    std::atomic<uint64_t> action_set_{0}, gaze_action_{0};
    uint64_t prox_action_ = 0;
    bool prox_reported_ = false;
};

} // namespace babble
