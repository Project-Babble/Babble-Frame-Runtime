// Babble SteamVR driver shim.
//
// Loaded by vrserver, which is always running on the Frame. The shim does not
// add any tracked devices; it only supervises the Babble runtime as a separate
// process so a runtime crash can never take down vrserver (and with it the
// Frame shell).
//
// Lifetime: the runtime inherits the read end of a pipe as fd 3. vrserver holds
// the write end, so if vrserver exits for any reason (including a crash) the
// runtime sees EOF on fd 3 and shuts down. Cleanup() also sends SIGTERM.

#include <openvr_driver.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(_WIN32)
#error "The Babble driver shim currently targets Linux only."
#endif

#define BABBLE_EXPORT extern "C" __attribute__((visibility("default")))

namespace {

constexpr int kParentFd = 3;
constexpr auto kPollInterval = std::chrono::milliseconds(500);
// Polls without driver.vrdrivermanifest before the folder counts as uninstalled.
constexpr int kUninstallPolls = 4;
constexpr auto kTermGrace = std::chrono::seconds(3);
constexpr int kMaxBackoffSeconds = 30;
// A runtime that stays up this long is considered healthy and resets backoff.
constexpr auto kHealthyUptime = std::chrono::seconds(60);

void Log(const char *fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (vr::VRDriverLog())
        vr::VRDriverLog()->Log(buf);
}

// Directory containing this .so: <driver root>/bin/linuxarm64
std::string ModuleDir() {
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void *>(&ModuleDir), &info) || !info.dli_fname)
        return {};
    std::string path = info.dli_fname;
    auto slash = path.rfind('/');
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

// $XDG_STATE_HOME/babble-frame-runtime, created if missing.
std::string StateDir() {
    std::string base;
    if (const char *xdg = getenv("XDG_STATE_HOME"); xdg && *xdg)
        base = xdg;
    else if (const char *home = getenv("HOME"); home && *home)
        base = std::string(home) + "/.local/state";
    else
        return "/tmp";
    mkdir(base.c_str(), 0755);
    std::string dir = base + "/babble-frame-runtime";
    mkdir(dir.c_str(), 0755);
    return dir;
}

// <driver root>: ModuleDir() is <root>/bin/linuxarm64.
std::string DriverRoot() {
    std::string dir = ModuleDir();
    for (int i = 0; i < 2; i++) {
        auto slash = dir.rfind('/');
        if (slash == std::string::npos)
            break;
        dir.resize(slash);
    }
    return dir;
}

bool DriverFolderPresent() {
    return access((DriverRoot() + "/driver.vrdrivermanifest").c_str(), F_OK) == 0;
}

// Steam deleted the driver folder (the app was uninstalled, or moved to
// another library). Drop it from SteamVR's external driver list so the next
// SteamVR start does not look for it, and remove the runtime's state dir
// (logs, generated input manifests). User settings in the config dir stay,
// so a reinstall keeps them; babble-setup --uninstall removes those too.
// The shim itself stays loaded in vrserver, idle, until SteamVR restarts.
void ForgetInstall() {
    namespace fs = std::filesystem;
    const std::string root = DriverRoot();
    std::string cfg;
    if (const char *x = getenv("XDG_CONFIG_HOME"); x && *x)
        cfg = x;
    else if (const char *home = getenv("HOME"); home && *home)
        cfg = std::string(home) + "/.config";
    if (!cfg.empty()) {
        const fs::path file = fs::path(cfg) / "openvr" / "openvrpaths.vrpath";
        try {
            std::ifstream in(file);
            nlohmann::json j = nlohmann::json::parse(in, nullptr, true, true);
            if (j.is_object() && j.contains("external_drivers") && j["external_drivers"].is_array()) {
                nlohmann::json kept = nlohmann::json::array();
                for (const auto &d : j["external_drivers"])
                    if (!(d.is_string() && d.get<std::string>() == root))
                        kept.push_back(d);
                if (kept.size() != j["external_drivers"].size()) {
                    j["external_drivers"] = kept.empty() ? nlohmann::json(nullptr) : kept;
                    const fs::path tmp = file.string() + ".babble-tmp";
                    std::ofstream out(tmp, std::ios::trunc);
                    out << j.dump(1, '\t') << "\n";
                    out.close();
                    if (out && rename(tmp.c_str(), file.c_str()) == 0)
                        Log("Babble: unregistered %s from %s\n", root.c_str(), file.c_str());
                    else
                        unlink(tmp.c_str());
                }
            }
        } catch (const std::exception &e) {
            Log("Babble: could not update %s: %s\n", file.c_str(), e.what());
        }
    }
    std::error_code ec;
    fs::remove_all(StateDir(), ec);
}

