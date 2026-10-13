#pragma once

#include <filesystem>

namespace babble::paths {

std::filesystem::path exe_path();

// $XDG_CONFIG_HOME/babble-frame-runtime (created on first call)
std::filesystem::path config_dir();

// $XDG_STATE_HOME/babble-frame-runtime (created on first call)
std::filesystem::path state_dir();

// $XDG_DATA_HOME/babble-frame-runtime (created on first call): recordings,
// installed add-on apps.
std::filesystem::path data_dir();

// $XDG_RUNTIME_DIR/babble-frame-runtime, mode 0700 (created on first call);
// state_dir() when there's no XDG_RUNTIME_DIR. For sockets.
std::filesystem::path runtime_dir();

// A leading "~/" becomes $HOME/ (config paths); anything else is unchanged.
std::string expand_home(const std::string &path);

// Runtime resources. Inside the driver folder the binary lives at
// <driver>/bin/<platform>/babble-runtime and resources at
// <driver>/resources/runtime; standalone builds fall back to <exe dir>/resources.
std::filesystem::path resource_dir();

} // namespace babble::paths
