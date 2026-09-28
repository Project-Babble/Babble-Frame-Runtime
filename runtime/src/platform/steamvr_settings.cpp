#include "platform/steamvr_settings.h"

#include "core/log.h"
#include "platform/steamvr_client.h"

#include <openvr.h>

#include <cmath>

namespace {
const babble::log::Logger LOG("settings");
} // namespace

namespace babble {

using json = nlohmann::json;

namespace {

using Type = SteamVrSettingsBridge::Entry::Type;

std::optional<Type> type_of(const json &v) {
    if (v.is_boolean())
        return Type::Bool;
    if (v.is_number())
        return Type::Int;
    if (v.is_string())
        return Type::String;
    return std::nullopt;
}

// The value as SteamVR stores it for this entry, or nullopt if the type doesn't fit.
std::optional<json> normalize(Type t, const json &v) {
    if (type_of(v) != t)
        return std::nullopt;
    return t == Type::Int ? json(int32_t(std::lround(v.get<double>()))) : v;
}

} // namespace

std::vector<SteamVrSettingsBridge::Entry> SteamVrSettingsBridge::entries_from_config(const json &cfg) {
    std::vector<Entry> out;
    const json sc = cfg.value("steamvr_settings", json::object());
    for (const json &v : sc.value("values", json::array())) {
        Entry e{v.value("key", ""), v.value("id", "")};
        json::json_pointer ptr;
        try {
            ptr = json::json_pointer(e.id);
        } catch (const json::exception &) {
            LOG.warn("steamvr settings: ignoring '%s' with a bad id '%s'", e.key.c_str(), e.id.c_str());
            continue;
        }
        const auto type = cfg.contains(ptr) ? type_of(cfg[ptr]) : std::nullopt;
        if (e.key.empty() || !type) {
            LOG.warn("steamvr settings: ignoring '%s' (%s is not a bool, number or string setting)", e.key.c_str(),
                     e.id.c_str());
            continue;
        }
        e.type = *type;
        out.push_back(std::move(e));
    }
    return out;
}

SteamVrSettingsBridge::SteamVrSettingsBridge(SteamVrClient &vr, std::string section, std::vector<Entry> entries)
    : vr_(vr), section_(std::move(section)), entries_(std::move(entries)) {}

std::optional<json> SteamVrSettingsBridge::read(const Entry &e) {
    std::optional<json> out;
    vr_.with_vr([&] {
        vr::EVRSettingsError err = vr::VRSettingsError_None;
        const char *sec = section_.c_str(), *key = e.key.c_str();
        switch (e.type) {
        case Type::Bool: {
            bool b = vr::VRSettings()->GetBool(sec, key, &err);
            if (err == vr::VRSettingsError_None)
                out = b;
            break;
        }
        case Type::Int: {
            int32_t i = vr::VRSettings()->GetInt32(sec, key, &err);
            if (err == vr::VRSettingsError_None)
                out = i;
            break;
        }
        case Type::String: {
            char buf[256] = {};
            vr::VRSettings()->GetString(sec, key, buf, sizeof(buf), &err);
            if (err == vr::VRSettingsError_None)
                out = std::string(buf);
            break;
        }
        }
    });
    return out;
}

bool SteamVrSettingsBridge::write(const Entry &e, const json &value) {
    vr::EVRSettingsError err = vr::VRSettingsError_None;
    const bool connected = vr_.with_vr([&] {
        const char *sec = section_.c_str(), *key = e.key.c_str();
        switch (e.type) {
        case Type::Bool: vr::VRSettings()->SetBool(sec, key, value.get<bool>(), &err); break;
        case Type::Int: vr::VRSettings()->SetInt32(sec, key, value.get<int32_t>(), &err); break;
        case Type::String: vr::VRSettings()->SetString(sec, key, value.get<std::string>().c_str(), &err); break;
        }
    });
    if (connected && err != vr::VRSettingsError_None)
        LOG.warn("steamvr settings: writing %s/%s failed (%d)", section_.c_str(), e.key.c_str(), int(err));
    return connected && err == vr::VRSettingsError_None;
}

std::vector<std::pair<std::string, json>> SteamVrSettingsBridge::poll(const json &cfg, uint64_t now_ns) {
    std::vector<std::pair<std::string, json>> changes;
    if (!vr_.connected()) {
        connection_ = 0;
        return changes;
    }
    if (connection_ != vr_.connection()) {
        // New connection: SteamVR shows what the runtime is using.
        known_.clear();
        for (const Entry &e : entries_) {
            auto v = normalize(e.type, cfg[json::json_pointer(e.id)]);
            if (v && write(e, *v))
                known_[e.id] = *v;
        }
        connection_ = vr_.connection();
        next_poll_ns_ = now_ns + 500000000ull;
        LOG.info("steamvr settings: \"%s\" section in sync (%zu values)", section_.c_str(), known_.size());
        return changes;
    }
    if (now_ns < next_poll_ns_)
        return changes;
    next_poll_ns_ = now_ns + 500000000ull;
    for (const Entry &e : entries_) {
        auto v = read(e);
        if (!v)
            continue;
        auto it = known_.find(e.id);
        if (it != known_.end() && it->second == *v)
            continue;
        known_[e.id] = *v;
        changes.emplace_back(e.id, *v);
    }
    return changes;
}

void SteamVrSettingsBridge::push(const std::string &id, const json &value) {
    for (const Entry &e : entries_) {
        if (e.id != id)
            continue;
        auto v = normalize(e.type, value);
        if (!v || (known_.count(id) && known_[id] == *v))
            continue;
        if (write(e, *v))
            known_[id] = *v;
    }
}

} // namespace babble
