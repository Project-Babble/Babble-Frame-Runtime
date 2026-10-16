#pragma once

#include "capture/github_release.h"
#include "core/threading.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace babble::capture {

// Installs, updates and removes the optional capture app from GitHub releases on its own "net" thread.
class AppInstaller {
public:
    enum class State { Unknown, Checking, NotInstalled, Installed, UpdateAvailable, Downloading, Installing, Failed };
    struct Snapshot {
        State state = State::Unknown;
        std::string installed;   // The installed version, or "" if there is none.
        std::string latest;      // The newest compatible version, or "" if it is unknown.
        std::string detail;      // The failure or a note.
        float progress = 0;      // The download progress from 0 to 1.
        std::string entry;       // The executable of the installed app, or "" if there is none.
        bool busy() const { return state == State::Checking || state == State::Downloading || state == State::Installing; }
    };

    AppInstaller(const nlohmann::json &cfg, int protocol, const nlohmann::json &threads);
    ~AppInstaller();

    void start();
    void check();
    void install();   // Installs or updates to the newest compatible release.
    void cancel();    // Stops a download.
    void remove();
    Snapshot snapshot() const;
    const std::filesystem::path &dir() const { return dir_; }

    // Returns the manifest of the app in `dir` if the app is complete and uses `protocol`.
    static std::optional<Manifest> validate_install(const std::filesystem::path &dir, int protocol, std::string *err);

private:
    enum class Request { Check, Install, Remove };
    void run();
    void do_check();
    void do_install();
    void do_remove();
    void refresh_installed();
    bool fetch(const std::string &url, std::string *out, std::string *err);
    bool download(const Asset &a, const std::filesystem::path &to, std::string *err);
    void set(State s, std::string detail = "");

    std::string repo_, prefix_, api_url_;
    std::filesystem::path dir_;
    bool prereleases_ = false;
    int protocol_;
    ThreadSpec thread_spec_;

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Request> requests_;
    bool stop_ = false;
    std::atomic<bool> cancel_{false};

    mutable std::mutex snap_mutex_;
    Snapshot snap_;
    std::optional<Candidate> latest_;   // Only the net thread uses this.
};

const char *to_string(AppInstaller::State s);

}
