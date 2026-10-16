#include "ui/image.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include <stb_image.h>
#pragma GCC diagnostic pop

#include <fstream>
#include <iterator>

namespace babble::ui {

std::optional<Image> load_png(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
    int w = 0, h = 0, channels = 0;
    stbi_uc *px = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &channels, 4);
    if (!px)
        return std::nullopt;
    Image img{w, h, std::vector<uint8_t>(px, px + size_t(w) * h * 4)};
    stbi_image_free(px);
    return img;
}

}
