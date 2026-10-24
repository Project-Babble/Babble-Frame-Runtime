#include <doctest/doctest.h>

#include "model/image_ops.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace babble;

#ifndef BABBLE_TEST_DATA_DIR
#error "BABBLE_TEST_DATA_DIR must point at runtime/tests/data"
#endif

namespace {

struct Pgm {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> px;
    img::View view() const { return {px.data(), w, h, w}; }
};

// Reads a binary PGM (P5, maxval 255) and skips comment lines.
Pgm read_pgm(const std::string &name) {
    std::ifstream in(std::string(BABBLE_TEST_DATA_DIR) + "/" + name, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), name);
    std::string magic;
    in >> magic;
    REQUIRE(magic == "P5");
    auto next_int = [&] {
        int v;
        while (!(in >> v)) {
            in.clear();
            std::string line;
            std::getline(in, line);
        }
        return v;
    };
    Pgm p;
    p.w = uint32_t(next_int());
    p.h = uint32_t(next_int());
    REQUIRE(next_int() == 255);
    in.get();
    p.px.resize(size_t(p.w) * p.h);
    in.read(reinterpret_cast<char *>(p.px.data()), std::streamsize(p.px.size()));
    REQUIRE(in.gcount() == std::streamsize(p.px.size()));
    return p;
}

// Returns the number of pixels that are different and the largest difference.
std::pair<size_t, int> diff(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
    size_t n = 0;
    int worst = 0;
    for (size_t i = 0; i < a.size(); i++) {
        int d = std::abs(int(a[i]) - int(b[i]));
        if (d) {
            n++;
            worst = std::max(worst, d);
        }
    }
    return {n, worst};
}

}

TEST_CASE("image ops match OpenCV 5.0.0 bit for bit") {
    for (const char *name : {"noise", "mouth", "eye"}) {
        CAPTURE(name);
        Pgm in = read_pgm(std::string(name) + "_in.pgm");

        Pgm want = read_pgm(std::string(name) + "_linear224.pgm");
        std::vector<uint8_t> got(224 * 224);
        img::resize_linear(in.view(), {got.data(), 224, 224, 224});
        auto [n1, w1] = diff(got, want.px);
        CHECK_MESSAGE(n1 == 0, "INTER_LINEAR: " << n1 << " px differ, max " << w1);

        Pgm want_eq = read_pgm(std::string(name) + "_linear224_eq.pgm");
        std::vector<uint8_t> eq(224 * 224);
        img::equalize_hist(want.view(), {eq.data(), 224, 224, 224});
        auto [n2, w2] = diff(eq, want_eq.px);
        CHECK_MESSAGE(n2 == 0, "equalizeHist: " << n2 << " px differ, max " << w2);

        Pgm want_area = read_pgm(std::string(name) + "_area160.pgm");
        std::vector<uint8_t> area(160 * 160);
        img::resize_area(in.view(), {area.data(), 160, 160, 160});
        auto [n3, w3] = diff(area, want_area.px);
        CHECK_MESSAGE(n3 == 0, "INTER_AREA: " << n3 << " px differ, max " << w3);

        // The resize from 400 to 96 for the combined eye model has a ratio of 4.1667, which is not an integer.
        Pgm want_area96 = read_pgm(std::string(name) + "_area96.pgm");
        std::vector<uint8_t> area96(96 * 96);
        img::resize_area(in.view(), {area96.data(), 96, 96, 96});
        auto [n4, w4] = diff(area96, want_area96.px);
        CHECK_MESSAGE(n4 == 0, "INTER_AREA 96: " << n4 << " px differ, max " << w4);
    }
}

