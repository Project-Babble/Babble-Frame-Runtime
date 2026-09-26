// babble-setup: what Steam runs when the user presses Play on the Babble
// Frame Runtime app.
//
// The driver folder Steam installs is only useful once SteamVR knows about
// it, so this registers the folder as an external driver (the same edit
// `vrpathreg adddriver` makes to openvrpaths.vrpath) and, if SteamVR is
// running without the driver loaded, restarts SteamVR so vrserver picks it
// up. On later launches the driver is already active and this just says so.
//
// Steam launches it inside the Steam Linux Runtime container, so it depends
// only on files under $HOME, /proc and whatever systemd or D-Bus tooling is
// reachable from there; every attempt is logged to
// $XDG_STATE_HOME/babble-frame-runtime/setup.log for debugging.

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

const char *const kDriverName = "babble";
const char *const kSteamVrUnit = "steamvr.service";

FILE *g_log = nullptr;

void Log(const char *fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    fprintf(stdout, "babble-setup: %s\n", buf);
    fflush(stdout);
    if (g_log) {
        fprintf(g_log, "%s\n", buf);
        fflush(g_log);
    }
}

fs::path xdg_dir(const char *env, const char *home_suffix) {
    if (const char *v = getenv(env); v && *v)
        return fs::path(v);
    if (const char *home = getenv("HOME"); home && *home)
        return fs::path(home) / home_suffix;
    return fs::path("/tmp");
}

fs::path state_dir() {
    fs::path dir = xdg_dir("XDG_STATE_HOME", ".local/state") / "babble-frame-runtime";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

// The runtime's user settings (settings.json).
fs::path config_dir() { return xdg_dir("XDG_CONFIG_HOME", ".config") / "babble-frame-runtime"; }

// <install dir>/bin/linuxarm64/babble-setup -> <install dir>
fs::path install_dir() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
        return fs::current_path();
    buf[n] = 0;
    return fs::path(buf).parent_path().parent_path().parent_path();
}

// $XDG_CONFIG_HOME/openvr/openvrpaths.vrpath, where vrpathreg keeps the
// runtime, config and external driver lists.
fs::path vrpath_file() { return xdg_dir("XDG_CONFIG_HOME", ".config") / "openvr" / "openvrpaths.vrpath"; }

bool is_driver_folder(const fs::path &dir) {
    std::error_code ec;
    return fs::is_regular_file(dir / "driver.vrdrivermanifest", ec);
}

std::string driver_name_of(const fs::path &dir) {
    std::ifstream in(dir / "driver.vrdrivermanifest");
    if (!in)
        return {};
    try {
        json m = json::parse(in, nullptr, true, true);
        return m.value("name", std::string());
    } catch (const json::exception &) {
        return {};
    }
}

