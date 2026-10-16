#pragma once

#include "ui/canvas.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string_view>
#include <vector>

namespace babble::ui {

// A TrueType font that stb_truetype rasterises, with a glyph cache for each pixel size.
class Font {
public:
    Font();
    ~Font();
    bool load(const std::filesystem::path &ttf);
    bool loaded() const { return !data_.empty(); }

    float measure(std::string_view text, float px) const;
    // Draws the text with the baseline at y and returns the pen position after the text.
    float draw(Canvas &c, float x, float y, std::string_view text, float px, Color color) const;
    // Returns the ascent above the baseline in pixels.
    float ascent(float px) const;
    float cap_height(float px) const { return ascent(px) * 0.7f; }

private:
    struct Glyph {
        int w = 0, h = 0, xoff = 0, yoff = 0;
        float advance = 0;
        std::vector<uint8_t> alpha;
    };
    const Glyph &glyph(uint32_t cp, float px) const;
    float scale(float px) const;

    std::vector<uint8_t> data_;
    struct Info;
    std::unique_ptr<Info> info_;
    mutable std::map<std::pair<uint32_t, int>, Glyph> cache_;   // The key is the code point and px*4.
};

}
