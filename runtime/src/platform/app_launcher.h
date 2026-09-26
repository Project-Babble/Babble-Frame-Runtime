#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

#include <sys/types.h>

namespace babble {

// Starts and stops one external helper app, e.g. the Aura Vis tracking
// visualizer, from the debug overlay. The child gets its own session, a clean
// signal mask and default dispositions (the runtime blocks its signals for
// signalfd), no shim pipe on fd 3, and its output appended to a log file.
//
// Config: {"command": ["~/aura_vis/run.sh", ...], "log": "<path>"}; a leading
// "~/" expands to $HOME. Main thread only.
class AppLauncher {
public:
    AppLauncher(const nlohmann::json &cfg, std::string default_log);

    bool available() const;   // the command exists and is executable
    bool running();           // also reaps the child once it exits
    bool start();
    void stop();              // SIGTERM to its process group
    const std::string &name() const { return name_; }

private:
    std::vector<std::string> argv_;
    std::string log_, name_;
    pid_t pid_ = -1;
};

} // namespace babble
