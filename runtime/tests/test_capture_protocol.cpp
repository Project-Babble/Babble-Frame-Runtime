#include <doctest/doctest.h>

#include "capture/capture_protocol.h"
#include "capture/capture_server.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace babble::capture;
using json = nlohmann::json;

namespace {

struct FakeBackend : Backend {
    bool recording = false;
    int stops = 0;
    std::string stop_reason;
    std::vector<std::string> labels;
    std::vector<Segment> segments;
    json hello_info() override { return {{"runtime", "V-test"}, {"recording", true}, {"upload", false}}; }
    Result start(const json &req) override {
        if (recording)
            return Result::fail("busy", "already recording");
        recording = true;
        Result r;
        r.data = {{"session", "s1"}, {"dir", "/tmp/s1"}};
        return r;
    }
    Result segment(const Segment &s) override {
        if (!recording)
            return Result::fail("bad_request", "not recording");
        labels.push_back(s.label);
        segments.push_back(s);
        return {};
    }
    Result stop(const std::string &reason) override {
        stops++;
        stop_reason = reason;
        if (!recording)
            return Result::fail("bad_request", "not recording");
        recording = false;
        return {};
    }
    json status() override { return {{"recording", recording}}; }
    Result queue_upload(const std::string &) override { return Result::fail("unavailable", "no upload service"); }
};

json one(const Connection::Output &o) {
    REQUIRE(o.messages.size() == 1);
    return o.messages[0];
}

}

TEST_CASE("capture protocol: hello first, then requests with their ids echoed") {
    FakeBackend b;
    Connection c;
    auto o = c.handle_line(R"({"op":"start","id":1})", b);
    CHECK(one(o)["code"] == "protocol");
    CHECK(o.close);

    Connection c2;
    json h = one(c2.handle_line(R"({"op":"hello","id":"a","protocol":2,"app":"t"})", b));
    CHECK(h["op"] == "hello");
    CHECK(h["id"] == "a");
    CHECK(h["protocol"] == kProtocol);
    CHECK(h["runtime"] == "V-test");

    json s = one(c2.handle_line(R"({"op":"start","id":2,"routine":"eye","consent":{"version":"t"}})", b));
    CHECK(s["op"] == "started");
    CHECK(s["id"] == 2);
    CHECK(s["session"] == "s1");
    CHECK(one(c2.handle_line(R"({"op":"start","id":3})", b))["code"] == "busy");

    CHECK(c2.handle_line(R"({"op":"segment","label":"JawOpen","start_ns":1,"end_ns":2})", b).messages.empty());
    CHECK(one(c2.handle_line(R"({"op":"segment","id":9,"label":"X","start_ns":1,"end_ns":2})", b))["op"] ==
          "segment_ok");
    CHECK(b.labels == std::vector<std::string>{"JawOpen", "X"});
    CHECK(one(c2.handle_line(R"({"op":"segment","label":"X","start_ns":-1,"end_ns":2})", b))["code"] ==
          "bad_request");

    CHECK(c2.handle_line(R"({"op":"stop","id":4})", b).messages.empty());   // The "stopped" reply is an event.
    CHECK(c2.pending_stop_id() == 4);
    CHECK(b.stop_reason == kStoppedByApp);
    CHECK(one(c2.handle_line(R"({"op":"status","id":5})", b))["recording"] == false);
    CHECK(one(c2.handle_line(R"({"op":"upload","id":6,"session":"s1"})", b))["code"] == "unavailable");
}

TEST_CASE("capture protocol 2: segments carry step and phase, start may carry routine_def") {
    FakeBackend b;
    Connection c;
    c.handle_line(R"({"op":"hello","protocol":2})", b);
    CHECK(one(c.handle_line(R"({"op":"start","id":1,"routine":"eye","routine_def":{"steps":[]}})", b))["op"] ==
          "started");
    CHECK(c.handle_line(R"({"op":"segment","label":"eye_closed","start_ns":1,"end_ns":2,"step":3,"phase":"hold"})", b)
              .messages.empty());
    CHECK(c.handle_line(R"({"op":"segment","label":"Neutral","start_ns":2,"end_ns":3})", b).messages.empty());
    REQUIRE(b.segments.size() == 2);
    CHECK(b.segments[0].step == 3);
    CHECK(b.segments[0].phase == "hold");
    CHECK_FALSE(b.segments[1].step.has_value());
    CHECK(b.segments[1].phase.empty());
    CHECK(one(c.handle_line(R"({"op":"segment","label":"X","start_ns":1,"end_ns":2,"step":"3"})", b))["code"] ==
          "bad_request");
    CHECK(one(c.handle_line(R"({"op":"segment","label":"X","start_ns":1,"end_ns":2,"phase":1})", b))["code"] ==
          "bad_request");
}

