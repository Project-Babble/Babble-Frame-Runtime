#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace babble {

class SteamVrClient;

// Mirrors runtime settings into the driver's section of SteamVR Settings
// (driver/package/resources/settings/settingsschema.vrsettings, the
// "Expression Tracking" section) and picks up changes made there.
//
// The runtime's config (settings.json) stays the source of truth: on every
// SteamVR connection the runtime's values are written to SteamVR, and from
// then on a value that differs from the last one seen or written is a user
// change. Drivers get no settings-changed event for their own section, so the
// values are polled (a few IPC calls, twice a second).
//
// Config "steamvr_settings": {"section": "driver_babble",
//   "values": [{"key": "mouthRate", "id": "/sources/mouth/process_fps"}, ...]}
// The type (toggle = bool, select = int) follows the config value's type.
class SteamVrSettingsBridge {
public:
    struct Entry {
        std::string key;   // name in the SteamVR section
        std::string id;    // JSON pointer into the runtime config
        bool is_bool = false;
    };

    // Entries whose id exists in cfg as a bool or number; others are skipped (logged).
    static std::vector<Entry> entries_from_config(const nlohmann::json &cfg);

    SteamVrSettingsBridge(SteamVrClient &vr, std::string section, std::vector<Entry> entries);

    // Main thread. Returns (id, value) for settings changed in SteamVR since
    // the last call. On a new connection it first writes cfg's values.
    std::vector<std::pair<std::string, nlohmann::json>> poll(const nlohmann::json &cfg, uint64_t now_ns);
    // The runtime changed a setting (dashboard, reload): mirror it.
    void push(const std::string &id, const nlohmann::json &value);

    bool empty() const { return entries_.empty(); }

private:
    std::optional<nlohmann::json> read(const Entry &e);
    bool write(const Entry &e, const nlohmann::json &value);

    SteamVrClient &vr_;
    std::string section_;
    std::vector<Entry> entries_;
    uint64_t connection_ = 0;
    uint64_t next_poll_ns_ = 0;
    std::map<std::string, nlohmann::json> known_;   // id -> last value seen in or written to SteamVR
};

} // namespace babble
