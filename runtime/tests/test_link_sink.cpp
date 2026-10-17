// These tests run BFR Link against a fake VRCFT module, with two loopback interfaces for the dongle and LAN.
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

// Collects the addresses in an OSC packet, which is a message or a bundle of messages.
void osc_addresses(const char *p, size_t n, std::set<std::string> *out, int *messages) {
    if (n >= 16 && memcmp(p, "#bundle", 8) == 0) {
        for (size_t at = 16; at + 4 <= n;) {
            uint32_t size;
            memcpy(&size, p + at, 4);
            size = ntohl(size);
            if (at + 4 + size > n)
                return;
            osc_addresses(p + at + 4, size, out, messages);
            at += 4 + size;
        }
    } else if (n && p[0] == '/') {
        out->insert(std::string(p, strnlen(p, n)));
        (*messages)++;
    }
}

// Acts as the VRCFT module, answers beacons and pings, and collects the OSC data that reaches its port.
struct FakeModule {
    uint16_t control_port = 0;
    int control = -1;
    std::string mid = "module-1";
    std::atomic<bool> busy{false}, answer_beacons{true}, drop_data{false};
    std::mutex mutex;
    std::set<std::string> silent_paths;   // The module ignores pings on these paths.
    std::set<std::string> data_addresses;
    uint64_t rx = 0;                      // The number of data datagrams that the module received.
    int data_messages = 0;
    int beacons = 0;
    std::optional<bool> ping_eye, ping_face;   // These are the tracking flags of the last ping.
    std::optional<bool> claims_eye, claims_face, restart;   // The pongs report these claims.
    std::thread thread;
    std::atomic<bool> stop{false};

    FakeModule() {
        // Bind the module to a free port on 127.0.0.1, and the link listens on 0.0.0.0 with the same port.
        const int probe = udp_socket(INADDR_LOOPBACK, 0);
        control_port = local_port(probe);
        close(probe);
        control = udp_socket(INADDR_LOOPBACK, control_port, true);
        thread = std::thread([this] { run(); });
    }
    ~FakeModule() {
        stop = true;
        thread.join();
        close(control);
    }

    void reply(const sockaddr_in &to, const link::Message &m) {
        const std::string text = link::encode(m);
        sendto(control, text.data(), text.size(), 0, reinterpret_cast<const sockaddr *>(&to), sizeof(to));
    }

    void run() {
        char buf[2048];
        while (!stop) {
            pollfd fds[1] = {{control, POLLIN, 0}};
            if (poll(fds, 1, 20) <= 0)
                continue;
            sockaddr_in from{};
            socklen_t len = sizeof(from);
            const ssize_t n = recvfrom(control, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&from), &len);
            if (n > 0 && buf[0] != '{') {   // OSC data comes on the same port.
                if (drop_data)
                    continue;
                std::lock_guard lock(mutex);
                rx++;
                osc_addresses(buf, size_t(n), &data_addresses, &data_messages);
                continue;
            }
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
                offer.busy = busy;
                reply(from, offer);
            } else if (m->type == "ping") {
                std::lock_guard lock(mutex);
                ping_eye = m->eye;
                ping_face = m->face;
                if (silent_paths.count(m->path))
                    continue;
                link::Message pong;
                pong.type = "pong";
                pong.mid = mid;
                pong.seq = m->seq;
                pong.rx = rx;
                pong.eye = claims_eye;
                pong.face = claims_face;
                pong.restart = restart;
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
    void claim(bool eye, bool face, bool restart_needed) {
        std::lock_guard lock(mutex);
        claims_eye = eye;
        claims_face = face;
        restart = restart_needed;
    }
    bool ping_flags(bool eye, bool face) {
        std::lock_guard lock(mutex);
        return ping_eye == eye && ping_face == face;
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
    // The dongle0 and lan0 interfaces are both loopback, so they give two independent sockets to the module.
    const uint32_t lo = INADDR_LOOPBACK, mask = 0xFF000000;
    return {{"dongle0", lo, lo, mask}, {"lan0", lo, lo, mask}};
}

json fast_config(uint16_t port) {
    return {{"type", "vrcft_link"}, {"control_port", port},        {"interfaces", {{"dongle0", "dongle"}}},
            {"beacon_s", {0.1, 0.3}}, {"ping_s", 0.05},             {"timeout_s", 0.3},
            {"name", "Test Frame"}};
}

}

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
    CHECK_FALSE(back->eye);   // An older headset does not send the tracking flags.
    CHECK_FALSE(back->face);
    m.eye = false;
    m.face = true;
    const std::string flags = link::encode(m);
    back = link::decode(flags.data(), flags.size());
    REQUIRE(back);
    CHECK(back->eye == false);
    CHECK(back->face == true);
    CHECK_FALSE(back->restart);

