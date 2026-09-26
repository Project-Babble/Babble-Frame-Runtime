#include <doctest/doctest.h>

#include "core/metrics.h"
#include "sink/sink.h"
#include "state/expression_state.h"

#include <nlohmann/json.hpp>

#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace babble;
using json = nlohmann::json;

namespace {

// A UDP listener on an ephemeral loopback port.
struct Receiver {
    int fd = -1;
    uint16_t port = 0;
    Receiver() {
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0);
        socklen_t len = sizeof(a);
        getsockname(fd, reinterpret_cast<sockaddr *>(&a), &len);
        port = ntohs(a.sin_port);
        timeval tv{2, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    ~Receiver() { close(fd); }
    std::string next() {
        char buf[4096];
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        return n > 0 ? std::string(buf, size_t(n)) : std::string();
    }
};

void set(ExpressionState &state, SignalId id, float v) {
    std::pair<SignalId, Signal> u{id, Signal{v, 1.0f, 1}};
    state.update(std::span(&u, 1));
}

} // namespace

TEST_CASE("the template plugin loads through the plugin output and receives values") {
    Receiver rx;
    json threads = json::object();
    ExpressionState state;
    StatsRegistry stats;
    const SignalId jaw = state.add_signal("jawOpen");
    const SignalId internal = state.add_signal("_LeftEyeLidRaw");   // hidden from plugins
    auto sink = make_sink(json{{"type", "plugin"},
                               {"library", BABBLE_TEMPLATE_PLUGIN},
                               {"config", {{"host", "127.0.0.1"}, {"port", rx.port}}}},
                          SinkDeps{"json_out", threads, state, stats});
    REQUIRE(sink);
    REQUIRE(sink->start());

    set(state, internal, 0.5f);
    set(state, jaw, 0.25f);
    std::string line;
    for (int i = 0; i < 5 && line.find("jawOpen") == std::string::npos; i++)
        line = rx.next();
    CHECK(line.find("\"jawOpen\":0.25") != std::string::npos);
    CHECK(line.find("_LeftEyeLidRaw") == std::string::npos);

    // A signal registered after start (pipelines start after outputs) shows up too.
    const SignalId lid = state.add_signal("LeftEyeLid");
    set(state, lid, 0.75f);
    for (int i = 0; i < 5 && line.find("LeftEyeLid") == std::string::npos; i++)
        line = rx.next();
    CHECK(line.find("\"LeftEyeLid\":0.75") != std::string::npos);

    sink->stop();
}

TEST_CASE("the plugin output refuses a missing library, a wrong ABI and a missing library name") {
    json threads = json::object();
    ExpressionState state;
    StatsRegistry stats;
    auto missing = make_sink(json{{"type", "plugin"}, {"library", "/nonexistent/libnothing.so"}},
                             SinkDeps{"a", threads, state, stats});
    REQUIRE(missing);
    CHECK_FALSE(missing->start());
    auto bad = make_sink(json{{"type", "plugin"}, {"library", BABBLE_BAD_ABI_PLUGIN}}, SinkDeps{"b", threads, state, stats});
    REQUIRE(bad);
    CHECK_FALSE(bad->start());
    auto unnamed = make_sink(json{{"type", "plugin"}}, SinkDeps{"c", threads, state, stats});
    REQUIRE(unnamed);
    CHECK_FALSE(unnamed->start());
}
