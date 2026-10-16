#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace babble::curl {

// These functions use the system curl and block, so call them from a worker thread.

// Gets a small body, such as an API reply, into *out, and returns false with *err on failure.
bool fetch(const std::string &url, std::string *out, std::string *err);

// Downloads a file to `to` and stops when cancelled() returns true, which it checks about 10 times a second.
bool download(const std::string &url, uint64_t size, const std::filesystem::path &to, std::string *err,
              const std::function<bool()> &cancelled, const std::function<void(float)> &progress);

}
