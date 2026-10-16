#pragma once

#include "frame/frame.h"

#include <cstdint>

namespace babble {

// Resize one gray frame into a dst region with bilinear interpolation.
void resize_gray(const Frame &src, uint8_t *dst, uint32_t dst_w, uint32_t dst_h, uint32_t dst_stride);

// Resize the left and right views and put them side by side in an out_h x out_w u8 buffer.
void preprocess_sbs_gray_u8(const StereoFrame &sf, uint8_t *dst, uint32_t out_h, uint32_t out_w);

}
