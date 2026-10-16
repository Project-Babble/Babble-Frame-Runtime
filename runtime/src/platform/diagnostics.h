#pragma once

// These functions collect the data for the startup log and for a diagnostics file, for problem reports.

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace babble::diag {

// Returns the session header with the version, the system, the config changes, the model files and the USB devices.
std::vector<std::string> session_info(const nlohmann::json &cfg, const std::filesystem::path &models_dir);

// Returns the name, state, wait channel and CPUs of each thread of this process.
std::vector<std::string> thread_report();

// Writes text to <dir>/diag-YYYYMMDD-HHMMSS.txt and returns the path, or an empty path on failure.
std::filesystem::path write_file(const std::filesystem::path &dir, const std::string &text, int keep = 5);

}
