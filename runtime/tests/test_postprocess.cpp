#include <doctest/doctest.h>

#include "model/postprocess/eye_post.h"
#if BABBLE_FRUNK_MOUTH
#include "model/postprocess/mouth_post.h"
#endif

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

#if BABBLE_FRUNK_MOUTH
mouth::Calibration shipped_calibration() {
    std::ifstream f(std::string(BABBLE_MODELS_DIR) + "/../calibration_mouth.json");
    return mouth::Calibration::from_json(json::parse(f));
}
#endif

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

#if BABBLE_FRUNK_MOUTH
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
#endif

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

TEST_CASE("rate scaling is the identity at the reference rate") {
    eye::RateScale s{90, 90};
    CHECK(s.k() == 1.0);
    CHECK(s.alpha(0.6) == 0.6);
    CHECK(s.alpha(0.2) == 0.2);
    CHECK(s.limit(0.6) == 0.6);
    CHECK(s.count(20 * 90.0) == 1800);
    CHECK(s.count(3 * 90.0) == 270);
    CHECK(s.count(0.1 * 90.0) == 9);   // int(0.1 * 90) like the reference
    CHECK(s.count(30) == 30);
}

TEST_CASE("at 30 Hz the eye filters behave like 90 Hz over the same time") {
    eye::RateScale s{90, 30};
    CHECK(s.k() == doctest::Approx(3.0));
    CHECK(s.count(20 * 90.0) == 600);   // 20 s window
    CHECK(s.count(3 * 90.0) == 90);     // 3 s warm-up
    CHECK(s.count(0.1 * 90.0) == 3);    // 0.1 s blink hold
    CHECK(s.count(30) == 10);           // lid reference recomputed every 1/3 s

    // Lid smoothing: one 30 Hz step equals three 90 Hz steps.
    double at90 = 0, at30 = 0;
    for (int i = 0; i < 3; i++)
        at90 += 0.6 * (1.0 - at90);
    at30 += s.alpha(0.6) * (1.0 - at30);
    CHECK(at30 == doctest::Approx(at90).epsilon(1e-12));

    // Pupil display: a step in the pupil size is followed at the same speed
    // (per second) at both rates, rate limit included.
    auto follow = [](double hz, double seconds) {
        eye::RateScale r{90, hz};
        eye::PupilTracker t(0.4, 0.1, int(r.count(9)), 0.2, 0.6, 10, 5);
        const double small = 0.5, large = 2.0;   // model outputs (area / 3000)
        auto res = t.update(1.0, small, r.k());
        for (int i = 0; i < int(1.0 * hz); i++)   // settle at the small size, eye open
            res = t.update(1.0, small, r.k());
        res = t.update(1.0, large, r.k());
        for (int i = 1; i < int(seconds * hz); i++)
            res = t.update(1.0, std::nullopt, r.k());
        return *res.px;
    };
    const double target = eye::PupilTracker::px_of(2.0), start = eye::PupilTracker::px_of(0.5);
    for (double sec : {0.1, 0.3, 1.0}) {
        const double p90 = follow(90, sec), p30 = follow(30, sec);
        CAPTURE(sec);
        // Same progress towards the new size within a couple of percent of the step.
        CHECK(std::fabs(p90 - p30) < 0.03 * (target - start));
    }
}

TEST_CASE("lid calibrator keeps its samples when retimed") {
    eye::LidCalibrator c(1800, 270);
    for (int i = 0; i < 400; i++)
        c.update(0.9);
    CHECK(c.ready());
    c.set_timing(600, 90, 10);   // switched to 30 Hz
    CHECK(c.ready());
    CHECK(c.ref() == doctest::Approx(0.9));
    CHECK(c.update(0.9) == doctest::Approx(1.0));
}

