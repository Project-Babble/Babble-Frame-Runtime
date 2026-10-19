#include "ui/canvas.h"

#include "ui/image.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace babble::ui {

namespace {

float round_box_sdf(float px, float py, Rect r, float radius) {
    radius = std::min({radius, r.w / 2, r.h / 2});
    float cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    float qx = std::fabs(px - cx) - (r.w / 2 - radius);
    float qy = std::fabs(py - cy) - (r.h / 2 - radius);
    float ox = std::max(qx, 0.f), oy = std::max(qy, 0.f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - radius;
}

}

Canvas::Canvas(int w, int h) : w_(w), h_(h), px_(size_t(w) * h * 4) {}

void Canvas::clear(Color c) {
    const size_t row = size_t(w_) * 4;
    for (size_t i = 0; i < row; i += 4) {
        px_[i] = c.r;
        px_[i + 1] = c.g;
        px_[i + 2] = c.b;
        px_[i + 3] = 255;
    }
    for (int y = 1; y < h_; y++)
        memcpy(&px_[y * row], px_.data(), row);
}

void Canvas::blend(int x, int y, Color c, float coverage) {
    if (x < 0 || y < 0 || x >= w_ || y >= h_ || coverage <= 0)
        return;
    put(&px_[(size_t(y) * w_ + x) * 4], c, int(std::min(coverage, 1.f) * c.a + 0.5f));
}

void Canvas::fill_rect(Rect r, Color c, float radius) {
    radius = std::min({radius, r.w / 2, r.h / 2});
    // These bounds hold the whole pixels inside the rect that are clear of the corners.
    const float ix0 = r.x + radius, ix1 = r.x + r.w - radius, iy0 = r.y + radius, iy1 = r.y + r.h - radius;
    const float ox0 = r.x, ox1 = r.x + r.w, oy0 = r.y, oy1 = r.y + r.h;
    fill_sdf(
        r.x, r.y, r.x + r.w, r.y + r.h, c, [&](float x, float y) { return round_box_sdf(x, y, r, radius); },
        [&](int x, int y) {
            const bool in_x = x >= ox0 && x + 1 <= ox1, in_y = y >= oy0 && y + 1 <= oy1;
            return in_x && in_y && ((x >= ix0 && x + 1 <= ix1) || (y >= iy0 && y + 1 <= iy1));
        });
}

void Canvas::stroke_rect(Rect r, Color c, float radius, float width) {
    fill_sdf(r.x, r.y, r.x + r.w, r.y + r.h, c, [&](float x, float y) {
        float d = round_box_sdf(x, y, r, radius);
        return std::max(d, -d - width);
    });
}

void Canvas::fill_circle(float cx, float cy, float radius, Color c) {
    const float inner2 = std::max(radius - 0.75f, 0.f) * std::max(radius - 0.75f, 0.f);
    fill_sdf(
        cx - radius, cy - radius, cx + radius, cy + radius, c,
        [&](float x, float y) { return std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)) - radius; },
        [&](int x, int y) {
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            return dx * dx + dy * dy <= inner2;
        });
}

void Canvas::line(float x0, float y0, float x1, float y1, float width, Color c) {
    const float hw = width / 2, dx = x1 - x0, dy = y1 - y0, len2 = std::max(dx * dx + dy * dy, 1e-6f);
    fill_sdf(std::min(x0, x1) - hw, std::min(y0, y1) - hw, std::max(x0, x1) + hw, std::max(y0, y1) + hw, c,
             [&](float x, float y) {
                 float t = std::clamp(((x - x0) * dx + (y - y0) * dy) / len2, 0.f, 1.f);
                 return std::hypot(x - (x0 + t * dx), y - (y0 + t * dy)) - hw;
             });
}

void Canvas::image(const Image &img, int x, int y, float radius) {
    const Rect r{float(x), float(y), float(img.width), float(img.height)};
    const float rad = std::min({radius, r.w / 2, r.h / 2});
    const int x0 = int(std::clamp<int64_t>(-int64_t(x), 0, img.width));
    const int x1 = int(std::clamp<int64_t>(int64_t(w_) - x, 0, img.width));
    const int y0 = int(std::clamp<int64_t>(-int64_t(y), 0, img.height));
    const int y1 = int(std::clamp<int64_t>(int64_t(h_) - y, 0, img.height));
    for (int iy = y0; iy < y1; iy++)
        for (int ix = x0; ix < x1; ix++) {
            const uint8_t *s = &img.rgba[(size_t(iy) * img.width + ix) * 4];
            if (!s[3])
                continue;
            // The coverage is the rounded-corner coverage multiplied by the alpha of the image.
            float cov = 1.0f;
            if (rad > 0 && (ix + 0.5f < rad || ix + 0.5f > r.w - rad) &&
                (iy + 0.5f < rad || iy + 0.5f > r.h - rad))
                cov = std::clamp(0.5f - round_box_sdf(x + ix + 0.5f, y + iy + 0.5f, r, rad), 0.f, 1.f);
            blend(x + ix, y + iy, Color{s[0], s[1], s[2], s[3]}, cov);
        }
}

void Canvas::cog(float cx, float cy, float r, Color c) {
    // The cog is a ring with eight rounded teeth and a hole at the hub.
    constexpr int kTeeth = 8;
    const float body = r * 0.74f, hole = r * 0.34f, tooth_w = r * 0.42f;
    fill_sdf(cx - r, cy - r, cx + r, cy + r, c, [&](float x, float y) {
        float px = x - cx, py = y - cy;
        float dist = std::hypot(px, py);
        // Fold the angle into one tooth sector, then measure the distance to a box along +x.
        float sector = 2 * std::numbers::pi_v<float> / kTeeth;
        float a = std::atan2(py, px);
        a = std::fmod(a + sector * 0.5f + 8 * std::numbers::pi_v<float>, sector) - sector * 0.5f;
        float lx = dist * std::cos(a), ly = dist * std::sin(a);
        float tooth = round_box_sdf(lx, ly, Rect{body - r * 0.1f, -tooth_w / 2, r - body + r * 0.1f, tooth_w}, r * 0.08f);
        float outer = std::min(dist - body, tooth);
        return std::max(outer, hole - dist);
    });
}

}
