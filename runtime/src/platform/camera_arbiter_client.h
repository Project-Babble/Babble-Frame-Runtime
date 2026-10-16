#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace babble {

// The state of the Babble camera arbiter in /etc/babble-camera, which the runtime only reads and never changes.
enum class ArbiterState {
    DriverMissing,             // The installer did not run.
    VersionTooOld,             // The installed arbiter is older than this runtime supports.
    ModulesMissingForKernel,   // The OS was updated and the modules are not rebuilt yet.
    BoardAbsent,               // The modules are present but the arbiter did not claim the port.
    Unrouted,                  // The driver owns the port but the streams or the FPGA mode are not ready.
    Routed,                    // The cameras are ready to open.
};

const char *to_string(ArbiterState s);

class CameraArbiterClient {
public:
    explicit CameraArbiterClient(const nlohmann::json &cfg);

    ArbiterState poll();
    ArbiterState state() const { return state_; }
    // Returns one sentence for the UI.
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

}
