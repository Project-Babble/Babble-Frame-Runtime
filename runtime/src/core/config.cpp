#include "core/config.h"

#include "core/log.h"
#include "core/paths.h"
#include "defaults_json.h"

#include <fstream>
#include <sstream>

namespace {
const babble::log::Logger LOG("config");
}

namespace babble {

std::optional<json> parse_json(const std::string &text, std::string *error) {
    try {
        return json::parse(text, nullptr, true, true);
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

const char *kind(const json &j) {
    if (j.is_object()) return "object";
    if (j.is_array()) return "array";
    if (j.is_string()) return "string";
    if (j.is_boolean()) return "boolean";
    if (j.is_number()) return "number";
    return "null";
}

// These options can have more than one type, for example "ipd" can be "auto" or a value in metres.
bool flexible(const std::string &key) { return key == "ipd" || key == "one_euro" || key == "pupil_one_euro"; }

void fix_types_at(json &cfg, const json &defaults, const std::string &path, std::vector<std::string> &fixed) {
    for (auto it = defaults.begin(); it != defaults.end(); ++it) {
        if (!cfg.contains(it.key()) || it.value().is_null() || flexible(it.key()))
            continue;
        json &v = cfg[it.key()];
        const std::string p = path + "/" + it.key();
        if (it.value().is_object() && v.is_object()) {
            fix_types_at(v, it.value(), p, fixed);
        } else if (std::string(kind(v)) != kind(it.value())) {
            fixed.push_back(p + ": " + kind(v) + ", expected " + kind(it.value()));
            v = it.value();
        }
    }
}

// Collects the JSON pointer of each leaf value in a JSON object.
void leaves(const json &j, const std::string &path, std::vector<std::string> &out) {
    for (auto it = j.begin(); it != j.end(); ++it) {
        const std::string p = path + "/" + it.key();
        if (it.value().is_object() && !it.value().empty())
            leaves(it.value(), p, out);
        else
            out.push_back(p);
    }
}

}

std::vector<std::string> fix_types(json &cfg, const json &defaults) {
    std::vector<std::string> fixed;
    if (cfg.is_object() && defaults.is_object())
        fix_types_at(cfg, defaults, "", fixed);
    return fixed;
}

const json &section(const json &cfg, const char *key) {
    static const json empty = json::object();
    if (!cfg.is_object())
        return empty;
    auto it = cfg.find(key);
    return it != cfg.end() && it->is_object() ? *it : empty;
}

namespace {

enum class Layer { Missing, Loaded, Broken };

// Reads one user layer, merges it into config and puts the layer in *layer_out.
Layer merge_layer(const std::filesystem::path &file, json &config, std::string *error, json *layer_out = nullptr) {
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
    if (layer_out)
        *layer_out = std::move(*user);
    return Layer::Loaded;
}

}

std::filesystem::path default_settings_file() { return paths::config_dir() / "settings.json"; }

bool save_setting(const std::filesystem::path &settings_file, const json::json_pointer &where, const json &value,
                  std::string *error) {
    json doc = json::object();
    {
        std::ifstream in(settings_file);
        if (in) {
            std::stringstream ss;
            ss << in.rdbuf();
            std::string parse_error;
            auto parsed = parse_json(ss.str(), &parse_error);
            if (parsed && parsed->is_object()) {
                doc = std::move(*parsed);
            } else {
                // Keep the broken file for inspection so that the saved settings are not lost.
                std::filesystem::path bad = settings_file;
                bad += ".bad";
                std::error_code ec;
                std::filesystem::rename(settings_file, bad, ec);
                LOG.warn("config: %s was unreadable (%s); moved it to %s and started a new one", settings_file.c_str(),
                         parsed ? "top level is not an object" : parse_error.c_str(), bad.c_str());
            }
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
        // A parse failure of the compiled-in defaults is a build bug.
        LOG.error("built-in defaults.json is invalid: %s", err.c_str());
        out.config = json::object();
    } else {
        out.config = std::move(*defaults);
    }

    out.user_file = user_file_override ? *user_file_override : paths::config_dir() / "config.json";
    out.user_loaded = merge_layer(out.user_file, out.config, &out.error) == Layer::Loaded;
    out.settings_file = settings_file_override ? *settings_file_override : default_settings_file();
    json settings_layer;
    out.settings_loaded =
        merge_layer(out.settings_file, out.config, &out.settings_error, &settings_layer) == Layer::Loaded;
    if (out.settings_loaded)
        leaves(settings_layer, "", out.settings_overrides);
    if (defaults)
        out.type_fixes = fix_types(out.config, *defaults);
    return out;
}

}
