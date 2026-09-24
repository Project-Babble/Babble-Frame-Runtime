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

namespace {

enum class Layer { Missing, Loaded, Broken };

// Reads one user layer and merges it into config.
Layer merge_layer(const std::filesystem::path &file, json &config, std::string *error) {
    std::ifstream in(file);
    if (!in)
        return Layer::Missing;
    std::stringstream ss;
    ss << in.rdbuf();
    auto user = parse_json(ss.str(), error);
    if (!user)
        return Layer::Broken;
    if (!user->is_object()) {
        *error = "top level is not an object";
        return Layer::Broken;
    }
    config = merge_config(std::move(config), *user);
    return Layer::Loaded;
}

} // namespace

std::filesystem::path default_settings_file() { return paths::config_dir() / "settings.json"; }

bool save_setting(const std::filesystem::path &settings_file, const json::json_pointer &where, const json &value,
                  std::string *error) {
    json doc = json::object();
    {
        std::ifstream in(settings_file);
        if (in) {
            std::stringstream ss;
            ss << in.rdbuf();
            auto parsed = parse_json(ss.str(), error);
            if (parsed && parsed->is_object())
                doc = std::move(*parsed);   // a broken file is replaced rather than kept
        }
    }
    doc[where] = value;
    std::filesystem::path tmp = settings_file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << doc.dump(2) << "\n";
        if (!out) {
            if (error)
                *error = "cannot write " + tmp.string();
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, settings_file, ec);
    if (ec && error)
        *error = ec.message();
    return !ec;
}

ConfigLoad load_config(const std::optional<std::filesystem::path> &user_file_override,
                       const std::optional<std::filesystem::path> &settings_file_override) {
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
    out.user_loaded = merge_layer(out.user_file, out.config, &out.error) == Layer::Loaded;
    out.settings_file = settings_file_override ? *settings_file_override : default_settings_file();
    out.settings_loaded = merge_layer(out.settings_file, out.config, &out.settings_error) == Layer::Loaded;
    return out;
}

} // namespace babble
