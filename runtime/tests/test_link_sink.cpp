// BFR Link (sink/link_sink.h) against a fake VRCFT module on loopback. Two
// fake interfaces, both 127.0.0.1, stand for the dongle and LAN paths; the
// fake module can stop answering pings per path to force failover.
#include <doctest/doctest.h>

#include "core/metrics.h"
#include "sink/link_sink.h"
#include "state/expression_state.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

using namespace babble;
using json = nlohmann::json;

namespace {

int udp_socket(uint32_t ip, uint16_t port, bool reuse = false) {
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    int one = 1;
    if (reuse)
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(ip);
    a.sin_port = htons(port);
    REQUIRE(bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0);
    return fd;
}

uint16_t local_port(int fd) {
    sockaddr_in a{};
    socklen_t len = sizeof(a);
    getsockname(fd, reinterpret_cast<sockaddr *>(&a), &len);
    return ntohs(a.sin_port);
}

// A stand-in for the VRCFT module: answers beacons with offers and pings
// with pongs, and counts the OSC data that reaches its data port.
struct FakeModule {
    uint16_t control_port = 0;
    int control = -1, data = -1;
    std::string mid = "module-1";
    std::atomic<bool> busy{false}, answer_beacons{true};
    std::mutex mutex;
    std::set<std::string> silent_paths;   // ping paths to ignore
    std::set<std::string> data_addresses;
    int beacons = 0;
    std::thread thread;
    std::atomic<bool> stop{false};

    FakeModule() {
        // Find a free port, then bind the module to it on 127.0.0.1; the
        // link listens on 0.0.0.0 with the same port (both reuse the address).
        const int probe = udp_socket(INADDR_LOOPBACK, 0);
        control_port = local_port(probe);
        close(probe);
        control = udp_socket(INADDR_LOOPBACK, control_port, true);
        data = udp_socket(INADDR_LOOPBACK, 0);
        thread = std::thread([this] { run(); });
    }
    ~FakeModule() {
        stop = true;
        thread.join();
        close(control);
        close(data);
    }

    void reply(const sockaddr_in &to, const link::Message &m) {
        const std::string text = link::encode(m);
        sendto(control, text.data(), text.size(), 0, reinterpret_cast<const sockaddr *>(&to), sizeof(to));
    }

    void run() {
        char buf[2048];
        while (!stop) {
            pollfd fds[2] = {{control, POLLIN, 0}, {data, POLLIN, 0}};
            if (poll(fds, 2, 20) <= 0)
                continue;
            if (fds[1].revents & POLLIN) {
                const ssize_t n = recv(data, buf, sizeof(buf), 0);
                if (n > 0) {
                    std::lock_guard lock(mutex);
                    data_addresses.insert(std::string(buf, strnlen(buf, size_t(n))));
                }
            }
            if (!(fds[0].revents & POLLIN))
                continue;
            sockaddr_in from{};
            socklen_t len = sizeof(from);
            const ssize_t n = recvfrom(control, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&from), &len);
            auto m = n > 0 ? link::decode(buf, size_t(n)) : std::nullopt;
            if (!m)
                continue;
            if (m->type == "beacon") {
                {
                    std::lock_guard lock(mutex);
                    beacons++;
                }
                if (!answer_beacons)
                    continue;
                link::Message offer;
                offer.type = "offer";
                offer.mid = mid;
                offer.name = "Test PC";
                offer.data_port = local_port(data);
                offer.busy = busy;
                reply(from, offer);
            } else if (m->type == "ping") {
                std::lock_guard lock(mutex);
                if (silent_paths.count(m->path))
                    continue;
                link::Message pong;
                pong.type = "pong";
                pong.mid = mid;
                pong.seq = m->seq;
                reply(from, pong);
            }
        }
    }

    void silence(const std::string &path, bool on) {
        std::lock_guard lock(mutex);
        on ? (void)silent_paths.insert(path) : (void)silent_paths.erase(path);
    }
    bool got_data(const std::string &address) {
        std::lock_guard lock(mutex);
        return data_addresses.count(address) > 0;
    }
    void clear_data() {
        std::lock_guard lock(mutex);
        data_addresses.clear();
    }
    int beacon_count() {
        std::lock_guard lock(mutex);
        return beacons;
    }
};

template <typename F>
bool eventually(F f, int ms = 3000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (f())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return f();
}

std::vector<link::Interface> two_paths() {
    // dongle0 and lan0, both loopback: two independent sockets to the module.
    const uint32_t lo = INADDR_LOOPBACK, mask = 0xFF000000;
    return {{"dongle0", lo, lo, mask}, {"lan0", lo, lo, mask}};
}

json fast_config(uint16_t port) {
    return {{"type", "vrcft_link"}, {"control_port", port},        {"interfaces", {{"dongle0", "dongle"}}},
            {"beacon_s", {0.1, 0.3}}, {"ping_s", 0.05},             {"timeout_s", 0.3},
            {"name", "Test Frame"}};
}

} // namespace

