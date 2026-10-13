#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace babble {

// SHA-256 (FIPS 180-4), streamed. Checks downloads and describes recorded
// files; on its own it shows a file arrived intact, not who made it.
class Sha256 {
public:
    Sha256();
    void update(const void *data, size_t n);
    std::array<uint8_t, 32> finish();   // the object is spent afterwards
    std::string finish_hex();

    static std::string hex(std::string_view data);
    // The file's digest as lowercase hex; nullopt if it can't be read.
    static std::optional<std::string> file_hex(const std::filesystem::path &path);

private:
    void block(const uint8_t *p);

    uint32_t h_[8];
    uint8_t buf_[64];
    size_t used_ = 0;
    uint64_t total_ = 0;
};

} // namespace babble
