#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace babble {

// A streamed SHA-256 that shows that a file is intact, but not who made it.
class Sha256 {
public:
    Sha256();
    void update(const void *data, size_t n);
    std::array<uint8_t, 32> finish();   // Do not use the object after this call.
    std::string finish_hex();

    static std::string hex(std::string_view data);
    // Returns the digest of the file as lowercase hex, or nullopt if the file is not readable.
    static std::optional<std::string> file_hex(const std::filesystem::path &path);

private:
    void block(const uint8_t *p);

    uint32_t h_[8];
    uint8_t buf_[64];
    size_t used_ = 0;
    uint64_t total_ = 0;
};

}
