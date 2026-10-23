// The Babble mouth and eye tracking runtime for the Steam Frame, which stops at EOF on fd 3 or at SIGTERM.

#include "babble_version.h"
#include "camera/tracking_source.h"
#include "camera/usb/babble_usb_source.h"
#include "capture/app_installer.h"
#include "update/runtime_updater.h"
#include "capture/capture_server.h"
#include "capture/uploader.h"
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
#if BABBLE_FRUNK_MOUTH
#include "platform/camera_arbiter_client.h"
#endif
#include "platform/eye_server_client.h"
#include "platform/steamvr_client.h"
#include "platform/steamvr_settings.h"
#include "sink/babble_osc_sink.h"
#include "sink/link_sink.h"
#include "sink/record_sink.h"
#include "sink/vrc_native_sink.h"
#include "sink/sink.h"
#include "sink/steam_link.h"
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
}

using namespace babble;

namespace {

// Polls the camera arbiter, which exists only in builds that have the Frame mouth cameras.
struct ArbiterPoll {
#if BABBLE_FRUNK_MOUTH
    CameraArbiterClient client;
    uint64_t idle_ns, streaming_ns, next = 0;
    explicit ArbiterPoll(const json &cfg) : client(cfg.value("arbiter", json::object())) {
        const auto p = section(cfg, "arbiter").value("poll_interval_ms", json::object());
        idle_ns = uint64_t(p.value("idle", 1000)) * 1000000ull;
        streaming_ns = uint64_t(p.value("streaming", 5000)) * 1000000ull;
    }
    const CameraArbiterClient *ptr() const { return &client; }
    void tick(uint64_t now, bool any_streaming) {
        if (now < next)
            return;
        client.poll();
        next = now + (any_streaming ? streaming_ns : idle_ns);
    }
    void recheck(uint64_t now) { next = now; }   // Poll the arbiter again immediately after a capture problem.
    std::string state() const { return to_string(client.state()); }
#else
    explicit ArbiterPoll(const json &) {}
    const CameraArbiterClient *ptr() const { return nullptr; }
    void tick(uint64_t, bool) {}
    void recheck(uint64_t) {}
    std::string state() const { return {}; }   // This build has no arbiter.
#endif
};

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
    if (!cl.legacy_output.empty())
        LOG.info("config: the saved output \"%s\" is now the switch of that output, and the other outputs are off",
                 cl.legacy_output.c_str());
    json &c = cl.config;
    if (opt.no_eye && c["sources"].contains("eye")) c["sources"]["eye"]["enabled"] = false;
    // The --no-mouth option turns off the Frame mouth cameras and the USB mouth camera.
    for (const char *m : {"mouth", "face"})
        if (opt.no_mouth && c["sources"].contains(m)) c["sources"][m]["enabled"] = false;
    // The --dump-frames option uses the "debug" output module and adds it if necessary.
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

// Applies the default log_level and the log_levels for each module.
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

// Makes one log line from the stats JSON with the pipeline rates, inference times and problem counters.
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

// Opens the log file, which is different for standalone runs so that the two logs stay separate.
void open_log_file(const json &c, bool supervised) {
    const json f = section(c, "log_file");
    const double max_mb = f.value("max_mb", 8.0);
    const int keep = f.value("keep", 3);
    const auto path = paths::state_dir() / (supervised ? "runtime.log" : "runtime-standalone.log");
    if (!log::open_file(path, size_t(std::max(0.5, max_mb) * 1024 * 1024), keep))
        LOG.warn("cannot write %s: %s; logging to stderr only", path.c_str(), strerror(errno));
}

// Returns the settings with "persist": false, which the runtime never writes to settings.json.
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
        const std::string page = row.value("page", "settings");
        c.page = page == "eye"           ? ui::ChoicePage::Eye
                 : page == "mouth"       ? ui::ChoicePage::Mouth
                 : page == "cameras"     ? ui::ChoicePage::Cameras
                 : page == "diagnostics" ? ui::ChoicePage::Diagnostics
                                         : ui::ChoicePage::Settings;
        c.options = c.boolean ? std::vector<double>{0, 1} : row.value("options", std::vector<double>{});
        c.option_labels = row.value("labels", std::vector<std::string>{});
        c.value = c.boolean ? double(cfg[ptr].get<bool>()) : cfg[ptr].get<double>();
        out.push_back(std::move(c));
    }
    return out;
}

// Returns the declared option of an output module for a setting key, or null if the module has no such option.
const json *module_option(const json &sink_cfg, const std::string &key) {
    // Search the config itself, because a pointer into a copy of the options is not valid after the return.
    const json &mod = section(sink_cfg, "module");
    const auto options = mod.find("options");
    if (options == mod.end() || !options->is_array())
        return nullptr;
    for (const json &o : *options)
        if (o.is_object() && o.value("key", "") == key)
            return &o;
    return nullptr;
}

// Returns why a value does not suit an option of an output module, or "" if it does.
std::string module_option_error(const json &opt, const json &value) {
    const std::string kind = opt.value("kind", "choice"), validate = opt.value("validate", "");
    if (kind == "choice") {
        for (const json &v : opt.value("values", json::array()))
            if (v == value)
                return "";
        return "not one of the choices";
    }
    if (opt.value("numeric", false) && !value.is_number_integer())
        return "not a number";
    if (!opt.value("numeric", false) && !value.is_string())
        return "not text";
    if (validate == "port" && (value.get<int64_t>() < 1 || value.get<int64_t>() > 65535))
        return "not a port from 1 to 65535";
    if (validate == "ipv4" && !valid_pc_address(value.get<std::string>()))
        return "not an IPv4 address";
    return "";
}

