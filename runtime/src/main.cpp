// babble-runtime: Babble mouth + eye tracking runtime for the Steam Frame.
//
// Normally spawned by the SteamVR driver shim (driver/src/driver_babble.cpp)
// with a pipe on fd 3; EOF there or SIGTERM means shut down. --standalone
// runs without the shim for development over SSH.

#include "babble_version.h"
#include "camera/tracking_source.h"
#include "core/clock.h"
#include "core/config.h"
#include "core/lifecycle.h"
#include "core/log.h"
#include "core/metrics.h"
#include "core/paths.h"
#include "core/threading.h"
#include "model/executorch_runner.h"
#include "model/pipeline.h"
#include "platform/app_launcher.h"
#include "platform/camera_arbiter_client.h"
#include "platform/eye_server_client.h"
#include "platform/steamvr_client.h"
#include "platform/steamvr_settings.h"
#include "sink/sink.h"
#include "state/expression_state.h"
#include "ui/dashboard.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

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
        LOG_ERROR("config: ignoring %s: %s", cl.user_file.c_str(), cl.error.c_str());
    else if (cl.user_loaded)
        LOG_INFO("config: loaded %s", cl.user_file.c_str());
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

void apply_log_level(const json &c) {
    log::Level lvl;
    std::string name = c.value("log_level", "info");
    if (log::parse_level(name.c_str(), &lvl))
        log::set_level(lvl);
}

