// babble-runtime: Babble mouth + eye tracking runtime for the Steam Frame.
//
// Normally spawned by the SteamVR driver shim (driver/src/driver_babble.cpp)
// with a pipe on fd 3; EOF there or SIGTERM means shut down. --standalone
// runs without the shim for development over SSH.

#include "babble_version.h"
#include "camera/tracking_source.h"
#include "camera/usb/babble_usb_source.h"
#include "core/clock.h"
#include "core/config.h"
#include "core/crash.h"
#include "core/lifecycle.h"
#include "core/log.h"
#include "core/metrics.h"
#include "core/paths.h"
#include "core/threading.h"
#include "model/executorch_runner.h"
#include "model/pipeline.h"
#include "platform/app_launcher.h"
#include "platform/diagnostics.h"
#include "platform/camera_arbiter_client.h"
#include "platform/eye_server_client.h"
#include "platform/steamvr_client.h"
#include "platform/steamvr_settings.h"
#include "sink/link_sink.h"
#include "sink/vrc_native_sink.h"
#include "sink/sink.h"
#include "state/expression_state.h"
#include "ui/dashboard.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

namespace {
const babble::log::Logger LOG("main");
} // namespace

using namespace babble;

namespace {

struct Options {
    std::optional<std::filesystem::path> config;
    bool standalone = false;
    std::string dump_dir;
    int dump_count = -1;
    int stats_interval = -1;
    bool no_eye = false, no_mouth = false;
    std::string log_level;
    int probe_orientation = 0;
};

void usage() {
    fprintf(stderr,
            "usage: babble-runtime [--config FILE] [--standalone] [--dump-frames DIR] [--dump-count N]\n"
            "                      [--stats-interval SECONDS] [--no-eye] [--no-mouth] [--log-level LEVEL]\n"
            "                      [--probe-orientation FRAMES]\n");
}

bool parse_args(int argc, char **argv, Options *o) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> const char * { return i + 1 < argc ? argv[++i] : nullptr; };
        const char *v = nullptr;
        if (a == "--config" && (v = next())) o->config = v;
        else if (a == "--standalone") o->standalone = true;
        else if (a == "--dump-frames" && (v = next())) o->dump_dir = v;
        else if (a == "--dump-count" && (v = next())) o->dump_count = atoi(v);
        else if (a == "--stats-interval" && (v = next())) o->stats_interval = atoi(v);
        else if (a == "--no-eye") o->no_eye = true;
        else if (a == "--no-mouth") o->no_mouth = true;
        else if (a == "--log-level" && (v = next())) o->log_level = v;
        else if (a == "--probe-orientation" && (v = next())) o->probe_orientation = atoi(v);
        else if (a == "-h" || a == "--help") { usage(); exit(0); }
        else { usage(); return false; }
    }
    return true;
}

json load(const Options &opt) {
    ConfigLoad cl = load_config(opt.config);
    if (!cl.error.empty())
        LOG.error("config: ignoring %s: %s", cl.user_file.c_str(), cl.error.c_str());
    else if (cl.user_loaded)
        LOG.info("config: loaded %s", cl.user_file.c_str());
    if (!cl.settings_error.empty())
        LOG.error("config: ignoring %s: %s (it will be moved aside on the next settings change)",
                  cl.settings_file.c_str(), cl.settings_error.c_str());
    else if (cl.settings_loaded) {
        std::string keys;
        for (const auto &k : cl.settings_overrides)
            keys += (keys.empty() ? "" : ", ") + k;
        LOG.info("config: loaded %s (saved settings, override config.json): %s", cl.settings_file.c_str(),
                 keys.empty() ? "nothing" : keys.c_str());
    }
    for (const auto &fix : cl.type_fixes)
        LOG.warn("config: %s; using the default", fix.c_str());
    json &c = cl.config;
    if (opt.no_eye && c["sources"].contains("eye")) c["sources"]["eye"]["enabled"] = false;
    if (opt.no_mouth && c["sources"].contains("mouth")) c["sources"]["mouth"]["enabled"] = false;
    // --dump-frames targets the "debug" output module (added if not configured).
    if (!opt.dump_dir.empty() || opt.dump_count >= 0) {
        json &d = c["sinks"]["debug"];
        if (!d.contains("type")) d["type"] = "debug";
        d["enabled"] = true;
        if (!opt.dump_dir.empty()) d["dump_dir"] = opt.dump_dir;
        if (opt.dump_count >= 0) d["dump_count"] = opt.dump_count;
        else if (!opt.dump_dir.empty()) d["dump_count"] = 10;
    }
    if (opt.stats_interval > 0) c["stats_interval_s"] = opt.stats_interval;
    if (!opt.log_level.empty()) c["log_level"] = opt.log_level;
    if (opt.probe_orientation > 0 && c["pipelines"].contains("mouth"))
        c["pipelines"]["mouth"]["probe_orientation"] = opt.probe_orientation;
    return cl.config;
}

// log_level (the default) and log_levels ({"usb": "debug", ...} per module).
void apply_log_level(const json &c) {
    log::Level lvl;
    std::string name = c.value("log_level", "info");
    if (log::parse_level(name.c_str(), &lvl))
        log::set_level(lvl);
    log::clear_module_levels();
    for (const auto &[module, value] : section(c, "log_levels").items()) {
        if (value.is_string() && log::parse_level(value.get<std::string>().c_str(), &lvl))
            log::set_module_level(module, lvl);
        else
            LOG.warn("log_levels: ignoring %s = %s", module.c_str(), value.dump().c_str());
    }
}

