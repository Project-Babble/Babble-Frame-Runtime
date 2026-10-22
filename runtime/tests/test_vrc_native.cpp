#include <doctest/doctest.h>

#include "core/metrics.h"
#include "sink/osc.h"
#include "sink/sink.h"
#include "sink/vrc_native_sink.h"
#include "state/expression_state.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

#include <sys/wait.h>

using namespace babble;
using json = nlohmann::json;

TEST_CASE("OSC blob messages are encoded per the OSC 1.0 spec") {
    uint8_t buf[64];
    const uint8_t data[5] = {1, 2, 3, 4, 5};
    // The parts are "/b" (4), ",b" (4), the big-endian size 5 (4) and the data 5 padded to 8.
    const size_t n = osc::encode_blob("/b", data, sizeof(data), buf, sizeof(buf));
    REQUIRE(n == 20);
    const uint8_t expect[20] = {'/', 'b', 0, 0, ',', 'b', 0, 0, 0, 0, 0, 5, 1, 2, 3, 4, 5, 0, 0, 0};
    CHECK(memcmp(buf, expect, 20) == 0);
    CHECK(osc::encode_blob("/b", data, sizeof(data), buf, 19) == 0);
    // The face blob is a 20-byte address block + 4 + 4 + 104.
    uint8_t face[vrc_native::kBlobSize] = {};
    CHECK(osc::encode_blob(vrc_native::kAddress, face, sizeof(face), buf, 0) == 0);
    uint8_t big[160];
    CHECK(osc::encode_blob(vrc_native::kAddress, face, sizeof(face), big, sizeof(big)) == 132);
}

TEST_CASE("signals map onto VRCFaceTracking's blob as its module maps them") {
    CHECK(vrc_native::target_of("LeftEyeX").kind == vrc_native::Target::LeftX);
    CHECK(vrc_native::target_of("RightEyeLid").kind == vrc_native::Target::RightOpen);
    CHECK(vrc_native::target_of("jawOpen").shapes == std::vector<uint8_t>{22});
    CHECK(vrc_native::target_of("mouthFunnel").shapes.size() == 4);
    CHECK(vrc_native::target_of("LeftEyeWiden").shapes == std::vector<uint8_t>{3});
    // This signal is not in the blob.
    CHECK(vrc_native::target_of("LeftEyeDilation").kind == vrc_native::Target::None);
    CHECK(vrc_native::target_of("_LeftEyeLidRaw").kind == vrc_native::Target::None);

    // When nothing is set, the eyes are open (closed bytes are 0) and all other values are 0.
    CHECK(vrc_native::encode({}, {}) == vrc_native::Blob{});

    const std::vector<std::string> names = {"RightEyeX", "RightEyeY", "LeftEyeX", "LeftEyeY",
                                            "RightEyeLid", "LeftEyeLid", "jawOpen", "tongueTwistLeft", "mouthFunnel"};
    std::vector<vrc_native::Target> targets;
    for (const auto &n : names)
        targets.push_back(vrc_native::target_of(n));
    const std::vector<std::optional<float>> values = {0.5f, -1.0f, -0.25f, 2.0f, 0.0f, 0.75f, 1.0f, 0.5f,
                                                      std::nullopt};
    const auto b = vrc_native::encode(targets, values);
    // The right gaze is +x, -x, +y, -y, and VRCFT truncates, so 0.5 * 255 = 127.
    CHECK(b[0] == 127);
    CHECK(b[1] == 0);
    CHECK(b[2] == 0);
    CHECK(b[3] == 255);
    // The left gaze is -x, +x, +y, -y, with a clamp.
    CHECK(b[4] == 63);
    CHECK(b[5] == 0);
    CHECK(b[6] == 255);
    CHECK(b[7] == 0);
    // Closed is 1 - openness, so the right eye is shut and the left eye is a quarter closed.
    CHECK(b[8] == 255);
    CHECK(b[9] == 63);
    for (int i = 10; i < 14; i++)
        CHECK(b[size_t(i)] == 0);
    CHECK(b[14 + 22] == 255);   // This is JawOpen.
    CHECK(b[14 + 83] == 127);   // This is TongueTwistLeft.
    CHECK(b[14 + 36] == 0);     // This is LipFunnelUpperRight, and mouthFunnel is not set.
}

namespace {

int udp_receiver(sockaddr_in *addr) {
    int rx = socket(AF_INET, SOCK_DGRAM, 0);
    *addr = {};
    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(bind(rx, reinterpret_cast<sockaddr *>(addr), sizeof(*addr)) == 0);
    socklen_t len = sizeof(*addr);
    REQUIRE(getsockname(rx, reinterpret_cast<sockaddr *>(addr), &len) == 0);
    return rx;
}

ssize_t receive(int rx, uint8_t *buf, size_t cap, int timeout_ms) {
    pollfd p{rx, POLLIN, 0};
    if (poll(&p, 1, timeout_ms) <= 0)
        return -1;
    return recv(rx, buf, cap, 0);
}

}