class RuntimeSupervisor {
public:
    bool Start(std::string exe, std::string logPath) {
        exe_ = std::move(exe);
        logPath_ = std::move(logPath);
        if (access(exe_.c_str(), X_OK) != 0) {
            Log("Babble: runtime not executable at %s (%s)\n", exe_.c_str(), strerror(errno));
            return false;
        }
        stop_ = false;
        thread_ = std::thread([this] { Loop(); });
        return true;
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable())
            thread_.join();
    }

private:
    void Loop() {
        int backoff = 1;
        while (!stop_) {
            auto started = std::chrono::steady_clock::now();
            if (!Spawn()) {
                if (!DriverFolderPresent()) {
                    Log("Babble: driver folder is gone (app uninstalled); giving up\n");
                    ForgetInstall();
                    return;
                }
                if (Sleep(std::chrono::seconds(backoff)))
                    break;
                backoff = std::min(backoff * 2, kMaxBackoffSeconds);
                continue;
            }

            int status = 0;
            bool exited = false;
            int missing = 0;
            while (!(exited = Reap(&status))) {
                if (Sleep(kPollInterval))
                    break;
                // Steam removing the folder under us is the uninstall signal:
                // stop tracking now rather than at the next SteamVR restart.
                missing = DriverFolderPresent() ? 0 : missing + 1;
                if (missing >= kUninstallPolls) {
                    Log("Babble: driver folder is gone (app uninstalled); stopping the runtime\n");
                    Terminate();
                    ForgetInstall();
                    return;
                }
            }
            if (!exited) {
                Terminate();
                break;
            }

            if (WIFEXITED(status))
                Log("Babble: runtime (pid %d) exited with status %d\n", pid_, WEXITSTATUS(status));
            else if (WIFSIGNALED(status))
                Log("Babble: runtime (pid %d) killed by signal %d\n", pid_, WTERMSIG(status));
            else
                Log("Babble: runtime (pid %d) is gone\n", pid_);
            ClosePipe();
            pid_ = -1;

            if (std::chrono::steady_clock::now() - started >= kHealthyUptime)
                backoff = 1;
            Log("Babble: restarting runtime in %ds\n", backoff);
            if (Sleep(std::chrono::seconds(backoff)))
                break;
            backoff = std::min(backoff * 2, kMaxBackoffSeconds);
        }
    }

    bool Spawn() {
        int fds[2];
        if (pipe2(fds, O_CLOEXEC) != 0) {
            Log("Babble: pipe2 failed: %s\n", strerror(errno));
            return false;
        }
        int logFd = open(logPath_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);

        // Everything the child needs is prepared before fork(): vrserver is
        // multithreaded, so the child may only make async-signal-safe calls.
        const char *argv[] = {exe_.c_str(), nullptr};
        const char *exe = exe_.c_str();
        int readFd = fds[0];

        pid_t pid = fork();
        if (pid < 0) {
            Log("Babble: fork failed: %s\n", strerror(errno));
            close(fds[0]);
            close(fds[1]);
            if (logFd >= 0)
                close(logFd);
            return false;
        }
        if (pid == 0) {
            setsid();
            // Signal handlers reset on exec, but vrserver may have blocked or
            // ignored signals the runtime relies on.
            sigset_t none;
            sigemptyset(&none);
            sigprocmask(SIG_SETMASK, &none, nullptr);
            signal(SIGPIPE, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);

            int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0)
                dup2(devnull, STDIN_FILENO);
            if (logFd >= 0) {
                dup2(logFd, STDOUT_FILENO);
                dup2(logFd, STDERR_FILENO);
            }
            dup2(readFd, kParentFd); // dup2 clears O_CLOEXEC on the new fd
            // Don't leak vrserver's descriptors into the runtime.
            syscall(SYS_close_range, kParentFd + 1, ~0U, 0);
            execv(exe, const_cast<char *const *>(argv));
            _exit(127);
        }

        close(fds[0]);
        if (logFd >= 0)
            close(logFd);
        writeFd_ = fds[1];
        pid_ = pid;
        Log("Babble: started runtime %s (pid %d), log %s\n", exe_.c_str(), pid_, logPath_.c_str());
        return true;
    }

    // Returns true once the child has exited. If vrserver ignores SIGCHLD the
    // kernel auto-reaps and waitpid reports ECHILD, so fall back to kill(0).
    bool Reap(int *status) {
        pid_t r = waitpid(pid_, status, WNOHANG);
        if (r == pid_)
            return true;
        if (r < 0 && errno == ECHILD) {
            *status = 0;
            return kill(pid_, 0) != 0;
        }
        return false;
    }

    void Terminate() {
        if (pid_ <= 0)
            return;
        Log("Babble: stopping runtime (pid %d)\n", pid_);
        ClosePipe();
        kill(pid_, SIGTERM);
        int status = 0;
        auto deadline = std::chrono::steady_clock::now() + kTermGrace;
        while (!Reap(&status) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (!Reap(&status)) {
            Log("Babble: runtime ignored SIGTERM, killing\n");
            kill(pid_, SIGKILL);
            waitpid(pid_, &status, 0);
        }
        pid_ = -1;
    }

    void ClosePipe() {
        if (writeFd_ >= 0) {
            close(writeFd_);
            writeFd_ = -1;
        }
    }

    // Sleeps for up to `d`; returns true if Stop() was requested.
    template <class Duration>
    bool Sleep(Duration d) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, d, [this] { return stop_.load(); });
    }

    std::string exe_;
    std::string logPath_;
    pid_t pid_ = -1;
    int writeFd_ = -1;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};
};

