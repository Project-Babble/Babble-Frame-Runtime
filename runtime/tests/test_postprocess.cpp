#include <doctest/doctest.h>

#include "model/postprocess/eye_post.h"
#include "model/postprocess/mouth_post.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>

using namespace babble;
using json = nlohmann::json;

namespace {

const json &golden() {
    static json g = [] {
        std::ifstream f(std::string(BABBLE_TEST_DATA_DIR) + "/post_golden.json");
        return json::parse(f);
    }();
    return g;
}

mouth::Calibration shipped_calibration() {
    std::ifstream f(std::string(BABBLE_MODELS_DIR) + "/../calibration_mouth.json");
    return mouth::Calibration::from_json(json::parse(f));
}

// Compares an optional reference value (null = None) with ours.
void check_opt(const json &want, std::optional<double> got, double tol, const char *what, size_t i, int e) {
    if (want.is_null()) {
        CHECK_MESSAGE(!got, what << " frame " << i << " eye " << e << ": expected None");
    } else {
        REQUIRE_MESSAGE(got, what << " frame " << i << " eye " << e << ": expected " << want.get<double>());
        CHECK_MESSAGE(std::fabs(*got - want.get<double>()) <= tol,
                      what << " frame " << i << " eye " << e << ": " << *got << " vs " << want.get<double>());
    }
}

} // namespace

TEST_CASE("mouth post-processing matches mouth_osc_pc.py --no-presence (manual and auto)") {
    const auto &preds = golden()["mouth_preds"];
    for (const char *mode_name : {"manual", "auto"}) {
        CAPTURE(mode_name);
        mouth::CalibMode mode;
        REQUIRE(mouth::parse_mode(mode_name, &mode));
        mouth::Post post(0.5f, mode, shipped_calibration());
        const auto &expected = golden()[std::string("mouth_") + mode_name]["expected"];
        REQUIRE(expected.size() == preds.size());
        float worst = 0;
        size_t worst_frame = 0;
        for (size_t f = 0; f < preds.size(); f++) {
            mouth::Vec p;
            for (size_t c = 0; c < mouth::kNumChannels; c++)
                p[c] = preds[f][c].get<float>();
            mouth::Vec got = post.process(p);
            for (size_t c = 0; c < mouth::kNumShapes; c++) {
                float d = std::fabs(got[c] - expected[f][c].get<float>());
                if (d > worst) {
                    worst = d;
                    worst_frame = f;
                }
            }
        }
        CHECK_MESSAGE(worst <= 1e-6f, "max |diff| " << worst << " at frame " << worst_frame);
    }
}

TEST_CASE("eye post-processing matches eye_osc_pc.py") {
    const auto &g = golden()["eye"];
    const double hz = g["hz"].get<double>();
    const int every = g["pupil_every"].get<int>();
    std::vector<eye::LidCalibrator> calib;
    std::vector<eye::PupilTracker> trk;
    for (int e = 0; e < 2; e++) {
        calib.emplace_back(size_t(20 * hz), size_t(3 * hz));
        trk.emplace_back(0.4, 0.1, int(0.1 * hz), 0.2, 0.6, size_t(std::max(10, 60 * 5)), size_t(5));
    }
    std::optional<double> sm[2];
    const auto &lids = g["lids"], &pupil = g["pupil"], &expected = g["expected"];
    for (size_t i = 0; i < lids.size(); i++) {
        double o[2];
        for (int e = 0; e < 2; e++) {
            double v = calib[e].update(lids[i][e].get<double>());
            sm[e] = sm[e] ? *sm[e] + 0.6 * (v - *sm[e]) : v;
            o[e] = std::max(*sm[e], 0.0);
            CHECK_MESSAGE(std::fabs(o[e] - expected[i]["o"][e].get<double>()) <= 1e-12, "openness frame " << i);
            CHECK_MESSAGE(std::fabs(calib[e].ref() - expected[i]["ref"][e].get<double>()) <= 1e-12, "ref frame " << i);
        }
        for (int e = 0; e < 2; e++) {
            std::optional<double> y;
            if (int(i) % every == 0)
                y = pupil[i][e].get<double>();
            auto r = trk[e].update(o[e], y);
            check_opt(expected[i]["px"][e], r.px, 1e-9, "px", i, e);
            check_opt(expected[i]["ratio"][e], r.ratio, 1e-9, "ratio", i, e);
            CHECK(r.live == expected[i]["live"][e].get<bool>());
            check_opt(expected[i]["dil"][e], eye::dilation(r, 2.0), 1e-9, "dilation", i, e);
        }
    }
}

TEST_CASE("percentile matches numpy linear interpolation") {
    CHECK(eye::median({3, 1, 2}) == 2.0);
    CHECK(eye::median({4, 1, 2, 3}) == 2.5);
    CHECK(eye::percentile({1, 2, 3, 4, 5}, 25) == doctest::Approx(2.0));
    CHECK(eye::percentile({10, 20}, 90) == doctest::Approx(19.0));
}
