#include "capture/app_installer.h"

#include "core/clock.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/sha256.h"
#include "platform/curl.h"
#include "platform/process.h"

#include <fstream>

#include <unistd.h>

namespace {
const babble::log::Logger LOG("apps");
}

namespace babble::capture {

namespace fs = std::filesystem;
using json = nlohmann::json;

const char *to_string(AppInstaller::State s) {
    switch (s) {
    case AppInstaller::State::Unknown: return "unknown";
    case AppInstaller::State::Checking: return "checking";
    case AppInstaller::State::NotInstalled: return "not installed";
    case AppInstaller::State::Installed: return "installed";
    case AppInstaller::State::UpdateAvailable: return "update available";
    case AppInstaller::State::Downloading: return "downloading";
    case AppInstaller::State::Installing: return "installing";
    case AppInstaller::State::Failed: return "failed";
    }
    return "?";
}

AppInstaller::AppInstaller(const json &cfg, int protocol, const json &threads)
    : repo_(cfg.value("repo", "Project-Babble/Babble-Guided-Dataset-Capture")),
      prefix_(cfg.value("asset_prefix", "babble-dataset-capture-")),
      api_url_(cfg.value("api_url", "https://api.github.com/repos/{repo}/releases?per_page=20")),
      dir_(paths::expand_home(cfg.value("dir", (paths::data_dir() / "apps" / "dataset-capture").string()))),
      prereleases_(cfg.value("prereleases", false)), protocol_(protocol),
      thread_spec_(thread_spec(threads, "net", "bbl-net")) {
    for (size_t at; (at = api_url_.find("{repo}")) != std::string::npos;)
        api_url_.replace(at, 6, repo_);
    refresh_installed();
}

AppInstaller::~AppInstaller() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cancel_ = true;
    cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

void AppInstaller::start() {
    if (!thread_.joinable())
        thread_ = start_thread(thread_spec_, [this] { run(); });
}

void AppInstaller::check() {
    std::lock_guard<std::mutex> lock(mutex_);
    requests_.push_back(Request::Check);
    cv_.notify_one();
}

void AppInstaller::install() {
    std::lock_guard<std::mutex> lock(mutex_);
    cancel_ = false;
    requests_.push_back(Request::Install);
    cv_.notify_one();
}

void AppInstaller::cancel() { cancel_ = true; }

void AppInstaller::remove() {
    std::lock_guard<std::mutex> lock(mutex_);
    requests_.push_back(Request::Remove);
    cv_.notify_one();
}

AppInstaller::Snapshot AppInstaller::snapshot() const {
    std::lock_guard<std::mutex> lock(snap_mutex_);
    return snap_;
}

void AppInstaller::set(State s, std::string detail) {
    std::lock_guard<std::mutex> lock(snap_mutex_);
    snap_.state = s;
    snap_.detail = std::move(detail);
    if (s != State::Downloading)
        snap_.progress = 0;
}

std::optional<Manifest> AppInstaller::validate_install(const fs::path &dir, int protocol, std::string *err) {
    std::ifstream f(dir / "manifest.json");
    if (!f) {
        if (err)
            *err = "no manifest.json";
        return std::nullopt;
    }
    const json j = json::parse(f, nullptr, false);
    auto m = parse_manifest(j, err);
    if (!m)
        return std::nullopt;
    if (m->protocol != protocol) {
        if (err)
            *err = "it speaks protocol " + std::to_string(m->protocol) + ", this runtime " + std::to_string(protocol);
        return std::nullopt;
    }
    const fs::path entry = dir / m->entry;
    if (access(entry.c_str(), X_OK) != 0) {
        if (err)
            *err = m->entry + " is missing or not executable";
        return std::nullopt;
    }
    return m;
}

void AppInstaller::refresh_installed() {
    std::string err;
    auto m = validate_install(dir_, protocol_, &err);
    std::lock_guard<std::mutex> lock(snap_mutex_);
    snap_.installed = m ? m->version : "";
    snap_.entry = m ? (dir_ / m->entry).string() : "";
    if (snap_.state == State::Unknown || snap_.state == State::NotInstalled || snap_.state == State::Installed)
        snap_.state = m ? State::Installed : State::NotInstalled;
    if (!m && fs::exists(dir_))
        snap_.detail = "the installed app isn't usable: " + err;
}

bool AppInstaller::fetch(const std::string &url, std::string *out, std::string *err) {
    return curl::fetch(url, out, err);
}

bool AppInstaller::download(const Asset &a, const fs::path &to, std::string *err) {
    return curl::download(
        a.url, a.size, to, err,
        [this] { return cancel_ || [&] { std::lock_guard<std::mutex> l(mutex_); return stop_; }(); },
        [this](float p) {
            std::lock_guard<std::mutex> lock(snap_mutex_);
            snap_.progress = p;
        });
}

void AppInstaller::do_check() {
    set(State::Checking);
    refresh_installed();
    const std::string installed = snapshot().installed;
    auto give_up = [&](const std::string &why) {
        LOG.warn("apps: dataset capture update check: %s", why.c_str());
        if (!installed.empty())
            set(State::Installed, "update check failed: " + why);
        else
            set(State::Failed, why);
    };
    std::string body, err;
    if (!fetch(api_url_, &body, &err))
        return give_up(err);
    const json api = json::parse(body, nullptr, false);
    if (api.is_discarded() || !api.is_array())
        return give_up("unexpected reply from the releases API");
    latest_.reset();
    for (const auto &c : candidates(parse_releases(api), prefix_, prereleases_)) {
        std::string mtext, merr;
        if (!fetch(c.manifest.url, &mtext, &merr))
            continue;
        auto m = parse_manifest(json::parse(mtext, nullptr, false), &merr);
        if (m && m->protocol == protocol_) {
            latest_ = c;
            std::lock_guard<std::mutex> lock(snap_mutex_);
            snap_.latest = m->version;
            break;
        }
    }
    if (!latest_)
        return installed.empty() ? set(State::Failed, "no release works with this runtime yet")
                                 : set(State::Installed, "no newer release works with this runtime");
    const std::string latest = snapshot().latest;
    if (installed.empty())
        set(State::NotInstalled);
    else if (installed != latest)
        set(State::UpdateAvailable);
    else
        set(State::Installed);
    LOG.info("apps: dataset capture: installed %s, newest compatible %s", installed.empty() ? "none" : installed.c_str(),
             latest.c_str());
}

void AppInstaller::do_install() {
    if (!latest_) {
        do_check();
        if (!latest_)
            return;
    }
    const Candidate c = *latest_;
    const fs::path work = dir_.string() + ".download", fresh = dir_.string() + ".new", old = dir_.string() + ".old";
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    auto fail = [&](const std::string &why) {
        LOG.error("apps: installing the dataset capture app failed: %s", why.c_str());
        fs::remove_all(work, ec);
        fs::remove_all(fresh, ec);
        refresh_installed();
        set(why == "cancelled" ? (snapshot().installed.empty() ? State::NotInstalled : State::Installed) : State::Failed,
            why == "cancelled" ? "" : why);
    };
    set(State::Downloading);
    LOG.info("apps: downloading %s (%s)", c.tarball.name.c_str(), c.release.tag.c_str());
    const fs::path tarball = work / c.tarball.name;
    std::string err;
    if (!download(c.tarball, tarball, &err))
        return fail(err);
    std::string shatext;
    if (!fetch(c.sha256.url, &shatext, &err))
        return fail("checksum: " + err);
    const auto want = parse_sha256_file(shatext, c.tarball.name);
    if (!want)
        return fail("the release's .sha256 file has no digest for " + c.tarball.name);
    set(State::Installing);
    if (Sha256::file_hex(tarball) != *want)
        return fail("the download doesn't match the release's checksum");
    fs::remove_all(fresh, ec);
    fs::create_directories(fresh, ec);
    std::string tar_err;
    if (Process::run({"tar", "-xzf", tarball.string(), "-C", fresh.string(), "--no-same-owner"}, nullptr, &tar_err,
                     120000) != 0)
        return fail("unpacking failed: " + tar_err);
    auto m = validate_install(fresh, protocol_, &err);
    if (!m)
        return fail("the release isn't usable: " + err);
    fs::remove_all(old, ec);
    if (fs::exists(dir_))
        fs::rename(dir_, old, ec);
    if (ec)
        return fail("cannot replace " + dir_.string() + ": " + ec.message());
    fs::create_directories(dir_.parent_path(), ec);
    fs::rename(fresh, dir_, ec);
    if (ec) {
        std::error_code ec2;
        fs::rename(old, dir_, ec2);   // Put the previous version back.
        return fail("cannot install to " + dir_.string() + ": " + ec.message());
    }
    fs::remove_all(old, ec);
    fs::remove_all(work, ec);
    LOG.info("apps: dataset capture %s installed in %s", m->version.c_str(), dir_.c_str());
    refresh_installed();
    set(State::Installed);
}

void AppInstaller::do_remove() {
    std::error_code ec;
    fs::remove_all(dir_, ec);
    if (ec) {
        set(State::Failed, "cannot remove " + dir_.string() + ": " + ec.message());
        return;
    }
    LOG.info("apps: dataset capture removed (%s)", dir_.c_str());
    {
        std::lock_guard<std::mutex> lock(snap_mutex_);
        snap_.installed.clear();
        snap_.entry.clear();
    }
    set(State::NotInstalled);
}

void AppInstaller::run() {
    for (;;) {
        Request r;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return stop_ || !requests_.empty(); });
            if (stop_)
                return;
            r = requests_.front();
            requests_.pop_front();
        }
        switch (r) {
        case Request::Check: do_check(); break;
        case Request::Install: do_install(); break;
        case Request::Remove: do_remove(); break;
        }
    }
}

}