// The settings panel's rows (config["dashboard"]["settings"]), with the
// current values from the config. Rows whose setting doesn't exist (e.g. a
// source that isn't configured) are left out.
std::vector<ui::Choice> dashboard_choices(const json &cfg) {
    std::vector<ui::Choice> out;
    for (const json &row : cfg["dashboard"].value("settings", json::array())) {
        ui::Choice c;
        c.id = row.value("id", "");
        json::json_pointer ptr;
        try {
            ptr = json::json_pointer(c.id);
        } catch (const json::exception &) {
            LOG_WARN("dashboard: ignoring setting with a bad id '%s'", c.id.c_str());
            continue;
        }
        if (!cfg.contains(ptr) || !cfg[ptr].is_number())
            continue;
        c.label = row.value("label", c.id);
        c.hint = row.value("hint", "");
        c.unit = row.value("unit", "");
        c.options = row.value("options", std::vector<double>{});
        c.value = cfg[ptr].get<double>();
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

    json cfg = load(opt);
    apply_log_level(cfg);
    const json threads = cfg.value("threads", json::object());
    apply_thread_spec(thread_spec(threads, "main", "babble-runtime"));

    const bool shim = ShimLink::present();
    LOG_INFO("babble-runtime %s starting (pid %d, %s, state dir %s)", BABBLE_VERSION_DISPLAY, getpid(),
             shim ? "supervised by driver shim" : "standalone", paths::state_dir().c_str());
    if (!shim && !opt.standalone)
        LOG_WARN("no shim link on fd 3; running unsupervised (use --standalone to silence this)");

    int sfd = make_signalfd();
    int ep = epoll_create1(EPOLL_CLOEXEC);
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (sfd < 0 || ep < 0 || tfd < 0) {
        LOG_ERROR("event setup failed: %s", strerror(errno));
        return 1;
    }
    itimerspec its{{0, 100000000}, {0, 100000000}};   // 10 Hz housekeeping tick
    timerfd_settime(tfd, 0, &its, nullptr);
    add_epoll(ep, sfd);
    add_epoll(ep, tfd);
    if (shim)
        add_epoll(ep, ShimLink::kFd);

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
    struct SinkEntry {
        std::string name;
        std::unique_ptr<ISink> sink;
    };
    std::vector<SinkEntry> sinks;
    std::vector<ISink *> frame_taps;
    const json kcfg = cfg.value("sinks", json::object());
    for (auto it = kcfg.begin(); it != kcfg.end(); ++it) {
        if (!it.value().value("enabled", true)) {
            LOG_INFO("sink %s: disabled", it.key().c_str());
            continue;
        }
        auto sink = make_sink(it.value(), SinkDeps{it.key(), threads, state, stats});
        if (!sink)
            continue;
        if (sink->wants_frames())
            frame_taps.push_back(sink.get());
        sinks.push_back({it.key(), std::move(sink)});
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
            LOG_INFO("pipeline %s: disabled", it.key().c_str());
            continue;
        }
        auto p = make_pipeline(it.value(),
                               PipelineDeps{it.key(), threads, slot_of, state, stats, frame_taps, models_dir, &steamvr,
                                            &eye_server});
        if (p && p->init())
            pipelines.push_back(std::move(p));
        else
            LOG_ERROR("pipeline %s: not started", it.key().c_str());
    }

    struct SourceEntry {
        std::string name;
        std::unique_ptr<ITrackingSource> src;
    };
    std::vector<SourceEntry> sources;
    for (auto it = scfg.begin(); it != scfg.end(); ++it) {
        if (!it.value().value("enabled", true)) {
            LOG_INFO("source %s: disabled", it.key().c_str());
            continue;
        }
        auto src = make_tracking_source(it.value(), SourceDeps{it.key(), threads, *slots[it.key()], stats, arbiter, &steamvr});
        if (src)
            sources.push_back({it.key(), std::move(src)});
    }

    for (auto it = sinks.begin(); it != sinks.end();) {
        if (it->sink->start()) {
            ++it;
            continue;
        }
        LOG_ERROR("sink %s: failed to start; dropping it", it->name.c_str());
        std::erase(frame_taps, it->sink.get());
        it = sinks.erase(it);
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
        LOG_INFO("dashboard: debug overlay %s", on ? "on" : "off");
    };
    set_dashboard(dcfg.value("enabled", false));
    // The tracking visualizer the debug overlay can start.
    AppLauncher visualizer(cfg.value("visualizer", json::object()),
                           (paths::state_dir() / "visualizer.log").string());
    const std::filesystem::path settings_file = default_settings_file();

    // The driver's "Expression Tracking" section in SteamVR Settings.
    SteamVrSettingsBridge steamvr_settings(steamvr, cfg["steamvr_settings"].value("section", "driver_babble"),
                                           SteamVrSettingsBridge::entries_from_config(cfg));

    // One path for every live setting change: the Babble tab, SteamVR
    // Settings, or a config reload (SIGHUP; not saved, the files already
    // have it). Applies it, keeps cfg current, saves panel/SteamVR changes to
    // settings.json, and shows the new value in the other places.
    enum class Origin { Dashboard, SteamVr, Reload };
    auto apply_setting = [&](const std::string &id, const json &value, Origin origin) {
        const json::json_pointer ptr(id);
        if (cfg.contains(ptr) && cfg[ptr] == value)
            return;
        bool applied = false;
        for (auto &s : sources) {
            if (id == "/sources/" + s.name + "/process_fps" && value.is_number() && value.get<double>() > 0) {
                s.src->set_process_fps(value.get<double>());
                applied = true;
            } else if (id == "/sources/" + s.name + "/tracking" && value.is_boolean()) {
                set_feed_tracking(s.name, value.get<bool>());
                applied = true;
            }
        }
        for (auto &p : pipelines) {
            const std::string prefix = "/pipelines/" + p->name() + "/";
            if (id.starts_with(prefix) && p->set_option(id.substr(prefix.size()), value))
                applied = true;
        }
        if (id == "/dashboard/enabled" && value.is_boolean()) {
            set_dashboard(value.get<bool>());
            applied = true;
        }
        if (!applied) {
            LOG_WARN("settings: ignoring %s = %s", id.c_str(), value.dump().c_str());
            return;
        }
        cfg[ptr] = value;
        if (origin != Origin::Reload) {
            std::string err;
            if (save_setting(settings_file, ptr, value, &err))
                LOG_INFO("settings: %s = %s from %s (saved to %s)", id.c_str(), value.dump().c_str(),
                         origin == Origin::Dashboard ? "the Babble tab" : "SteamVR Settings", settings_file.c_str());
            else
                LOG_ERROR("settings: saving %s failed: %s", id.c_str(), err.c_str());
        }
        if (dashboard->started() && value.is_number())
            dashboard->set_choice(id, value.get<double>());   // feed switches follow the source's state
        if (origin != Origin::SteamVr)
            steamvr_settings.push(id, value);
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

    const auto poll_cfg = cfg["arbiter"].value("poll_interval_ms", json::object());
    const uint64_t arbiter_idle_ns = uint64_t(poll_cfg.value("idle", 1000)) * 1000000ull;
    const uint64_t arbiter_streaming_ns = uint64_t(poll_cfg.value("streaming", 5000)) * 1000000ull;
    const uint64_t stats_ns = uint64_t(std::max(1, cfg.value("stats_interval_s", 5))) * 1000000000ull;
    uint64_t next_arbiter = 0, next_stats = mono_ns() + stats_ns;
    bool running = true;
    const char *exit_reason = "?";
    Worn last_worn = Worn::Unknown;

    while (running) {
        epoll_event evs[4];
        int n = epoll_wait(ep, evs, 4, 1000);
        if (n < 0 && errno != EINTR) {
            LOG_ERROR("epoll_wait: %s", strerror(errno));
            break;
        }
        for (int i = 0; i < n; i++) {
            int fd = evs[i].data.fd;
            if (fd == sfd) {
                signalfd_siginfo si{};
                while (read(sfd, &si, sizeof(si)) == sizeof(si)) {
                    if (si.ssi_signo == SIGHUP) {
                        json fresh = load(opt);
                        apply_log_level(fresh);
                        for (auto &s : sources)
                            for (const char *key : {"process_fps", "tracking"}) {
                                const json::json_pointer ptr("/sources/" + s.name + "/" + key);
                                if (fresh.contains(ptr))
                                    apply_setting(ptr.to_string(), fresh[ptr], Origin::Reload);
                            }
                        for (const char *id :
                             {"/pipelines/eye/openness_source", "/pipelines/gaze/gaze_source", "/dashboard/enabled"}) {
                            const json::json_pointer ptr(id);
                            if (fresh.contains(ptr))
                                apply_setting(id, fresh[ptr], Origin::Reload);
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
            if (dashboard->take_visualizer_request()) {
                if (visualizer.running())
                    visualizer.stop();
                else
                    visualizer.start();
            }
            ui::Status st;
            st.steamvr = steamvr.connected();
            st.worn = steamvr.worn();
            st.mouth = feed_status(ui::kFeedSources[0]);
            st.eye = feed_status(ui::kFeedSources[1]);
            st.visualizer = !visualizer.available() ? ui::AppState::Unavailable
                            : visualizer.running()  ? ui::AppState::Running
                                                    : ui::AppState::Stopped;
            dashboard->set_status(st);
        }

        if (now >= next_stats) {
            std::string line = std::string("arbiter=") + to_string(arbiter.state()) +
                               " headset=" + to_string(steamvr.worn());
            for (auto &s : sources)
                line += " " + s.name + "=" + s.src->state_name();
            LOG_INFO("status: %s", line.c_str());
            LOG_INFO("stats: %s", stats.render().c_str());
            next_stats = now + stats_ns;
        }
    }

    LOG_INFO("shutting down: %s", exit_reason);
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
    LOG_INFO("stopped");
    return 0;
}
