#include "platform/app_launcher.h"

#include "core/log.h"
#include "core/paths.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
const babble::log::Logger LOG("launcher");
}

extern char **environ;

namespace babble {

using paths::expand_home;

AppLauncher::AppLauncher(const nlohmann::json &cfg, std::string default_log) {
    for (const auto &a : cfg.value("command", nlohmann::json::array()))
        if (a.is_string())
            argv_.push_back(expand_home(a.get<std::string>()));
    log_ = expand_home(cfg.value("log", default_log));
    const nlohmann::json env = cfg.value("env", nlohmann::json::object());
    for (const auto &[k, v] : env.items())
        if (v.is_string())
            env_[k] = v.get<std::string>();
    name_ = argv_.empty() ? "" : std::filesystem::path(argv_[0]).parent_path().filename().string();
}

bool AppLauncher::available() const { return !argv_.empty() && access(argv_[0].c_str(), X_OK) == 0; }

bool AppLauncher::running() {
    if (pid_ <= 0)
        return false;
    int status = 0;
    if (waitpid(pid_, &status, WNOHANG) == pid_) {
        LOG.info("launcher: %s (pid %d) exited", argv_[0].c_str(), int(pid_));
        pid_ = -1;
        return false;
    }
    return true;
}

bool AppLauncher::start() {
    if (running() || !available())
        return running();
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, log_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    posix_spawn_file_actions_adddup2(&fa, STDOUT_FILENO, STDERR_FILENO);
    posix_spawn_file_actions_addclose(&fa, 3);   // Close fd 3 because only the runtime can use the shim link.

    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    sigset_t none, defaults;
    sigemptyset(&none);
    sigemptyset(&defaults);
    for (int sig : {SIGTERM, SIGINT, SIGHUP, SIGPIPE, SIGCHLD})
        sigaddset(&defaults, sig);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &defaults);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSID);

    std::vector<char *> argv;
    for (auto &a : argv_)
        argv.push_back(a.data());
    argv.push_back(nullptr);
    // Use the environment of the runtime and add the configured "env" entries.
    std::vector<std::string> env_store;
    for (char **e = environ; e && *e; e++) {
        std::string kv = *e;
        if (!env_.count(kv.substr(0, kv.find('='))))
            env_store.push_back(std::move(kv));
    }
    for (const auto &[k, v] : env_)
        env_store.push_back(k + "=" + v);
    std::vector<char *> envp;
    for (auto &kv : env_store)
        envp.push_back(kv.data());
    envp.push_back(nullptr);
    pid_t pid = -1;
    const int rc = posix_spawn(&pid, argv_[0].c_str(), &fa, &at, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    if (rc != 0) {
        LOG.error("launcher: starting %s failed: %s", argv_[0].c_str(), strerror(rc));
        return false;
    }
    pid_ = pid;
    LOG.info("launcher: started %s (pid %d, log %s)", argv_[0].c_str(), int(pid_), log_.c_str());
    return true;
}

AppLauncher::~AppLauncher() {
    if (running())
        stop();
}

void AppLauncher::stop() {
    if (!running())
        return;
    // Signal the process group because the child session holds the script and the app that it started.
    kill(-pid_, SIGTERM);
    LOG.info("launcher: stopping %s (pid %d)", argv_[0].c_str(), int(pid_));
}

}
