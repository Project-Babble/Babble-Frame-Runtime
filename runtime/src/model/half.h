#pragma once

// float32 <-> IEEE binary16 for fp16 model inputs/outputs (round to nearest
// even, like torch's .half()). Stored as uint16_t so buffers don't depend on
// compiler support for _Float16 in headers.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace babble {

inline void to_half(const float *src, uint16_t *dst, size_t n) {
    for (size_t i = 0; i < n; i++) {
        _Float16 h = static_cast<_Float16>(src[i]);
        memcpy(&dst[i], &h, 2);
    }
}

inline void from_half(const uint16_t *src, float *dst, size_t n) {
    for (size_t i = 0; i < n; i++) {
        _Float16 h;
        memcpy(&h, &src[i], 2);
        dst[i] = static_cast<float>(h);
    }
}

} // namespace babble
