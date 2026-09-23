#include <doctest/doctest.h>

#include "core/metrics.h"
#include "sink/sink.h"
#include "state/expression_state.h"

#include <chrono>
#include <thread>

using namespace babble;
using json = nlohmann::json;

TEST_CASE("the sink registry builds known types and rejects unknown ones") {
    json threads = json::object();
    ExpressionState state;
    StatsRegistry stats;

    auto dbg = make_sink(json{{"type", "debug"}}, SinkDeps{"debug", threads, state, stats});
    REQUIRE(dbg);
    CHECK_FALSE(dbg->wants_frames());   // no dump_dir configured
    CHECK(dbg->start());
    dbg->stop();

    auto dumper = make_sink(json{{"type", "debug"}, {"dump_dir", "/tmp/babble-test-dump"}, {"dump_count", 1}},
                            SinkDeps{"dbg2", threads, state, stats});
    REQUIRE(dumper);
    CHECK(dumper->wants_frames());

    CHECK_FALSE(make_sink(json{{"type", "osc_not_yet"}}, SinkDeps{"x", threads, state, stats}));
}

TEST_CASE("ExpressionState wakes waiting outputs on update") {
    ExpressionState state;
    SignalId jaw = state.add_signal("jawOpen");
    CHECK(state.add_signal("jawOpen") == jaw);

    uint64_t seq = state.snapshot().seq;
    std::thread writer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::pair<SignalId, Signal> v{jaw, Signal{0.5f, 1.0f, 123}};
        state.update(std::span(&v, 1));
    });
    auto t0 = std::chrono::steady_clock::now();
    uint64_t now_seq = state.wait_update(seq, std::chrono::seconds(2));
    writer.join();
    CHECK(now_seq == seq + 1);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1));
    auto snap = state.snapshot();
    CHECK(snap.signals[jaw].value == doctest::Approx(0.5f));

    // No update: returns on timeout with the same sequence.
    CHECK(state.wait_update(now_seq, std::chrono::milliseconds(10)) == now_seq);
}
