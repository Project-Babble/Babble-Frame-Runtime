#include "platform/diagnostics.h"

#include "babble_version.h"
#include "camera/usb/usb_device.h"
#include "core/config.h"
#include "defaults_json.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

#include <sys/utsname.h>
#include <unistd.h>

namespace babble::diag {

namespace {

namespace fs = std::filesystem;

std::string read_line(const fs::path &p) {
    std::ifstream in(p);
    std::string s;
    std::getline(in, s);
    return s;
}

// Returns a value from /etc/os-release without its quotes.
std::string os_release(const std::string &key) {
    std::ifstream in("/etc/os-release");
    std::string line;
    while (std::getline(in, line))
        if (line.rfind(key + "=", 0) == 0) {
            std::string v = line.substr(key.size() + 1);
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
                v = v.substr(1, v.size() - 2);
            return v;
        }
    return "";
}

// Returns true for config keys, such as upload credentials, whose values must not go in the log.
bool secret_path(const std::string &path) {
    const std::string leaf = path.substr(path.rfind('/') + 1);
    for (const char *w : {"key", "secret", "password", "token", "credential"})
        if (leaf.find(w) != std::string::npos)
            return true;
    return false;
}

std::string short_value(const nlohmann::json &v) {
    std::string s = v.dump();
    return s.size() > 120 ? s.substr(0, 117) + "..." : s;
}

}

std::vector<std::string> session_info(const nlohmann::json &cfg, const fs::path &models_dir) {
    std::vector<std::string> out;
    utsname u{};
    uname(&u);
    double uptime = 0;
    if (FILE *f = fopen("/proc/uptime", "r")) {
        if (fscanf(f, "%lf", &uptime) != 1)
            uptime = 0;
        fclose(f);
    }
    out.push_back(std::string("version ") + BABBLE_VERSION_DISPLAY + ", profile " + BABBLE_PROFILE_NAME + ", pid " +
                  std::to_string(getpid()));
    out.push_back(std::string("system ") + u.sysname + " " + u.release + " " + u.machine + ", " +
                  os_release("PRETTY_NAME") + " (build " + os_release("BUILD_ID") + "), up " +
                  std::to_string(int(uptime / 60)) + " min");

    // Log the changes that the config makes to the built-in defaults.
    std::string err;
    auto defaults = parse_json(kDefaultsJson, &err);
    if (defaults) {
        const auto patch = nlohmann::json::diff(*defaults, cfg);
        if (patch.empty())
            out.push_back("config: all defaults");
        for (const auto &op : patch) {
            const std::string what = op.value("op", "");
            const std::string path = op.value("path", "");
            if (what == "remove")
                out.push_back("config: " + path + " removed");
            else if (secret_path(path))
                out.push_back("config: " + path + " set (not logged)");
            else
                out.push_back("config: " + path + " = " + short_value(op.value("value", nlohmann::json())));
        }
    }

    std::error_code ec;
    std::vector<std::string> models;
    for (const auto &e : fs::directory_iterator(models_dir, ec))
        if (e.path().extension() == ".pte")
            models.push_back(e.path().filename().string() + " (" +
                             std::to_string((e.file_size(ec) + 512 * 1024) / (1024 * 1024)) + " MB)");
    std::sort(models.begin(), models.end());
    std::string m;
    for (const auto &s : models)
        m += (m.empty() ? "" : ", ") + s;
    out.push_back("models in " + models_dir.string() + ": " + (m.empty() ? "none" : m));

    std::string devices;
    for (const auto &d : usb::list_all()) {
        char vp[16];
        snprintf(vp, sizeof(vp), "%04x:%04x", d.vendor, d.product);
        if (d.vendor == 0x1d6b)
            continue;   // Skip the Linux root hubs.
        devices += (devices.empty() ? "" : ", ") + std::string(vp) + " " + usb::device_label(d);
    }
    out.push_back("usb devices: " + (devices.empty() ? std::string("none") : devices));
    return out;
}

std::vector<std::string> thread_report() {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto &t : fs::directory_iterator("/proc/self/task", ec)) {
        const std::string tid = t.path().filename().string();
        const std::string comm = read_line(t.path() / "comm");
        const std::string wchan = read_line(t.path() / "wchan");
        std::string state = "?", cpus = "?";
        std::ifstream st(t.path() / "status");
        std::string line;
        while (std::getline(st, line)) {
            if (line.rfind("State:", 0) == 0)
                state = line.substr(line.find_first_not_of(" \t", 6));
            else if (line.rfind("Cpus_allowed_list:", 0) == 0)
                cpus = line.substr(line.find_first_not_of(" \t", 18));
        }
        out.push_back("thread " + tid + " " + comm + ": " + state + ", waiting in " +
                      (wchan.empty() || wchan == "0" ? "-" : wchan) + ", cpus " + cpus);
    }
    return out;
}

fs::path write_file(const fs::path &dir, const std::string &text, int keep) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    const time_t now = time(nullptr);
    tm local{};
    localtime_r(&now, &local);
    char name[64];
    strftime(name, sizeof(name), "diag-%Y%m%d-%H%M%S.txt", &local);
    const fs::path path = dir / name;
    {
        std::ofstream f(path);
        f << text;
        if (!f)
            return {};
    }
    // Keep only the newest `keep` files.
    std::vector<fs::path> all;
    for (const auto &e : fs::directory_iterator(dir, ec))
        if (e.path().filename().string().rfind("diag-", 0) == 0)
            all.push_back(e.path());
    std::sort(all.begin(), all.end());
    for (size_t i = 0; i + size_t(keep) < all.size(); i++)
        fs::remove(all[i], ec);
    return path;
}

}
