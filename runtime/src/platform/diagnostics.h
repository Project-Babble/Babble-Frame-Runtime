#pragma once

// What to put in the log at startup and in a diagnostics file, so a problem
// report can be understood without access to the headset.

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace babble::diag {

// The session header: version, kernel, SteamOS build, uptime, what the
// config changes from the defaults, the model files, the USB devices.
std::vector<std::string> session_info(const nlohmann::json &cfg, const std::filesystem::path &models_dir);

// Every thread of this process: name, state, what it's waiting in, CPUs.
std::vector<std::string> thread_report();

// Writes text to <dir>/diag-YYYYMMDD-HHMMSS.txt, keeping the newest `keep`
// such files; returns the path ("" on failure).
std::filesystem::path write_file(const std::filesystem::path &dir, const std::string &text, int keep = 5);

} // namespace babble::diag