int add_epoll(int ep, int fd) {
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    return epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
}

}

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
    LOG.info("babble-runtime %s (%s profile) starting (pid %d, %s, state dir %s)", BABBLE_VERSION_DISPLAY,
             BABBLE_PROFILE_NAME, getpid(), shim ? "supervised by driver shim" : "standalone",
             paths::state_dir().c_str());
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
    itimerspec its{{0, 100000000}, {0, 100000000}};   // The housekeeping timer runs at 10 Hz.
    if (timerfd_settime(tfd, 0, &its, nullptr) != 0 || add_epoll(ep, sfd) != 0 || add_epoll(ep, tfd) != 0 ||
        (shim && add_epoll(ep, ShimLink::kFd) != 0)) {
        LOG.error("event setup failed: %s", strerror(errno));
        return 1;
    }

    StatsRegistry stats;
    ExpressionState state;
    ArbiterPoll arbiter(cfg);
    // This is the only SteamVR connection of the runtime.
    SteamVrClient steamvr;
    {
        const json vc = cfg.value("steamvr", json::object());
        SteamVrClient::Options o;
        o.manifest_dir = (paths::state_dir() / "steamvr_input").string();
        o.controller_type = vc.value("controller_type", o.controller_type);
        o.proximity_path = vc.value("proximity_path", o.proximity_path);
        o.eyetracking_path = vc.value("eyetracking_path", o.eyetracking_path);
        o.absent_after_s = vc.value("absent_after_s", o.absent_after_s);
        steamvr.start(thread_spec(threads, "steamvr", "bbl-steamvr"), o);
    }
    // The client for the SteamVR eye-service shared memory reads only while a pipeline asks for it.
    EyeServerClient eye_server;
    {
        const json ec = cfg.value("eye_server", json::object());
        EyeServerClient::Options o;
        o.path = ec.value("path", o.path);
        eye_server.start(thread_spec(threads, ec.value("thread", "eyesrv"), "bbl-eyesrv"), o, &stats);
    }

    // Each configured source has one output slot, and pipelines find their slot by name.
    const json scfg = cfg.value("sources", json::object());
    std::map<std::string, std::unique_ptr<LatestSlot<StereoFrame>>> slots;
    for (auto it = scfg.begin(); it != scfg.end(); ++it)
        slots[it.key()] = std::make_unique<LatestSlot<StereoFrame>>();

    // A sink with an "output" role is an output module, which runs while its switch in "outputs" is on.
    struct SinkEntry {
        std::string name;
        std::unique_ptr<ISink> sink;
        std::string output{};
        bool running = false;
    };
    std::vector<SinkEntry> sinks;
    std::vector<ISink *> frame_taps;
    // The config of a sink, with the shared PC address for the output modules that use it.
    auto sink_config = [&](const std::string &name) {
        json c = cfg["sinks"][name];
        if (section(c, "module").value("uses_pc_address", false))
            c["pc_address"] = cfg.value("pc_address", "");
        return c;
    };
    auto sink_deps = [&](const std::string &name) {
        return SinkDeps{name, threads, state, stats, BABBLE_VERSION_DISPLAY, BABBLE_PROFILE_NAME};
    };
    const json kcfg = cfg.value("sinks", json::object());
    for (auto it = kcfg.begin(); it != kcfg.end(); ++it) {
        if (!it.value().value("enabled", true)) {
            LOG.info("sink %s: disabled", it.key().c_str());
            continue;
        }
        if (!it.value().contains("type")) {
            // Ignore saved settings for an output that this build does not have.
            LOG.info("sink %s: not part of this build; ignoring its saved settings", it.key().c_str());
            continue;
        }
        auto sink = make_sink(sink_config(it.key()), sink_deps(it.key()));
        if (!sink)
            continue;
        if (sink->wants_frames())
            frame_taps.push_back(sink.get());
        sinks.push_back({it.key(), std::move(sink), it.value().value("output", "")});
    }

    // Use one XNNPACK thread because its threadpool ignores thread pinning and each model has its own pinned thread.
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
        std::string last_state{};   // The last state, which the log uses to show state changes.
    };
    std::vector<SourceEntry> sources;
    for (auto it = scfg.begin(); it != scfg.end(); ++it) {
        if (!it.value().value("enabled", true)) {
            LOG.info("source %s: disabled", it.key().c_str());
            continue;
        }
        if (!it.value().contains("type")) {
            // Ignore saved settings for a source that this build does not have.
            LOG.info("source %s: not part of this build; ignoring its saved settings", it.key().c_str());
            continue;
        }
        auto src = make_tracking_source(it.value(),
                                        SourceDeps{it.key(), threads, *slots[it.key()], stats, arbiter.ptr(), &steamvr});
        if (src)
            sources.push_back({it.key(), std::move(src)});
    }

    // Each output module has an on/off switch, and any number of them can run at the same time.
    auto output_on = [&](const std::string &id) {
        const json &o = section(cfg, "outputs");
        return o.contains(id) && o[id].is_boolean() && o[id].get<bool>();
    };
    auto output_label = [&](const SinkEntry &k) {
        return section(section(cfg, "sinks"), k.name.c_str()).value("module", json::object()).value("label", k.name);
    };
    for (auto &[id, on] : section(cfg, "outputs").items())
        if (on == true && std::none_of(sinks.begin(), sinks.end(), [&](const SinkEntry &k) { return k.output == id; }))
            LOG.warn("output: %s is on, but this build has no module for it", id.c_str());
    for (auto it = sinks.begin(); it != sinks.end();) {
        if (!it->output.empty() && !output_on(it->output)) {
            ++it;   // This sink starts when the user turns its output on.
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
    {
        std::string on;
        for (const auto &k : sinks)
            if (!k.output.empty() && k.running)
                on += (on.empty() ? "" : ", ") + output_label(k);
        LOG.info("outputs: %s", on.empty() ? "none" : on.c_str());
    }
    auto route_outputs = [&] {
        for (auto &k : sinks) {
            if (k.output.empty())
                continue;
            const bool want = output_on(k.output);
            if (want && !k.running) {
                k.running = k.sink->start();
                if (k.running)
                    LOG.info("output: %s on", output_label(k).c_str());
                else
                    LOG.error("sink %s: failed to start", k.name.c_str());
            } else if (!want && k.running) {
                k.sink->stop();
                k.running = false;
                LOG.info("output: %s off", output_label(k).c_str());
            }
        }
    };
    // The diagnostics report has the status of the first PC link.
    LinkSink *pc_link = nullptr;
    std::map<std::string, uint64_t> sent_last;   // The number of messages of each output at the last status refresh.
    auto find_status_sinks = [&] {
        pc_link = nullptr;
        for (auto &k : sinks)
            if (!pc_link)
                pc_link = dynamic_cast<LinkSink *>(k.sink.get());
    };
    find_status_sinks();
    for (auto &p : pipelines)
        p->start();

    // The PC links tell the module which tracking is on, so that the module claims only that tracking.
    auto update_link_features = [&] {
        auto on = [&](std::initializer_list<const char *> feeds) {
            return std::any_of(sources.begin(), sources.end(), [&](const auto &s) {
                return s.src->tracking() && std::find(feeds.begin(), feeds.end(), s.name) != feeds.end();
            });
        };
        const bool eye = on({"eye"}), face = on({"mouth", "face"});
        for (auto &k : sinks)
            if (auto *l = dynamic_cast<LinkSink *>(k.sink.get()))
                l->set_features(eye, face);
    };
    // When the user turns a feed off, its source stops the cameras and its pipelines send neutral values.
    auto set_feed_tracking = [&](const std::string &feed, bool on) {
        for (auto &s : sources)
            if (s.name == feed)
                s.src->set_tracking(on);
        for (auto &p : pipelines)
            if (p->feed() == feed)
                p->set_tracking(on);
        update_link_features();
    };
    for (auto &s : sources)
        if (!s.src->tracking())
            set_feed_tracking(s.name, false);
    update_link_features();

    // Makes an output module again from its config, so that a change to one of its settings takes effect.
    auto rebuild_sink = [&](SinkEntry &k) {
        const bool was_running = k.running;
        if (k.running)
            k.sink->stop();
        k.running = false;
        k.sink.reset();   // The old module removes its stats before the new one adds them.
        // This is never null because make_sink() made a module of the same type at startup.
        k.sink = make_sink(sink_config(k.name), sink_deps(k.name));
        find_status_sinks();
        update_link_features();
        if (was_running && !(k.running = k.sink->start()))
            LOG.error("sink %s: failed to start", k.name.c_str());
        LOG.info("output: %s restarted with its new settings", output_label(k).c_str());
    };

#if BABBLE_FRUNK_MOUTH
    // The mouth_source setting selects the Frame mouth cameras or a USB camera, and the other source stays off.
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
    // The Mouth tracking rate also applies to the USB camera, with its own process_fps as the maximum.
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
    // These values show the mouth feed on the overlay.
    auto mouth_feed = [&] { return mouth_from_usb() ? "face" : ui::kFeedSources[0]; };
    auto mouth_label = [&] { return mouth_from_usb() ? "usb" : "frunk"; };
    const std::string mouth_tracking_id = "/sources/mouth/tracking";
    // Returns nullopt for other settings, false if it rejects the setting, or true if it applies the setting.
    auto apply_mouth_setting = [&](const std::string &id, const json &value) -> std::optional<bool> {
        if (id == "/sources/face/tracking") {
            // Reject this setting because it can cause two mouth trackers to run at the same time.
            LOG.warn("settings: ignoring %s: set /sources/mouth/tracking and /mouth_source instead", id.c_str());
            return false;
        }
        const bool routing = (id == "/sources/mouth/tracking" && value.is_boolean()) ||
                             (id == "/mouth_source" && (value == "frunk" || value == "usb")) ||
                             (id == "/sources/mouth/process_fps" && value.is_number() && value.get<double>() > 0);
        if (!routing)
            return std::nullopt;
        cfg[json::json_pointer(id)] = value;
        if (id == "/sources/mouth/process_fps") {
            for (auto &s : sources)
                if (s.name == "mouth")
                    s.src->set_process_fps(value.get<double>());
            if (face_source)
                face_source->set_process_fps(usb_mouth_rate());
        } else {
            route_mouth();
        }
        return true;
    };
#else
    // In this build, the USB Babble tracker is the mouth and uses ordinary source settings.
    auto mouth_feed = [] { return "face"; };
    auto mouth_label = [] { return "usb"; };
    const std::string mouth_tracking_id = "/sources/face/tracking";
    auto apply_mouth_setting = [](const std::string &, const json &) -> std::optional<bool> { return std::nullopt; };
#endif

    // The runtime reads the USB camera list at start and when the overlay asks for a refresh.
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

    // The dashboard object stays for the whole run because its GPU objects must outlive the SteamVR connection.
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
            o.mouth_tracking_id = mouth_tracking_id;
            dashboard->start(steamvr, thread_spec(threads, "ui", "bbl-ui"), o, dashboard_choices(cfg));
        } else {
            dashboard->stop();
        }
        LOG.info("dashboard: debug overlay %s", on ? "on" : "off");
    };
    set_dashboard(dcfg.value("enabled", false));
    // The debug overlay can start this tracking visualizer.
    AppLauncher visualizer(cfg.value("visualizer", json::object()),
                           (paths::state_dir() / "visualizer.log").string());
    const std::filesystem::path settings_file = default_settings_file();

    // This client handles the "Expression Tracking" section of the driver in SteamVR Settings.
    SteamVrSettingsBridge steamvr_settings(steamvr, section(cfg, "steamvr_settings").value("section", "driver_babble"),
                                           SteamVrSettingsBridge::entries_from_config(cfg));

    // Applies a live setting change from any origin, saves it if necessary, and shows the new value elsewhere.
    enum class Origin { Dashboard, SteamVr, Reload };
    auto origin_name = [](Origin o) {
        return o == Origin::Dashboard ? "the Babble tab" : o == Origin::SteamVr ? "SteamVR Settings" : "config reload";
    };
    const std::set<std::string> unsaved = unsaved_settings(cfg);
    std::string output_error;   // The last rejected entry on the Outputs page, which the page shows.
    auto apply_setting_unchecked = [&](const std::string &id, const json &value, Origin origin) {
        const json::json_pointer ptr(id);
        if (cfg.contains(ptr) && cfg[ptr] == value)
            return;
        LOG.debug("settings: %s = %s from %s (was %s)", id.c_str(), value.dump().c_str(), origin_name(origin),
                  cfg.contains(ptr) ? cfg[ptr].dump().c_str() : "unset");
        bool applied = false, mouth_handled = false;
        if (auto r = apply_mouth_setting(id, value)) {
            if (!*r)
                return;
            applied = mouth_handled = true;
        }
        for (auto &s : sources) {
            if (mouth_handled)
                break;
            if (id == "/sources/" + s.name + "/process_fps" && value.is_number() && value.get<double>() > 0) {
                s.src->set_process_fps(value.get<double>());
                applied = true;
            } else if (id == "/sources/" + s.name + "/tracking" && value.is_boolean()) {
                set_feed_tracking(s.name, value.get<bool>());
                applied = true;
            } else if (id.starts_with("/sources/" + s.name + "/") &&
                       s.src->set_option(id.substr(s.name.size() + 10), value)) {
                applied = true;   // This is an option of the module, such as the USB camera choice.
            }
        }
        for (auto &p : pipelines) {
            const std::string prefix = "/pipelines/" + p->name() + "/";
            if (id.starts_with(prefix) && p->set_option(id.substr(prefix.size()), value))
                applied = true;
        }
        for (auto &k : sinks) {
            const std::string prefix = "/sinks/" + k.name + "/";
            if (!id.starts_with(prefix))
                continue;
            const std::string key = id.substr(prefix.size());
            if (k.sink->set_option(key, value)) {
                applied = true;
            } else if (const json *opt = module_option(cfg["sinks"][k.name], key)) {
                // The module has no live option, so make it again with the new value.
                if (const std::string err = module_option_error(*opt, value); !err.empty()) {
                    output_error = opt->value("label", key) + ": " + (value.is_string() ? value.get<std::string>()
                                                                                          : value.dump()) +
                                   " is " + err;
                    LOG.warn("settings: %s = %s is %s", id.c_str(), value.dump().c_str(), err.c_str());
                    return;
                }
                cfg[ptr] = value;
                rebuild_sink(k);
                output_error.clear();
                applied = true;
            }
        }
        if (id.starts_with("/outputs/") && value.is_boolean() &&
            std::any_of(sinks.begin(), sinks.end(), [&](const SinkEntry &k) { return "/outputs/" + k.output == id; })) {
            cfg[ptr] = value;
            route_outputs();
            applied = true;
        }
        if (id == "/pc_address" && value.is_string()) {
            if (!valid_pc_address(value.get<std::string>())) {
                output_error = "PC Address: " + value.get<std::string>() + " is not an IPv4 address";
                LOG.warn("settings: %s = %s is not an IPv4 address", id.c_str(), value.dump().c_str());
                return;
            }
            cfg[ptr] = value;
            LOG.info("output: PC address %s", value.get<std::string>().empty() ? "automatic" : value.dump().c_str());
            for (auto &k : sinks)
                if (!k.output.empty() && section(cfg["sinks"][k.name], "module").value("uses_pc_address", false))
                    rebuild_sink(k);
            output_error.clear();
            applied = true;
        }
        if (id == "/dashboard/enabled" && value.is_boolean()) {
            set_dashboard(value.get<bool>());
            applied = true;
        }
        if (!applied) {
            LOG.warn("settings: ignoring %s = %s", id.c_str(), value.dump().c_str());
            // Send our value back because SteamVR already shows the rejected value.
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
            dashboard->set_choice(id, value.is_boolean() ? double(value.get<bool>()) : value.get<double>());
        if (origin != Origin::SteamVr)
            steamvr_settings.push(id, value);
    };
    // Log a malformed id or a value of the wrong type, and do not stop the runtime.
    auto apply_setting = [&](const std::string &id, const json &value, Origin origin) {
        try {
            apply_setting_unchecked(id, value, origin);
        } catch (const json::exception &e) {
            LOG.warn("settings: ignoring %s = %s: %s", id.c_str(), value.dump().c_str(), e.what());
        }
    };

    // The tracking rate of a source is the number of frames per second that its pipelines processed.
    std::vector<uint64_t> processed_last(pipelines.size(), 0);
    for (size_t i = 0; i < pipelines.size(); i++)
        processed_last[i] = pipelines[i]->processed();
    std::map<std::string, double> source_hz;
    uint64_t rates_at = mono_ns();
    auto feed_status = [&](const std::string &name) {
        ui::FeedStatus f;
        auto it = std::find_if(sources.begin(), sources.end(), [&](const SourceEntry &e) { return e.name == name; });
        if (it == sources.end())
            return f;   // A source that is not configured or is disabled shows Off with no switch.
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
            // A source that streams but has no processed frames yet shows Starting.
            auto hz = source_hz.find(name);
            f.hz = hz == source_hz.end() ? 0 : hz->second;
            f.health = f.hz > 0.5 ? ui::Health::Working : ui::Health::Starting;
            break;
        }
        }
        return f;
    };

    // The diagnostics file has all the data to examine a problem report without the headset.
    std::string last_stats = "{}";   // The latest stats in JSON.
    std::string last_diagnostics;    // The path of the last diagnostics file.
    auto diagnostics_text = [&] {
        std::ostringstream o;
        const time_t wall = time(nullptr);
        char when[64];
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S %Z", localtime(&wall));
        o << "Babble Frame Runtime diagnostics, " << when << "\n\n== session\n";
        for (const auto &line : diag::session_info(cfg, paths::resource_dir() / "models"))
            o << line << "\n";
        o << "\n== status\n" << (arbiter.state().empty() ? "" : "arbiter " + arbiter.state() + ", ") << "headset "
          << to_string(steamvr.worn()) << ", steamvr " << (steamvr.connected() ? "connected" : "not connected")
          << ", mouth source " << mouth_label() << "\n";
        for (auto &src : sources)
            o << "source " << src.name << ": " << src.src->state_name() << (src.src->tracking() ? "" : " (tracking off)")
              << (src.src->last_error().empty() ? "" : ", last error: " + src.src->last_error()) << "\n";
        for (auto &p : pipelines)
            o << "pipeline " << p->name() << ": source " << p->source() << ", " << p->processed() << " frames\n";
        for (auto &k : sinks)
            o << "output " << k.name << (k.output.empty() ? "" : k.running ? " (on)" : " (off)") << "\n";
        o << "pc address: " << (cfg.value("pc_address", "").empty() ? "automatic" : cfg.value("pc_address", ""))
          << "\n";
        if (pc_link) {
            const auto ls = pc_link->status();
            o << "pc link: " << (ls.linked ? "linked to \"" + ls.module + "\" via " + ls.path + " (" + ls.address + ")"
                                           : std::string("searching"))
              << (ls.rtt_ms >= 0 ? ", rtt " + std::to_string(ls.rtt_ms) + " ms" : "")
              << (ls.data == LinkSink::Data::Arriving ? ", data arriving"
                  : ls.data == LinkSink::Data::Missing ? ", DATA NOT ARRIVING" : "")
              << (ls.claims_eye ? std::string(", module claims eye ") + (*ls.claims_eye ? "on" : "off") +
                                      ", face " + (*ls.claims_face ? "on" : "off")
                                : "")
              << (ls.restart ? ", VRCFT RESTART NEEDED" : "") << "\n";
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

    // The recorder gets the frames of every source, and the capture app controls it over a local socket.
    RecordSink *recorder = nullptr;
    for (auto &k : sinks)
        if (auto *r = dynamic_cast<RecordSink *>(k.sink.get()); r && k.running)
            recorder = r;
    if (recorder) {
        for (auto &s : sources)
            s.src->set_frame_tap(recorder);
        // The recorder writes the Valve eye-service samples to valve_eye.bin in the session.
        eye_server.set_listener([recorder](const eye_server::Sample &s) { recorder->add_eye_sample(s); });
    }
    auto find_source = [&](const std::string &name) -> ITrackingSource * {
        for (auto &s : sources)
            if (s.name == name)
                return s.src.get();
        return nullptr;
    };
    auto source_json = [&](const std::string &name) {
        json j = nullptr;
        if (ITrackingSource *src = find_source(name)) {
            j = {{"state", src->state_name()}, {"streaming", src->streaming()}};
            if (src->camera_fps() > 0)
                j["camera_fps"] = src->camera_fps();
        }
        return j;
    };
    // The uploader sends consented sessions in the background, and never while the recorder records.
    const json capture_cfg = section(cfg, "capture");
    capture::Uploader uploader(capture_cfg.value("upload", json::object()),
                               recorder ? recorder->dir() : paths::data_dir() / "captures", threads,
                               [&] { return recorder && recorder->status().recording; });
    uploader.start();
    capture::FnBackend capture_backend;
    // The status has the live eye camera rate because an XR app can change the display refresh rate.
    auto eye_json = [&] {
        json eye = source_json("eye");
        if (eye.is_object() && steamvr.display_hz() > 0)
            eye["display_hz"] = steamvr.display_hz();
        return eye;
    };
    capture_backend.on_hello = [&] {
        json eye = eye_json();
        json mouth = source_json(mouth_feed());
        if (mouth.is_object())
            mouth["kind"] = mouth_label();
        return json{{"runtime", BABBLE_VERSION_DISPLAY},
                    {"profile", BABBLE_PROFILE_NAME},
                    {"recording", recorder && recorder->available()},
                    {"upload", uploader.available()},
                    {"sources", {{"eye", eye}, {"mouth", mouth}}}};
    };
    capture_backend.on_start = [&](const json &req) {
        if (!recorder || !recorder->available())
            return capture::Result::fail("unavailable", "recording isn't available in this runtime (no encoder)");
        const std::string routine = req.value("routine", "");
        auto streaming = [&](const std::string &name) {
            ITrackingSource *s = find_source(name);
            return s && s->streaming();
        };
        if (routine == "eye" && !streaming("eye"))
            return capture::Result::fail("no_source", "the eye cameras aren't streaming");
        if (routine == "mouth" && !streaming(mouth_feed()))
            return capture::Result::fail("no_source", "no mouth camera is streaming");
        if (std::none_of(sources.begin(), sources.end(), [](const SourceEntry &s) { return s.src->streaming(); }))
            return capture::Result::fail("no_source", "no camera is streaming");
        RecordSink::SessionSpec spec;
        spec.routine = routine;
        spec.meta = req.value("meta", json::object());
        spec.consent = req.value("consent", json(nullptr));
        spec.routine_def = req.value("routine_def", json(nullptr));
        spec.valve_eye = find_source("eye") != nullptr;
        for (auto &s : sources) {
            // The encoder gets this nominal rate, and the timestamps give the real timing.
            double fps = s.src->camera_fps();
            const json &sc = cfg["sources"][s.name];
            if (fps <= 0)
                fps = sc.value("sensor_fps", sc.value("fps", 0.0));
            spec.source_fps[s.name] = fps > 0 ? fps : 30.0;
        }
        if (ITrackingSource *eye = find_source("eye"); eye && eye->camera_fps() > 0)
            spec.context["eye_camera_fps"] = eye->camera_fps();
        if (steamvr.display_hz() > 0)
            spec.context["display_hz"] = steamvr.display_hz();
        spec.context["mouth_source"] = mouth_label();
        spec.context["eye_rate_divisor"] = cfg["sources"].value("eye", json::object()).value("rate_divisor", 1);
        if (spec.valve_eye)
            eye_server.want(EyeServerClient::Record, true);   // This stays on until the session finishes.
        const auto r = recorder->start_session(spec);
        if (!r.ok) {
            eye_server.want(EyeServerClient::Record, false);
            return capture::Result::fail(r.error, r.message);
        }
        capture::Result ok;
        ok.data = {{"session", r.id}, {"dir", r.dir}};
        return ok;
    };
    capture_backend.on_segment = [&](const capture::Segment &seg) {
        json extra = json::object();
        if (seg.step)
            extra["step"] = *seg.step;
        if (!seg.phase.empty())
            extra["phase"] = seg.phase;
        std::string err;
        if (!recorder || !recorder->add_label(seg.label, seg.start_ns, seg.end_ns, &err, extra))
            return capture::Result::fail("bad_request", recorder ? err : "not recording");
        return capture::Result{};
    };
    capture_backend.on_stop = [&](const std::string &reason) {
        if (!recorder || !recorder->stop_session(reason))
            return capture::Result::fail("bad_request", "not recording");
        return capture::Result{};
    };
    // The protocol uses "mouth" for the current mouth feed, and other sources keep their names.
    auto protocol_source = [&](const std::string &name) -> std::string {
        return name == mouth_feed() ? "mouth" : name;
    };
    auto capture_status = [&] {
        json s = {{"recording", false}};
        const auto up = uploader.snapshot();
        s["uploads"] = {{"pending", up.pending},
                        {"active", up.active.empty() ? json(nullptr) : json(up.active)},
                        {"stage", up.stage.empty() ? json(nullptr) : json(up.stage)},
                        {"progress", up.progress}};
        json src = {{"eye", eye_json()}, {"mouth", source_json(mouth_feed())}};
        if (src["mouth"].is_object())
            src["mouth"]["kind"] = mouth_label();
        if (recorder) {
            const auto st = recorder->status();
            s["recording"] = st.recording;
            if (st.recording) {
                s["session"] = st.id;
                s["finishing"] = st.finishing;
                s["elapsed_ns"] = mono_ns() - st.started_ns;
                for (const auto &[name, c] : st.sources) {
                    json &e = src[protocol_source(name)];
                    if (!e.is_object())
                        e = json::object();
                    e["frames"] = c.frames;
                    e["dropped"] = c.dropped;
                }
            }
        }
        s["sources"] = src;
        return s;
    };
    capture_backend.on_status = capture_status;
    capture_backend.on_upload = [&](const std::string &session) { return uploader.queue(session); };
    // The installer gets the optional capture app from its GitHub releases.
    const json app_cfg = capture_cfg.value("app", json::object());
    capture::AppInstaller installer(app_cfg, capture::kProtocol, threads);
    installer.start();
    const uint64_t app_check_ns = uint64_t(std::max(1.0, app_cfg.value("check_hours", 6.0)) * 3600e9);
    uint64_t app_checked_at = 0;
    // The updater checks for runtime updates one minute after start and then every few hours.
    const json update_cfg = section(cfg, "update");
    update::RuntimeUpdater updater(update_cfg,
                                   {BABBLE_VERSION, std::string(BABBLE_VERSION) != BABBLE_VERSION_DISPLAY,
                                    update::RuntimeUpdater::own_install_root()},
                                   threads);
    updater.start();
    const uint64_t update_check_ns = uint64_t(std::max(1.0, update_cfg.value("check_hours", 6.0)) * 3600e9);
    uint64_t update_check_at = mono_ns() + 60'000'000'000ull;
    std::optional<AppLauncher> capture_app;
    std::string capture_app_entry;   // The entry that capture_app uses.
    capture::Server capture_server(capture_backend);
    std::vector<int> capture_fds;   // These descriptors are registered with epoll.
    auto sync_capture_fds = [&] {
        for (int fd : capture_fds)
            epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
        capture_fds = capture_server.fds();
        for (int fd : capture_fds)
            add_epoll(ep, fd);
    };
    if (recorder) {
        std::string err;
        if (capture_server.listen(paths::runtime_dir() / "capture.sock", &err))
            sync_capture_fds();
        else
            LOG.error("capture: no control socket: %s", err.c_str());
    }
    uint64_t next_capture_status = 0;

    // Write all stats to stats.jsonl at each interval, and write a short summary to the log.
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
    uint64_t next_stats = mono_ns() + stats_ns;
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
                                for (const char *key : {"process_fps", "rate_divisor", "tracking"}) {
                                    const json::json_pointer ptr("/sources/" + s.name + "/" + key);
                                    if (fresh.contains(ptr))
                                        apply_setting(ptr.to_string(), fresh[ptr], Origin::Reload);
                                }
                            for (const char *id :
                                 {"/pipelines/eye/openness_source", "/pipelines/gaze/gaze_source",
                                  "/pipelines/gaze/smoothing", "/pipelines/face/mirror", "/dashboard/enabled",
                                  "/mouth_source", "/pc_address"}) {
                                const json::json_pointer ptr(id);
                                if (fresh.contains(ptr))
                                    apply_setting(id, fresh[ptr], Origin::Reload);
                            }
                            for (auto &[out_id, on] : section(fresh, "outputs").items())
                                apply_setting("/outputs/" + out_id, on, Origin::Reload);
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
            } else if (std::find(capture_fds.begin(), capture_fds.end(), fd) != capture_fds.end()) {
                capture_server.on_readable(fd);
            }
        }
        if (capture_server.take_fds_changed())
            sync_capture_fds();
        // Make the launcher again when an install or a removal changes the capture app.
        if (const std::string entry = installer.snapshot().entry; entry != capture_app_entry) {
            if (capture_app && capture_app->running())
                capture_app->stop();
            capture_app.reset();
            if (!entry.empty())
                capture_app.emplace(json{{"command", {entry}},
                                         {"env", {{"BABBLE_CAPTURE_SOCKET", capture_server.path().string()}}}},
                                    (paths::state_dir() / "dataset-capture.log").string());
            capture_app_entry = entry;
        }
        if (!running)
            break;

        uint64_t now = mono_ns();
        bool any_streaming = false;
        for (auto &s : sources)
            any_streaming = any_streaming || s.src->streaming();
        arbiter.tick(now, any_streaming);
        // When the user puts on or takes off the headset, the pipelines send neutral values and reset their state.
        if (Worn w = steamvr.worn(); w != Worn::Unknown && w != last_worn) {
            if (last_worn != Worn::Unknown || w == Worn::No)
                for (auto &p : pipelines)
                    p->on_presence(w == Worn::Yes);
            last_worn = w;
        }
        for (auto &s : sources) {
            bool was = s.src->streaming();
            s.src->tick(now);
            // Log each state change on one line, with the reason if there is one.
            if (const char *st_now = s.src->state_name(); s.last_state != st_now) {
                LOG.info("source %s: %s -> %s%s", s.name.c_str(), s.last_state.empty() ? "(start)" : s.last_state.c_str(),
                         st_now,
                         s.src->last_error().empty() ? "" : (" (" + s.src->last_error() + ")").c_str());
                s.last_state = st_now;
            }
            if (was && !s.src->streaming())
                arbiter.recheck(now);
        }

        if (recorder) {
            if (auto fin = recorder->take_finished()) {
                eye_server.want(EyeServerClient::Record, false);
                json ev = {{"session", fin->id}, {"dir", fin->dir}, {"status", fin->status}};
                if (!fin->error.empty())
                    ev["error"] = fin->error;
                if (!fin->stop_reason.empty())
                    ev["reason"] = fin->stop_reason;
                for (const auto &[name, c] : fin->sources) {
                    ev["frames"][protocol_source(name)] = c.frames;
                    ev["dropped"][protocol_source(name)] = c.dropped;
                }
                capture_server.stopped(ev);
            }
            if (capture_server.greeted() && now >= next_capture_status &&
                (recorder->status().recording || uploader.snapshot().pending > 0)) {
                json s = capture_status();
                s["op"] = "status";
                capture_server.send(s);
                next_capture_status = now + 1000000000ull;
            }
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
                // Exit cleanly so that the driver shim starts the runtime again.
                if (!shim)
                    LOG.warn("restart requested, but no driver shim is supervising this runtime: it will just exit");
                exit_reason = "restart requested from the Babble tab";
                running = false;
            }
            if (dashboard->take_capture_check_request() && !installer.snapshot().busy() &&
                (app_checked_at == 0 || now - app_checked_at >= app_check_ns)) {
                installer.check();
                app_checked_at = now;
            }
            if (now >= update_check_at) {
                if (!updater.snapshot().busy())
                    updater.check();
                update_check_at = now + update_check_ns;
            }
            if (dashboard->take_update_request()) {
                using U = update::RuntimeUpdater::State;
                switch (updater.snapshot().state) {
                case U::Available: updater.install(); break;
                case U::Downloading: updater.cancel(); break;
                case U::Installed: updater.restart_steamvr(); break;
                case U::Failed: updater.install(); break;   // This checks again first if no release was found.
                default: break;
                }
            }
            using CaptureAction = ui::DashboardView::ClickResult::CaptureAction;
            if (const CaptureAction a = dashboard->take_capture_request(); a != CaptureAction::None) {
                using S = capture::AppInstaller::State;
                const auto snap = installer.snapshot();
                const bool app_running = capture_app && capture_app->running();
                if (a == CaptureAction::Remove) {
                    if (!app_running && !snap.busy())
                        installer.remove();
                } else if (app_running) {
                    capture_app->stop();
                } else if (snap.state == S::Downloading) {
                    installer.cancel();
                } else if (snap.state == S::NotInstalled || snap.state == S::UpdateAvailable) {
                    installer.install();
                } else if (snap.state == S::Failed) {
                    installer.check();
                    app_checked_at = now;
                } else if (snap.state == S::Installed && capture_app) {
                    capture_app->start();
                }
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
            st.mouth = feed_status(mouth_feed());
            st.eye = feed_status(ui::kFeedSources[1]);
            st.visualizer = !visualizer.available() ? ui::AppState::Unavailable
                            : visualizer.running()  ? ui::AppState::Running
                                                    : ui::AppState::Stopped;
            st.mouth_source = mouth_label();
            st.mouth_source_selectable = BABBLE_FRUNK_MOUTH;
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
            for (auto &s : sources) {
                // Show the eye rate that the share of the camera rate gives.
                const json::json_pointer ptr("/sources/" + s.name + "/rate_divisor");
                const double cam = s.src->camera_fps();
                if (cam > 0 && cfg.contains(ptr) && cfg[ptr].is_number() && cfg[ptr].get<double>() >= 1) {
                    char buf[96];
                    snprintf(buf, sizeof(buf), "%.0f Hz now, from %.0f Hz eye cameras.",
                             cam / cfg[ptr].get<double>(), cam);
                    st.choice_hints[ptr.to_string()] = buf;
                }
            }
            // The Outputs page has a row for each output module, and the front page shows the first one that is on.
            st.pc_address = cfg.value("pc_address", "");
            st.output_error = output_error;
            int outputs_on = 0;
            std::vector<const SinkEntry *> modules;
            for (const auto &k : sinks)
                if (!k.output.empty())
                    modules.push_back(&k);
            auto order = [&](const SinkEntry *k) {
                return section(section(section(cfg, "sinks"), k->name.c_str()), "module").value("order", 100);
            };
            std::stable_sort(modules.begin(), modules.end(),
                             [&](const SinkEntry *a, const SinkEntry *b) { return order(a) < order(b); });
            for (const SinkEntry *entry : modules) {
                const SinkEntry &k = *entry;
                const json &kc = cfg["sinks"][k.name];
                const json &mod = section(kc, "module");
                ui::OutputModule m;
                m.id = k.output;
                m.label = output_label(k);
                m.hint = mod.value("hint", "");
                m.on = k.running;
                outputs_on += k.running;
                // A sender shows that it works while its message count goes up.
                auto sending = [&](uint64_t sent) {
                    const bool moving = sent != sent_last[k.name];
                    sent_last[k.name] = sent;
                    return moving;
                };
                if (auto *l = dynamic_cast<LinkSink *>(k.sink.get()); l && k.running) {
                    const auto ls = l->status();
                    m.ok = ls.linked && ls.data != LinkSink::Data::Missing && !ls.restart;
                    const std::string path = ls.path == "lan" ? "LAN" : ls.path == "usb" ? "USB" : ls.path;
                    m.status = !ls.linked ? "Searching for VRCFaceTracking\u2026"
                               : ls.data == LinkSink::Data::Missing
                                   ? "Linked, but no data reaches it. Check Windows Firewall."
                                   : ls.restart ? "Restart VRCFaceTracking to use the tracking that is on."
                                                : "Linked to " + (ls.module.empty() ? ls.address : ls.module) +
                                                      " via " + path;
                    if (!st.link.available) {
                        st.link = {true, ls.linked, ls.path, ls.module, ls.address, ls.rtt_ms};
                        st.link.no_data = ls.data == LinkSink::Data::Missing;
                        st.link.restart = ls.restart;
                    }
                } else if (auto *n = dynamic_cast<VrcNativeSink *>(k.sink.get()); n && k.running) {
                    const auto ns = n->status();
                    m.ok = !ns.kind.empty() && sending(ns.sent);
                    m.status = ns.kind.empty()       ? "Waiting for a Steam Link session"
                               : ns.kind == "frame" ? "Sending to VRChat on this headset"
                                                    : "Sending to VRChat at " + ns.address;
                    if (!st.link.available) {
                        st.link.available = st.link.native = true;
                        st.link.linked = m.ok;
                        st.link.path = ns.kind;
                        st.link.address = ns.address;
                    }
                } else if (auto *o = dynamic_cast<BabbleOscSink *>(k.sink.get()); o && k.running) {
                    const auto os = o->status();
                    const std::string to = os.kind.empty() ? "" : os.address + ":" + std::to_string(os.port);
                    m.ok = !os.kind.empty() && sending(os.sent);
                    m.status = to.empty() ? "Set a PC address, or stream with Steam Link" : "Sending to " + to;
                    if (!st.link.available) {
                        st.link.available = st.link.osc = true;
                        st.link.linked = m.ok;
                        st.link.address = to;
                    }
                } else if (k.running) {
                    m.ok = true;
                    m.status = "Running";
                }
                for (const json &o : mod.value("options", json::array())) {
                    // A key can be a path, such as "config/port" for a setting of a plugin.
                    const std::string key = o.value("key", "");
                    const json::json_pointer at("/" + key);
                    if (key.empty() || !kc.contains(at))
                        continue;
                    ui::ModuleOption opt;
                    opt.id = "/sinks/" + k.name + "/" + key;
                    opt.label = o.value("label", key);
                    opt.text = o.value("kind", "choice") == "text";
                    opt.numeric = o.value("numeric", false);
                    for (const json &v : o.value("values", json::array()))
                        opt.values.push_back(v.dump());
                    opt.labels = o.value("labels", std::vector<std::string>{});
                    opt.value = opt.text && kc[at].is_string() ? kc[at].get<std::string>() : kc[at].dump();
                    m.options.push_back(std::move(opt));
                }
                st.outputs.push_back(std::move(m));
            }
            st.link.more = st.link.available ? outputs_on - 1 : 0;
            {
                using S = capture::AppInstaller::State;
                using R = ui::CaptureRow::State;
                const auto snap = installer.snapshot();
                ui::CaptureRow row;
                row.version = snap.installed;
                row.latest = snap.latest;
                row.detail = snap.detail;
                row.progress = snap.progress;
                if (capture_app && capture_app->running()) {
                    row.state = R::Running;
                } else {
                    switch (snap.state) {
                    case S::Unknown: row.state = snap.installed.empty() ? R::NotInstalled : R::Installed; break;
                    case S::Checking: row.state = R::Checking; break;
                    case S::NotInstalled: row.state = R::NotInstalled; break;
                    case S::Installed: row.state = R::Installed; break;
                    case S::UpdateAvailable: row.state = R::UpdateAvailable; break;
                    case S::Downloading: row.state = R::Downloading; break;
                    case S::Installing: row.state = R::Installing; break;
                    case S::Failed: row.state = R::Failed; break;
                    }
                }
                // Hide the row if there is no recorder and the app is not installed.
                if (!recorder && snap.installed.empty())
                    row.state = R::Hidden;
                if (recorder) {
                    row.recording = recorder->status().recording;
                    if (!recorder->available() && row.state == R::NotInstalled)
                        row.detail = "ffmpeg isn't installed, so nothing can be recorded";
                }
                const auto up = uploader.snapshot();
                row.uploads_pending = up.pending;
                row.upload_progress = up.progress;
                row.upload_stage = up.stage;
                st.capture = row;
            }
            {
                using U = update::RuntimeUpdater::State;
                using N = ui::UpdateNotice::State;
                const auto up = updater.snapshot();
                ui::UpdateNotice n;
                n.version = up.latest;
                n.detail = up.detail;
                n.progress = up.progress;
                switch (up.state) {
                case U::Available: n.state = N::Available; break;
                case U::Downloading: n.state = N::Downloading; break;
                case U::Installing: n.state = N::Installing; break;
                case U::Installed: n.state = N::RestartNeeded; break;
                case U::Failed: n.state = N::Failed; break;
                default: n.state = N::Hidden; break;
                }
                st.update = n;
            }
            dashboard->set_status(st);
        }

        if (const uint64_t until = log::verbose_until(); until && now >= until) {
            log::verbose_until(0);
            LOG.info("verbose logging ended");
        }
        if (now >= next_stats) {
            std::string line = (arbiter.state().empty() ? "" : "arbiter=" + arbiter.state() + " ") +
                               "headset=" + to_string(steamvr.worn());
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
    // Exit after 5 s if the shutdown hangs, so that the shim can start a new runtime.
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        LOG.error("shutdown is stuck after 5 s; exiting now. Threads:");
        for (const auto &line : diag::thread_report())
            LOG.error("  %s", line.c_str());
        _exit(1);
    }).detach();
    dashboard->stop();
    capture_server.close();
    for (auto &s : sources)
        s.src->set_frame_tap(nullptr);   // The recorder stops with the sinks.
    eye_server.set_listener(nullptr);
    // Stop the consumers first so that nothing holds frames when the sources stop.
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
    dashboard->release_gpu();   // OpenVR requires that its Vulkan objects outlive the connection.
    LOG.info("stopped");
    return 0;
}
