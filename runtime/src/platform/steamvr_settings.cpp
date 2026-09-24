#include "platform/steamvr_settings.h"

#include "core/log.h"
#include "platform/steamvr_client.h"

#include <openvr.h>

#include <cmath>

namespace babble {

using json = nlohmann::json;

std::vector<SteamVrSettingsBridge::Entry> SteamVrSettingsBridge::entries_from_config(const json &cfg) {
    std::vector<Entry> out;
    const json sc = cfg.value("steamvr_settings", json::object());
    for (const json &v : sc.value("values", json::array())) {
        Entry e{v.value("key", ""), v.value("id", "")};
        json::json_pointer ptr;
        try {
            ptr = json::json_pointer(e.id);
        } catch (const json::exception &) {
            LOG_WARN("steamvr settings: ignoring '%s' with a bad id '%s'", e.key.c_str(), e.id.c_str());
            continue;
        }
        if (e.key.empty() || !cfg.contains(ptr) || !(cfg[ptr].is_boolean() || cfg[ptr].is_number())) {
            LOG_WARN("steamvr settings: ignoring '%s' (%s is not a bool or number setting)", e.key.c_str(),
                     e.id.c_str());
            continue;
        }
        e.is_bool = cfg[ptr].is_boolean();
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
        if (e.is_bool) {
            bool b = vr::VRSettings()->GetBool(section_.c_str(), e.key.c_str(), &err);
            if (err == vr::VRSettingsError_None)
                out = b;
        } else {
            int32_t i = vr::VRSettings()->GetInt32(section_.c_str(), e.key.c_str(), &err);
            if (err == vr::VRSettingsError_None)
                out = i;
        }
    });
    return out;
}

bool SteamVrSettingsBridge::write(const Entry &e, const json &value) {
    vr::EVRSettingsError err = vr::VRSettingsError_None;
    const bool connected = vr_.with_vr([&] {
        if (e.is_bool)
            vr::VRSettings()->SetBool(section_.c_str(), e.key.c_str(), value.get<bool>(), &err);
        else
            vr::VRSettings()->SetInt32(section_.c_str(), e.key.c_str(), int32_t(std::lround(value.get<double>())),
                                       &err);
    });
    if (connected && err != vr::VRSettingsError_None)
        LOG_WARN("steamvr settings: writing %s/%s failed (%d)", section_.c_str(), e.key.c_str(), int(err));
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
            const json v = cfg[json::json_pointer(e.id)];
            if ((e.is_bool ? v.is_boolean() : v.is_number()) && write(e, v))
                known_[e.id] = e.is_bool ? v : json(int32_t(std::lround(v.get<double>())));
        }
        connection_ = vr_.connection();
        next_poll_ns_ = now_ns + 500000000ull;
        LOG_INFO("steamvr settings: \"%s\" section in sync (%zu values)", section_.c_str(), known_.size());
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
        if (e.id != id || (e.is_bool ? !value.is_boolean() : !value.is_number()))
            continue;
        const json v = e.is_bool ? value : json(int32_t(std::lround(value.get<double>())));
        if (known_.count(id) && known_[id] == v)
            continue;
        if (write(e, v))
            known_[id] = v;
    }
}

} // namespace babble
