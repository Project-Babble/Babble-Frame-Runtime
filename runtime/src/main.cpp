// babble-runtime: Babble mouth + eye tracking runtime for the Steam Frame.
//
// Normally spawned by the SteamVR driver shim (driver/src/driver_babble.cpp)
// with a pipe on fd 3; EOF there or SIGTERM means shut down. --standalone
// runs without the shim for development over SSH.

#include "camera/eye/dsp_eye_source.h"
#include "camera/mouth/frame_mouth_source.h"
#include "core/clock.h"
#include "core/config.h"
#include "core/lifecycle.h"
#include "core/log.h"
#include "core/metrics.h"
#include "core/paths.h"
#include "core/threading.h"
#include "model/pipeline.h"
#include "platform/camera_arbiter_client.h"
#include "sink/debug_sink.h"
#include "state/expression_state.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
};

void usage() {
    fprintf(stderr,
            "usage: babble-runtime [--config FILE] [--standalone] [--dump-frames DIR] [--dump-count N]\n"
            "                      [--stats-interval SECONDS] [--no-eye] [--no-mouth] [--log-level LEVEL]\n");
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
    if (opt.no_eye) c["eye"]["enabled"] = false;
    if (opt.no_mouth) c["mouth"]["enabled"] = false;
    if (!opt.dump_dir.empty()) c["sinks"]["debug"]["dump_dir"] = opt.dump_dir;
    if (opt.dump_count >= 0) c["sinks"]["debug"]["dump_count"] = opt.dump_count;
    if (!opt.dump_dir.empty() && opt.dump_count < 0) c["sinks"]["debug"]["dump_count"] = 10;
    if (opt.stats_interval > 0) c["stats_interval_s"] = opt.stats_interval;
    if (!opt.log_level.empty()) c["log_level"] = opt.log_level;
    return cl.config;
}

void apply_log_level(const json &c) {
    log::Level lvl;
    std::string name = c.value("log_level", "info");
    if (log::parse_level(name.c_str(), &lvl))
        log::set_level(lvl);
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
    LOG_INFO("babble-runtime starting (pid %d, %s, state dir %s)", getpid(),
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
    LatestSlot<StereoFrame> mouth_frames, eye_frames;

    const json dbg = cfg["sinks"].value("debug", json::object());
    std::unique_ptr<DebugSink> debug;
    if (dbg.value("enabled", true)) {
        std::string dump = dbg.contains("dump_dir") && dbg["dump_dir"].is_string() ? dbg["dump_dir"].get<std::string>() : "";
        debug = std::make_unique<DebugSink>(thread_spec(threads, "sink", "bbl-sink"), dbg.value("rate_hz", 2.0), dump,
                                            dbg.value("dump_count", 0));
    }

    std::vector<std::unique_ptr<Pipeline>> pipelines;
    const json pcfg = cfg.value("pipelines", json::object());
    for (auto [name, key, input] : {std::tuple{"mouth", "infM", &mouth_frames}, std::tuple{"eye", "infE", &eye_frames}}) {
        if (!pcfg.contains(name) || !pcfg[name].value("enabled", true))
            continue;
        auto p = std::make_unique<Pipeline>(name, pcfg[name], thread_spec(threads, key, key == std::string("infM") ? "bbl-infM" : "bbl-infE"),
                                            *input, state, stats, debug.get());
        if (p->init())
            pipelines.push_back(std::move(p));
    }

    CameraArbiterClient arbiter(cfg.value("arbiter", json::object()));
    FrameMouthSource mouth(cfg.value("mouth", json::object()), threads, mouth_frames, stats);
    DspEyeSource eye(cfg.value("eye", json::object()), threads, eye_frames, stats);

    if (debug)
        debug->start(state, stats);
    for (auto &p : pipelines)
        p->start();

    const auto poll_cfg = cfg["arbiter"].value("poll_interval_ms", json::object());
    const uint64_t arbiter_idle_ns = uint64_t(poll_cfg.value("idle", 1000)) * 1000000ull;
    const uint64_t arbiter_streaming_ns = uint64_t(poll_cfg.value("streaming", 5000)) * 1000000ull;
    const uint64_t stats_ns = uint64_t(std::max(1, cfg.value("stats_interval_s", 5))) * 1000000000ull;
    uint64_t next_arbiter = 0, next_stats = mono_ns() + stats_ns;
    bool routed = false;
    bool running = true;
    const char *exit_reason = "?";

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
                        // Live settings: the two tracking rates for now.
                        json fresh = load(opt);
                        apply_log_level(fresh);
                        mouth.set_process_fps(fresh["mouth"].value("process_fps", 120.0));
                        eye.set_process_fps(fresh["eye"].value("process_fps", 120.0));
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
        bool streaming = mouth.state() == FrameMouthSource::State::Streaming;
        if (now >= next_arbiter) {
            ArbiterState a = arbiter.poll();
            routed = a == ArbiterState::Routed;
            next_arbiter = now + (streaming ? arbiter_streaming_ns : arbiter_idle_ns);
        }
        mouth.tick(now, routed);
        if (mouth.state() != FrameMouthSource::State::Streaming && streaming)
            next_arbiter = now;   // capture trouble: re-check routing right away
        eye.tick(now);

        if (now >= next_stats) {
            LOG_INFO("status: arbiter=%s mouth=%s eye=%s", to_string(arbiter.state()), mouth.state_name(),
                     eye.state_name());
            LOG_INFO("stats: %s", stats.render().c_str());
            next_stats = now + stats_ns;
        }
    }

    LOG_INFO("shutting down: %s", exit_reason);
    // Order: consumers first so nothing holds frames when sources stop.
    for (auto &p : pipelines)
        p->stop();
    if (debug)
        debug->stop();
    mouth.shutdown();
    eye.shutdown();
    pipelines.clear();
    LOG_INFO("stopped");
    return 0;
}
