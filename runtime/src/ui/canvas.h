#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace babble::ui {

struct Image;

struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 255;
    static constexpr Color hex(uint32_t rgb, uint8_t a = 255) {
        return {uint8_t(rgb >> 16), uint8_t(rgb >> 8), uint8_t(rgb), a};
    }
    Color with_alpha(uint8_t alpha) const { return {r, g, b, alpha}; }
};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

// An RGBA8 image that the CPU draws on, with anti-aliased shapes from signed distances.
class Canvas {
public:
    Canvas(int w, int h);
    int width() const { return w_; }
    int height() const { return h_; }
    const uint8_t *data() const { return px_.data(); }

    void clear(Color c);
    void fill_rect(Rect r, Color c, float radius = 0);
    void stroke_rect(Rect r, Color c, float radius, float width);
    void fill_circle(float cx, float cy, float radius, Color c);
    void line(float x0, float y0, float x1, float y1, float width, Color c);   // The line has round caps.
    // Draws a settings cog with its centre at (cx, cy) and an outer radius of r.
    void cog(float cx, float cy, float r, Color c);
    // Blends an RGBA image 1:1 with its top-left corner at (x, y) and rounded corners.
    void image(const Image &img, int x, int y, float radius = 0);

    // Blends c over one pixel with coverage 0..1.
    void blend(int x, int y, Color c, float coverage);

private:
    // Blends each pixel in the box by the coverage from sdf, and skips sdf where inner(x, y) is true.
    template <typename Sdf, typename Inner>
    void fill_sdf(float x0, float y0, float x1, float y1, Color c, Sdf sdf, Inner inner) {
        const int ix0 = std::max(0, int(std::floor(x0 - 1))), iy0 = std::max(0, int(std::floor(y0 - 1)));
        const int ix1 = std::min(w_, int(std::ceil(x1 + 1))), iy1 = std::min(h_, int(std::ceil(y1 + 1)));
        for (int y = iy0; y < iy1; y++) {
            uint8_t *row = &px_[size_t(y) * w_ * 4];
            for (int x = ix0; x < ix1; x++) {
                if (inner(x, y)) {
                    put(row + x * 4, c, c.a);
                    continue;
                }
                const float cov = std::clamp(0.5f - sdf(x + 0.5f, y + 0.5f), 0.f, 1.f);
                if (cov > 0)
                    put(row + x * 4, c, int(cov * c.a + 0.5f));
            }
        }
    }
    template <typename Sdf>
    void fill_sdf(float x0, float y0, float x1, float y1, Color c, Sdf sdf) {
        fill_sdf(x0, y0, x1, y1, c, sdf, [](int, int) { return false; });
    }
    // Blends c over one pixel with an integer alpha of 0..255 that is exact at 0 and 255.
    static void put(uint8_t *p, Color c, int a) {
        if (a >= 255) {
            p[0] = c.r;
            p[1] = c.g;
            p[2] = c.b;
            return;
        }
        p[0] = uint8_t(p[0] + ((c.r - p[0]) * a + 127) / 255);
        p[1] = uint8_t(p[1] + ((c.g - p[1]) * a + 127) / 255);
        p[2] = uint8_t(p[2] + ((c.b - p[2]) * a + 127) / 255);
    }

    int w_, h_;
    std::vector<uint8_t> px_;
};

}
