// PipelineBase's hold rule: headset off / feed off publish neutral values once,
// and nothing the pipeline thread finishes afterwards may overwrite them.
#include <doctest/doctest.h>

#include "core/clock.h"
#include "core/metrics.h"
#include "model/fusionnet_mouth.h"
#include "model/pipeline.h"
#include "state/expression_state.h"

using namespace babble;

namespace {

struct ProbePipeline : PipelineBase {
    ProbePipeline(const nlohmann::json &cfg, PipelineDeps deps) : PipelineBase(cfg, std::move(deps), "infM") {}
    bool init() override {
        if (!bind_input())
            return false;
        id = deps_.state.add_signal("jawOpen");
        return true;
    }
    void process(const StereoFrame &) override {}
    std::vector<std::pair<SignalId, Signal>> neutral(uint64_t now) const override { return {{id, {0.0f, 0.0f, now}}}; }
    void reset_session() override { resets++; }
    // What process() does at the end of a frame.
    void emit(float v) {
        std::pair<SignalId, Signal> u{id, Signal{v, 1.0f, mono_ns()}};
        publish(std::span(&u, 1));
    }
    SignalId id{};
    int resets = 0;
};

} // namespace

TEST_CASE("a held pipeline publishes neutral values once and drops frames until released") {
    const nlohmann::json threads = nlohmann::json::object();
    ExpressionState state;
    StatsRegistry stats;
    LatestSlot<StereoFrame> slot;
    ProbePipeline p({{"from", "cam"}},
                    PipelineDeps{"probe", threads, [&](const std::string &) { return &slot; }, state, stats, {}, {}});
    REQUIRE(p.init());
    auto jaw = [&] { return state.snapshot().signals[p.id].value; };

    p.emit(0.7f);
    CHECK(jaw() == doctest::Approx(0.7f));

    p.on_presence(false);   // headset off: neutral, reset
    CHECK(jaw() == 0.0f);
    CHECK(p.resets == 1);
    p.emit(0.9f);           // a frame that was in flight: dropped
    CHECK(jaw() == 0.0f);

    p.set_tracking(false);  // the feed goes off too: neutral again, no harm
    p.on_presence(true);    // headset back on, but the feed is still off
    p.emit(0.5f);
    CHECK(jaw() == 0.0f);

    p.set_tracking(true);   // nothing holds it any more
    p.emit(0.4f);
    CHECK(jaw() == doctest::Approx(0.4f));

    p.on_presence(true);    // already worn: no change, no reset
    CHECK(p.resets == 2);
}

TEST_CASE("mouth preprocessing takes frames larger than the default 400x400") {
    const nlohmann::json threads = nlohmann::json::object();
    ExpressionState state;
    StatsRegistry stats;
    LatestSlot<StereoFrame> slot;
    FusionNetMouth p({{"from", "mouth"}, {"model", "fusionnet_int8_xnnpack_qat_linear.pte"}, {"rotate", 90}},
                     PipelineDeps{"mouth", threads, [&](const std::string &) { return &slot; }, state, stats, {},
                                  BABBLE_MODELS_DIR});
    REQUIRE(p.init());
    std::vector<uint8_t> px(512 * 512, 128);
    auto frame = [&] {
        auto f = std::make_shared<Frame>();
        f->width = f->height = f->stride = 512;
        f->data = px.data();
        f->size = px.size();
        return f;
    };
    StereoFrame sf{frame(), frame(), 0, 0};
    std::vector<float> v0(224 * 224), v1(224 * 224);
    p.prepare(sf, v0.data(), v1.data());   // used to write past its 400x400 scratch buffers
    CHECK(v0[0] >= 0.0f);
    CHECK(v1[224 * 224 - 1] <= 1.0f);
}
