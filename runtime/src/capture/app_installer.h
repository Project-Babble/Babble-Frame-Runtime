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

// Installs, updates and removes the dataset capture app (an optional add-on)
// from its GitHub releases: the newest release whose manifest speaks our
// control protocol. Network and archive work runs curl and tar on its own
// thread ("net"); the main thread sends requests and reads a snapshot.
//
// Config ("capture.app"): repo, asset_prefix, api_url ({repo} is replaced),
// dir (the install folder), prereleases.
//
// An install downloads the tarball, checks it against the release's .sha256
// (integrity, not authorship), extracts it beside the install folder, checks
// its manifest, then swaps it in. The running app is never replaced: main
// doesn't ask while it runs.
class AppInstaller {
public:
    enum class State { Unknown, Checking, NotInstalled, Installed, UpdateAvailable, Downloading, Installing, Failed };
    struct Snapshot {
        State state = State::Unknown;
        std::string installed;   // version, "" if none
        std::string latest;      // newest compatible version, "" if unknown
        std::string detail;      // the failure, or a note
        float progress = 0;      // 0..1 while downloading
        std::string entry;       // the installed app's executable, "" if none
        bool busy() const { return state == State::Checking || state == State::Downloading || state == State::Installing; }
    };

    AppInstaller(const nlohmann::json &cfg, int protocol, const nlohmann::json &threads);
    ~AppInstaller();

    void start();
    void check();
    void install();   // install, or update to the newest compatible release
    void cancel();    // stops a download
    void remove();
    Snapshot snapshot() const;
    const std::filesystem::path &dir() const { return dir_; }

    // The installed app in `dir`, if it's complete and speaks `protocol`.
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
    std::optional<Candidate> latest_;   // net thread
};

const char *to_string(AppInstaller::State s);

} // namespace babble::capture