    link::Message offer;
    offer.type = "offer";
    offer.mid = "m";
    offer.busy = true;
    const std::string o = link::encode(offer);
    back = link::decode(o.data(), o.size());
    REQUIRE(back);
    CHECK(back->busy);

    link::Message pong;
    pong.type = "pong";
    pong.seq = 7;
    pong.rx = 12345;
    const std::string pt = link::encode(pong);
    back = link::decode(pt.data(), pt.size());
    REQUIRE(back);
    CHECK(back->rx == 12345u);
    pong.rx.reset();   // An older module does not send a count.
    const std::string old = link::encode(pong);
    back = link::decode(old.data(), old.size());
    REQUIRE(back);
    CHECK_FALSE(back->rx);

    for (const char *bad : {"", "not json", "/jawOpen", R"({"proto":"other","v":1,"type":"ping"})",
                            R"({"proto":"bfr-link","v":2,"type":"ping"})", R"({"proto":"bfr-link","v":1})",
                            R"({"proto":"bfr-link","v":1,"type":"ping","seq":"x"})",
                            R"({"proto":"bfr-link","v":1,"type":"ping","eye":"yes"})"})
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

    // The link uses the preferred path and sends the current values as a bundle to the control port of the module.
    CHECK(eventually([&] { return sink.active_path() == "dongle"; }));
    CHECK(eventually([&] { return module.got_data("/jawOpen"); }));
    CHECK_FALSE(module.got_data("/_internal"));

    // When the dongle path stops answering, the link fails over to the LAN and sends the state again.
    module.clear_data();
    module.silence("dongle", true);
    CHECK(eventually([&] { return sink.active_path() == "lan"; }));
    CHECK(eventually([&] { return module.got_data("/jawOpen"); }));

    // When the dongle comes back, the link moves back to it after 3 good pongs.
    module.silence("dongle", false);
    CHECK(eventually([&] { return sink.active_path() == "dongle"; }));

