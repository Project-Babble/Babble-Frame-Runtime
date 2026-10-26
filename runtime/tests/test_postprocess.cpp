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

}

#if BABBLE_FRUNK_MOUTH
TEST_CASE("mouth post-processing: unfiltered it is the reference calibration") {
    const auto &preds = golden()["mouth_preds"];
    const mouth::Calibration cal = shipped_calibration();
    mouth::Post manual(std::nullopt, mouth::CalibMode::Manual, shipped_calibration());
    mouth::Post automatic(std::nullopt, mouth::CalibMode::Auto, shipped_calibration());
    mouth::AutoCalibrator ref;
    for (size_t f = 0; f < preds.size(); f++) {
        mouth::Vec p;
        for (size_t c = 0; c < mouth::kNumChannels; c++)
            p[c] = preds[f][c].get<float>();
        const mouth::Vec m = manual.process(p, 1.0 / 60), want_m = cal.normalize(p);
        const mouth::Vec a = automatic.process(p, 1.0 / 60), want_a = ref.process(p, true);
        for (size_t c = 0; c < mouth::kNumShapes; c++) {
            CHECK(m[c] == want_m[c]);
            CHECK(a[c] == want_a[c]);
        }
    }
}

TEST_CASE("mouth post-processing: the One Euro filter steadies noise and follows a jaw drop") {
    mouth::Post post(mouth::kOneEuro, mouth::CalibMode::Raw, mouth::Calibration{});
    mouth::Vec p{};
    double sum = 0, sq = 0;
    for (int i = 0; i < 240; i++) {   // The input has 4 s of +-0.05 jitter around 0.3 at 60 Hz.
        p[4] = 0.3f + ((i % 2) ? 0.05f : -0.05f);
        const float y = post.process(p, i ? 1.0 / 60 : 0.0)[4];
        if (i >= 120) {
            sum += y;
            sq += double(y) * y;
        }
    }
    const double mean = sum / 120, sd = std::sqrt(sq / 120 - mean * mean);
    CHECK(mean == doctest::Approx(0.3).epsilon(0.02));
    CHECK(sd < 0.01);   // The standard deviation of the input is 0.05.
    float y = 0;
    for (int i = 0; i < 12; i++) {   // The jaw drops to 0.9, and the output must follow within 200 ms.
        p[4] = 0.9f;
        y = post.process(p, 1.0 / 60)[4];
    }
    CHECK(y > 0.85f);
}
#endif

TEST_CASE("eye lid calibration and pupil gating match eye_osc_pc.py") {
    const auto &g = golden()["eye"];
    const double hz = g["hz"].get<double>();
    const int every = g["pupil_every"].get<int>();
    std::vector<eye::LidCalibrator> calib;
    std::vector<eye::PupilTracker> trk;
    for (int e = 0; e < 2; e++) {
        calib.emplace_back(size_t(20 * hz), size_t(3 * hz));
        trk.emplace_back(0.4, 0.1, int(0.1 * hz), std::nullopt, 0.6, size_t(std::max(10, 60 * 5)), size_t(5));
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
            // Only the pupil liveness must match the reference, because the shown diameter uses a different filter.
            auto r = trk[e].update(o[e], y);
            CHECK(r.live == expected[i]["live"][e].get<bool>());
        }
    }
}

TEST_CASE("one_euro_params: settings, true for the defaults, false for none") {
    const OneEuroParams d{1.0, 2.0, 3.0};
    CHECK(one_euro_params(json{{"beta", 7.5}}, d) == OneEuroParams{1.0, 7.5, 3.0});
    CHECK(one_euro_params(json(true), d) == d);
    CHECK_FALSE(one_euro_params(json(false), d));
    CHECK(one_euro_params(json::object(), d) == d);
}

