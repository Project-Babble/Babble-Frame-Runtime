#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace babble::curl {

// Downloads through the system curl (the add-on app installer, the runtime's
// own updates). Blocking: call from a worker thread.

// A small body (an API reply, a .sha256 file) into *out. False with *err.
bool fetch(const std::string &url, std::string *out, std::string *err);

// A file to `to`. `size` (0 = unknown) drives progress (0..1); cancelled() is
// polled about 10 times a second and stops the download ("cancelled"). False with *err.
bool download(const std::string &url, uint64_t size, const std::filesystem::path &to, std::string *err,
              const std::function<bool()> &cancelled, const std::function<void(float)> &progress);

} // namespace babble::curl