TEST_CASE("integer INTER_AREA and the area + linear resize match OpenCV 5.0.0 bit for bit") {
    for (const char *name : {"noise", "mouth", "eye"}) {
        CAPTURE(name);
        Pgm in = read_pgm(std::string(name) + "_in.pgm");

        // 400 -> 100 is a 4x block mean, which OpenCV rounds half to even.
        Pgm want100 = read_pgm(std::string(name) + "_area100.pgm");
        std::vector<uint8_t> a100(100 * 100);
        img::resize_area(in.view(), {a100.data(), 100, 100, 100});
        auto [n1, w1] = diff(a100, want100.px);
        CHECK_MESSAGE(n1 == 0, "INTER_AREA 4x: " << n1 << " px differ, max " << w1);

        // The eye kit stage 1 input is the 4x area resize and then INTER_LINEAR to 96.
        Pgm want96 = read_pgm(std::string(name) + "_arealin96.pgm");
        std::vector<uint8_t> a96(96 * 96), mid;
        img::resize_area_linear(in.view(), {a96.data(), 96, 96, 96}, mid);
        auto [n2, w2] = diff(a96, want96.px);
        CHECK_MESSAGE(n2 == 0, "area 100 + linear 96: " << n2 << " px differ, max " << w2);

        // 128 -> 64 is a 2x2 mean, which the OpenCV SIMD path rounds half up.
        Pgm want64 = read_pgm(std::string(name) + "_area64.pgm");
        std::vector<uint8_t> a64(64 * 64);
        img::resize_area({in.px.data() + 100 * 400 + 100, 128, 128, 400}, {a64.data(), 64, 64, 64});
        auto [n3, w3] = diff(a64, want64.px);
        CHECK_MESSAGE(n3 == 0, "INTER_AREA 2x: " << n3 << " px differ, max " << w3);
    }
}

TEST_CASE("get_rect_sub_pix matches the headset OpenCV 5.0.0 bit for bit") {
    std::ifstream f(std::string(BABBLE_TEST_DATA_DIR) + "/subpix_golden.json");
    REQUIRE(f.good());
    auto g = nlohmann::json::parse(f);
    const uint32_t size = g["size"];
    const auto centres = g["centres"].get<std::vector<std::array<float, 2>>>();
    REQUIRE(!centres.empty());
    for (const char *name : {"noise", "eye"}) {
        Pgm in = read_pgm(std::string(name) + "_in.pgm");
        for (size_t k = 0; k < centres.size(); k++) {
            CAPTURE(name);
            CAPTURE(k);
            Pgm want = read_pgm(std::string(name) + "_sub" + std::to_string(k) + ".pgm");
            std::vector<uint8_t> got(size_t(size) * size);
            img::get_rect_sub_pix(in.view(), {got.data(), size, size, size}, centres[k][0], centres[k][1]);
            auto [n, w] = diff(got, want.px);
            CHECK_MESSAGE(n == 0, "centre (" << centres[k][0] << ", " << centres[k][1] << "): " << n << " px differ, max " << w);
        }
    }
}

TEST_CASE("flips and rotations") {
    const uint8_t src[6] = {1, 2, 3, 4, 5, 6};   // The image is 3 wide and 2 high.
    img::View v{src, 3, 2, 3};
    uint8_t out[6];

    img::flip_horizontal(v, {out, 3, 2, 3});
    CHECK(std::vector<uint8_t>(out, out + 6) == std::vector<uint8_t>{3, 2, 1, 6, 5, 4});
    img::flip_vertical(v, {out, 3, 2, 3});
    CHECK(std::vector<uint8_t>(out, out + 6) == std::vector<uint8_t>{4, 5, 6, 1, 2, 3});
    img::rotate(v, {out, 3, 2, 3}, 2);
    CHECK(std::vector<uint8_t>(out, out + 6) == std::vector<uint8_t>{6, 5, 4, 3, 2, 1});
    // A 90° counter-clockwise rotation makes the right column the top row.
    img::rotate(v, {out, 2, 3, 2}, 1);
    CHECK(std::vector<uint8_t>(out, out + 6) == std::vector<uint8_t>{3, 6, 2, 5, 1, 4});
    img::rotate(v, {out, 2, 3, 2}, 3);
    CHECK(std::vector<uint8_t>(out, out + 6) == std::vector<uint8_t>{4, 1, 5, 2, 6, 3});
}