// Adds (or with remove=true drops) the install dir in external_drivers.
// Stale entries for a previous install of this same driver are dropped too,
// so moving the Steam library does not leave SteamVR looking at both.
// Returns true if the file changed; *ok is false if it could not be updated.
bool update_registration(const fs::path &dir, bool remove, bool *ok) {
    *ok = false;
    const fs::path file = vrpath_file();
    json root = json::object();
    {
        std::ifstream in(file);
        if (in) {
            try {
                root = json::parse(in, nullptr, true, true);
            } catch (const json::exception &e) {
                Log("%s is not valid JSON (%s); leaving it alone", file.c_str(), e.what());
                return false;
            }
        }
    }
    if (!root.is_object())
        root = json::object();

    json drivers = json::array();
    if (root.contains("external_drivers") && root["external_drivers"].is_array())
        drivers = root["external_drivers"];

    json kept = json::array();
    bool changed = false;
    bool present = false;
    for (const json &d : drivers) {
        if (!d.is_string())
            continue;
        const fs::path p(d.get<std::string>());
        std::error_code ec;
        if (p == dir || fs::equivalent(p, dir, ec)) {
            if (remove || present) {   // present twice: keep one
                changed = true;
                continue;
            }
            present = true;
            kept.push_back(dir.string());
            continue;
        }
        // Another copy of this driver is stale (SteamVR would try to load
        // both): a folder whose manifest names it, or a vanished folder with
        // this app's install folder name. Other vendors' entries are theirs.
        const bool same_app = p.filename() == dir.filename();
        if ((fs::exists(p, ec) && driver_name_of(p) == kDriverName) || (!fs::exists(p, ec) && same_app)) {
            Log("dropping stale external driver entry %s", p.c_str());
            changed = true;
            continue;
        }
        kept.push_back(d);
    }
    if (!remove && !present) {
        kept.push_back(dir.string());
        changed = true;
    }
    if (!changed) {
        *ok = true;
        return false;
    }

    root["external_drivers"] = kept.empty() ? json(nullptr) : kept;
    if (!root.contains("jsonid"))
        root["jsonid"] = "vrpathreg";
    if (!root.contains("version"))
        root["version"] = 1;

    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const fs::path tmp = file.string() + ".babble-tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            Log("cannot write %s: %s", tmp.c_str(), strerror(errno));
            return false;
        }
        out << root.dump(1, '\t') << "\n";
    }
    if (rename(tmp.c_str(), file.c_str()) != 0) {
        Log("cannot replace %s: %s", file.c_str(), strerror(errno));
        unlink(tmp.c_str());
        return false;
    }
    *ok = true;
    return true;
}

// Processes with this comm name, from /proc (shared with the host even inside
// the Steam Linux Runtime container).
int count_processes(const char *comm) {
    int n = 0;
    DIR *d = opendir("/proc");
    if (!d)
        return -1;
    while (dirent *e = readdir(d)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        char path[sizeof("/proc//comm") + sizeof(e->d_name)];
        snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        char name[64] = {0};
        if (fgets(name, sizeof(name), f)) {
            name[strcspn(name, "\n")] = 0;
            if (strcmp(name, comm) == 0)
                n++;
        }
        fclose(f);
    }
    closedir(d);
    return n;
}

int run(const std::vector<std::string> &argv) {
    std::vector<char *> av;
    for (const std::string &a : argv)
        av.push_back(const_cast<char *>(a.c_str()));
    av.push_back(nullptr);
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        execvp(av[0], av.data());
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid)
        return -1;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
}

std::string join(const std::vector<std::string> &argv) {
    std::string s;
    for (const std::string &a : argv)
        s += (s.empty() ? "" : " ") + a;
    return s;
}

bool exists(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// Asks the user session's systemd to restart SteamVR. On the Frame vrserver
// lives in steamvr.service and the gamescope session is part of it, so this
// is the same restart the system itself performs. Tried in order until one
// exits 0: systemctl from PATH, the host's systemctl through /run/host (how
// the Steam Linux Runtime container exposes the host), then the session bus.
bool restart_steamvr() {
    std::vector<std::vector<std::string>> attempts = {
        {"systemctl", "--user", "--no-block", "restart", kSteamVrUnit},
    };
    if (exists("/run/host/usr/bin/systemctl"))
        attempts.push_back({"/run/host/usr/lib/ld-linux-aarch64.so.1", "--library-path",
                            "/run/host/usr/lib:/run/host/usr/lib/systemd:/run/host/usr/lib64:/run/host/lib",
                            "/run/host/usr/bin/systemctl", "--user", "--no-block", "restart", kSteamVrUnit});
    attempts.push_back({"busctl", "--user", "call", "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager", "RestartUnit", "ss", kSteamVrUnit, "replace"});
    attempts.push_back({"dbus-send", "--session", "--print-reply", "--dest=org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager.RestartUnit",
                        std::string("string:") + kSteamVrUnit, "string:replace"});
    for (const auto &a : attempts) {
        int rc = run(a);
        Log("restart attempt: %s -> %d", join(a).c_str(), rc);
        if (rc == 0)
            return true;
    }
    return false;
}

void log_environment(const fs::path &dir) {
    Log("install dir %s", dir.c_str());
    std::error_code ec;
    Log("uid %d cwd %s", getuid(), fs::current_path(ec).c_str());
    for (const char *v : {"XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS", "PRESSURE_VESSEL_RUNTIME",
                          "STEAM_COMPAT_TOOL_PATHS", "SteamAppId", "PATH"})
        if (const char *s = getenv(v))
            Log("env %s=%s", v, s);
    for (const char *p : {"/run/pressure-vessel", "/run/host", "/opt/steamvr", "/usr/bin/systemctl",
                          "/usr/bin/busctl", "/usr/bin/dbus-send"})
        Log("%s: %s", p, exists(p) ? "present" : "absent");
    if (const char *rt = getenv("XDG_RUNTIME_DIR")) {
        std::string bus = std::string(rt) + "/bus";
        Log("%s: %s", bus.c_str(), exists(bus) ? "present" : "absent");
    }
}

void usage() {
    fprintf(stderr,
            "usage: babble-setup [--status] [--no-restart] [--unregister]\n"
            "       babble-setup --uninstall   (Steam's uninstall hook: unregister, keep SteamVR\n"
            "                                   running, remove the state and settings dirs)\n");
}

} // namespace

