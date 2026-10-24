#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

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

// This is cv2.resize(..., interpolation=cv2.INTER_AREA) for CV_8UC1, for any downscale. Integer factors use the
// OpenCV fast path, which rounds a 2x2 mean half up and other block means half to even.
void resize_area(View src, Mut dst);

// This is a fast stand-in for a non-integer INTER_AREA downscale: INTER_AREA by the largest integer factor that keeps
// the image at least as big as dst (for example 400 -> 100), then INTER_LINEAR to dst (100 -> 96). The non-integer
// INTER_AREA reads every source pixel with fractional weights and costs about 17 times more for 400 -> 96.
// mid is scratch memory, and it is resized as needed.
void resize_area_linear(View src, Mut dst, std::vector<uint8_t> &mid);

// This is cv2.getRectSubPix(src, (dst.width, dst.height), (cx, cy)) for CV_8UC1: a bilinear crop centred on (cx, cy)
// that repeats the edge pixels outside the image. It follows the portable OpenCV code, which the aarch64 build on the
// headset uses. The x86 wheels use IPP instead, which rounds exact ties down and so can differ by 1.
void get_rect_sub_pix(View src, Mut dst, float cx, float cy);

// This is cv2.equalizeHist for CV_8UC1, and src.data can be the same as dst.data.
void equalize_hist(View src, Mut dst);

// This is cv2.flip(src, 1), which mirrors left to right.
void flip_horizontal(View src, Mut dst);

// This is cv2.flip(src, 0), which mirrors top to bottom.
void flip_vertical(View src, Mut dst);

// This is cv2.rotate by quarter_turns_ccw counter-clockwise quarter turns, and dst must have the result size.
void rotate(View src, Mut dst, int quarter_turns_ccw);

}
