#pragma once

#include "frame/frame.h"

#include <cstdint>

namespace babble {

// Resizes one gray frame into a dst region (bilinear).
void resize_gray(const Frame &src, uint8_t *dst, uint32_t dst_w, uint32_t dst_h, uint32_t dst_stride);

// "sbs_gray_u8": left and right views resized to out_h x (out_w/2) each and
// placed side by side in an out_h x out_w u8 buffer.
void preprocess_sbs_gray_u8(const StereoFrame &sf, uint8_t *dst, uint32_t out_h, uint32_t out_w);

} // namespace babble
