#include "model/image_ops.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace babble::img {

namespace {

// This is cvRound, which rounds half to even as the OpenCV lrint does.
inline int round_even(double v) { return int(std::nearbyint(v)); }
inline uint8_t sat_u8(int v) { return uint8_t(std::clamp(v, 0, 255)); }

constexpr int kCoefBits = 11;              // This is INTER_RESIZE_COEF_BITS.
constexpr int kCoefScale = 1 << kCoefBits; // This is INTER_RESIZE_COEF_SCALE.

// This table holds the source index and fixed-point weights for each destination coordinate, as OpenCV does.
struct LinearTab {
    std::vector<int> ofs;
    std::vector<int16_t> alpha;   // There are 2 weights for each destination coordinate.
    int limit;                    // A coordinate at or above limit reads one source sample.
};

LinearTab linear_tab(uint32_t ssize, uint32_t dsize) {
    LinearTab t;
    t.ofs.resize(dsize);
    t.alpha.resize(dsize * 2);
    t.limit = int(dsize);
    const double scale = double(ssize) / double(dsize);
    for (uint32_t d = 0; d < dsize; d++) {
        float f = float((d + 0.5) * scale - 0.5);
        int s = int(std::floor(f));
        f -= float(s);
        if (s < 0) {
            f = 0;
            s = 0;
        }
        if (s + 1 >= int(ssize)) {
            t.limit = std::min(t.limit, int(d));
            if (s >= int(ssize) - 1) {
                f = 0;
                s = int(ssize) - 1;
            }
        }
        t.ofs[d] = s;
        t.alpha[d * 2] = int16_t(round_even((1.f - f) * kCoefScale));
        t.alpha[d * 2 + 1] = int16_t(round_even(f * kCoefScale));
    }
    return t;
}

// This is one entry of the OpenCV computeResizeAreaTab table.
struct AreaTab {
    int di, si;
    float alpha;
};

std::vector<AreaTab> area_tab(int ssize, int dsize, double scale) {
    std::vector<AreaTab> tab;
    for (int dx = 0; dx < dsize; dx++) {
        double fsx1 = dx * scale;
        double fsx2 = fsx1 + scale;
        double cell = std::min(scale, ssize - fsx1);
        int sx1 = int(std::ceil(fsx1)), sx2 = int(std::floor(fsx2));
        sx2 = std::min(sx2, ssize - 1);
        sx1 = std::min(sx1, sx2);
        if (sx1 - fsx1 > 1e-3)
            tab.push_back({dx, sx1 - 1, float((sx1 - fsx1) / cell)});
        for (int sx = sx1; sx < sx2; sx++)
            tab.push_back({dx, sx, float(1.0 / cell)});
        if (fsx2 - sx2 > 1e-3)
            tab.push_back({dx, sx2, float(std::min(std::min(fsx2 - sx2, 1.), cell) / cell)});
    }
    return tab;
}

}

void resize_linear(View src, Mut dst) {
    const LinearTab xt = linear_tab(src.width, dst.width);
    const LinearTab yt = linear_tab(src.height, dst.height);
    const int dw = int(dst.width);

    // Resize each source row horizontally into int buffers, as the OpenCV HResizeLinear does.
    auto hresize = [&](int sy, std::vector<int> &row) {
        const uint8_t *S = src.data + size_t(sy) * src.stride;
        int dx = 0;
        for (; dx < xt.limit; dx++) {
            int sx = xt.ofs[dx];
            row[dx] = S[sx] * xt.alpha[dx * 2] + S[sx + 1] * xt.alpha[dx * 2 + 1];
        }
        for (; dx < dw; dx++)
            row[dx] = S[xt.ofs[dx]] * kCoefScale;
    };

    std::vector<int> r0(dw), r1(dw);
    int have0 = -1, have1 = -1;
    for (uint32_t dy = 0; dy < dst.height; dy++) {
        int sy0 = yt.ofs[dy];
        int sy1 = std::min(sy0 + 1, int(src.height) - 1);
        // Use the resized rows again for the next destination rows.
        if (have0 != sy0) {
            if (have1 == sy0) {
                std::swap(r0, r1);
                std::swap(have0, have1);
            } else {
                hresize(sy0, r0);
                have0 = sy0;
            }
        }
        if (have1 != sy1) {
            hresize(sy1, r1);
            have1 = sy1;
        }
        const int b0 = yt.alpha[dy * 2], b1 = yt.alpha[dy * 2 + 1];
        uint8_t *D = dst.data + size_t(dy) * dst.stride;
        // Use the exact rounding of the OpenCV VResizeLinear.
        for (int x = 0; x < dw; x++)
            D[x] = uint8_t((((b0 * (r0[x] >> 4)) >> 16) + ((b1 * (r1[x] >> 4)) >> 16) + 2) >> 2);
    }
}

