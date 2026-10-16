#pragma once

#include <cstddef>
#include <cstdint>

namespace babble::jpeg {

// Decodes a baseline JPEG to 8-bit grayscale in out, and returns false if the decode fails or the image does not fit.
bool decode_gray(const uint8_t *data, size_t size, uint8_t *out, size_t capacity, int *width, int *height);

}