TEST_CASE("the vrc_native module sends the blob to its target") {
    sockaddr_in addr{};
    const int rx = udp_receiver(&addr);
    json threads = json::object();
    ExpressionState state;
    StatsRegistry stats;
    const SignalId jaw = state.add_signal("jawOpen");
    auto sink = make_sink(json{{"type", "vrc_native"}, {"host", "127.0.0.1"}, {"port", ntohs(addr.sin_port)}},
                          SinkDeps{"vrc_native", threads, state, stats});
    REQUIRE(sink);
    REQUIRE(sink->start());
    std::pair<SignalId, Signal> u{jaw, Signal{1.0f, 1.0f, 42}};
    state.update(std::span(&u, 1));

    uint8_t buf[256];
    const ssize_t n = receive(rx, buf, sizeof(buf), 2000);
    REQUIRE(n == 132);
    CHECK(memcmp(buf, "/tracking/face/v1\0\0\0,b\0\0", 24) == 0);
    CHECK(buf[27] == 102);             // This is the blob size.
    CHECK(buf[28 + 14 + 22] == 255);   // This is JawOpen.
    // The sink counts a send after sendto() returns, so the packet can arrive before the count changes.
    auto st = static_cast<VrcNativeSink *>(sink.get())->status();
    for (int i = 0; i < 100 && st.sent < 1; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        st = static_cast<VrcNativeSink *>(sink.get())->status();
    }
    CHECK(st.kind == "host");
    CHECK(st.sent >= 1);

    // After a stop (the Output setting changed), the sink sends nothing more.
    sink->stop();
    while (receive(rx, buf, sizeof(buf), 100) > 0) {}
    u.second = Signal{0.5f, 1.0f, 43};
    state.update(std::span(&u, 1));
    CHECK(receive(rx, buf, sizeof(buf), 300) < 0);
    CHECK(static_cast<VrcNativeSink *>(sink.get())->status().kind.empty());
    close(rx);
}

TEST_CASE("auto targets the Steam Link PC while its session runs, else this headset") {
    const auto dir = std::filesystem::temp_directory_path() / ("babble_vrlink_" + std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    const auto log = dir / "vrlink.txt";
    auto write_log = [&](int pid) {
        std::ofstream out(log, std::ios::trunc);
        out << "x [Info] - vrlink 2.17.10 startup with PID=" << pid << ", config=/x\n"
            << "x [Info] - UDPServerAddresses:\n"
            << "x [Info] - \t10.0.0.9 *\n"
            << "x [Info] - Soft AES PERF\n";
    };
    // Make a live process named vrlink that sleeps.
    const pid_t child = fork();
    if (child == 0) {
        execl("/bin/sleep", "vrlink", "20", static_cast<char *>(nullptr));
        _exit(127);
    }
    REQUIRE(child > 0);
    write_log(child);

    json threads = json::object();
    ExpressionState state;
    StatsRegistry stats;
    const SignalId jaw = state.add_signal("jawOpen");
    auto sink = make_sink(json{{"type", "vrc_native"}, {"port", 9}, {"steam_link_log", log.string()}},
                          SinkDeps{"vrc_native", threads, state, stats});
    REQUIRE(sink);
    auto *native = static_cast<VrcNativeSink *>(sink.get());
    auto wait_for = [&](const std::string &kind) {
        for (int i = 0; i < 100 && native->status().kind != kind; i++) {
            std::pair<SignalId, Signal> u{jaw, Signal{0.5f, 1.0f, uint64_t(100 + i)}};
            state.update(std::span(&u, 1));
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return native->status();
    };
    REQUIRE(sink->start());
    auto st = wait_for("pc");
    CHECK(st.kind == "pc");
    CHECK(st.address == "10.0.0.9");
    sink->stop();

    // The session ended, so its process is gone.
    kill(child, SIGKILL);
    waitpid(child, nullptr, 0);
    REQUIRE(sink->start());
    st = wait_for("frame");
    CHECK(st.kind == "frame");
    CHECK(st.address == "127.0.0.1");
    sink->stop();
    std::filesystem::remove_all(dir);
}

TEST_CASE("the PC address of the Outputs page comes before Steam Link, and a fixed host before both") {
    sockaddr_in addr{};
    const int rx = udp_receiver(&addr);
    json threads = json::object();
    ExpressionState state;
    StatsRegistry stats;
    const SignalId jaw = state.add_signal("jawOpen");
    uint64_t t = 7;
    auto status_after_send = [&](VrcNativeSink *native) {
        uint8_t buf[256];
        while (receive(rx, buf, sizeof(buf), 50) > 0) {}   // Drop what the last sink sent.
        std::pair<SignalId, Signal> u{jaw, Signal{0.5f, 1.0f, ++t}};
        state.update(std::span(&u, 1));
        CHECK(receive(rx, buf, sizeof(buf), 2000) == 132);
        // The sink counts a send after sendto() returns, so the packet can arrive before the status changes.
        for (int i = 0; i < 100 && native->status().sent < 1; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return native->status();
    };

    auto sink = make_sink(json{{"type", "vrc_native"}, {"port", ntohs(addr.sin_port)}, {"pc_address", "127.0.0.1"},
                               {"steam_link_log", "/nonexistent/vrlink.txt"}},
                          SinkDeps{"vrc_native", threads, state, stats});
    REQUIRE(sink->start());
    auto st = status_after_send(static_cast<VrcNativeSink *>(sink.get()));
    CHECK(st.kind == "pc");
    CHECK(st.address == "127.0.0.1");
    sink->stop();

    // "frame" means VRChat on this headset, whatever the PC address is.
    sink = make_sink(json{{"type", "vrc_native"}, {"port", ntohs(addr.sin_port)}, {"pc_address", "10.1.2.3"},
                          {"target", "frame"}},
                     SinkDeps{"vrc_native", threads, state, stats});
    REQUIRE(sink->start());
    CHECK(status_after_send(static_cast<VrcNativeSink *>(sink.get())).kind == "frame");
    sink->stop();

    // A bad PC address is ignored.
    sink = make_sink(json{{"type", "vrc_native"}, {"port", ntohs(addr.sin_port)}, {"host", "127.0.0.1"},
                          {"pc_address", "not an ip"}},
                     SinkDeps{"vrc_native", threads, state, stats});
    REQUIRE(sink->start());
    CHECK(status_after_send(static_cast<VrcNativeSink *>(sink.get())).kind == "host");
    sink->stop();
    close(rx);
}
