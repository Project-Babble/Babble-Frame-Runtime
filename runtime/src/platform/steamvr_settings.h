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

// Copies runtime settings to the driver section of SteamVR Settings, and polls that section for user changes.
class SteamVrSettingsBridge {
public:
    struct Entry {
        std::string key;   // The name in the SteamVR section.
        std::string id;    // A JSON pointer into the runtime config.
        enum class Type { Bool, Int, String } type = Type::Int;   // The type follows the type of the config value.
    };

    // Returns the entries whose id exists in cfg with a usable type, and logs and skips the others.
    static std::vector<Entry> entries_from_config(const nlohmann::json &cfg);

    SteamVrSettingsBridge(SteamVrClient &vr, std::string section, std::vector<Entry> entries);

    // Returns the settings that changed in SteamVR since the last call, and only the main thread can call it.
    std::vector<std::pair<std::string, nlohmann::json>> poll(const nlohmann::json &cfg, uint64_t now_ns);
    // Copies a setting that the runtime changed to SteamVR.
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
    std::map<std::string, nlohmann::json> known_;   // The last value for each id in SteamVR.
};

}
