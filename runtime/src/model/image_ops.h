#pragma once

#include <cstddef>
#include <cstdint>

namespace babble::img {

// A borrowed 8-bit single-channel image.
struct View {
    const uint8_t *data = nullptr;
    uint32_t width = 0, height = 0, stride = 0;
};

// A writable 8-bit single-channel image.
struct Mut {
    uint8_t *data = nullptr;
    uint32_t width = 0, height = 0, stride = 0;
    operator View() const { return {data, width, height, stride}; }
};

// Operations that reproduce OpenCV (opencv-python 5.0.0) bit for bit, because
// the models were trained and validated on OpenCV-preprocessed frames.
// Verified against goldens in runtime/tests/data (tools/golden/make_golden.py).

// cv2.resize(..., interpolation=cv2.INTER_LINEAR) for CV_8UC1.
void resize_linear(View src, Mut dst);

// cv2.resize(..., interpolation=cv2.INTER_AREA) for CV_8UC1, downscaling by a
// non-integer factor (OpenCV's general area path; integer factors use a
// different fast path in OpenCV and are not needed here).
void resize_area(View src, Mut dst);

// cv2.equalizeHist for CV_8UC1. In place is allowed (src.data == dst.data).
void equalize_hist(View src, Mut dst);

// cv2.flip(src, 1): mirror left-right.
void flip_horizontal(View src, Mut dst);

// cv2.flip(src, 0): mirror top-bottom.
void flip_vertical(View src, Mut dst);

// cv2.rotate with ROTATE_90_COUNTERCLOCKWISE (quarter_turns_ccw = 1),
// ROTATE_180 (2) or ROTATE_90_CLOCKWISE (3); 0 copies. dst must be sized for the result.
void rotate(View src, Mut dst, int quarter_turns_ccw);

} // namespace babble::img
