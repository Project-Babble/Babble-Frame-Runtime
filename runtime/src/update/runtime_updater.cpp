#include "update/runtime_updater.h"

#include "core/log.h"
#include "core/paths.h"
#include "core/sha256.h"
#include "platform/curl.h"
#include "platform/process.h"

#include <algorithm>

namespace {
const babble::log::Logger LOG("update");
} // namespace

namespace babble::update {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
// "V1.0-Alpha-5" -> "V1.0 Alpha 5", as the overlay shows versions.
std::string spaced(std::string tag) {
    std::replace(tag.begin(), tag.end(), '-', ' ');
    return tag;
}
} // namespace

const char *to_string(RuntimeUpdater::State s) {
    switch (s) {
    case RuntimeUpdater::State::Unsupported: return "unsupported";
    case RuntimeUpdater::State::Idle: return "idle";
    case RuntimeUpdater::State::Checking: return "checking";
    case RuntimeUpdater::State::UpToDate: return "up to date";
    case RuntimeUpdater::State::Available: return "update available";
    case RuntimeUpdater::State::Downloading: return "downloading";
    case RuntimeUpdater::State::Installing: return "installing";
    case RuntimeUpdater::State::Installed: return "installed, restart needed";
    case RuntimeUpdater::State::Failed: return "failed";
    }
    return "?";
}

std::optional<RuntimeRelease> pick_update(const json &api, const Version &current, bool prereleases) {
    std::optional<RuntimeRelease> best;
    for (const auto &r : capture::parse_releases(api)) {
        if (r.draft || (r.prerelease && !prereleases))
            continue;
        const auto v = parse_version(r.tag);
        if (!v || *v <= current || (best && *v <= best->version))
            continue;
        const std::string name = "babble-frame-runtime.linuxarm64." + r.tag + ".tar.xz";
        const capture::Asset *tar = nullptr, *sha = nullptr;
        for (const auto &a : r.assets) {
            if (a.name == name)
                tar = &a;
            if (a.name == name + ".sha256")
                sha = &a;
        }
        if (tar && sha)
            best = RuntimeRelease{*v, r.tag, r.prerelease, *tar, *sha};
    }
    return best;
}

fs::path RuntimeUpdater::own_install_root() {
    return paths::exe_path().parent_path().parent_path().parent_path();
}

std::string RuntimeUpdater::unsupported_reason(const Options &o) {
    if (o.dev_build)
        return "a dev build; it updates from source";
    if (!parse_version(o.version))
        return "the version \"" + o.version + "\" isn't a release version";
    std::error_code ec;
    if (!fs::exists(o.install_root / "driver.vrdrivermanifest", ec))
        return "not running from a driver folder";
    if (!fs::exists(o.install_root / ".install-source", ec))
        return "not installed by install.sh (a Steam install updates through Steam)";
    return "";
}

RuntimeUpdater::RuntimeUpdater(const json &cfg, Options options, const json &threads)
    : options_(std::move(options)), current_(parse_version(options_.version)),
      repo_(cfg.value("repo", "Project-Babble/Babble-Frame-Runtime")),
      api_url_(cfg.value("api_url", "https://api.github.com/repos/{repo}/releases?per_page=30")),
      thread_spec_(thread_spec(threads, "net", "bbl-net")) {
    for (size_t at; (at = api_url_.find("{repo}")) != std::string::npos;)
        api_url_.replace(at, 6, repo_);
    // "auto": testers on a prerelease get the next prerelease; a stable build only stable ones.
    const json pre = cfg.value("prereleases", json("auto"));
    prereleases_ = pre.is_boolean() ? pre.get<bool>() : (current_ && current_->prerelease());
    std::string why = cfg.value("enabled", true) ? unsupported_reason(options_) : "turned off in the config";
    snap_.state = why.empty() ? State::Idle : State::Unsupported;
    snap_.detail = why;
    if (why.empty())
        LOG.info("update: updates from %s (%s)", repo_.c_str(), prereleases_ ? "prereleases too" : "stable releases");
    else
        LOG.info("update: no updates from GitHub: %s", why.c_str());
}

RuntimeUpdater::~RuntimeUpdater() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cancel_ = true;
    cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

void RuntimeUpdater::start() {
    if (snapshot().state != State::Unsupported && !thread_.joinable())
        thread_ = start_thread(thread_spec_, [this] { run(); });
}

void RuntimeUpdater::check() {
    std::lock_guard<std::mutex> lock(mutex_);
    requests_.push_back(Request::Check);
    cv_.notify_one();
}

void RuntimeUpdater::install() {
    std::lock_guard<std::mutex> lock(mutex_);
    cancel_ = false;
    requests_.push_back(Request::Install);
    cv_.notify_one();
}

void RuntimeUpdater::cancel() { cancel_ = true; }