// One line from the stats JSON for the log: per pipeline the rate and
// inference time (this interval), plus any counter that isn't zero among the
// ones that mean trouble (drops, errors, restarts).
std::string stats_summary(const std::string &stats_json) {
    std::string err;
    auto j = parse_json(stats_json, &err);
    if (!j || !j->is_object())
        return "(no stats)";
    std::string out, trouble;
    for (auto it = j->begin(); it != j->end(); ++it) {
        const std::string &k = it.key();
        if (k.rfind("pipe.", 0) == 0 && k.size() > 4 && k.ends_with(".fps")) {
            const std::string name = k.substr(5, k.size() - 9);
            const auto inf = j->value("pipe." + name + ".inference", json::object());
            char buf[128];
            snprintf(buf, sizeof(buf), "%s %.1f/s", name.c_str(), it.value().is_number() ? it.value().get<double>() : 0.0);
            out += (out.empty() ? "" : ", ") + std::string(buf);
            if (inf.value("n", 0) > 0) {
                snprintf(buf, sizeof(buf), " (%.1f/%.1f ms)", inf.value("p50_us", 0) / 1000.0, inf.value("p99_us", 0) / 1000.0);
                out += buf;
            }
        } else if (it.value().is_number_integer() && it.value().get<int64_t>() > 0 &&
                   (k.find("exhausted") != std::string::npos || k.find("error") != std::string::npos ||
                    k.find("dropped") != std::string::npos || k.find("restarts") != std::string::npos ||
                    k.find("undecodable") != std::string::npos || k.find("faults") != std::string::npos ||
                    k.find("timeouts") != std::string::npos)) {
            trouble += (trouble.empty() ? "" : ", ") + k + " " + std::to_string(it.value().get<int64_t>());
        }
    }
    return (out.empty() ? std::string("no pipelines") : out) + " (inference p50/p99)" +
           (trouble.empty() ? "" : "; counters: " + trouble);
}

// The log file: runtime.log in the state dir under the shim (standalone runs
// get their own file, so they don't interleave with the supervised runtime).
void open_log_file(const json &c, bool supervised) {
    const json f = section(c, "log_file");
    const double max_mb = f.value("max_mb", 8.0);
    const int keep = f.value("keep", 3);
    const auto path = paths::state_dir() / (supervised ? "runtime.log" : "runtime-standalone.log");
    if (!log::open_file(path, size_t(std::max(0.5, max_mb) * 1024 * 1024), keep))
        LOG.warn("cannot write %s: %s; logging to stderr only", path.c_str(), strerror(errno));
}

// The settings panel's rows (config["dashboard"]["settings"]), with the
// current values from the config. Rows whose setting doesn't exist (e.g. a
// source that isn't configured) are left out.
// Settings the debug overlay changes for this run only ("persist": false):
// never written to settings.json.
std::set<std::string> unsaved_settings(const json &cfg) {
    std::set<std::string> out;
    for (const json &row : section(cfg, "dashboard").value("settings", json::array()))
        if (!row.value("persist", true))
            out.insert(row.value("id", ""));
    return out;
}

std::vector<ui::Choice> dashboard_choices(const json &cfg) {
    std::vector<ui::Choice> out;
    for (const json &row : section(cfg, "dashboard").value("settings", json::array())) {
        ui::Choice c;
        c.id = row.value("id", "");
        json::json_pointer ptr;
        try {
            ptr = json::json_pointer(c.id);
        } catch (const json::exception &) {
            LOG.warn("dashboard: ignoring setting with a bad id '%s'", c.id.c_str());
            continue;
        }
        if (!cfg.contains(ptr) || !(cfg[ptr].is_number() || cfg[ptr].is_boolean()))
            continue;
        c.label = row.value("label", c.id);
        c.hint = row.value("hint", "");
        c.unit = row.value("unit", "");
        c.boolean = cfg[ptr].is_boolean();
        c.diagnostics = row.value("page", "settings") == "diagnostics";
        c.options = c.boolean ? std::vector<double>{0, 1} : row.value("options", std::vector<double>{});
        c.value = c.boolean ? double(cfg[ptr].get<bool>()) : cfg[ptr].get<double>();
        out.push_back(std::move(c));
    }
    return out;
}

int add_epoll(int ep, int fd) {
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    return epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
}

} // namespace

