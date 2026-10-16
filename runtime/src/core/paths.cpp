#include "core/paths.h"

#include <cstdlib>
#include <system_error>

#include <unistd.h>

namespace babble::paths {

namespace fs = std::filesystem;

namespace {

fs::path xdg_dir(const char *env, const char *home_suffix) {
    if (const char *v = getenv(env); v && *v)
        return fs::path(v);
    if (const char *home = getenv("HOME"); home && *home)
        return fs::path(home) / home_suffix;
    return fs::path("/tmp");
}

fs::path ensure(fs::path dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

}

fs::path exe_path() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
        return fs::path("babble-runtime");
    buf[n] = 0;
    return fs::path(buf);
}

fs::path config_dir() { return ensure(xdg_dir("XDG_CONFIG_HOME", ".config") / "babble-frame-runtime"); }

fs::path state_dir() { return ensure(xdg_dir("XDG_STATE_HOME", ".local/state") / "babble-frame-runtime"); }

fs::path data_dir() { return ensure(xdg_dir("XDG_DATA_HOME", ".local/share") / "babble-frame-runtime"); }

fs::path runtime_dir() {
    const char *v = getenv("XDG_RUNTIME_DIR");
    if (!v || !*v)
        return state_dir();
    fs::path dir = ensure(fs::path(v) / "babble-frame-runtime");
    std::error_code ec;
    fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);
    return dir;
}

std::string expand_home(const std::string &path) {
    if (path.rfind("~/", 0) == 0)
        if (const char *home = getenv("HOME"); home && *home)
            return std::string(home) + path.substr(1);
    return path;
}

fs::path resource_dir() {
    fs::path exe_dir = exe_path().parent_path();
    std::error_code ec;
    fs::path in_driver = exe_dir.parent_path().parent_path() / "resources" / "runtime";
    if (fs::is_directory(in_driver, ec))
        return in_driver;
    return exe_dir / "resources";
}

}
