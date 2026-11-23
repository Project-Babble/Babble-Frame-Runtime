#include <doctest/doctest.h>

#include "model/eye_kit.h"

#include "core/clock.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

using namespace babble;

namespace {

// Reads a binary PGM (P5, maxval 255) with no comment lines, as tools/golden/make_eye_kit_golden.py writes it.
std::vector<uint8_t> read_pgm(const std::string &name, uint32_t &w, uint32_t &h) {
    std::ifstream in(std::string(BABBLE_TEST_DATA_DIR) + "/" + name, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), name);
    std::string magic;
    int maxval = 0;
    in >> magic >> w >> h >> maxval;
    REQUIRE(magic == "P5");
    REQUIRE(maxval == 255);
    in.get();
    std::vector<uint8_t> px(size_t(w) * h);
    in.read(reinterpret_cast<char *>(px.data()), std::streamsize(px.size()));
    REQUIRE(in.gcount() == std::streamsize(px.size()));
    return px;
}

}

TEST_CASE("EyeKit runs both stages on real frames and matches the fp32 PyTorch chain") {
    ExecuTorchRunner::set_threads(1);
    EyeKit kit;
    REQUIRE(kit.load(std::string(BABBLE_MODELS_DIR) + "/eye_kit_stage1_fp16.pte",
                     std::string(BABBLE_MODELS_DIR) + "/eye_kit_stage2_fp16.pte"));
    CHECK(kit.res() == 96);
    CHECK(kit.crop_res() == 48);
    CHECK(kit.crop_px() == 96);
    CHECK(kit.fp16());
    CHECK(kit.has_widen());
    CHECK(kit.has_squint());
    CHECK_FALSE(kit.has_history());

    std::ifstream f(std::string(BABBLE_TEST_DATA_DIR) + "/eye_kit_golden.json");
    REQUIRE(f.good());
    const auto pairs = nlohmann::json::parse(f)["pairs"];
    REQUIRE(pairs.size() >= 1);
    for (size_t k = 0; k < pairs.size(); k++) {
        CAPTURE(k);
        uint32_t w[2], h[2];
        std::vector<uint8_t> px[2];
        for (int v = 0; v < 2; v++)
            px[v] = read_pgm("eyekit_pair" + std::to_string(k) + "_v" + std::to_string(v) + ".pgm", w[v], h[v]);
        std::array<img::View, 2> views{img::View{px[0].data(), w[0], h[0], w[0]}, img::View{px[1].data(), w[1], h[1], w[1]}};
        EyeKit::Result r;
        EyeKit::Timing t;
        // For a temporal stage 1, the reference history is the frame's own embedding at each lag (0.1 s and 0.25 s).
        // The same frame at 0 s and at 0.15 s gives that history to the run at 0.25 s.
        kit.reset_history();
        REQUIRE(kit.run(views, r, 0));
        REQUIRE(kit.run(views, r, 150000000));
        REQUIRE(kit.run(views, r, 250000000, &t));
        CHECK(t.infer1 > 0);
        CHECK(t.infer2 > 0);
        const auto &g = pairs[k];
        for (int v = 0; v < 2; v++) {
            CAPTURE(v);
            // The C++ runs fp16 programs and the reference is fp32 PyTorch with x86 OpenCV, whose getRectSubPix can
            // differ by 1 at ties. On the device the fp16 chain was within lid 0.003, 0.7 px and 0.07 mm of fp32.
            CHECK(std::fabs(r.lid[v] - g["lid"][v].get<double>()) <= 0.01);
            // The centre of a closed eye has no pupil to find, so it can move more (2 px with v9).
            const double centre_tol = g["lid"][v].get<double>() < 0.1 ? 3.0 : 1.5;
            CHECK(std::hypot(r.x[v] - g["x"][v].get<double>(), r.y[v] - g["y"][v].get<double>()) <= centre_tol);
            CHECK(std::fabs(r.mm[v] - g["mm"][v].get<double>()) <= 0.15);
            CHECK(std::fabs(r.widen[v] - g["widen"][v].get<double>()) <= 0.02);
            CHECK(std::fabs(r.squint[v] - g["squint"][v].get<double>()) <= 0.02);
            CHECK(r.mm[v] >= EyeKit::kMmOffset);
            CHECK(r.mm[v] <= EyeKit::kMmOffset + EyeKit::kMmScale);
        }
    }
}

TEST_CASE("EyeKit refuses models that do not match the stage contracts") {
    ExecuTorchRunner::set_threads(1);
    EyeKit kit;
    // The face model has a [1,1,224,224] input and a [1,45] output, so it is not a stage 1 model.
    CHECK_FALSE(kit.load(std::string(BABBLE_MODELS_DIR) + "/face_mono_224_fp16.pte",
                         std::string(BABBLE_MODELS_DIR) + "/eye_kit_stage2_fp16.pte"));
    // The stage 1 model has one input, so it is not a stage 2 model.
    CHECK_FALSE(kit.load(std::string(BABBLE_MODELS_DIR) + "/eye_kit_stage1_fp16.pte",
                         std::string(BABBLE_MODELS_DIR) + "/eye_kit_stage1_fp16.pte"));
    // The v9 stage 1 has a [2,36] second input, so it is not a stage 2 model.
    const std::string temporal = std::string(BABBLE_TEST_DATA_DIR) + "/eye_kit_v9_stage1_fp16.pte";
    CHECK_FALSE(kit.load(temporal, temporal));
    // The v9 stage 1 history is [2,2*(16+2)], so it needs two lags.
    CHECK_FALSE(kit.load(temporal,
                         std::string(BABBLE_MODELS_DIR) + "/eye_kit_stage2_fp16.pte", 0, {0.1}));
    CHECK_FALSE(kit.load(temporal,
                         std::string(BABBLE_MODELS_DIR) + "/eye_kit_stage2_fp16.pte", 0, {0.1, 0.2, 0.3}));
    CHECK_FALSE(kit.load(temporal,
                         std::string(BABBLE_MODELS_DIR) + "/eye_kit_stage2_fp16.pte", 0, {}));
    CHECK(kit.load(temporal,
                   std::string(BABBLE_MODELS_DIR) + "/eye_kit_stage2_fp16.pte"));
}

