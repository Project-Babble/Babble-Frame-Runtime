#include "model/preprocess.h"

#include <algorithm>

namespace babble {

void resize_gray(const Frame &src, uint8_t *dst, uint32_t dst_w, uint32_t dst_h, uint32_t dst_stride) {
    if (!dst_w || !dst_h || !src.width || !src.height)
        return;
    // Use fixed-point bilinear interpolation with aligned pixel centers.
    const uint32_t sx = (src.width << 16) / dst_w;
    const uint32_t sy = (src.height << 16) / dst_h;
    for (uint32_t y = 0; y < dst_h; y++) {
        int32_t fy = int32_t(y * sy + sy / 2) - 0x8000;
        fy = std::max(fy, 0);
        uint32_t y0 = std::min(uint32_t(fy) >> 16, src.height - 1);
        uint32_t y1 = std::min(y0 + 1, src.height - 1);
        uint32_t wy = (uint32_t(fy) >> 8) & 0xff;
        const uint8_t *r0 = src.data + size_t(y0) * src.stride;
        const uint8_t *r1 = src.data + size_t(y1) * src.stride;
        uint8_t *out = dst + size_t(y) * dst_stride;
        for (uint32_t x = 0; x < dst_w; x++) {
            int32_t fx = int32_t(x * sx + sx / 2) - 0x8000;
            fx = std::max(fx, 0);
            uint32_t x0 = std::min(uint32_t(fx) >> 16, src.width - 1);
            uint32_t x1 = std::min(x0 + 1, src.width - 1);
            uint32_t wx = (uint32_t(fx) >> 8) & 0xff;
            uint32_t top = r0[x0] * (256 - wx) + r0[x1] * wx;
            uint32_t bot = r1[x0] * (256 - wx) + r1[x1] * wx;
            out[x] = uint8_t((top * (256 - wy) + bot * wy + (1u << 15)) >> 16);
        }
    }
}

void preprocess_sbs_gray_u8(const StereoFrame &sf, uint8_t *dst, uint32_t out_h, uint32_t out_w) {
    const uint32_t half = out_w / 2;
    resize_gray(*sf.left, dst, half, out_h, out_w);
    resize_gray(*sf.right, dst + half, half, out_h, out_w);
}

}
