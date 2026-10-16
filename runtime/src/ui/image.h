#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace babble::ui {

// An RGBA8 image with row 0 at the top.
struct Image {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
};

// Decodes a PNG with stb_image and returns nullopt if it cannot read the file.
std::optional<Image> load_png(const std::filesystem::path &path);

}