class BabbleProvider : public vr::IServerTrackedDeviceProvider {
public:
    vr::EVRInitError Init(vr::IVRDriverContext *context) override {
        VR_INIT_SERVER_DRIVER_CONTEXT(context);
        std::string exe = ModuleDir() + "/babble-runtime";
        std::string log = StateDir() + "/runtime.log";
        Log("Babble: driver shim loaded\n");
        // Failing to start the runtime is not fatal to SteamVR; stay loaded
        // and report it rather than returning an init error.
        supervisor_.Start(exe, log);
        return vr::VRInitError_None;
    }

    void Cleanup() override {
        supervisor_.Stop();
        Log("Babble: driver shim unloaded\n");
        VR_CLEANUP_SERVER_DRIVER_CONTEXT();
    }

    const char *const *GetInterfaceVersions() override { return vr::k_InterfaceVersions; }
    void RunFrame() override {}
    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override {}
    void LeaveStandby() override {}

private:
    RuntimeSupervisor supervisor_;
};

BabbleProvider g_provider;

} // namespace

BABBLE_EXPORT void *HmdDriverFactory(const char *interfaceName, int *returnCode) {
    if (strcmp(interfaceName, vr::IServerTrackedDeviceProvider_Version) == 0)
        return &g_provider;
    if (returnCode)
        *returnCode = vr::VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