int main(int argc, char **argv) {
    bool unregister = false, status_only = false, no_restart = false, purge = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--unregister") unregister = true;
        else if (a == "--uninstall") { unregister = true; no_restart = true; purge = true; }
        else if (a == "--status") status_only = true;
        else if (a == "--no-restart") no_restart = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { usage(); return 2; }
    }

    const fs::path log_path = state_dir() / "setup.log";
    g_log = fopen(log_path.c_str(), "a");
    if (g_log) {
        time_t now = time(nullptr);
        char ts[64];
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));
        fprintf(g_log, "---- %s ----\n", ts);
    }

    const fs::path dir = install_dir();
    log_environment(dir);
    if (!is_driver_folder(dir)) {
        Log("no driver.vrdrivermanifest in %s; is this running from the installed app?", dir.c_str());
        return 1;
    }

    const int runtime_procs = count_processes("babble-runtime");
    const int vrserver_procs = count_processes("vrserver");
    Log("vrserver running: %s; babble-runtime running: %s", vrserver_procs > 0 ? "yes" : "no",
        runtime_procs > 0 ? "yes" : "no");
    if (status_only)
        return 0;

    bool ok = false;
    const bool changed = update_registration(dir, unregister, &ok);
    if (!ok)
        return 1;
    if (unregister)
        Log(changed ? "unregistered the Babble driver from SteamVR" : "the Babble driver was not registered");
    else
        Log(changed ? "registered %s as a SteamVR driver" : "%s is already registered as a SteamVR driver", dir.c_str());

    if (purge) {
        // Steam deletes the install folder next; the running driver shim
        // notices that and stops the runtime, so nothing else is needed for
        // tracking to end. What Steam does not know about is ours to remove.
        if (g_log) {
            fclose(g_log);
            g_log = nullptr;
        }
        std::error_code ec;
        for (const fs::path &d : {state_dir(), config_dir()}) {
            fs::remove_all(d, ec);
            Log("removed %s%s", d.c_str(), ec ? " (failed)" : "");
        }
    }

    // SteamVR reads the list when it starts, so with no vrserver there is
    // nothing more to do; likewise when the driver is already in the state
    // we want.
    if (vrserver_procs <= 0) {
        Log("SteamVR is not running; it will %s the driver next time it starts", unregister ? "skip" : "load");
        return 0;
    }
    const bool active = runtime_procs > 0;
    if (!unregister && active) {
        Log("Babble tracking is active");
        return 0;
    }
    if (unregister && !active) {
        Log("the Babble driver is not loaded");
        return 0;
    }
    if (no_restart) {
        Log("not restarting SteamVR (--no-restart); the change applies on its next start");
        return 0;
    }

    Log("restarting SteamVR so it %s the Babble driver", unregister ? "unloads" : "loads");
    if (!restart_steamvr()) {
        Log("could not restart SteamVR automatically; restart the headset (or SteamVR) to finish");
        return 3;
    }
    return 0;
}