namespace {

// This is the OpenCV resizeAreaFast for a downscale by integer factors kx and ky.
void resize_area_int(View src, Mut dst, uint32_t kx, uint32_t ky) {
    const float scale = 1.f / float(kx * ky);
    const bool half_up = kx == 2 && ky == 2;   // The OpenCV SIMD path for 2x2 rounds half up; saturate_cast rounds half to even.
    std::vector<int> col(size_t(dst.width) * kx);   // These are the column sums of the ky source rows of one output row.
    for (uint32_t dy = 0; dy < dst.height; dy++) {
        std::fill(col.begin(), col.end(), 0);
        for (uint32_t y = 0; y < ky; y++) {   // This plain loop is vectorised by the compiler.
            const uint8_t *S = src.data + size_t(dy * ky + y) * src.stride;
            for (size_t x = 0; x < col.size(); x++)
                col[x] += S[x];
        }
        uint8_t *D = dst.data + size_t(dy) * dst.stride;
        const uint32_t area = kx * ky;
        if (kx == 4 && ky == 4) {   // The 400 -> 100 eye resize. sum / 16 is exact, so round half to even in integers.
            for (uint32_t dx = 0; dx < dst.width; dx++) {
                const int *c = &col[size_t(dx) * 4];
                const int sum = c[0] + c[1] + c[2] + c[3], q = sum >> 4, r = sum & 15;
                D[dx] = uint8_t(q + (r > 8 || (r == 8 && (q & 1))));
            }
            continue;
        }
        if (half_up) {   // The 128 -> 64 crop resize.
            for (uint32_t dx = 0; dx < dst.width; dx++)
                D[dx] = uint8_t((col[size_t(dx) * 2] + col[size_t(dx) * 2 + 1] + 2) >> 2);
            continue;
        }
        for (uint32_t dx = 0; dx < dst.width; dx++) {
            int sum = 0;
            for (uint32_t x = 0; x < kx; x++)
                sum += col[size_t(dx) * kx + x];
            D[dx] = area == 1 ? uint8_t(sum) : sat_u8(round_even(float(sum) * scale));
        }
    }
}

}

void resize_area(View src, Mut dst) {
    if (src.width % dst.width == 0 && src.height % dst.height == 0) {
        resize_area_int(src, dst, src.width / dst.width, src.height / dst.height);
        return;
    }
    const double sx = double(src.width) / dst.width, sy = double(src.height) / dst.height;
    const auto xtab = area_tab(int(src.width), int(dst.width), sx);
    const auto ytab = area_tab(int(src.height), int(dst.height), sy);
    const int dw = int(dst.width);

    std::vector<float> buf(dw), sum(dw, 0.f);
    int prev_dy = ytab.empty() ? 0 : ytab[0].di;
    int prev_sy = -1;
    for (const AreaTab &yt : ytab) {
        if (yt.si != prev_sy) {
            const uint8_t *S = src.data + size_t(yt.si) * src.stride;
            std::fill(buf.begin(), buf.end(), 0.f);
            for (const AreaTab &xt : xtab)
                buf[xt.di] += float(S[xt.si]) * xt.alpha;
            prev_sy = yt.si;
        }
        if (yt.di != prev_dy) {
            uint8_t *D = dst.data + size_t(prev_dy) * dst.stride;
            for (int x = 0; x < dw; x++) {
                D[x] = sat_u8(round_even(sum[x]));
                sum[x] = yt.alpha * buf[x];
            }
            prev_dy = yt.di;
        } else {
            for (int x = 0; x < dw; x++)
                sum[x] += yt.alpha * buf[x];
        }
    }
    uint8_t *D = dst.data + size_t(prev_dy) * dst.stride;
    for (int x = 0; x < dw; x++)
        D[x] = sat_u8(round_even(sum[x]));
}

void equalize_hist(View src, Mut dst) {
    uint32_t hist[256] = {0};
    for (uint32_t y = 0; y < src.height; y++) {
        const uint8_t *S = src.data + size_t(y) * src.stride;
        for (uint32_t x = 0; x < src.width; x++)
            hist[S[x]]++;
    }
    const uint32_t total = src.width * src.height;
    int i = 0;
    while (i < 255 && !hist[i])
        i++;
    uint8_t lut[256];
    if (hist[i] == total) {
        for (uint32_t y = 0; y < dst.height; y++)
            memset(dst.data + size_t(y) * dst.stride, i, dst.width);
        return;
    }
    const float scale = (256 - 1.f) / float(total - hist[i]);
    int sum = 0;
    for (lut[i++] = 0; i < 256; i++) {
        sum += int(hist[i]);
        lut[i] = sat_u8(round_even(float(sum) * scale));
    }
    for (uint32_t y = 0; y < src.height; y++) {
        const uint8_t *S = src.data + size_t(y) * src.stride;
        uint8_t *D = dst.data + size_t(y) * dst.stride;
        for (uint32_t x = 0; x < src.width; x++)
            D[x] = lut[S[x]];
    }
}

void flip_horizontal(View src, Mut dst) {
    for (uint32_t y = 0; y < src.height; y++) {
        const uint8_t *S = src.data + size_t(y) * src.stride;
        uint8_t *D = dst.data + size_t(y) * dst.stride;
        for (uint32_t x = 0; x < src.width; x++)
            D[x] = S[src.width - 1 - x];
    }
}

