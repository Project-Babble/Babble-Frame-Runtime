#pragma once

#include <filesystem>

namespace babble::paths {

std::filesystem::path exe_path();

// Returns $XDG_CONFIG_HOME/babble-frame-runtime and makes it on the first call.
std::filesystem::path config_dir();

// Returns $XDG_STATE_HOME/babble-frame-runtime and makes it on the first call.
std::filesystem::path state_dir();

// Returns $XDG_DATA_HOME/babble-frame-runtime for recordings and add-on apps, and makes it on the first call.
std::filesystem::path data_dir();

// Returns the 0700 socket folder in $XDG_RUNTIME_DIR, or state_dir() if XDG_RUNTIME_DIR is not set.
std::filesystem::path runtime_dir();

// Changes a leading "~/" into $HOME/ and keeps all other paths unchanged.
std::string expand_home(const std::string &path);

// Returns <driver>/resources/runtime in the driver folder, or <exe dir>/resources for a standalone build.
std::filesystem::path resource_dir();

}
