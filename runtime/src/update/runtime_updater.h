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

// A runtime release that has a Steam Frame tarball and its .sha256 file.
struct RuntimeRelease {
    Version version;
    std::string tag;
    bool prerelease = false;
    capture::Asset tarball, sha256;
};

// Returns the newest release that is newer than `current`, and never returns a draft.
std::optional<RuntimeRelease> pick_update(const nlohmann::json &api, const Version &current, bool prereleases);

// Checks GitHub for runtime releases and installs a newer release on its own thread for installs from tools/install.sh.
class RuntimeUpdater {
public:
    enum class State { Unsupported, Idle, Checking, UpToDate, Available, Downloading, Installing, Installed, Failed };
    struct Snapshot {
        State state = State::Unsupported;
        std::string latest;   // The version of the newer release, or of the installed release.
        std::string detail;   // The reason for Unsupported or Failed.
        float progress = 0;   // The download progress, from 0 to 1.
        bool busy() const { return state == State::Checking || state == State::Downloading || state == State::Installing; }
    };
    struct Options {
        std::string version;                  // The value of BABBLE_VERSION.
        bool dev_build = false;               // True if BABBLE_VERSION_DISPLAY contains "(dev ...)".
        std::filesystem::path install_root;   // The driver folder that this binary runs from.
    };

    RuntimeUpdater(const nlohmann::json &cfg, Options options, const nlohmann::json &threads);
    ~RuntimeUpdater();

    void start();
    void check();
    void install();   // Installs the newer release that the last check found.
    void cancel();    // Stops a download.
    // Restarts SteamVR and this runtime to load an installed update.
    void restart_steamvr();
    Snapshot snapshot() const;

    // Returns the driver folder of the binary that runs now.
    static std::filesystem::path own_install_root();
    // Returns the reason why `o` cannot update itself, or "" if it can.
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
    std::optional<RuntimeRelease> latest_;   // Only the net thread uses this member.
};

const char *to_string(RuntimeUpdater::State s);

}