TEST_CASE("the eye openness filter steadies the rest and still shows a blink") {
    // Test the lid default with jitter at neutral (1.0 +- 0.03) and a 150 ms blink at 90 Hz.
    OneEuro f(1, eye::kLidOneEuro);
    const double dt = 1.0 / 90;
    double y = f.apply(1.0, 0.0), sq = 0;
    for (int i = 0; i < 180; i++) {
        y = f.apply(1.0 + ((i % 2) ? 0.03 : -0.03), dt);
        if (i >= 90)
            sq += (y - 1.0) * (y - 1.0);
    }
    CHECK(std::sqrt(sq / 90) < 0.015);   // The jitter of the input is 0.03.
    double low = y;
    for (int i = 0; i < 14; i++)          // The lid is closed (0.1) for about 150 ms.
        low = std::min(low, f.apply(0.1, dt));
    CHECK(low < 0.2);                     // The blink reaches almost closed.
    int frames = 0;
    for (y = low; y < 0.9 && frames < 90; frames++)
        y = f.apply(1.0, dt);
    CHECK(frames <= 6);                   // The lid opens again within about 65 ms.
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
    CHECK(s.count(0.1 * 90.0) == 9);   // The count is int(0.1 * 90), as in the reference.
    CHECK(s.count(30) == 30);
}

TEST_CASE("at 30 Hz the eye filters behave like 90 Hz over the same time") {
    eye::RateScale s{90, 30};
    CHECK(s.k() == doctest::Approx(3.0));
    CHECK(s.count(20 * 90.0) == 600);   // The window is 20 s.
    CHECK(s.count(3 * 90.0) == 90);     // The warm-up is 3 s.
    CHECK(s.count(0.1 * 90.0) == 3);    // The blink hold is 0.1 s.
    CHECK(s.count(30) == 10);           // The lid reference is calculated again every 1/3 s.

    // For lid smoothing, one 30 Hz step is equal to three 90 Hz steps.
    double at90 = 0, at30 = 0;
    for (int i = 0; i < 3; i++)
        at90 += 0.6 * (1.0 - at90);
    at30 += s.alpha(0.6) * (1.0 - at30);
    CHECK(at30 == doctest::Approx(at90).epsilon(1e-12));

    // The pupil display follows a step in pupil size at the same speed per second at both rates, with the rate limit.
    auto follow = [](double hz, double seconds) {
        eye::RateScale r{90, hz};
        eye::PupilTracker t(0.4, 0.1, int(r.count(9)), eye::kPupilOneEuroMm, 0.06, 10, 5);
        const double small = 3.0, large = 6.0, dt = 1.0 / hz;   // These are eye kit diameters in mm.
        auto res = t.update(1.0, small, r.k());
        for (int i = 0; i < int(1.0 * hz); i++)   // Let the size settle at the small value with the eye open.
            res = t.update(1.0, small, r.k(), dt);
        res = t.update(1.0, large, r.k(), dt);
        for (int i = 1; i < int(seconds * hz); i++)
            res = t.update(1.0, std::nullopt, r.k(), dt);
        return *res.px;
    };
    const double target = 6.0, start = 3.0;
    for (double sec : {0.1, 0.3, 1.0}) {
        const double p90 = follow(90, sec), p30 = follow(30, sec);
        CAPTURE(sec);
        // The progress towards the new size must be the same within a few percent of the step.
        CHECK(std::fabs(p90 - p30) < 0.03 * (target - start));
    }
}

