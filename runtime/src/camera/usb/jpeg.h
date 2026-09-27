#pragma once

#include <cstddef>
#include <cstdint>

namespace babble::jpeg {

// Decodes a baseline JPEG to 8-bit grayscale into out (capacity bytes). Sets
// width and height; false if it's not a JPEG we can decode or doesn't fit.
bool decode_gray(const uint8_t *data, size_t size, uint8_t *out, size_t capacity, int *width, int *height);

} // namespace babble::jpeg