TEST_CASE("capture protocol: bad input is answered, not fatal") {
    FakeBackend b;
    Connection c;
    CHECK(one(c.handle_line("not json", b))["code"] == "bad_request");
    CHECK(one(c.handle_line("[1,2]", b))["code"] == "bad_request");
    auto o = c.handle_line(R"({"op":"hello","protocol":1})", b);   // This app uses format 1.
    CHECK(one(o)["code"] == "protocol");
    CHECK(o.close);
    Connection c2;
    c2.handle_line(R"({"op":"hello","protocol":2})", b);
    CHECK(one(c2.handle_line(R"({"op":"dance","id":1})", b))["code"] == "bad_request");
    CHECK(one(c2.handle_line(R"({"op":"start","meta":"x"})", b))["code"] == "bad_request");
    CHECK(one(c2.handle_line(R"({"op":"start","routine_def":[1]})", b))["code"] == "bad_request");
    CHECK(one(c2.handle_line(R"({"op":"upload"})", b))["code"] == "bad_request");
}

namespace {

int connect_to(const std::filesystem::path &p) {
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, p.c_str(), sizeof(a.sun_path) - 1);
    if (connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

std::string read_line(int fd, Server &srv) {
    std::string s;
    char ch;
    for (int i = 0; i < 400; i++) {
        for (int f : srv.fds())
            srv.on_readable(f);
        while (recv(fd, &ch, 1, MSG_DONTWAIT) == 1) {
            if (ch == '\n')
                return s;
            s += ch;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return s;
}

void send_line(int fd, const std::string &s) {
    const std::string l = s + "\n";
    REQUIRE(send(fd, l.data(), l.size(), 0) == ssize_t(l.size()));
}

}

TEST_CASE("capture server: one client at a time; a disconnect stops the recording") {
    FakeBackend b;
    Server srv(b);
    const auto path = std::filesystem::temp_directory_path() / ("babble_capture_" + std::to_string(getpid()) + ".sock");
    std::string err;
    REQUIRE_MESSAGE(srv.listen(path, &err), err);

    const int a = connect_to(path);
    REQUIRE(a >= 0);
    send_line(a, R"({"op":"hello","id":1,"protocol":2})");
    CHECK(json::parse(read_line(a, srv))["op"] == "hello");
    CHECK(srv.greeted());

    const int second = connect_to(path);
    REQUIRE(second >= 0);
    CHECK(json::parse(read_line(second, srv))["code"] == "busy");
    close(second);

    send_line(a, R"({"op":"start","id":2})");
    CHECK(json::parse(read_line(a, srv))["op"] == "started");
    send_line(a, R"({"op":"stop","id":3})");
    for (int i = 0; i < 10; i++)
        for (int f : srv.fds())
            srv.on_readable(f);
    srv.stopped({{"session", "s1"}});
    json st = json::parse(read_line(a, srv));
    CHECK(st["op"] == "stopped");
    CHECK(st["id"] == 3);

    send_line(a, R"({"op":"start","id":4})");
    CHECK(json::parse(read_line(a, srv))["op"] == "started");
    const int stops = b.stops;
    close(a);
    for (int i = 0; i < 50 && srv.connected(); i++) {
        for (int f : srv.fds())
            srv.on_readable(f);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK_FALSE(srv.connected());
    CHECK(b.stops == stops + 1);
    CHECK(b.stop_reason == kAppDisconnected);
    CHECK_FALSE(b.recording);
    srv.close();
    CHECK_FALSE(std::filesystem::exists(path));
}
