#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>

namespace babble {

using json = nlohmann::json;

struct ConfigLoad {
    json config;                           // defaults, then the user file, then settings
    std::filesystem::path user_file;       // where the user file was looked for
    bool user_loaded = false;
    std::string error;                     // non-empty if the user file was present but rejected
    std::filesystem::path settings_file;   // where the settings file was looked for
    bool settings_loaded = false;
    std::string settings_error;
};

// Parses the compiled-in defaults and deep-merges the user file, then the
// settings file, over them (RFC 7386 merge patch: objects merge recursively,
// arrays and scalars replace, null removes). A broken file is reported and
// ignored, never fatal.
//
// Two user layers: config.json is the hand-edited file and is never written
// by the runtime; settings.json belongs to the settings panel (save_setting)
// and wins over config.json.
ConfigLoad load_config(const std::optional<std::filesystem::path> &user_file_override,
                       const std::optional<std::filesystem::path> &settings_file_override = std::nullopt);

// $XDG_CONFIG_HOME/babble-frame-runtime/settings.json
std::filesystem::path default_settings_file();

// Sets one value in the settings file (read-modify-write, atomic replace).
bool save_setting(const std::filesystem::path &settings_file, const json::json_pointer &where, const json &value,
                  std::string *error);

// Same merge used by load_config, exposed for tests.
json merge_config(json defaults, const json &user);

// Parses JSON allowing // and /* */ comments; returns nullopt on error.
std::optional<json> parse_json(const std::string &text, std::string *error);

} // namespace babble
