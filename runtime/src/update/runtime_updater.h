#pragma once

#include "capture/github_release.h"
#include "core/threading.h"
#include "update/version.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace babble::update {

// A runtime release with a Steam Frame build: babble-frame-runtime.linuxarm64.<tag>.tar.xz
// and its .sha256 (what the Build workflow publishes).
struct RuntimeRelease {
    Version version;
    std::string tag;
    bool prerelease = false;
    capture::Asset tarball, sha256;
};

// The newest release newer than `current`, from GET /repos/<owner>/<repo>/releases.
// Drafts never; prereleases only if asked. Pure, for tests.
std::optional<RuntimeRelease> pick_update(const nlohmann::json &api, const Version &current, bool prereleases);

// The runtime's own updates, for installs made by tools/install.sh (or
// tools/get.sh): checks the GitHub releases, and on request downloads the
// newer tarball, checks it against its .sha256 and runs the install.sh inside
// it on our folder. SteamVR keeps running the old build until it restarts
// (restart_steamvr()). Steam installs (Steam updates them) and dev builds never
// update. Network and install work run on their own thread; the main thread
// sends requests and reads a snapshot.
//
// Config ("update"): enabled, repo, api_url ({repo} is replaced), prereleases
// ("auto": only while running a prerelease; true; false), check_hours (main).
class RuntimeUpdater {
public:
    enum class State { Unsupported, Idle, Checking, UpToDate, Available, Downloading, Installing, Installed, Failed };
    struct Snapshot {
        State state = State::Unsupported;
        std::string latest;   // the newer release's version ("V1.0 Alpha 5"), or the one installed
        std::string detail;   // why it's unsupported, or the failure
        float progress = 0;   // 0..1 while downloading
        bool busy() const { return state == State::Checking || state == State::Downloading || state == State::Installing; }
    };
    struct Options {
        std::string version;                  // BABBLE_VERSION
        bool dev_build = false;               // BABBLE_VERSION_DISPLAY carries "(dev ...)"
        std::filesystem::path install_root;   // the driver folder this binary runs from
    };

    RuntimeUpdater(const nlohmann::json &cfg, Options options, const nlohmann::json &threads);
    ~RuntimeUpdater();

    void start();
    void check();
    void install();   // the newer release found by the last check
    void cancel();    // stops a download
    // Restarts SteamVR (and with it this runtime) to load an installed update.
    void restart_steamvr();
    Snapshot snapshot() const;

    // The driver folder of the running binary (<root>/bin/<platform>/babble-runtime).
    static std::filesystem::path own_install_root();
    // Why `o` can't update itself ("" if it can).
    static std::string unsupported_reason(const Options &o);

private:
    enum class Request { Check, Install };
    void run();
    void do_check();
    void do_install();
    void set(State s, std::string detail = "");

    Options options_;
    std::optional<Version> current_;
    std::string repo_, api_url_;
    bool prereleases_ = false;
    ThreadSpec thread_spec_;

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Request> requests_;
    bool stop_ = false;
    std::atomic<bool> cancel_{false};

    mutable std::mutex snap_mutex_;
    Snapshot snap_;
    std::optional<RuntimeRelease> latest_;   // net thread
};

const char *to_string(RuntimeUpdater::State s);

} // namespace babble::update
