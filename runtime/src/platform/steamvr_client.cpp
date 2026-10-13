#include "platform/steamvr_client.h"

#include "core/clock.h"
#include "core/log.h"

#include <nlohmann/json.hpp>
#include <openvr.h>

#include <filesystem>
#include <fstream>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace {
const babble::log::Logger LOG("steamvr");
} // namespace

namespace babble {

SteamVrClient::~SteamVrClient() { stop(); }

void SteamVrClient::start(ThreadSpec thread, Options options) {
    presence_ = PresenceFilter(uint64_t(std::max(0.0, options.absent_after_s) * 1e9));
    thread_spec_ = std::move(thread);
    options_ = std::move(options);
    stop_ = false;
    thread_ = start_thread(thread_spec_, [this] { run(); });
}

void SteamVrClient::stop() {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
}

bool SteamVrClient::with_vr(const std::function<void()> &fn) const {
    // Exclusive: OpenVR client calls aren't documented as thread-safe, and the
    // client (proximity) and gaze threads both use IVRInput.
    std::unique_lock<std::shared_mutex> lock(vr_mutex_);
    if (!connected_)
        return false;
    fn();
    return true;
}

bool SteamVrClient::connect() {
    std::unique_lock<std::shared_mutex> lock(vr_mutex_);
    vr::EVRInitError err = vr::VRInitError_None;
    vr::VR_Init(&err, vr::VRApplication_Background);
    if (err != vr::VRInitError_None)
        return false;
    vr::ETrackedPropertyError perr = vr::TrackedProp_Success;
    has_prox_ = vr::VRSystem()->GetBoolTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
                                                            vr::Prop_ContainsProximitySensor_Bool, &perr);
    connected_ = true;
    connection_++;
    LOG.info("steamvr: connected (headset %s a proximity sensor)", has_prox_ ? "has" : "reports no");
    return true;
}

void SteamVrClient::disconnect() {
    std::unique_lock<std::shared_mutex> lock(vr_mutex_);
    if (!connected_)
        return;
    connected_ = false;
    vr::VR_Shutdown();
    worn_ = Worn::Unknown;
}

bool SteamVrClient::setup_input() {
    // One manifest for the process: proximity (boolean) + eye tracking.
    namespace fs = std::filesystem;
    using nlohmann::json;
    fs::path dir = options_.manifest_dir;
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string binding = "bindings_" + options_.controller_type + ".json";
    json b = {{"controller_type", options_.controller_type},
              {"bindings",
               {{"/actions/babble",
                 {{"sources", json::array({{{"path", options_.proximity_path},
                                            {"mode", "button"},
                                            {"inputs", {{"click", {{"output", "/actions/babble/in/proximity"}}}}}}})},
                  {"eyetracking", json::array({{{"path", options_.eyetracking_path},
                                                {"output", "/actions/babble/in/gaze"}}})}}}}}};
    std::ofstream(dir / binding) << b.dump(2);
    json m = {{"default_bindings", json::array({{{"controller_type", options_.controller_type}, {"binding_url", binding}}})},
              {"actions", json::array({{{"name", "/actions/babble/in/proximity"}, {"type", "boolean"}},
                                       {{"name", "/actions/babble/in/gaze"}, {"type", "eyetracking"}}})},
              {"action_sets", json::array({{{"name", "/actions/babble"}, {"usage", "single"}}})},
              {"localization", json::array()}};
    std::ofstream(dir / "actions.json") << m.dump(2);

    bool ok = false;
    with_vr([&] {
        auto *in = vr::VRInput();
        vr::VRActionSetHandle_t set = 0;
        vr::VRActionHandle_t prox = 0, gaze = 0;
        ok = in && in->SetActionManifestPath((dir / "actions.json").c_str()) == vr::VRInputError_None &&
             in->GetActionSetHandle("/actions/babble", &set) == vr::VRInputError_None &&
             in->GetActionHandle("/actions/babble/in/proximity", &prox) == vr::VRInputError_None &&
             in->GetActionHandle("/actions/babble/in/gaze", &gaze) == vr::VRInputError_None;
        action_set_ = set;
        prox_action_ = prox;
        gaze_action_ = gaze;
    });
    LOG.info("steamvr: input manifest %s", ok ? "ready (proximity + eye tracking)" : "setup FAILED");
    return ok;
}

// The raw proximity sensor; nullopt if the action isn't bound/active.
std::optional<bool> SteamVrClient::read_proximity() {
    std::optional<bool> out;
    with_vr([&] {
        if (!prox_action_)
            return;
        vr::VRActiveActionSet_t active{};
        active.ulActionSet = action_set_;
        vr::VRInput()->UpdateActionState(&active, sizeof(active), 1);
        vr::InputDigitalActionData_t d{};
        if (vr::VRInput()->GetDigitalActionData(prox_action_, &d, sizeof(d), vr::k_ulInvalidInputValueHandle) ==
                vr::VRInputError_None &&
            d.bActive)
            out = d.bState;
    });
    if (out && !prox_reported_) {
        LOG.info("steamvr: using the raw proximity sensor for presence");
        prox_reported_ = true;
    }
    return out;
}

