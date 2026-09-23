#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace babble {

// Observe-only view of the Babble camera arbiter installed under
// /etc/babble-camera (one-time root install). The runtime never routes
// streams, loads modules or restarts SteamVR; it only reads state and waits.
enum class ArbiterState {
    DriverMissing,             // installer never ran
    VersionTooOld,             // installed arbiter older than this runtime supports
    ModulesMissingForKernel,   // OS update, modules not rebuilt yet
    BoardAbsent,               // modules present but the arbiter did not claim the port
    Unrouted,                  // driver owns the port but streams are not routed (or FPGA not in VCINT)
    Routed,                    // cameras ready to open
};

const char *to_string(ArbiterState s);

class CameraArbiterClient {
public:
    explicit CameraArbiterClient(const nlohmann::json &cfg);

    ArbiterState poll();
    ArbiterState state() const { return state_; }
    // One sentence suitable for the UI.
    std::string describe() const;
    std::optional<int> installed_version() const { return version_; }

private:
    bool unit_active() const;
    std::optional<uint32_t> fpga_mode() const;

    std::string root_, module_, unit_, read_reg_, media_dev_;
    std::vector<std::string> sensor_prefixes_;
    int min_version_ = 1;
    std::string last_route_error_, last_unrouted_;
    ArbiterState state_ = ArbiterState::DriverMissing;
    std::optional<int> version_;
    bool warned_no_version_ = false;
    std::string kernel_;
};

} // namespace babble
