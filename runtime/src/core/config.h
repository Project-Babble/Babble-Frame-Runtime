#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace babble {

using json = nlohmann::json;

struct ConfigLoad {
    json config;                           // The defaults with the user file and the settings file merged over them.
    std::filesystem::path user_file;       // The path where the loader looked for the user file.
    bool user_loaded = false;
    std::string error;                     // This is not empty if the user file exists but the loader rejected it.
    std::filesystem::path settings_file;   // The path where the loader looked for the settings file.
    bool settings_loaded = false;
    std::string settings_error;
    // The keys that had a type different from the defaults, which the loader reset to the default value.
    std::vector<std::string> type_fixes;
    // The leaf keys that settings.json sets over config.json.
    std::vector<std::string> settings_overrides;
};

// Merges config.json and then settings.json over the defaults with RFC 7386, and reports and ignores a broken file.
ConfigLoad load_config(const std::optional<std::filesystem::path> &user_file_override,
                       const std::optional<std::filesystem::path> &settings_file_override = std::nullopt);

// Returns $XDG_CONFIG_HOME/babble-frame-runtime/settings.json.
std::filesystem::path default_settings_file();

// Sets one value in the settings file and replaces the file atomically.
bool save_setting(const std::filesystem::path &settings_file, const json::json_pointer &where, const json &value,
                  std::string *error);

// The merge that load_config uses, which is public for the tests.
json merge_config(json defaults, const json &user);

// Resets each key of cfg that has a JSON kind different from defaults, and returns the keys that it reset.
std::vector<std::string> fix_types(json &cfg, const json &defaults);

// Returns a config section, or an empty object if the section is missing, and never inserts like operator[].
const json &section(const json &cfg, const char *key);

// Parses JSON that can contain comments, and returns nullopt on an error.
std::optional<json> parse_json(const std::string &text, std::string *error);

}