void flip_vertical(View src, Mut dst) {
    for (uint32_t y = 0; y < src.height; y++)
        memcpy(dst.data + size_t(y) * dst.stride, src.data + size_t(src.height - 1 - y) * src.stride, src.width);
}

void rotate(View src, Mut dst, int q) {
    q = ((q % 4) + 4) % 4;
    const uint32_t w = src.width, h = src.height;
    for (uint32_t y = 0; y < dst.height; y++) {
        uint8_t *D = dst.data + size_t(y) * dst.stride;
        for (uint32_t x = 0; x < dst.width; x++) {
            uint32_t sx, sy;
            switch (q) {
            case 0: sx = x; sy = y; break;
            case 1: sx = w - 1 - y; sy = x; break;          // This is 90° counter-clockwise.
            case 2: sx = w - 1 - x; sy = h - 1 - y; break;  // This is 180°.
            default: sx = y; sy = h - 1 - x; break;         // This is 90° clockwise.
            }
            D[x] = src.data[size_t(sy) * src.stride + sx];
        }
    }
}

void resize_area_linear(View src, Mut dst, std::vector<uint8_t> &mid) {
    // Use the largest factor k that divides both sides and keeps the middle image at least as big as dst.
    uint32_t k = 1;
    for (uint32_t c = 2; src.width / c >= dst.width && src.height / c >= dst.height; c++)
        if (src.width % c == 0 && src.height % c == 0)
            k = c;
    const uint32_t mw = src.width / k, mh = src.height / k;
    if (k == 1 || (mw == dst.width && mh == dst.height)) {   // No integer step helps, or it is the whole resize.
        resize_area(src, dst);
        return;
    }
    mid.resize(size_t(mw) * mh);
    resize_area(src, {mid.data(), mw, mh, mw});
    resize_linear({mid.data(), mw, mh, mw}, dst);
}

// This is the OpenCV getRectSubPix_Cn_ for uchar with 16-bit fixed-point weights (scale_fixpt, cast_8u) and adjustRect.
void get_rect_sub_pix(View src, Mut dst, float cx, float cy) {
    const int W = int(src.width), H = int(src.height), ww = int(dst.width), wh = int(dst.height);
    const ptrdiff_t step = ptrdiff_t(src.stride);
    cx -= float(ww - 1) * 0.5f;
    cy -= float(wh - 1) * 0.5f;
    const int ix = int(std::floor(cx)), iy = int(std::floor(cy));
    const float a = cx - float(ix), b = cy - float(iy);
    auto fix = [](float w) { return round_even(double(w * float(1 << 16))); };
    const int a11 = fix((1.f - a) * (1.f - b)), a12 = fix(a * (1.f - b)), a21 = fix((1.f - a) * b), a22 = fix(a * b);
    const int b1 = fix(1.f - b), b2 = fix(b);
    auto cast = [](int v) { return uint8_t((v + (1 << 15)) >> 16); };
    const uint8_t *S = src.data;

    if (0 <= ix && ix < W - ww && 0 <= iy && iy < H - wh) {   // The window is inside the image.
        for (int i = 0; i < wh; i++) {
            const uint8_t *r0 = S + (iy + i) * step + ix, *r1 = r0 + step;
            uint8_t *D = dst.data + size_t(i) * dst.stride;
            for (int j = 0; j < ww; j++)
                D[j] = cast(r0[j] * a11 + r0[j + 1] * a12 + r1[j] * a21 + r1[j + 1] * a22);
        }
        return;
    }
    // The window is partly outside, so clamp it as adjustRect does. off is an offset from S, and every read is in the image.
    ptrdiff_t off = 0;
    int rx = 0, rw = ww, ry = 0, rh = wh;
    if (ix >= 0)
        off += ix;
    else
        rx = std::min(-ix, ww);
    if (ix >= W - ww) {
        rw = W - ix - 1;
        if (rw < 0) {
            off += rw;
            rw = 0;
        }
    }
    if (iy >= 0)
        off += iy * step;
    else
        ry = -iy;
    if (iy >= H - wh) {
        rh = H - iy - 1;
        if (rh < 0) {
            off += rh * step;
            rh = 0;
        }
    }
    off -= rx;
    for (int i = 0; i < wh; i++) {
        ptrdiff_t off2 = off + step;
        if (i < ry || i >= rh)   // Rows above and below the image repeat the edge row.
            off2 -= step;
        uint8_t *D = dst.data + size_t(i) * dst.stride;
        std::fill(D, D + rx, cast(S[off + rx] * b1 + S[off2 + rx] * b2));
        std::fill(D + rw, D + ww, cast(S[off + rw] * b1 + S[off2 + rw] * b2));
        for (int j = rx; j < rw; j++)
            D[j] = cast(S[off + j] * a11 + S[off + j + 1] * a12 + S[off2 + j] * a21 + S[off2 + j + 1] * a22);
        if (i < rh)
            off = off2;
    }
}

}