TEST_CASE("EyeKit uses the history only when a stored frame is near each lag") {
    ExecuTorchRunner::set_threads(1);
    EyeKit kit;
    REQUIRE(kit.load(std::string(BABBLE_TEST_DATA_DIR) + "/eye_kit_v9_stage1_fp16.pte",
                     std::string(BABBLE_MODELS_DIR) + "/eye_kit_stage2_fp16.pte"));
    REQUIRE(kit.has_history());
    uint32_t w[2], h[2];
    std::vector<uint8_t> px[2];
    for (int v = 0; v < 2; v++)
        px[v] = read_pgm("eyekit_pair0_v" + std::to_string(v) + ".pgm", w[v], h[v]);
    std::array<img::View, 2> views{img::View{px[0].data(), w[0], h[0], w[0]}, img::View{px[1].data(), w[1], h[1], w[1]}};
    auto lid_at = [&](std::initializer_list<uint64_t> times) {
        EyeKit::Result r;
        kit.reset_history();
        for (uint64_t t : times)
            REQUIRE(kit.run(views, r, t));
        return r.lid;
    };
    const auto none = lid_at({0});
    // A frame 2 s ago is too old for both lags, so the result is the same as with no history.
    const auto stale = lid_at({0, 2000000000});
    const auto full = lid_at({0, 150000000, 250000000});
    // The history restarts after reset_history().
    const auto again = lid_at({0});
    for (int v = 0; v < 2; v++) {
        CHECK(stale[v] == doctest::Approx(none[v]).epsilon(1e-6));
        CHECK(again[v] == doctest::Approx(none[v]).epsilon(1e-6));
        CHECK(std::isfinite(full[v]));
    }
    // A clock step back starts a new history.
    EyeKit::Result r;
    kit.reset_history();
    REQUIRE(kit.run(views, r, 150000000));
    REQUIRE(kit.run(views, r, 250000000));
    REQUIRE(kit.run(views, r, 0));
    for (int v = 0; v < 2; v++)
        CHECK(r.lid[v] == doctest::Approx(none[v]).epsilon(1e-6));
}

// Run on the headset with: taskset -c 4 ./runtime/babble_tests -tc="eye kit timing*" --no-skip -s
TEST_CASE("eye kit timing on real frames" * doctest::skip()) {
    ExecuTorchRunner::set_threads(1);
    EyeKit kit;
    REQUIRE(kit.load(std::string(BABBLE_MODELS_DIR) + "/eye_kit_stage1_fp16.pte",
                     std::string(BABBLE_MODELS_DIR) + "/eye_kit_stage2_fp16.pte"));
    uint32_t w[2], h[2];
    std::vector<uint8_t> px[2];
    for (int v = 0; v < 2; v++)
        px[v] = read_pgm("eyekit_pair0_v" + std::to_string(v) + ".pgm", w[v], h[v]);
    std::array<img::View, 2> views{img::View{px[0].data(), w[0], h[0], w[0]}, img::View{px[1].data(), w[1], h[1], w[1]}};
    constexpr int kRuns = 2000;
    auto median_ms = [](std::vector<uint64_t> v) {
        std::sort(v.begin(), v.end());
        return double(v[v.size() / 2]) / 1e6;
    };

    const uint32_t res = kit.res();
    std::vector<uint8_t> small(size_t(res) * res), mid;
    std::vector<uint64_t> area, area_lin;
    for (int i = 0; i < kRuns; i++) {
        uint64_t t0 = mono_ns();
        for (int v = 0; v < 2; v++)
            img::resize_area(views[v], {small.data(), res, res, res});
        uint64_t t1 = mono_ns();
        for (int v = 0; v < 2; v++)
            img::resize_area_linear(views[v], {small.data(), res, res, res}, mid);
        uint64_t t2 = mono_ns();
        area.push_back(t1 - t0);
        area_lin.push_back(t2 - t1);
    }

    EyeKit::Result r;
    std::vector<uint64_t> total, p1, i1, p2, i2;
    for (int i = 0; i < 50; i++)
        kit.run(views, r, uint64_t(i) * 11000000);
    for (int i = 0; i < kRuns; i++) {
        EyeKit::Timing t;
        uint64_t t0 = mono_ns();
        REQUIRE(kit.run(views, r, uint64_t(50 + i) * 11000000, &t));
        total.push_back(mono_ns() - t0);
        p1.push_back(t.prep1);
        i1.push_back(t.infer1);
        p2.push_back(t.prep2);
        i2.push_back(t.infer2);
    }
    MESSAGE("both eyes, median of " << kRuns << " runs (ms): resize_area 400->" << res << " " << median_ms(area)
            << " | resize_area_linear 400->100->" << res << " " << median_ms(area_lin) << " || eye kit: stage 1 resize "
            << median_ms(p1) << ", stage 1 " << median_ms(i1) << ", crops " << median_ms(p2) << ", stage 2 "
            << median_ms(i2) << ", whole chain " << median_ms(total));
}
