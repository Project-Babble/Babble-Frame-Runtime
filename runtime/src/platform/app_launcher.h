#pragma once

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <vector>

#include <sys/types.h>

namespace babble {

// Starts and stops one external helper app from the debug overlay, and only the main thread can use it.
class AppLauncher {
public:
    AppLauncher(const nlohmann::json &cfg, std::string default_log);
    ~AppLauncher();   // Stops the app because a restart of the runtime must not start a second copy.

    bool available() const;   // Returns true if the command exists and is executable.
    bool running();           // Also reaps the child after it exits.
    bool start();
    void stop();              // Sends SIGTERM to the process group of the app.
    const std::string &name() const { return name_; }

private:
    std::vector<std::string> argv_;
    std::string log_, name_;
    std::map<std::string, std::string> env_;
    pid_t pid_ = -1;
};

}
