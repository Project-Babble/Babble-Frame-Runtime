#pragma once

#include <filesystem>

namespace babble::paths {

std::filesystem::path exe_path();

// $XDG_CONFIG_HOME/babble-frame-runtime (created on first call)
std::filesystem::path config_dir();

// $XDG_STATE_HOME/babble-frame-runtime (created on first call)
std::filesystem::path state_dir();

// Runtime resources. Inside the driver folder the binary lives at
// <driver>/bin/<platform>/babble-runtime and resources at
// <driver>/resources/runtime; standalone builds fall back to <exe dir>/resources.
std::filesystem::path resource_dir();

} // namespace babble::paths