Worn PresenceFilter::update(Worn raw, uint64_t now) {
    if (raw != Worn::No) {
        if (absent_since_ && out_ == Worn::Yes)
            blips_++;   // back before it counted: a sensor blip
        absent_since_ = 0;
        out_ = raw;
        return out_;
    }
    if (out_ != Worn::Yes) {   // nothing to hold on to (unknown, or already off)
        out_ = Worn::No;
        return out_;
    }
    if (!absent_since_)
        absent_since_ = now;
    if (now - absent_since_ >= absent_after_ns_) {
        out_ = Worn::No;
        absent_since_ = 0;
    }
    return out_;
}

void SteamVrClient::refresh_activity() {
    vr::EDeviceActivityLevel level = vr::k_EDeviceActivityLevel_Unknown;
    with_vr([&] { level = vr::VRSystem()->GetTrackedDeviceActivityLevel(vr::k_unTrackedDeviceIndex_Hmd); });
    Worn w;
    if (auto prox = read_proximity()) {
        w = *prox ? Worn::Yes : Worn::No;
    } else {
        switch (level) {
        case vr::k_EDeviceActivityLevel_UserInteraction:
            w = Worn::Yes;
            break;
        case vr::k_EDeviceActivityLevel_Idle:
        case vr::k_EDeviceActivityLevel_Idle_Timeout:
        case vr::k_EDeviceActivityLevel_Standby:
            w = Worn::No;
            break;
        default:   // UserInteraction_Timeout: in between; keep the last decision
            w = worn_.load() == Worn::Unknown ? Worn::Yes : worn_.load();
            break;
        }
    }
    const uint64_t blips = presence_.blips();
    w = presence_.update(w, mono_ns());
    if (presence_.blips() != blips)
        LOG.debug("steamvr: ignored a %s blip of the proximity sensor (%llu so far)", "not worn",
                  (unsigned long long)presence_.blips());
    Worn prev = worn_.exchange(w);
    if (prev != w)
        LOG.info("steamvr: headset %s (activity level %d)", to_string(w), int(level));
}

void SteamVrClient::refresh_ipd() {
    float v = 0;
    vr::ETrackedPropertyError err = vr::TrackedProp_Success;
    with_vr([&] {
        v = vr::VRSystem()->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_UserIpdMeters_Float,
                                                          &err);
    });
    // Anything outside the human range means the property isn't really there.
    if (err != vr::TrackedProp_Success || !(v > 0.045f && v < 0.085f))
        return;
    if (std::fabs(v - ipd_.load()) > 1e-5)
        LOG.info("steamvr: IPD %.1f mm", v * 1000.0);
    ipd_ = v;
}

void SteamVrClient::refresh_display_hz() {
    float v = 0;
    vr::ETrackedPropertyError err = vr::TrackedProp_Success;
    with_vr([&] {
        v = vr::VRSystem()->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
                                                          vr::Prop_DisplayFrequency_Float, &err);
    });
    if (err != vr::TrackedProp_Success || !(v > 10.0f && v < 1000.0f))
        return;
    if (std::fabs(v - display_hz_.load()) > 0.5)
        LOG.info("steamvr: display refresh %.0f Hz", v);
    display_hz_ = v;
}

void SteamVrClient::run() {
    uint64_t next_try = 0, next_poll = 0, next_display = 0;
    while (!stop_) {
        const uint64_t now = mono_ns();
        if (!connected_) {
            // The runtime starts while vrserver is still coming up: retry quietly.
            if (now >= next_try && !connect())
                next_try = now + 2000000000ull;
            if (connected_) {
                setup_input();
                refresh_ipd();
                refresh_activity();
            }
            sleep_ns(100000000);
            continue;
        }

        bool quit = false, ipd_changed = false, activity = false;
        with_vr([&] {
            vr::VREvent_t ev;
            while (vr::VRSystem()->PollNextEvent(&ev, sizeof(ev))) {
                switch (ev.eventType) {
                case vr::VREvent_Quit: quit = true; break;
                case vr::VREvent_IpdChanged: ipd_changed = true; break;
                case vr::VREvent_TrackedDeviceUserInteractionStarted:
                case vr::VREvent_TrackedDeviceUserInteractionEnded:
                case vr::VREvent_EnterStandbyMode:
                case vr::VREvent_LeaveStandbyMode:
                    activity |= ev.trackedDeviceIndex == vr::k_unTrackedDeviceIndex_Hmd ||
                                ev.eventType == vr::VREvent_EnterStandbyMode ||
                                ev.eventType == vr::VREvent_LeaveStandbyMode;
                    break;
                default: break;
                }
            }
            if (quit)
                vr::VRSystem()->AcknowledgeQuit_Exiting();
        });
        if (quit) {
            LOG.info("steamvr: SteamVR is quitting; disconnecting");
            disconnect();
            continue;
        }
        if (ipd_changed)
            refresh_ipd();
        // The refresh rate changes with a display mode change; once a second is plenty.
        if (now >= next_display) {
            refresh_display_hz();
            next_display = now + 1000000000ull;
        }
        // The proximity sensor is polled at 10 Hz (events cover the activity fallback).
        if (activity || now >= next_poll) {
            refresh_activity();
            next_poll = now + 100000000ull;
        }
        sleep_ns(50000000);
    }
    disconnect();
}

} // namespace babble