TEST_CASE("lid calibrator keeps its samples when retimed") {
    eye::LidCalibrator c(1800, 270);
    for (int i = 0; i < 400; i++)
        c.update(0.9);
    CHECK(c.ready());
    c.set_timing(600, 90, 10);   // The rate changed to 30 Hz.
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
    r.tick(t + 5'000'000'000ull);   // A 5 s pause is not an interval.
    CHECK(r.hz() == doctest::Approx(30).epsilon(0.01));
}

namespace {

// Feeds a constant raw openness for `seconds` at `hz` and returns the last output.
eye::OpennessCalibrator::Out hold(eye::OpennessCalibrator &c, double raw, double seconds, double hz = 90) {
    eye::OpennessCalibrator::Out out;
    for (int i = 0; i < int(seconds * hz); i++)
        out = c.update(raw, 1.0 / hz);
    return out;
}

eye::OpennessCalibrator seeded(double neutral) {
    eye::OpennessCalibrator c;
    hold(c, neutral, 4.0);   // The calibrator skips 1 s, warms up for 2 s and then runs.
    REQUIRE(c.ready());
    return c;
}

}

TEST_CASE("openness baseline: seeds from the warm-up and reads 1 at neutral") {
    eye::OpennessCalibrator c;
    auto first = hold(c, 0.7, 0.5);
    CHECK_FALSE(first.ready);
    // The value passes through without calibration until the calibrator has a seed.
    CHECK(first.openness == doctest::Approx(0.7));
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
    for (int i = 0; i < 900; i++) {   // The input is 10 s at 0.85, which is 1.21x neutral.
        auto out = c.update(0.85, 1.0 / 90);
        if (i > 45)   // Wait until the smoothing catches up.
            min_widen = std::min(min_widen, out.widen);
    }
    CHECK(c.baseline() == doctest::Approx(b).epsilon(0.01));
    CHECK(min_widen > 0.95);   // The value does not erode during the hold.
}

TEST_CASE("openness baseline: blinks don't move it, slow drift is followed") {
    auto c = seeded(0.7);
    for (int k = 0; k < 20; k++) {   // Make 20 blinks of 150 ms, one each second.
        hold(c, 0.7, 0.85);
        hold(c, 0.05, 0.15);
    }
    CHECK(c.baseline() == doctest::Approx(0.7).epsilon(0.005));

    for (int s = 0; s < 60; s++)     // The value increases by 10% over a minute, as from a strap shift.
        hold(c, 0.7 * (1.0 + 0.1 * (s + 1) / 60.0), 1.0);
    hold(c, 0.77, 60);
    CHECK(c.baseline() == doctest::Approx(0.77).epsilon(0.02));
}

TEST_CASE("openness ceiling: seed first, then the person's own wide peak") {
    auto c = seeded(0.7);
    hold(c, 0.7, 5);
    CHECK(c.ceiling() == doctest::Approx(1.15));
    // The seed scales a mild widening that comes before a peak.
    auto mild = hold(c, 0.7 * 1.075, 1);
    CHECK(mild.widen == doctest::Approx(0.5).epsilon(0.05));
    hold(c, 0.7, 2);
    // A sustained wide at 1.21x raises the ceiling, so the same wide later reads about 1.
    hold(c, 0.85, 2);
    CHECK(c.ceiling() == doctest::Approx(0.85 / 0.7).epsilon(0.02));
    hold(c, 0.7, 3);
    CHECK(hold(c, 0.85, 1).widen > 0.95);
    // A single-frame spike does not move the ceiling.
    const double before = c.ceiling();
    c.update(3.0, 1.0 / 90);
    hold(c, 0.7, 1);
    CHECK(c.ceiling() == doctest::Approx(before));
    hold(c, 3.0, 1);
    CHECK(c.ceiling() <= 1.6);
}

TEST_CASE("openness closed level: seed first, then the bottom of the person's own blinks") {
    eye::OpennessCalibrator::Params p;
    p.closed_seed = 0.3;
    eye::OpennessCalibrator c(p);
    hold(c, 0.7, 4);
    REQUIRE(c.ready());
    CHECK(c.closed() == doctest::Approx(0.3));
    // At the seed, a squint at 0.65x neutral is half closed and anything at 0.3x or below is closed.
    CHECK(hold(c, 0.7 * 0.65, 0.2).lid == doctest::Approx(0.5).epsilon(0.01));
    CHECK(hold(c, 0.7 * 0.3, 0.2).lid == doctest::Approx(0.0));
    CHECK(hold(c, 0.7, 0.5).lid == doctest::Approx(1.0));
    // Blinks that reach 0.1x neutral lower the closed level, so that the same blinks later read fully closed.
    for (int k = 0; k < 5; k++) {
        hold(c, 0.7, 0.85);
        hold(c, 0.07, 0.15);
    }
    CHECK(c.closed() == doctest::Approx(0.1).epsilon(0.01));
    CHECK(hold(c, 0.07, 0.15).lid == doctest::Approx(0.0));
    CHECK(hold(c, 0.7 * 0.55, 0.2).lid == doctest::Approx(0.5).epsilon(0.01));
    // A single-frame dip does not move the closed level.
    hold(c, 0.7, 1);
    c.update(0.0, 1.0 / 90);
    hold(c, 0.7, 1);
    CHECK(c.closed() == doctest::Approx(0.1).epsilon(0.01));
    // Widen is not changed by the closed level.
    CHECK(hold(c, 0.7 * 1.075, 1).widen == doctest::Approx(0.5).epsilon(0.05));
}

TEST_CASE("openness closed level: one deep squeeze does not stop normal closes from reading closed") {
    eye::OpennessCalibrator::Params p;
    p.closed_seed = 0.3;
    eye::OpennessCalibrator c(p);
    hold(c, 0.7, 4);
    for (int k = 0; k < 5; k++) {
        hold(c, 0.7, 0.85);
        hold(c, 0.14, 0.3);   // Normal closes reach 0.2x neutral.
    }
    hold(c, 0.7, 0.85);
    hold(c, 0.0, 0.5);        // One hard squeeze reaches 0.
    hold(c, 0.7, 0.85);
    CHECK(c.closed() == doctest::Approx(0.2).epsilon(0.01));
    CHECK(hold(c, 0.14, 0.3).lid == doctest::Approx(0.0));
}

TEST_CASE("openness closed level: goes up for a person whose closed eye reads above the seed") {
    eye::OpennessCalibrator::Params p;
    p.closed_seed = 0.3;
    eye::OpennessCalibrator c(p);
    hold(c, 0.7, 4);
    CHECK(hold(c, 0.7 * 0.4, 0.3).lid > 0.1);   // At the seed, 0.4x neutral is not closed.
    for (int k = 0; k < 5; k++) {
        hold(c, 0.7, 0.85);
        hold(c, 0.7 * 0.4, 0.3);
    }
    CHECK(c.closed() == doctest::Approx(0.4).epsilon(0.01));
    CHECK(hold(c, 0.7 * 0.4, 0.3).lid == doctest::Approx(0.0));
}

TEST_CASE("openness closed level: a single-frame dip does not move it at 24 updates/s") {
    eye::RateScale r{90, 24};
    eye::OpennessCalibrator::Params p;
    p.closed_seed = 0.3;
    p.skip = r.count(90);
    p.warmup = r.count(180);
    p.every = r.count(12);
    p.peak_window = r.count(9);
    p.trough_window = r.count(5);
    eye::OpennessCalibrator c(p);
    hold(c, 0.7, 4, 24);
    REQUIRE(c.ready());
    for (int k = 0; k < 5; k++) {
        hold(c, 0.7, 1, 24);
        c.update(0.0, 1.0 / 24);
    }
    hold(c, 0.7, 1, 24);
    CHECK(c.closed() == doctest::Approx(0.3));
}

TEST_CASE("openness closed level: a seed of 0 keeps Lid equal to the openness") {
    auto c = seeded(0.7);
    for (int k = 0; k < 5; k++) {
        hold(c, 0.7, 0.85);
        hold(c, 0.07, 0.15);
    }
    CHECK(c.closed() == doctest::Approx(0.0));
    CHECK(hold(c, 0.35, 0.3).lid == doctest::Approx(0.5).epsilon(0.01));
}

TEST_CASE("openness calibration behaves the same over time at 30 Hz") {
    eye::RateScale r{90, 30};
    eye::OpennessCalibrator::Params p;
    p.skip = r.count(90);
    p.warmup = r.count(180);
    p.every = r.count(12);
    p.peak_window = r.count(9);
    eye::OpennessCalibrator c(p, eye::kLidOneEuro);
    hold(c, 0.7, 4, 30);
    REQUIRE(c.ready());
    hold(c, 0.7, 10, 30);
    const double b = c.baseline();
    auto out = hold(c, 0.85, 5, 30);
    CHECK(c.baseline() == doctest::Approx(b).epsilon(0.01));
    CHECK(out.widen > 0.95);
}

TEST_CASE("openness ceiling never exceeds the model's saturation, per eye") {
    // Two eyes with different neutrals both reach Widen 1 at raw 1.0 (saturation).
    for (double neutral : {0.72, 0.9}) {
        CAPTURE(neutral);
        auto c = seeded(neutral);
        hold(c, neutral, 5);
        CHECK(c.ceiling() <= 1.0 / neutral + 1e-9);
        hold(c, 1.0, 2);
        CHECK(hold(c, 1.0, 1).widen > 0.95);
        // Glitches above saturation cannot raise the ceiling past it.
        hold(c, 1.4, 1);
        hold(c, neutral, 2);
        CHECK(c.ceiling() * c.baseline() <= 1.0 + 1e-9);
        CHECK(hold(c, 1.0, 1).widen > 0.95);
    }
    // A neutral at saturation still gets a small span, so there is never a division by about 0.
    auto c = seeded(0.99);
    CHECK(c.ceiling() >= 1.02);
}
