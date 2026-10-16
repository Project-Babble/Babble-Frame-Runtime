#pragma once

#include <cstddef>
#include <cstdint>

namespace babble::img {

// This is a borrowed 8-bit single-channel image.
struct View {
    const uint8_t *data = nullptr;
    uint32_t width = 0, height = 0, stride = 0;
};

// This is a writable 8-bit single-channel image.
struct Mut {
    uint8_t *data = nullptr;
    uint32_t width = 0, height = 0, stride = 0;
    operator View() const { return {data, width, height, stride}; }
};

// These operations give the same bits as OpenCV because the models were trained on OpenCV frames.

// This is cv2.resize(..., interpolation=cv2.INTER_LINEAR) for CV_8UC1.
void resize_linear(View src, Mut dst);

// This is cv2.resize(..., interpolation=cv2.INTER_AREA) for CV_8UC1, for a downscale by a non-integer factor.
void resize_area(View src, Mut dst);

// This is cv2.equalizeHist for CV_8UC1, and src.data can be the same as dst.data.
void equalize_hist(View src, Mut dst);

// This is cv2.flip(src, 1), which mirrors left to right.
void flip_horizontal(View src, Mut dst);

// This is cv2.flip(src, 0), which mirrors top to bottom.
void flip_vertical(View src, Mut dst);

// This is cv2.rotate by quarter_turns_ccw counter-clockwise quarter turns, and dst must have the result size.
void rotate(View src, Mut dst, int quarter_turns_ccw);

}
