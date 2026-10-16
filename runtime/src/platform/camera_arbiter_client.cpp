#include "platform/camera_arbiter_client.h"

#include "core/log.h"
#include "platform/media_graph.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

#include <sys/utsname.h>

namespace {
const babble::log::Logger LOG("arbiter");
}

namespace babble {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kFpgaVcint = 0x00021211;

std::string run_capture(const std::string &cmd) {
    std::string out;
    FILE *p = popen(cmd.c_str(), "r");
    if (!p)
        return out;
    char buf[256];
    while (fgets(buf, sizeof(buf), p))
        out += buf;
    pclose(p);
    return out;
}

}

const char *to_string(ArbiterState s) {
    switch (s) {
    case ArbiterState::DriverMissing: return "DriverMissing";
    case ArbiterState::VersionTooOld: return "VersionTooOld";
    case ArbiterState::ModulesMissingForKernel: return "ModulesMissingForKernel";
    case ArbiterState::BoardAbsent: return "BoardAbsent";
    case ArbiterState::Unrouted: return "Unrouted";
    case ArbiterState::Routed: return "Routed";
    }
    return "?";
}

CameraArbiterClient::CameraArbiterClient(const nlohmann::json &cfg)
    : root_(cfg.value("root", "/etc/babble-camera")), module_(cfg.value("module", "steamframe_ov6211_stereo_subdev")),
      unit_(cfg.value("streams_unit", "babble-camera-streams.service")),
      read_reg_(cfg.value("fpga_read_reg", "/usr/lib/deckard-fpga/read_reg")),
      media_dev_(cfg.value("media_device", "/dev/media0")), min_version_(cfg.value("min_version", 1)) {
    for (const auto &p : cfg.value("sensor_entities", nlohmann::json::array()))
        sensor_prefixes_.push_back(p.get<std::string>());
    utsname u{};
    if (uname(&u) == 0)
        kernel_ = u.release;
}

bool CameraArbiterClient::unit_active() const {
    // Read the systemd symlink that exists only while the unit is active, so no exec is necessary.
    std::error_code ec;
    fs::path marker = fs::path("/run/systemd/units") / ("invocation:" + unit_);
    if (fs::is_directory("/run/systemd/units", ec))
        return fs::exists(fs::symlink_status(marker, ec));
    return run_capture("systemctl is-active " + unit_ + " 2>/dev/null").rfind("active", 0) == 0;
}

std::optional<uint32_t> CameraArbiterClient::fpga_mode() const {
    std::error_code ec;
    if (!fs::exists(read_reg_, ec))
        return std::nullopt;
    // The tool prints a line such as " FPGA reg 0x12 = 0x00021211" with the value as the last token.
    std::string out = run_capture(read_reg_ + " 0x12 2>/dev/null");
    auto pos = out.find_last_of("x");
    if (pos == std::string::npos || pos == 0)
        return std::nullopt;
    try {
        return uint32_t(std::stoul(out.substr(pos - 1), nullptr, 16));
    } catch (...) {
        return std::nullopt;
    }
}

ArbiterState CameraArbiterClient::poll() {
    ArbiterState prev = state_;
    std::error_code ec;
    if (!fs::is_directory(root_, ec)) {
        state_ = ArbiterState::DriverMissing;
    } else {
        version_.reset();
        std::ifstream vf(fs::path(root_) / "VERSION");
        int v = 0;
        if (vf >> v)
            version_ = v;
        if (!version_ && !warned_no_version_) {
            LOG.warn("arbiter: %s/VERSION missing; assuming a compatible pre-versioning install", root_.c_str());
            warned_no_version_ = true;
        }

        bool module_loaded = fs::is_directory(fs::path("/sys/module") / module_, ec);
        if (version_ && *version_ < min_version_)
            state_ = ArbiterState::VersionTooOld;
        else if (!module_loaded && !fs::is_directory(fs::path(root_) / "modules" / kernel_, ec))
            state_ = ArbiterState::ModulesMissingForKernel;
        else if (!module_loaded)
            state_ = ArbiterState::BoardAbsent;
        else if (!unit_active())
            state_ = ArbiterState::Unrouted;
        else {
            auto mode = fpga_mode();
            // If the FPGA mode is unknown, trust the streams unit because it does not route without VCINT.
            bool fpga_ok = !mode || *mode == kFpgaVcint;
            // Check the media graph because a SteamVR restart can disable the links while the unit stays active.
            RouteCheck rc = check_sensor_routes(media_dev_, sensor_prefixes_);
            if (!rc.error.empty() && rc.error != last_route_error_)
                LOG.warn("arbiter: cannot read media graph (%s); trusting the streams unit", rc.error.c_str());
            last_route_error_ = rc.error;
            bool links_ok = !rc.error.empty() || rc.ok;
            if (!links_ok) {
                std::string list;
                for (const auto &s : rc.unrouted)
                    list += (list.empty() ? "" : ", ") + s;
                if (list != last_unrouted_)
                    LOG.warn("arbiter: streams unit is active but not routed: %s", list.c_str());
                last_unrouted_ = list;
            } else {
                last_unrouted_.clear();
            }
            state_ = fpga_ok && links_ok ? ArbiterState::Routed : ArbiterState::Unrouted;
        }
    }
    if (state_ != prev)
        LOG.info("arbiter: %s -> %s (%s)", to_string(prev), to_string(state_), describe().c_str());
    return state_;
}

std::string CameraArbiterClient::describe() const {
    switch (state_) {
    case ArbiterState::DriverMissing:
        return "Camera driver is not installed. Run the Babble camera installer once.";
    case ArbiterState::VersionTooOld:
        return "Camera driver is out of date. Re-run the Babble camera installer.";
    case ArbiterState::ModulesMissingForKernel:
        return "SteamOS was updated; the camera driver is rebuilding and needs one reboot.";
    case ArbiterState::BoardAbsent:
        return "Mouth camera board not detected on the lower port.";
    case ArbiterState::Unrouted:
        return "Mouth cameras are not routed; rebooting the headset restores them.";
    case ArbiterState::Routed:
        return "Mouth cameras ready.";
    }
    return "";
}

}