    // When all paths are quiet, the link searches again and stops the data.
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

TEST_CASE("link: pings tell the module which tracking is on") {
    FakeModule module;
    ExpressionState state;
    StatsRegistry stats;
    const json threads = json::object();
    LinkSink sink(fast_config(module.control_port), SinkDeps{"pc", threads, state, stats});
    sink.set_interface_source(two_paths);
    REQUIRE(sink.start());
    CHECK(eventually([&] { return module.ping_flags(true, true); }));
    sink.set_features(false, true);
    CHECK(eventually([&] { return module.ping_flags(false, true); }));
    sink.set_features(true, false);
    CHECK(eventually([&] { return module.ping_flags(true, false); }));

    // An older module reports no claims.
    CHECK(eventually([&] { return sink.status().linked; }));
    CHECK_FALSE(sink.status().claims_eye);
    CHECK_FALSE(sink.status().restart);
    // The pongs report what the module claims, and if VRCFaceTracking must restart.
    module.claim(false, true, true);
    CHECK(eventually([&] { return sink.status().restart; }));
    CHECK(sink.status().claims_eye == false);
    CHECK(sink.status().claims_face == true);
    module.claim(true, false, false);
    CHECK(eventually([&] { return !sink.status().restart && sink.status().claims_eye == true; }));
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
    module.busy = false;   // The module is free again, so the next beacon makes a link.
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
    cfg["beacon_s"] = {30, 30};   // The sink sends only the first beacon unless it gets a query.
    LinkSink sink(cfg, SinkDeps{"pc", threads, state, stats});
    sink.set_interface_source(two_paths);
    REQUIRE(sink.start());
    CHECK(eventually([&] { return module.beacon_count() >= 2; }));   // The first beacon goes on both paths.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const int before = module.beacon_count();

    // The query goes to 127.0.0.2, so it reaches the 0.0.0.0 listener of the link and not the
    // 127.0.0.1 socket of the module.
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

TEST_CASE("link: data goes out bundled, and missing data shows in the status") {
    FakeModule module;
    ExpressionState state;
    StatsRegistry stats;
    const json threads = json::object();
    std::vector<SignalId> ids;
    for (int i = 0; i < 60; i++)   // This is more signals than one bundle can hold.
        ids.push_back(state.add_signal("signal" + std::to_string(i)));
    LinkSink sink(fast_config(module.control_port), SinkDeps{"pc", threads, state, stats});
    sink.set_interface_source(two_paths);
    REQUIRE(sink.start());
    REQUIRE(eventually([&] { return sink.active_path() == "dongle"; }));

    std::atomic<bool> feeding{true};
    std::thread feeder([&] {   // The feeder sends tracking data at about 50 Hz.
        for (uint64_t t = 1; feeding; t++) {
            std::vector<std::pair<SignalId, Signal>> u;
            for (SignalId id : ids)
                u.push_back({id, Signal{0.5f, 1.0f, t}});
            state.update(std::span(u));
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    CHECK(eventually([&] { return sink.status().data == LinkSink::Data::Arriving; }));
    {
        std::lock_guard lock(module.mutex);
        CHECK(module.data_addresses.count("/signal59"));
        // Bundles make far fewer datagrams than values.
        CHECK(module.rx * 10 < uint64_t(module.data_messages));
    }

    // When tracking stops and starts again, the sink must not report "missing" while the pongs catch up.
    feeding = false;
    feeder.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(3500));
    feeding = true;
    feeder = std::thread([&] {
        for (uint64_t t = 100000; feeding; t++) {
            std::vector<std::pair<SignalId, Signal>> u;
            for (SignalId id : ids)
                u.push_back({id, Signal{0.5f, 1.0f, t}});
            state.update(std::span(u));
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    for (int i = 0; i < 40; i++) {
        CHECK(sink.status().data != LinkSink::Data::Missing);
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    // When a firewall drops the data, the pongs still come but the count stops.
    module.drop_data = true;
    CHECK(eventually([&] { return sink.status().data == LinkSink::Data::Missing; }, 8000));
    CHECK(sink.active_path() == "dongle");   // The link stays because the control path works.
    module.drop_data = false;
    CHECK(eventually([&] { return sink.status().data == LinkSink::Data::Arriving; }, 8000));
    feeding = false;
    feeder.join();
    sink.stop();
}

TEST_CASE("link: dongle beacons go only to stations on its own subnet") {
    auto ip = [](const char *s) {
        in_addr a{};
        inet_pton(AF_INET, s, &a);
        return ntohl(a.s_addr);
    };
    const link::Interface dongle{"wlanap", ip("10.35.78.1"), ip("10.35.78.255"), ip("255.255.255.0")};
    // The ARP table on the dongle can also list the LAN and VPN addresses of the PC.
    const auto out = link::on_subnet(dongle, {ip("10.35.78.25"), ip("192.168.0.149"), ip("100.71.126.17"),
                                              ip("10.35.78.1"), ip("10.35.78.255"), ip("10.35.78.30")});
    CHECK(out == std::vector<uint32_t>{ip("10.35.78.25"), ip("10.35.78.30")});
}
