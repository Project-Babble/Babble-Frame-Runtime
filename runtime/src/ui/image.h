#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace babble::ui {

// An RGBA8 image (row 0 at the top), e.g. an icon loaded from a PNG.
struct Image {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
};

// Decodes a PNG with stb_image; nullopt if it can't be read.
std::optional<Image> load_png(const std::filesystem::path &path);

} // namespace babble::ui
