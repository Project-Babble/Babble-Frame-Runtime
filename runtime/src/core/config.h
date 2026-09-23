#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>

namespace babble {

using json = nlohmann::json;

struct ConfigLoad {
    json config;                       // defaults with the user file merged on top
    std::filesystem::path user_file;   // where the user file was looked for
    bool user_loaded = false;
    std::string error;                 // non-empty if the user file was present but rejected
};

// Parses the compiled-in defaults and deep-merges the user file over them
// (RFC 7386 merge patch: objects merge recursively, arrays and scalars replace,
// null removes). A broken user file is reported and ignored, never fatal.
ConfigLoad load_config(const std::optional<std::filesystem::path> &user_file_override);

// Same merge used by load_config, exposed for tests.
json merge_config(json defaults, const json &user);

// Parses JSON allowing // and /* */ comments; returns nullopt on error.
std::optional<json> parse_json(const std::string &text, std::string *error);

} // namespace babble
