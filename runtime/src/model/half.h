#pragma once

// Convert between float32 and IEEE binary16 with round to nearest even, as torch .half() does.

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

}
