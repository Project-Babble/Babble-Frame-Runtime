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

// The worn state of the headset, where Unknown means that callers must not pause.
enum class Worn { Unknown, Yes, No };

inline const char *to_string(Worn w) {
    switch (w) {
    case Worn::Unknown: return "unknown";
    case Worn::Yes: return "worn";
    case Worn::No: return "not worn";
    }
    return "?";
}

// Accepts "not worn" only after it lasts absent_after_ns, because the sensor shows "off" for about 0.5 s while worn.
class PresenceFilter {
public:
    explicit PresenceFilter(uint64_t absent_after_ns) : absent_after_ns_(absent_after_ns) {}
    Worn update(Worn raw, uint64_t now_ns);
    uint64_t blips() const { return blips_; }   // The number of "not worn" periods that were too short to count.

private:
    uint64_t absent_after_ns_;
    Worn out_ = Worn::Unknown;
    uint64_t absent_since_ = 0;   // The value is 0 when the sensor does not read "not worn".
    uint64_t blips_ = 0;
};

// The single background OpenVR connection of the runtime, and other modules must use with_vr() for OpenVR calls.
class SteamVrClient {
public:
    ~SteamVrClient();

    struct Options {
        std::string manifest_dir;                              // The directory for the input manifest.
        std::string controller_type = "frame_hmd";
        std::string proximity_path = "/user/head/proximity";
        std::string eyetracking_path = "/user/head/eyetracking";
        double absent_after_s = 2.0;   // The time that "not worn" must last before it counts.
    };
    void start(ThreadSpec thread, Options options);
    void stop();

    Worn worn() const { return worn_.load(); }
    bool connected() const { return connected_.load(); }
    // Increments on each connection, so do the setup for each connection again when it changes.
    uint64_t connection() const { return connection_.load(); }
    // Returns the headset IPD from SteamVR in metres, or 0 until it is known.
    double ipd() const { return ipd_.load(); }
    // Returns the display refresh rate in Hz, which SteamVR uses to set the eye camera rate, or 0 until it is known.
    double display_hz() const { return display_hz_.load(); }

    // These input handles are valid while connected, and are 0 if the setup failed.
    uint64_t gaze_action() const { return gaze_action_.load(); }
    uint64_t action_set() const { return action_set_.load(); }

    // Runs fn with a lock on all other OpenVR calls, or returns false if not connected.
    bool with_vr(const std::function<void()> &fn) const;

private:
    void run();
    bool connect();
    void disconnect();
    void refresh_activity();
    bool setup_input();
    std::optional<bool> read_proximity();
    void refresh_ipd();
    void refresh_display_hz();

    ThreadSpec thread_spec_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    mutable std::shared_mutex vr_mutex_;
    std::atomic<bool> connected_{false};
    std::atomic<uint64_t> connection_{0};
    std::atomic<Worn> worn_{Worn::Unknown};
    std::atomic<double> ipd_{0.0};
    std::atomic<double> display_hz_{0.0};
    bool has_prox_ = false;
    Options options_;
    std::atomic<uint64_t> action_set_{0}, gaze_action_{0};
    uint64_t prox_action_ = 0;
    bool prox_reported_ = false;
    PresenceFilter presence_{2'000'000'000ull};
};

}
