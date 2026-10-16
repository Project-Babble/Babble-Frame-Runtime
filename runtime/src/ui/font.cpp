#include "ui/font.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stb_truetype.h>
#pragma GCC diagnostic pop

#include <cmath>
#include <fstream>
#include <iterator>

namespace babble::ui {

struct Font::Info {
    stbtt_fontinfo f;
};

namespace {

// Decodes the next UTF-8 code point and changes invalid bytes to U+FFFD.
uint32_t next_cp(std::string_view s, size_t &i) {
    auto b = static_cast<unsigned char>(s[i++]);
    if (b < 0x80)
        return b;
    int n = b >= 0xf0 ? 3 : b >= 0xe0 ? 2 : b >= 0xc0 ? 1 : -1;
    if (n < 0 || i + n > s.size())
        return 0xfffd;
    uint32_t cp = b & (0x3f >> n);
    for (int k = 0; k < n; k++)
        cp = (cp << 6) | (static_cast<unsigned char>(s[i++]) & 0x3f);
    return cp;
}

}

Font::Font() = default;
Font::~Font() = default;

bool Font::load(const std::filesystem::path &ttf) {
    std::ifstream in(ttf, std::ios::binary);
    if (!in)
        return false;
    data_.assign(std::istreambuf_iterator<char>(in), {});
    info_ = std::make_unique<Info>();
    if (!stbtt_InitFont(&info_->f, data_.data(), stbtt_GetFontOffsetForIndex(data_.data(), 0))) {
        data_.clear();
        return false;
    }
    cache_.clear();
    return true;
}

float Font::scale(float px) const { return stbtt_ScaleForMappingEmToPixels(&info_->f, px); }

float Font::ascent(float px) const {
    if (!loaded())
        return 0;
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&info_->f, &asc, &desc, &gap);
    return asc * scale(px);
}

const Font::Glyph &Font::glyph(uint32_t cp, float px) const {
    auto key = std::make_pair(cp, int(std::lround(px * 4)));
    auto it = cache_.find(key);
    if (it != cache_.end())
        return it->second;
    Glyph g;
    const float s = scale(px);
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&info_->f, int(cp), &adv, &lsb);
    g.advance = adv * s;
    unsigned char *bmp = stbtt_GetCodepointBitmap(&info_->f, s, s, int(cp), &g.w, &g.h, &g.xoff, &g.yoff);
    if (bmp) {
        g.alpha.assign(bmp, bmp + size_t(g.w) * g.h);
        stbtt_FreeBitmap(bmp, nullptr);
    }
    return cache_.emplace(key, std::move(g)).first->second;
}

float Font::measure(std::string_view text, float px) const {
    if (!loaded())
        return 0;
    float x = 0;
    uint32_t prev = 0;
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = next_cp(text, i);
        if (prev)
            x += stbtt_GetCodepointKernAdvance(&info_->f, int(prev), int(cp)) * scale(px);
        x += glyph(cp, px).advance;
        prev = cp;
    }
    return x;
}

float Font::draw(Canvas &c, float x, float y, std::string_view text, float px, Color color) const {
    if (!loaded())
        return x;
    uint32_t prev = 0;
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = next_cp(text, i);
        if (prev)
            x += stbtt_GetCodepointKernAdvance(&info_->f, int(prev), int(cp)) * scale(px);
        const Glyph &g = glyph(cp, px);
        const int gx = int(std::lround(x)) + g.xoff, gy = int(std::lround(y)) + g.yoff;
        for (int row = 0; row < g.h; row++)
            for (int col = 0; col < g.w; col++)
                if (uint8_t a = g.alpha[size_t(row) * g.w + col])
                    c.blend(gx + col, gy + row, color, a / 255.f);
        x += g.advance;
        prev = cp;
    }
    return x;
}

}
