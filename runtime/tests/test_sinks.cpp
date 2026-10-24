#include <doctest/doctest.h>

#include "core/metrics.h"
#include "sink/babble_osc_sink.h"
#include "sink/osc.h"
#include "sink/sink.h"
#include "state/expression_state.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <thread>

using namespace babble;
using json = nlohmann::json;

TEST_CASE("the sink registry builds known types and rejects unknown ones") {
    json threads = json::object();
    ExpressionState state;
    StatsRegistry stats;

    auto dbg = make_sink(json{{"type", "debug"}}, SinkDeps{"debug", threads, state, stats});
    REQUIRE(dbg);
    CHECK_FALSE(dbg->wants_frames());   // No dump_dir is configured.
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

    // Without an update, the wait returns on timeout with the same sequence.
    CHECK(state.wait_update(now_seq, std::chrono::milliseconds(10)) == now_seq);
}

TEST_CASE("OSC float messages are encoded per the OSC 1.0 spec") {
    uint8_t buf[64];
    // The 8 chars of "/jawOpen" and a NUL pad to 12, ",f" pads to 4, and float 0.5 is 0x3f000000 big-endian.
    size_t n = osc::encode_float("/jawOpen", 0.5f, buf, sizeof(buf));
    REQUIRE(n == 20);
    const uint8_t expect[20] = {'/', 'j', 'a', 'w', 'O', 'p', 'e', 'n', 0, 0, 0, 0, ',', 'f', 0, 0, 0x3f, 0, 0, 0};
    CHECK(memcmp(buf, expect, 20) == 0);
    // Three chars and a NUL make exactly 4 bytes.
    CHECK(osc::encode_float("/ab", 1.0f, buf, sizeof(buf)) == 12);
    CHECK(osc::encode_float("/too-long-for-the-buffer", 1.0f, buf, 8) == 0);
}

TEST_CASE("the osc module sends changed signals to its UDP target") {
    // Make a receiver on an ephemeral loopback port.
    int rx = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(bind(rx, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    getsockname(rx, reinterpret_cast<sockaddr *>(&addr), &len);
    timeval tv{2, 0};
    setsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    json threads = json::object();
    ExpressionState state;
    StatsRegistry stats;
    SignalId jaw = state.add_signal("jawOpen");
    state.add_signal("tongueOut");   // The sink must not send a signal that never got an update.
    SignalId internal = state.add_signal("_LeftEyeLidRaw");   // The sink never sends an internal signal.
    auto sink = make_sink(json{{"type", "osc"}, {"port", ntohs(addr.sin_port)}, {"prefix", "/babble"}},
                          SinkDeps{"osc", threads, state, stats});
    REQUIRE(sink);
    REQUIRE(sink->start());

    std::pair<SignalId, Signal> v[2] = {{internal, Signal{0.5f, 1.0f, 42}}, {jaw, Signal{0.25f, 1.0f, 42}}};
    state.update(v);

    uint8_t buf[256];
    ssize_t n = recv(rx, buf, sizeof(buf), 0);
    timeval short_tv{0, 300000};
    setsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, &short_tv, sizeof(short_tv));
    uint8_t extra[256];
    const ssize_t more = recv(rx, extra, sizeof(extra), 0);   // The internal signal must not follow.
    sink->stop();
    close(rx);
    CHECK(more < 0);
    REQUIRE(n > 0);
    uint8_t expect[64];
    size_t en = osc::encode_float("/babble/jawOpen", 0.25f, expect, sizeof(expect));
    CHECK(size_t(n) == en);
    CHECK(memcmp(buf, expect, en) == 0);
}

TEST_CASE("babble_osc sends Baballonia OSC to the PC address, and waits without one") {
    using Pick = std::optional<std::pair<std::string, std::string>>;
    CHECK(BabbleOscSink::choose("10.0.0.1", "10.0.0.2", "10.0.0.3") == Pick{{"10.0.0.1", "host"}});
    CHECK(BabbleOscSink::choose("", "10.0.0.2", "10.0.0.3") == Pick{{"10.0.0.2", "pc_address"}});
    CHECK(BabbleOscSink::choose("", "", "10.0.0.3") == Pick{{"10.0.0.3", "steam_link"}});
    CHECK(BabbleOscSink::choose("", "", std::nullopt) == std::nullopt);
    // Target "frame" sends to this headset over a PC address, and "auto" uses the headset when no PC is known.
    CHECK(BabbleOscSink::choose("", "10.0.0.2", "10.0.0.3", "frame") == Pick{{"127.0.0.1", "frame"}});
    CHECK(BabbleOscSink::choose("10.0.0.1", "", std::nullopt, "frame") == Pick{{"10.0.0.1", "host"}});
    CHECK(BabbleOscSink::choose("", "", "10.0.0.3", "auto") == Pick{{"10.0.0.3", "steam_link"}});
    CHECK(BabbleOscSink::choose("", "", std::nullopt, "auto") == Pick{{"127.0.0.1", "frame"}});

    int rx = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(bind(rx, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    getsockname(rx, reinterpret_cast<sockaddr *>(&addr), &len);
    timeval tv{2, 0};
    setsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    json threads = json::object();
    ExpressionState state;
    StatsRegistry stats;
    SignalId jaw = state.add_signal("jawOpen");
    auto sink = make_sink(json{{"type", "babble_osc"}, {"port", ntohs(addr.sin_port)}, {"pc_address", "127.0.0.1"}},
                          SinkDeps{"babble_osc", threads, state, stats});
    REQUIRE(sink);
    REQUIRE(sink->start());
    auto *osc_sink = static_cast<BabbleOscSink *>(sink.get());
    for (int i = 0; i < 100 && osc_sink->status().kind.empty(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(osc_sink->status().kind == "pc_address");
    CHECK(osc_sink->status().address == "127.0.0.1");

    std::pair<SignalId, Signal> v{jaw, Signal{0.75f, 1.0f, 42}};
    state.update(std::span(&v, 1));
    uint8_t buf[256];
    const ssize_t n = recv(rx, buf, sizeof(buf), 0);
    REQUIRE(n > 0);
    uint8_t expect[64];
    const size_t en = osc::encode_float("/jawOpen", 0.75f, expect, sizeof(expect));
    CHECK(size_t(n) == en);
    CHECK(memcmp(buf, expect, en) == 0);
    sink->stop();

    // With target "pc" and without an address or a Steam Link session, the module runs but sends nothing.
    auto idle = make_sink(json{{"type", "babble_osc"}, {"port", ntohs(addr.sin_port)}, {"target", "pc"},
                               {"steam_link_log", "/nonexistent/vrlink.txt"}},
                          SinkDeps{"babble_osc", threads, state, stats});
    REQUIRE(idle->start());
    timeval short_tv{0, 300000};
    setsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, &short_tv, sizeof(short_tv));
    v.second = Signal{0.5f, 1.0f, 43};
    state.update(std::span(&v, 1));
    CHECK(recv(rx, buf, sizeof(buf), 0) < 0);
    CHECK(static_cast<BabbleOscSink *>(idle.get())->status().kind.empty());
    idle->stop();
    CHECK_FALSE(make_sink(json{{"type", "babble_osc"}, {"port", 0}}, SinkDeps{"b", threads, state, stats})->start());
    close(rx);
}
