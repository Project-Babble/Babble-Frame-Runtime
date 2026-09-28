#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

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
    // Keys whose type didn't match the defaults ("/sources/mouth/tracking: string, expected boolean");
    // they were reset to the default value.
    std::vector<std::string> type_fixes;
    // Leaf keys set by settings.json (they win over config.json).
    std::vector<std::string> settings_overrides;
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

// Resets every key of cfg whose JSON kind (object, array, string, boolean,
// number) differs from the same key in defaults, so later reads can trust the
// types. A null default accepts anything. Returns what was reset.
std::vector<std::string> fix_types(json &cfg, const json &defaults);

// A config section, or an empty object if it's missing or not an object
// (never inserts, unlike operator[]).
const json &section(const json &cfg, const char *key);

// Parses JSON allowing // and /* */ comments; returns nullopt on error.
std::optional<json> parse_json(const std::string &text, std::string *error);

} // namespace babble
