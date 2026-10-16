#include "camera/usb/jpeg.h"

#include <cstring>

// This file has its own static copy of stb_image with only the JPEG decoder.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include <stb_image.h>
#pragma GCC diagnostic pop

namespace babble::jpeg {

bool decode_gray(const uint8_t *data, size_t size, uint8_t *out, size_t capacity, int *width, int *height) {
    int w = 0, h = 0, channels = 0;
    stbi_uc *px = stbi_load_from_memory(data, int(size), &w, &h, &channels, 1);
    if (!px)
        return false;
    const bool fits = size_t(w) * size_t(h) <= capacity;
    if (fits)
        memcpy(out, px, size_t(w) * size_t(h));
    stbi_image_free(px);
    *width = w;
    *height = h;
    return fits;
}

}