TEST_CASE("update rate follows frame intervals and skips pauses") {
    eye::UpdateRate r(90);
    uint64_t t = 1'000'000'000;
    for (int i = 0; i < 300; i++, t += 33'333'333)
        r.tick(t);
    CHECK(r.hz() == doctest::Approx(30).epsilon(0.01));
    r.tick(t + 5'000'000'000ull);   // a 5 s pause is not an interval
    CHECK(r.hz() == doctest::Approx(30).epsilon(0.01));
}

namespace {

// Feeds a constant raw openness for `seconds` at `hz`; returns the last output.
eye::OpennessCalibrator::Out hold(eye::OpennessCalibrator &c, double raw, double seconds, double hz = 90,
                                  double alpha = 0.6) {
    eye::OpennessCalibrator::Out out;
    for (int i = 0; i < int(seconds * hz); i++)
        out = c.update(raw, alpha);
    return out;
}

eye::OpennessCalibrator seeded(double neutral) {
    eye::OpennessCalibrator c;
    hold(c, neutral, 4.0);   // 1 s skipped, 2 s warm-up, then running
    REQUIRE(c.ready());
    return c;
}

} // namespace

TEST_CASE("openness baseline: seeds from the warm-up and reads 1 at neutral") {
    eye::OpennessCalibrator c;
    auto first = hold(c, 0.7, 0.5);
    CHECK_FALSE(first.ready);
    CHECK(first.openness == doctest::Approx(0.7));   // uncalibrated pass-through until seeded
    auto out = hold(c, 0.7, 3.5);
    REQUIRE(out.ready);
    CHECK(c.baseline() == doctest::Approx(0.7));
    CHECK(out.openness == doctest::Approx(1.0).epsilon(0.001));
    CHECK(out.widen == doctest::Approx(0.0));
}

TEST_CASE("openness baseline: a low neutral calibrates (no absolute 0.6 gate)") {
    auto c = seeded(0.5);
    auto out = hold(c, 0.5, 5);
    CHECK(c.baseline() == doctest::Approx(0.5));
    CHECK(out.openness == doctest::Approx(1.0).epsilon(0.001));
}

TEST_CASE("openness baseline: a 10 s wide hold keeps the neutral and keeps Widen up") {
    auto c = seeded(0.7);
    hold(c, 0.7, 20);
    const double b = c.baseline();
    double min_widen = 1;
    for (int i = 0; i < 900; i++) {   // 10 s at 0.85 (1.21x neutral)
        auto out = c.update(0.85, 0.6);
        if (i > 45)   // after the smoothing has caught up
            min_widen = std::min(min_widen, out.widen);
    }
    CHECK(c.baseline() == doctest::Approx(b).epsilon(0.01));
    CHECK(min_widen > 0.95);   // no erosion over the hold
}

TEST_CASE("openness baseline: blinks don't move it, slow drift is followed") {
    auto c = seeded(0.7);
    for (int k = 0; k < 20; k++) {   // 20 blinks of 150 ms every second
        hold(c, 0.7, 0.85);
        hold(c, 0.05, 0.15);
    }
    CHECK(c.baseline() == doctest::Approx(0.7).epsilon(0.005));

    for (int s = 0; s < 60; s++)     // 10% higher over a minute (a strap shift)
        hold(c, 0.7 * (1.0 + 0.1 * (s + 1) / 60.0), 1.0);
    hold(c, 0.77, 60);
    CHECK(c.baseline() == doctest::Approx(0.77).epsilon(0.02));
}

TEST_CASE("openness ceiling: seed first, then the person's own wide peak") {
    auto c = seeded(0.7);
    hold(c, 0.7, 5);
    CHECK(c.ceiling() == doctest::Approx(1.15));
    // A mild widening before any peak is scaled by the seed.
    auto mild = hold(c, 0.7 * 1.075, 1);
    CHECK(mild.widen == doctest::Approx(0.5).epsilon(0.05));
    hold(c, 0.7, 2);
    // A sustained wide at 1.21x raises the ceiling; the same wide later reads ~1.
    hold(c, 0.85, 2);
    CHECK(c.ceiling() == doctest::Approx(0.85 / 0.7).epsilon(0.02));
    hold(c, 0.7, 3);
    CHECK(hold(c, 0.85, 1).widen > 0.95);
    // A single-frame spike doesn't move it; the cap bounds glitches.
    const double before = c.ceiling();
    c.update(3.0, 1.0);
    hold(c, 0.7, 1);
    CHECK(c.ceiling() == doctest::Approx(before));
    hold(c, 3.0, 1);
    CHECK(c.ceiling() <= 1.6);
}

TEST_CASE("openness calibration behaves the same over time at 30 Hz") {
    eye::RateScale r{90, 30};
    eye::OpennessCalibrator::Params p;
    p.skip = r.count(90);
    p.warmup = r.count(180);
    p.every = r.count(12);
    p.peak_window = r.count(9);
    eye::OpennessCalibrator c(p);
    hold(c, 0.7, 4, 30, r.alpha(0.6));
    REQUIRE(c.ready());
    hold(c, 0.7, 10, 30, r.alpha(0.6));
    const double b = c.baseline();
    auto out = hold(c, 0.85, 5, 30, r.alpha(0.6));
    CHECK(c.baseline() == doctest::Approx(b).epsilon(0.01));
    CHECK(out.widen > 0.95);
}

TEST_CASE("openness ceiling never exceeds the model's saturation, per eye") {
    // Two eyes with different neutrals: at raw 1.0 (saturation) both reach Widen 1.
    for (double neutral : {0.72, 0.9}) {
        CAPTURE(neutral);
        auto c = seeded(neutral);
        hold(c, neutral, 5);
        CHECK(c.ceiling() <= 1.0 / neutral + 1e-9);
        hold(c, 1.0, 2);
        CHECK(hold(c, 1.0, 1).widen > 0.95);
        // Glitches above saturation can't raise the ceiling past it.
        hold(c, 1.4, 1);
        hold(c, neutral, 2);
        CHECK(c.ceiling() * c.baseline() <= 1.0 + 1e-9);
        CHECK(hold(c, 1.0, 1).widen > 0.95);
    }
    // A neutral at saturation still gets a small span, never a division by ~0.
    auto c = seeded(0.99);
    CHECK(c.ceiling() >= 1.02);
}