TEST_CASE("link messages round trip and reject strangers") {
    link::Message m;
    m.type = "ping";
    m.sid = "abc";
    m.path = "dongle";
    m.seq = 42;
    m.active = true;
    const std::string text = link::encode(m);
    auto back = link::decode(text.data(), text.size());
    REQUIRE(back);
    CHECK(back->type == "ping");
    CHECK(back->sid == "abc");
    CHECK(back->path == "dongle");
    CHECK(back->seq == 42);
    CHECK(back->active);

    link::Message offer;
    offer.type = "offer";
    offer.mid = "m";
    offer.data_port = 8888;
    offer.busy = true;
    const std::string o = link::encode(offer);
    back = link::decode(o.data(), o.size());
    REQUIRE(back);
    CHECK(back->data_port == 8888);
    CHECK(back->busy);

    for (const char *bad : {"", "not json", "/jawOpen", R"({"proto":"other","v":1,"type":"ping"})",
                            R"({"proto":"bfr-link","v":2,"type":"ping"})", R"({"proto":"bfr-link","v":1})",
                            R"({"proto":"bfr-link","v":1,"type":"ping","seq":"x"})"})
        CHECK_FALSE(link::decode(bad, strlen(bad)));
}

TEST_CASE("link: finds the module, streams, fails over and back, and searches when it's gone") {
    FakeModule module;
    ExpressionState state;
    StatsRegistry stats;
    const json threads = json::object();
    const SignalId jaw = state.add_signal("jawOpen");
    state.add_signal("_internal");
    std::pair<SignalId, Signal> v{jaw, Signal{0.5f, 1.0f, 1}};
    state.update(std::span(&v, 1));

    LinkSink sink(fast_config(module.control_port), SinkDeps{"pc", threads, state, stats});
    sink.set_interface_source(two_paths);
    REQUIRE(sink.start());

    // Linked over the preferred path, and the current values are sent.
    CHECK(eventually([&] { return sink.active_path() == "dongle"; }));
    CHECK(eventually([&] { return module.got_data("/jawOpen"); }));
    CHECK_FALSE(module.got_data("/_internal"));

    // The dongle path stops answering: fail over to the LAN, resending the state.
    module.clear_data();
    module.silence("dongle", true);
    CHECK(eventually([&] { return sink.active_path() == "lan"; }));
    CHECK(eventually([&] { return module.got_data("/jawOpen"); }));

    // The dongle comes back: after 3 good pongs the link moves back to it.
    module.silence("dongle", false);
    CHECK(eventually([&] { return sink.active_path() == "dongle"; }));

    // Every path goes quiet: searching again, and no more data.
    module.silence("dongle", true);
    module.silence("lan", true);
    CHECK(eventually([&] { return sink.active_path().empty(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    module.clear_data();
    v.second.t_ns = 2;
    state.update(std::span(&v, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK_FALSE(module.got_data("/jawOpen"));

    sink.stop();
}

TEST_CASE("link: a busy module is not used") {
    FakeModule module;
    module.busy = true;
    ExpressionState state;
    StatsRegistry stats;
    const json threads = json::object();
    LinkSink sink(fast_config(module.control_port), SinkDeps{"pc", threads, state, stats});
    sink.set_interface_source(two_paths);
    REQUIRE(sink.start());
    CHECK(eventually([&] { return module.beacon_count() >= 4; }));
    CHECK(sink.active_path().empty());
    module.busy = false;   // free again: the next beacon links
    CHECK(eventually([&] { return !sink.active_path().empty(); }));
    sink.stop();
}

TEST_CASE("link: a module query gets an immediate beacon") {
    FakeModule module;
    module.answer_beacons = false;
    ExpressionState state;
    StatsRegistry stats;
    const json threads = json::object();
    json cfg = fast_config(module.control_port);
    cfg["beacon_s"] = {30, 30};   // only the first beacon, unless asked
    LinkSink sink(cfg, SinkDeps{"pc", threads, state, stats});
    sink.set_interface_source(two_paths);
    REQUIRE(sink.start());
    CHECK(eventually([&] { return module.beacon_count() >= 2; }));   // the first beacon, on both paths
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const int before = module.beacon_count();

    // The query goes to 127.0.0.2: it reaches the link's 0.0.0.0 listener, not the module's 127.0.0.1 socket.
    link::Message query;
    query.type = "query";
    query.mid = module.mid;
    const std::string text = link::encode(query);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(module.control_port);
    inet_pton(AF_INET, "127.0.0.2", &to.sin_addr);
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    sendto(fd, text.data(), text.size(), 0, reinterpret_cast<const sockaddr *>(&to), sizeof(to));
    close(fd);
    CHECK(eventually([&] { return module.beacon_count() > before; }, 1000));
    sink.stop();
}
