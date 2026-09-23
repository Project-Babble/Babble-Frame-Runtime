#include "core/config.h"

#include "core/log.h"
#include "core/paths.h"
#include "defaults_json.h"

#include <fstream>
#include <sstream>

namespace babble {

std::optional<json> parse_json(const std::string &text, std::string *error) {
    try {
        return json::parse(text, nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/true);
    } catch (const json::exception &e) {
        if (error)
            *error = e.what();
        return std::nullopt;
    }
}

json merge_config(json defaults, const json &user) {
    defaults.merge_patch(user);
    return defaults;
}

ConfigLoad load_config(const std::optional<std::filesystem::path> &user_file_override) {
    ConfigLoad out;
    std::string err;
    auto defaults = parse_json(kDefaultsJson, &err);
    if (!defaults) {
        // The defaults are compiled in; failing to parse them is a build bug.
        LOG_ERROR("built-in defaults.json is invalid: %s", err.c_str());
        out.config = json::object();
    } else {
        out.config = std::move(*defaults);
    }

    out.user_file = user_file_override ? *user_file_override : paths::config_dir() / "config.json";
    std::ifstream in(out.user_file);
    if (!in)
        return out;

    std::stringstream ss;
    ss << in.rdbuf();
    auto user = parse_json(ss.str(), &err);
    if (!user) {
        out.error = err;
        return out;
    }
    if (!user->is_object()) {
        out.error = "top level is not an object";
        return out;
    }
    out.config = merge_config(std::move(out.config), *user);
    out.user_loaded = true;
    return out;
}

} // namespace babble