int main(int argc, char **argv) {
    Options opt;
    if (!parse_args(argc, argv, &opt))
        return 2;
    setvbuf(stderr, nullptr, _IOLBF, 0);
    crash::install();

    json cfg = load(opt);
    open_log_file(cfg, ShimLink::present());
    apply_log_level(cfg);
    const json threads = cfg.value("threads", json::object());
    apply_thread_spec(thread_spec(threads, "main", "babble-runtime"));

    const bool shim = ShimLink::present();
    LOG.info("babble-runtime %s starting (pid %d, %s, state dir %s)", BABBLE_VERSION_DISPLAY, getpid(),
             shim ? "supervised by driver shim" : "standalone", paths::state_dir().c_str());
    if (!shim && !opt.standalone)
        LOG.warn("no shim link on fd 3; running unsupervised (use --standalone to silence this)");
    for (const auto &line : diag::session_info(cfg, paths::resource_dir() / "models"))
        LOG.info("session: %s", line.c_str());

    int sfd = make_signalfd();
    int ep = epoll_create1(EPOLL_CLOEXEC);
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (sfd < 0 || ep < 0 || tfd < 0) {
        LOG.error("event setup failed: %s", strerror(errno));
        return 1;
    }
    itimerspec its{{0, 100000000}, {0, 100000000}};   // 10 Hz housekeeping tick
    if (timerfd_settime(tfd, 0, &its, nullptr) != 0 || add_epoll(ep, sfd) != 0 || add_epoll(ep, tfd) != 0 ||
        (shim && add_epoll(ep, ShimLink::kFd) != 0)) {
        LOG.error("event setup failed: %s", strerror(errno));
        return 1;
    }

    StatsRegistry stats;
    ExpressionState state;
    CameraArbiterClient arbiter(cfg.value("arbiter", json::object()));
    // The one SteamVR connection: headset presence (proximity sensor), IPD, gaze input.
    SteamVrClient steamvr;
    {
        const json vc = cfg.value("steamvr", json::object());
        SteamVrClient::Options o;
        o.manifest_dir = (paths::state_dir() / "steamvr_input").string();
        o.controller_type = vc.value("controller_type", o.controller_type);
        o.proximity_path = vc.value("proximity_path", o.proximity_path);
        o.eyetracking_path = vc.value("eyetracking_path", o.eyetracking_path);
        steamvr.start(thread_spec(threads, "steamvr", "bbl-steamvr"), o);
    }
    // SteamVR's eye-service shared memory (independent gaze, native openness);
    // it only reads while a pipeline asks for it.
    EyeServerClient eye_server;
    {
        const json ec = cfg.value("eye_server", json::object());
        EyeServerClient::Options o;
        o.path = ec.value("path", o.path);
        o.version = ec.value("version", o.version);
        eye_server.start(thread_spec(threads, ec.value("thread", "eyesrv"), "bbl-eyesrv"), o, &stats);
    }

    // One output slot per configured source; pipelines find theirs by name.
    const json scfg = cfg.value("sources", json::object());
    std::map<std::string, std::unique_ptr<LatestSlot<StereoFrame>>> slots;
    for (auto it = scfg.begin(); it != scfg.end(); ++it)
        slots[it.key()] = std::make_unique<LatestSlot<StereoFrame>>();

    // Output modules, built from config["sinks"] by type.
    // Sinks with an "output" role ("vrcft", "vrc_native") run only while the
    // Output setting (/output) picks that role; the rest always run.
    struct SinkEntry {
        std::string name;
        std::unique_ptr<ISink> sink;
        std::string output{};
        bool running = false;
    };
    std::vector<SinkEntry> sinks;
    std::vector<ISink *> frame_taps;
    const json kcfg = cfg.value("sinks", json::object());
    for (auto it = kcfg.begin(); it != kcfg.end(); ++it) {
        if (!it.value().value("enabled", true)) {
            LOG.info("sink %s: disabled", it.key().c_str());
            continue;
        }
        auto sink = make_sink(it.value(), SinkDeps{it.key(), threads, state, stats});
        if (!sink)
            continue;
        if (sink->wants_frames())
            frame_taps.push_back(sink.get());
        sinks.push_back({it.key(), std::move(sink), it.value().value("output", "")});
    }

    // Model pipelines, built from config["pipelines"] by type. XNNPACK's
    // threadpool is process-global and ignores per-thread pinning, so keep it
    // at one thread: each model runs on its own pinned pipeline thread.
    ExecuTorchRunner::set_threads(1);
    const std::filesystem::path models_dir = paths::resource_dir() / "models";
    auto slot_of = [&](const std::string &source) -> LatestSlot<StereoFrame> * {
        auto it = slots.find(source);
        return it == slots.end() ? nullptr : it->second.get();
    };
    std::vector<std::unique_ptr<IPipeline>> pipelines;
    const json pcfg = cfg.value("pipelines", json::object());
    for (auto it = pcfg.begin(); it != pcfg.end(); ++it) {
        if (!it.value().value("enabled", true)) {
            LOG.info("pipeline %s: disabled", it.key().c_str());
            continue;
        }
        auto p = make_pipeline(it.value(),
                               PipelineDeps{it.key(), threads, slot_of, state, stats, frame_taps, models_dir, &steamvr,
                                            &eye_server});
        if (p && p->init())
            pipelines.push_back(std::move(p));
        else
            LOG.error("pipeline %s: not started", it.key().c_str());
    }

    struct SourceEntry {
        std::string name;
        std::unique_ptr<ITrackingSource> src;
        std::string last_state{};   // for logging state changes
    };
    std::vector<SourceEntry> sources;
    for (auto it = scfg.begin(); it != scfg.end(); ++it) {
        if (!it.value().value("enabled", true)) {
            LOG.info("source %s: disabled", it.key().c_str());
            continue;
        }
        auto src = make_tracking_source(it.value(), SourceDeps{it.key(), threads, *slots[it.key()], stats, arbiter, &steamvr});
        if (src)
            sources.push_back({it.key(), std::move(src)});
    }

    // Where tracking goes: VRCFaceTracking (the PC link) or VRChat itself.
    auto output_choice = [&] {
        const json &o = cfg.value("output", json("vrcft"));
        return o.is_string() ? o.get<std::string>() : std::string("vrcft");
    };
    for (auto it = sinks.begin(); it != sinks.end();) {
        if (!it->output.empty() && it->output != output_choice()) {
            ++it;   // started when the Output setting picks it
            continue;
        }
        if (it->sink->start()) {
            it->running = true;
            ++it;
            continue;
        }
        LOG.error("sink %s: failed to start; dropping it", it->name.c_str());
        std::erase(frame_taps, it->sink.get());
        it = sinks.erase(it);
    }
    LOG.info("output: %s", output_choice() == "vrc_native" ? "VRChat native" : "VRCFaceTracking");
    auto route_output = [&] {
        const std::string want = output_choice();
        for (auto &k : sinks) {
            if (k.output.empty())
                continue;
            if (k.output == want && !k.running) {
                k.running = k.sink->start();
                if (!k.running)
                    LOG.error("sink %s: failed to start", k.name.c_str());
            } else if (k.output != want && k.running) {
                k.sink->stop();
                k.running = false;
                LOG.info("sink %s: stopped (output is %s)", k.name.c_str(), want.c_str());
            }
        }
    };
    // For the debug overlay's status line: the PC link and the native sender (the first of each).
    LinkSink *pc_link = nullptr;
    VrcNativeSink *vrc_native = nullptr;
    const SinkEntry *pc_link_entry = nullptr, *vrc_native_entry = nullptr;
    uint64_t native_sent_last = 0;   // blobs sent at the last status refresh
    for (auto &k : sinks) {
        if (!pc_link && (pc_link = dynamic_cast<LinkSink *>(k.sink.get())))
            pc_link_entry = &k;
        if (!vrc_native && (vrc_native = dynamic_cast<VrcNativeSink *>(k.sink.get())))
            vrc_native_entry = &k;
    }
    for (auto &p : pipelines)
        p->start();

    // A feed's user on/off: the source stops its cameras, and every pipeline
    // of that feed (incl. SteamVR gaze for the eyes) goes neutral.
    auto set_feed_tracking = [&](const std::string &feed, bool on) {
        for (auto &s : sources)
            if (s.name == feed)
                s.src->set_tracking(on);
        for (auto &p : pipelines)
            if (p->feed() == feed)
                p->set_tracking(on);
    };
    for (auto &s : sources)
        if (!s.src->tracking())
            set_feed_tracking(s.name, false);

    // The mouth comes from the Frame's own cameras, the "frunk" (source
    // "mouth"), or a USB camera such as a Babble face tracker (source "face"),
    // per "mouth_source". The Mouth Tracking switch (/sources/mouth/tracking)
    // turns whichever it is on or off; the other one stays off.
    auto setting_at = [&](const char *pointer, const json &fallback) {
        const json::json_pointer ptr(pointer);
        return cfg.contains(ptr) && std::string(cfg[ptr].type_name()) == fallback.type_name() ? cfg[ptr] : fallback;
    };
    auto mouth_from_usb = [&] { return setting_at("/mouth_source", "frunk") == "usb"; };
    auto route_mouth = [&] {
        const bool on = setting_at("/sources/mouth/tracking", true).get<bool>();
        set_feed_tracking("mouth", on && !mouth_from_usb());
        set_feed_tracking("face", on && mouth_from_usb());
    };
    route_mouth();
    // The Mouth tracking rate applies to the USB camera too, capped at its own
    // process_fps (the mono face model is costly: 30 by default).
    ITrackingSource *face_source = nullptr;
    for (auto &s : sources)
        if (s.name == "face")
            face_source = s.src.get();
    auto usb_mouth_rate = [&] {
        const double rate = setting_at("/sources/mouth/process_fps", 0.0).get<double>();
        const double cap = setting_at("/sources/face/process_fps", 30.0).get<double>();
        return rate > 0 ? std::min(rate, cap) : cap;
    };
    if (face_source)
        face_source->set_process_fps(usb_mouth_rate());

    // The USB camera source, and the camera list the debug overlay shows
    // (read at start and whenever the overlay asks for a refresh).
    BabbleUsbSource *usb_source = nullptr;
    for (auto &s : sources)
        if (!usb_source)
            usb_source = dynamic_cast<BabbleUsbSource *>(s.src.get());
    std::vector<ui::UsbCamera> usb_cameras;
    auto refresh_usb_cameras = [&] {
        usb_cameras.clear();
        for (const auto &f : usb::list_uvc()) {
            char vp[16];
            snprintf(vp, sizeof(vp), "%04x:%04x", f.vendor, f.product);
            std::string detail = vp;
            if (!f.serial.empty())
                detail += " \u00b7 serial " + f.serial;
            usb_cameras.push_back({usb::device_id(f), usb::device_label(f), detail});
        }
    };
    refresh_usb_cameras();

    // The "Babble" tab in the SteamVR dashboard: a debug overlay, shown while
    // dashboard.enabled is on (the Debug Overlay toggle in SteamVR Settings).
    // The object lives for the whole run so its GPU objects outlive the
    // SteamVR connection; the setting only starts and stops the tab.
    const json dcfg = cfg.value("dashboard", json::object());
    auto dashboard = std::make_unique<ui::Dashboard>();
    auto set_dashboard = [&](bool on) {
        if (on == dashboard->started())
            return;
        if (on) {
            ui::Dashboard::Options o;
            o.fonts_dir = paths::resource_dir() / "fonts";
            o.icons_dir = paths::resource_dir() / "icons";
            o.width_m = dcfg.value("width_m", o.width_m);
            o.state = &state;
            dashboard->start(steamvr, thread_spec(threads, "ui", "bbl-ui"), o, dashboard_choices(cfg));
        } else {
            dashboard->stop();
        }
        LOG.info("dashboard: debug overlay %s", on ? "on" : "off");
    };
    set_dashboard(dcfg.value("enabled", false));
    // The tracking visualizer the debug overlay can start.
    AppLauncher visualizer(cfg.value("visualizer", json::object()),
                           (paths::state_dir() / "visualizer.log").string());
    const std::filesystem::path settings_file = default_settings_file();

    // The driver's "Expression Tracking" section in SteamVR Settings.
    SteamVrSettingsBridge steamvr_settings(steamvr, section(cfg, "steamvr_settings").value("section", "driver_babble"),
                                           SteamVrSettingsBridge::entries_from_config(cfg));

    // One path for every live setting change: the Babble tab, SteamVR
    // Settings, or a config reload (SIGHUP; not saved, the files already
    // have it). Applies it, keeps cfg current, saves panel/SteamVR changes to
    // settings.json, and shows the new value in the other places.
    enum class Origin { Dashboard, SteamVr, Reload };
    auto origin_name = [](Origin o) {
        return o == Origin::Dashboard ? "the Babble tab" : o == Origin::SteamVr ? "SteamVR Settings" : "config reload";
    };
    const std::set<std::string> unsaved = unsaved_settings(cfg);
    auto apply_setting_unchecked = [&](const std::string &id, const json &value, Origin origin) {
        const json::json_pointer ptr(id);
        if (cfg.contains(ptr) && cfg[ptr] == value)
            return;
        LOG.debug("settings: %s = %s from %s (was %s)", id.c_str(), value.dump().c_str(), origin_name(origin),
                  cfg.contains(ptr) ? cfg[ptr].dump().c_str() : "unset");
        bool applied = false;
        if (id == "/sources/face/tracking") {
            // The USB camera follows Mouth Tracking and Mouth Source; switching it
            // on by itself would run two mouth trackers at once.
            LOG.warn("settings: ignoring %s: set /sources/mouth/tracking and /mouth_source instead", id.c_str());
            return;
        }
        const bool mouth_routing = (id == "/sources/mouth/tracking" && value.is_boolean()) ||
                                   (id == "/mouth_source" && (value == "frunk" || value == "usb")) ||
                                   (id == "/sources/mouth/process_fps" && value.is_number() && value.get<double>() > 0);
        if (mouth_routing) {
            cfg[ptr] = value;
            if (id == "/sources/mouth/process_fps") {
                for (auto &s : sources)
                    if (s.name == "mouth")
                        s.src->set_process_fps(value.get<double>());
                if (face_source)
                    face_source->set_process_fps(usb_mouth_rate());
            } else {
                route_mouth();
            }
            applied = true;
        }
        for (auto &s : sources) {
            if (mouth_routing)
                break;
            if (id == "/sources/" + s.name + "/process_fps" && value.is_number() && value.get<double>() > 0) {
                s.src->set_process_fps(value.get<double>());
                applied = true;
            } else if (id == "/sources/" + s.name + "/tracking" && value.is_boolean()) {
                set_feed_tracking(s.name, value.get<bool>());
                applied = true;
            } else if (id.starts_with("/sources/" + s.name + "/") &&
                       s.src->set_option(id.substr(s.name.size() + 10), value)) {
                applied = true;   // a module's own option (the USB camera choice)
            }
        }
        for (auto &p : pipelines) {
            const std::string prefix = "/pipelines/" + p->name() + "/";
            if (id.starts_with(prefix) && p->set_option(id.substr(prefix.size()), value))
                applied = true;
        }
        for (auto &k : sinks) {
            const std::string prefix = "/sinks/" + k.name + "/";
            if (id.starts_with(prefix) && k.sink->set_option(id.substr(prefix.size()), value))
                applied = true;
        }
        if (id == "/output" && (value == "vrcft" || value == "vrc_native")) {
            cfg[ptr] = value;
            route_output();
            LOG.info("output: %s", value == "vrc_native" ? "VRChat native" : "VRCFaceTracking");
            applied = true;
        }
        if (id == "/dashboard/enabled" && value.is_boolean()) {
            set_dashboard(value.get<bool>());
            applied = true;
        }
        if (!applied) {
            LOG.warn("settings: ignoring %s = %s", id.c_str(), value.dump().c_str());
            // SteamVR already shows the rejected value: put ours back.
            if (origin == Origin::SteamVr && cfg.contains(ptr))
                steamvr_settings.push(id, cfg[ptr]);
            return;
        }
        cfg[ptr] = value;
        if (origin == Origin::Reload)
            LOG.info("settings: %s = %s (config reload)", id.c_str(), value.dump().c_str());
        if (origin != Origin::Reload && unsaved.count(id)) {
            LOG.info("settings: %s = %s from %s (this run only)", id.c_str(), value.dump().c_str(), origin_name(origin));
        } else if (origin != Origin::Reload) {
            std::string err;
            if (save_setting(settings_file, ptr, value, &err))
                LOG.info("settings: %s = %s from %s (saved to %s)", id.c_str(), value.dump().c_str(),
                         origin == Origin::Dashboard ? "the Babble tab" : "SteamVR Settings", settings_file.c_str());
            else
                LOG.error("settings: saving %s failed: %s", id.c_str(), err.c_str());
        }
        if (dashboard->started() && (value.is_number() || value.is_boolean()))
            dashboard->set_choice(id, value.is_boolean() ? double(value.get<bool>()) : value.get<double>());   // feed switches follow the source's state
        if (origin != Origin::SteamVr)
            steamvr_settings.push(id, value);
    };
    // A malformed id or a value of the wrong type is logged, never fatal.
    auto apply_setting = [&](const std::string &id, const json &value, Origin origin) {
        try {
            apply_setting_unchecked(id, value, origin);
        } catch (const json::exception &e) {
            LOG.warn("settings: ignoring %s = %s: %s", id.c_str(), value.dump().c_str(), e.what());
        }
    };

    // Tracking rate per source: frames its pipelines processed per second.
    std::vector<uint64_t> processed_last(pipelines.size(), 0);
    for (size_t i = 0; i < pipelines.size(); i++)
        processed_last[i] = pipelines[i]->processed();
    std::map<std::string, double> source_hz;
    uint64_t rates_at = mono_ns();
    auto feed_status = [&](const std::string &name) {
        ui::FeedStatus f;
        auto it = std::find_if(sources.begin(), sources.end(), [&](const SourceEntry &e) { return e.name == name; });
        if (it == sources.end())
            return f;   // not configured or disabled: Off, no switch
        f.available = true;
        f.tracking = it->src->tracking();
        switch (it->src->activity()) {
        case SourceActivity::Off: f.health = ui::Health::Off; break;
        case SourceActivity::Starting: f.health = ui::Health::Starting; break;
        case SourceActivity::Paused: f.health = ui::Health::Paused; break;
        case SourceActivity::Fault:
            f.health = ui::Health::Fault;
            f.detail = it->src->last_error();
            break;
        case SourceActivity::Streaming: {
            // Streaming but nothing processed yet (or no pipeline reads it): still starting.
            auto hz = source_hz.find(name);
            f.hz = hz == source_hz.end() ? 0 : hz->second;
            f.health = f.hz > 0.5 ? ui::Health::Working : ui::Health::Starting;
            break;
        }
        }
        return f;
    };

    const auto poll_cfg = section(cfg, "arbiter").value("poll_interval_ms", json::object());
    const uint64_t arbiter_idle_ns = uint64_t(poll_cfg.value("idle", 1000)) * 1000000ull;
    const uint64_t arbiter_streaming_ns = uint64_t(poll_cfg.value("streaming", 5000)) * 1000000ull;
    // Diagnostics file (SIGUSR1, the Babble tab): everything needed to look
    // into a problem report without the headset.
    std::string last_stats = "{}";   // the latest stats render (JSON)
    std::string last_diagnostics;    // the last diagnostics file written
    auto diagnostics_text = [&] {
        std::ostringstream o;
        const time_t wall = time(nullptr);
        char when[64];
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S %Z", localtime(&wall));
        o << "Babble Frame Runtime diagnostics, " << when << "\n\n== session\n";
        for (const auto &line : diag::session_info(cfg, paths::resource_dir() / "models"))
            o << line << "\n";
        o << "\n== status\narbiter " << to_string(arbiter.state()) << ", headset " << to_string(steamvr.worn())
          << ", steamvr " << (steamvr.connected() ? "connected" : "not connected") << ", mouth source "
          << (mouth_from_usb() ? "usb" : "frunk") << "\n";
        for (auto &src : sources)
            o << "source " << src.name << ": " << src.src->state_name() << (src.src->tracking() ? "" : " (tracking off)")
              << (src.src->last_error().empty() ? "" : ", last error: " + src.src->last_error()) << "\n";
        for (auto &p : pipelines)
            o << "pipeline " << p->name() << ": source " << p->source() << ", " << p->processed() << " frames\n";
        for (auto &k : sinks)
            o << "output " << k.name << "\n";
        if (pc_link) {
            const auto ls = pc_link->status();
            o << "pc link: " << (ls.linked ? "linked to \"" + ls.module + "\" via " + ls.path + " (" + ls.address + ")"
                                           : std::string("searching"))
              << (ls.rtt_ms >= 0 ? ", rtt " + std::to_string(ls.rtt_ms) + " ms" : "")
              << (ls.data == LinkSink::Data::Arriving ? ", data arriving"
                  : ls.data == LinkSink::Data::Missing ? ", DATA NOT ARRIVING" : "")
              << "\n";
        }
        for (const auto &c : usb_cameras)
            o << "usb camera: " << c.label << " (" << c.id << ")"
              << (usb_source && usb_source->active_camera() == c.id ? " streaming" : "") << "\n";
        o << "\n== signals (value, age)\n";
        const auto snap = state.snapshot();
        const uint64_t now_ns = mono_ns();
        for (size_t i = 0; i < snap.names.size(); i++) {
            const auto &sig = snap.signals[i];
            char line[160];
            snprintf(line, sizeof(line), "%-24s %8.4f  %s\n", snap.names[i].c_str(), sig.value,
                     sig.t_ns ? (std::to_string((now_ns - sig.t_ns) / 1000000) + " ms").c_str() : "never set");
            o << line;
        }
        o << "\n== threads\n";
        for (const auto &line : diag::thread_report())
            o << line << "\n";
        o << "\n== stats (last interval)\n" << last_stats << "\n\n== effective config\n" << cfg.dump(2)
          << "\n\n== recent log\n";
        for (const auto &line : log::recent(300))
            o << line;
        return o.str();
    };
    auto write_diagnostics = [&](const char *why) {
        const auto path = diag::write_file(paths::state_dir(), diagnostics_text());
        if (path.empty())
            LOG.error("diagnostics: cannot write to %s", paths::state_dir().c_str());
        else
            LOG.info("diagnostics written to %s (%s)", path.c_str(), why);
        return path;
    };

    // Stats: the full set to stats.jsonl every interval; in the log, the status
    // line when it changes and a short summary every summary_s.
    log::RotatingFile stats_file;
    {
        const json sf = section(cfg, "log_file");
        stats_file.open(paths::state_dir() / "stats.jsonl", size_t(sf.value("stats_max_mb", 4.0) * 1024 * 1024),
                        sf.value("keep", 3));
    }
    const uint64_t summary_ns = uint64_t(std::max(5, cfg.value("stats_summary_s", 60))) * 1000000000ull;
    uint64_t next_summary = mono_ns() + summary_ns;
    std::string last_status;
    const uint64_t stats_ns = uint64_t(std::max(1, cfg.value("stats_interval_s", 5))) * 1000000000ull;
    uint64_t next_arbiter = 0, next_stats = mono_ns() + stats_ns;
    bool running = true;
    const char *exit_reason = "?";
    Worn last_worn = Worn::Unknown;

    while (running) {
        epoll_event evs[4];
        int n = epoll_wait(ep, evs, 4, 1000);
        if (n < 0 && errno != EINTR) {
            LOG.error("epoll_wait: %s", strerror(errno));
            break;
        }
        for (int i = 0; i < n; i++) {
            int fd = evs[i].data.fd;
            if (fd == sfd) {
                signalfd_siginfo si{};
                while (read(sfd, &si, sizeof(si)) == sizeof(si)) {
                    if (si.ssi_signo == SIGUSR1) {
                        write_diagnostics("SIGUSR1");
                    } else if (si.ssi_signo == SIGHUP) {
                        try {
                            json fresh = load(opt);
                            apply_log_level(fresh);
                            for (auto &s : sources)
                                for (const char *key : {"process_fps", "tracking"}) {
                                    const json::json_pointer ptr("/sources/" + s.name + "/" + key);
                                    if (fresh.contains(ptr))
                                        apply_setting(ptr.to_string(), fresh[ptr], Origin::Reload);
                                }
                            for (const char *id :
                                 {"/pipelines/eye/openness_source", "/pipelines/gaze/gaze_source", "/dashboard/enabled",
                                  "/mouth_source"}) {
                                const json::json_pointer ptr(id);
                                if (fresh.contains(ptr))
                                    apply_setting(id, fresh[ptr], Origin::Reload);
                            }
                        } catch (const json::exception &e) {
                            LOG.error("config: reload failed: %s", e.what());
                        }
                    } else {
                        exit_reason = si.ssi_signo == SIGTERM ? "SIGTERM" : "SIGINT";
                        running = false;
                    }
                }
            } else if (fd == tfd) {
                uint64_t ticks;
                while (read(tfd, &ticks, sizeof(ticks)) > 0) {
                }
            } else if (fd == ShimLink::kFd) {
                if (!ShimLink::drain()) {
                    exit_reason = "shim link closed (vrserver exited)";
                    running = false;
                }
            }
        }
        if (!running)
            break;

        uint64_t now = mono_ns();
        bool any_streaming = false;
        for (auto &s : sources)
            any_streaming = any_streaming || s.src->streaming();
        if (now >= next_arbiter) {
            arbiter.poll();
            next_arbiter = now + (any_streaming ? arbiter_streaming_ns : arbiter_idle_ns);
        }
        // Headset put on / taken off: pipelines publish neutral values and
        // reset per-session state; sources pause on their own tick.
        if (Worn w = steamvr.worn(); w != Worn::Unknown && w != last_worn) {
            if (last_worn != Worn::Unknown || w == Worn::No)
                for (auto &p : pipelines)
                    p->on_presence(w == Worn::Yes);
            last_worn = w;
        }
        for (auto &s : sources) {
            bool was = s.src->streaming();
            s.src->tick(now);
            // Every state change, one uniform line (with the reason when there is one).
            if (const char *st_now = s.src->state_name(); s.last_state != st_now) {
                LOG.info("source %s: %s -> %s%s", s.name.c_str(), s.last_state.empty() ? "(start)" : s.last_state.c_str(),
                         st_now,
                         s.src->last_error().empty() ? "" : (" (" + s.src->last_error() + ")").c_str());
                s.last_state = st_now;
            }
            if (was && !s.src->streaming())
                next_arbiter = now;   // capture trouble: re-check the platform right away
        }

        if (now - rates_at >= 1000000000ull) {
            std::map<std::string, double> hz;
            for (size_t i = 0; i < pipelines.size(); i++) {
                const uint64_t n = pipelines[i]->processed();
                const double r = double(n - processed_last[i]) * 1e9 / double(now - rates_at);
                processed_last[i] = n;
                if (const std::string &src = pipelines[i]->source(); !src.empty())
                    hz[src] = std::max(hz[src], r);
            }
            source_hz = std::move(hz);
            rates_at = now;
        }
        for (auto &[id, value] : steamvr_settings.poll(cfg, now))
            apply_setting(id, value, Origin::SteamVr);
        if (dashboard->started()) {
            for (auto &[id, value] : dashboard->take_changes())
                apply_setting(id, value, Origin::Dashboard);
            if (dashboard->take_camera_refresh_request())
                refresh_usb_cameras();
            if (const int v = dashboard->take_verbose_request(); v >= 0) {
                log::verbose_until(v ? mono_ns() + 10ull * 60 * 1000000000ull : 0);
                LOG.info("verbose logging %s from the Babble tab", v ? "on for 10 minutes" : "off");
            }
            if (dashboard->take_diagnostics_request())
                last_diagnostics = write_diagnostics("the Babble tab").string();
            if (dashboard->take_restart_request()) {
                // A clean exit: the driver shim starts the runtime again.
                if (!shim)
                    LOG.warn("restart requested, but no driver shim is supervising this runtime: it will just exit");
                exit_reason = "restart requested from the Babble tab";
                running = false;
            }
            if (dashboard->take_visualizer_request()) {
                if (visualizer.running())
                    visualizer.stop();
                else
                    visualizer.start();
            }
            ui::Status st;
            st.steamvr = steamvr.connected();
            st.worn = steamvr.worn();
            st.mouth = feed_status(mouth_from_usb() ? "face" : ui::kFeedSources[0]);
            st.eye = feed_status(ui::kFeedSources[1]);
            st.visualizer = !visualizer.available() ? ui::AppState::Unavailable
                            : visualizer.running()  ? ui::AppState::Running
                                                    : ui::AppState::Stopped;
            st.mouth_source = mouth_from_usb() ? "usb" : "frunk";
            if (const uint64_t until = log::verbose_until(); until > now)
                st.verbose_minutes_left = int((until - now) / 60000000000ull) + 1;
            st.last_diagnostics = last_diagnostics;
            st.log_file = log::file_path().string();
            st.stats_file = stats_file.path().string();
            st.cameras = usb_cameras;
            if (usb_source) {
                st.camera = usb_source->camera();
                st.active_camera = usb_source->active_camera();
            }
            if (pc_link && pc_link_entry->running) {
                const auto ls = pc_link->status();
                st.link = {true, ls.linked, ls.path, ls.module, ls.address, ls.rtt_ms};
                st.link.no_data = ls.data == LinkSink::Data::Missing;
            } else if (vrc_native && vrc_native_entry->running) {
                // Native: where the blob goes; "linked" while blobs are going out.
                const auto ns = vrc_native->status();
                st.link.available = true;
                st.link.native = true;
                st.link.linked = !ns.kind.empty() && ns.sent != native_sent_last;
                st.link.path = ns.kind;
                st.link.address = ns.address;
                native_sent_last = ns.sent;
            }
            dashboard->set_status(st);
        }

        if (const uint64_t until = log::verbose_until(); until && now >= until) {
            log::verbose_until(0);
            LOG.info("verbose logging ended");
        }
        if (now >= next_stats) {
            std::string line = std::string("arbiter=") + to_string(arbiter.state()) +
                               " headset=" + to_string(steamvr.worn());
            for (auto &s : sources)
                line += " " + s.name + "=" + s.src->state_name();
            last_stats = stats.render();
            {
                const time_t wall = time(nullptr);
                char when[32];
                strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%S", localtime(&wall));
                stats_file.write(std::string("{\"time\":\"") + when + "\",\"status\":\"" + line + "\",\"stats\":" +
                                 last_stats + "}\n");
            }
            const bool summary = now >= next_summary;
            if (line != last_status || summary)
                LOG.info("status: %s", line.c_str());
            last_status = line;
            if (summary) {
                LOG.info("summary: %s", stats_summary(last_stats).c_str());
                next_summary = now + summary_ns;
            }
            next_stats = now + stats_ns;
        }
    }

    LOG.info("shutting down: %s", exit_reason);
    // Shutdown has a few seconds: if something hangs (SteamVR's VR_Shutdown has
    // been seen to), exit anyway so the shim can start a fresh runtime.
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        LOG.error("shutdown is stuck after 5 s; exiting now. Threads:");
        for (const auto &line : diag::thread_report())
            LOG.error("  %s", line.c_str());
        _exit(1);
    }).detach();
    dashboard->stop();
    // Order: consumers first so nothing holds frames when sources stop.
    for (auto &p : pipelines)
        p->stop();
    for (auto &k : sinks)
        k.sink->stop();
    for (auto &s : sources)
        s.src->shutdown();
    sources.clear();
    pipelines.clear();
    eye_server.stop();
    steamvr.stop();
    dashboard->release_gpu();   // OpenVR wants its Vulkan objects to outlive the connection
    LOG.info("stopped");
    return 0;
}