void RuntimeUpdater::restart_steamvr() {
    if (snapshot().state != State::Installed)
        return;
    LOG.info("update: restarting SteamVR to load %s", snapshot().latest.c_str());
    // --no-block: the restart stops this process too; systemd carries on without us.
    std::string err;
    if (Process::run({"systemctl", "--user", "--no-block", "restart", "steamvr.service"}, nullptr, &err, 5000) != 0)
        LOG.error("update: restarting SteamVR failed: %s", err.c_str());
}

RuntimeUpdater::Snapshot RuntimeUpdater::snapshot() const {
    std::lock_guard<std::mutex> lock(snap_mutex_);
    return snap_;
}

void RuntimeUpdater::set(State s, std::string detail) {
    std::lock_guard<std::mutex> lock(snap_mutex_);
    snap_.state = s;
    snap_.detail = std::move(detail);
    if (s != State::Downloading)
        snap_.progress = 0;
}

void RuntimeUpdater::do_check() {
    if (snapshot().state == State::Installed)
        return;   // waiting for SteamVR to restart into it
    set(State::Checking);
    // A failed check (offline, say) isn't worth a "failed" pill: keep what an
    // earlier check found, or show nothing, and try again next time.
    auto give_up = [&](const std::string &why) {
        LOG.warn("update: check failed: %s", why.c_str());
        set(latest_ ? State::Available : State::Idle, "update check failed: " + why);
    };
    std::string body, err;
    if (!curl::fetch(api_url_, &body, &err))
        return give_up(err);
    const json api = json::parse(body, nullptr, false);
    if (api.is_discarded() || !api.is_array())
        return give_up("unexpected reply from the releases API");
    latest_ = pick_update(api, *current_, prereleases_);
    if (!latest_) {
        set(State::UpToDate);
        LOG.info("update: %s is the newest", options_.version.c_str());
        return;
    }
    {
        std::lock_guard<std::mutex> lock(snap_mutex_);
        snap_.latest = spaced(latest_->tag);
    }
    set(State::Available);
    LOG.info("update: %s is available (running %s)", latest_->tag.c_str(), options_.version.c_str());
}

void RuntimeUpdater::do_install() {
    if (!latest_) {
        do_check();
        if (!latest_)
            return;
    }
    const RuntimeRelease r = *latest_;
    // Beside the install: same filesystem as the folder install.sh swaps in.
    const fs::path work = options_.install_root.parent_path() / ".babble-runtime-update";
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    auto fail = [&](const std::string &why) {
        fs::remove_all(work, ec);
        if (why == "cancelled")
            return set(State::Available);
        LOG.error("update: installing %s failed: %s", r.tag.c_str(), why.c_str());
        set(State::Failed, why);
    };
    set(State::Downloading);
    LOG.info("update: downloading %s", r.tarball.name.c_str());
    const fs::path tarball = work / r.tarball.name;
    std::string err;
    const bool ok = curl::download(
        r.tarball.url, r.tarball.size, tarball, &err,
        [this] { return cancel_ || [&] { std::lock_guard<std::mutex> l(mutex_); return stop_; }(); },
        [this](float p) {
            std::lock_guard<std::mutex> lock(snap_mutex_);
            snap_.progress = p;
        });
    if (!ok)
        return fail(err);
    std::string shatext;
    if (!curl::fetch(r.sha256.url, &shatext, &err))
        return fail("checksum: " + err);
    const auto want = capture::parse_sha256_file(shatext, r.tarball.name);
    if (!want)
        return fail("the release's .sha256 file has no digest for " + r.tarball.name);
    set(State::Installing);
    if (Sha256::file_hex(tarball) != *want)
        return fail("the download doesn't match the release's checksum");
    if (Process::run({"tar", "-xJf", tarball.string(), "-C", work.string(), "install.sh"}, nullptr, &err, 60000) != 0)
        return fail("the release has no install.sh: " + err);
    // The release's own installer, on our folder; SteamVR restarts later, on request.
    std::string out;
    const int rc = Process::run({"bash", (work / "install.sh").string(), tarball.string(), "--dir",
                                 options_.install_root.string(), "--no-restart"},
                                &out, &err, 180000);
    for (const std::string &text : {out, err}) {
        size_t at = 0;
        while (at < text.size()) {
            const size_t nl = std::min(text.find('\n', at), text.size());
            if (nl > at)
                LOG.info("update: install.sh: %s", text.substr(at, nl - at).c_str());
            at = nl + 1;
        }
    }
    if (rc != 0)
        return fail("install.sh failed (exit " + std::to_string(rc) + "; see the log)");
    fs::remove_all(work, ec);
    LOG.info("update: %s installed in %s; SteamVR runs it after a restart", r.tag.c_str(),
             options_.install_root.c_str());
    set(State::Installed);
}

void RuntimeUpdater::run() {
    for (;;) {
        Request q;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return stop_ || !requests_.empty(); });
            if (stop_)
                return;
            q = requests_.front();
            requests_.pop_front();
        }
        switch (q) {
        case Request::Check: do_check(); break;
        case Request::Install:
            if (snapshot().state != State::Installed)
                do_install();
            break;
        }
    }
}

} // namespace babble::update
