#include <doctest/doctest.h>

#include "camera/eye/dsp_client.h"

#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace babble;

namespace {
void put32(std::vector<uint8_t> &m, uint32_t off, uint32_t v) { memcpy(&m[off], &v, 4); }

std::vector<uint8_t> good_block(const DspClient::Layout &l) {
    std::vector<uint8_t> m(DspClient::kRegionBytes, 0);
    const uint32_t b = l.ctrl_base;
    put32(m, b + l.idx_l, 3);
    put32(m, b + l.idx_r, DspClient::kNotReady);
    // On the headset, the idle contents are (4,4,1,8,0,0,0,0) on both sides.
    const uint32_t idle[8] = {4, 4, 1, 8, 0, 0, 0, 0};
    for (uint32_t i = 0; i < l.slots; i++) {
        put32(m, b + l.locks_l + 4 * i, idle[i]);
        put32(m, b + l.locks_r + 4 * i, idle[i]);
    }
    put32(m, b + l.enables, 1);
    put32(m, b + l.enables + 4, 1);
    float fps = 15.0f;
    memcpy(&m[b + l.fps], &fps, 4);
    return m;
}
}

TEST_CASE("accepts a plausible control block at both known bases") {
    for (uint32_t base : {0x234000u, 0x124000u}) {
        DspClient::Layout l;
        l.ctrl_base = base;
        auto m = good_block(l);
        std::string why;
        CHECK_MESSAGE(DspClient::validate(m.data(), m.size(), l, &why), why);
    }
}

TEST_CASE("rejects garbage words") {
    DspClient::Layout l;
    l.ctrl_base = 0x234000;
    std::string why;

    auto m = good_block(l);
    put32(m, l.ctrl_base + l.idx_l, 12);
    CHECK_FALSE(DspClient::validate(m.data(), m.size(), l, &why));

    m = good_block(l);
    put32(m, l.ctrl_base + l.locks_r + 8, 0xdeadbeef);
    CHECK_FALSE(DspClient::validate(m.data(), m.size(), l, &why));

    m = good_block(l);
    put32(m, l.ctrl_base + l.enables + 4, 5);
    CHECK_FALSE(DspClient::validate(m.data(), m.size(), l, &why));

    m = good_block(l);
    put32(m, l.ctrl_base + l.fps, 0x7fc00000);   // The value is NaN.
    CHECK_FALSE(DspClient::validate(m.data(), m.size(), l, &why));
}

TEST_CASE("the wrong base is rejected when only the other one is valid") {
    DspClient::Layout real;
    real.ctrl_base = 0x234000;
    auto m = good_block(real);
    // Fill the old location with typical data that is not control data.
    for (uint32_t off = 0x124000; off < 0x124000 + 0x200; off += 4)
        put32(m, off, 0x41424344);
    DspClient::Layout old;
    old.ctrl_base = 0x124000;
    std::string why;
    CHECK_FALSE(DspClient::validate(m.data(), m.size(), old, &why));
    CHECK(DspClient::validate(m.data(), m.size(), real, &why));
}

TEST_CASE("rejects layouts that would run past the mapping") {
    DspClient::Layout l;
    l.ctrl_base = 0xF00000;
    std::vector<uint8_t> m(DspClient::kRegionBytes, 0);
    std::string why;
    CHECK_FALSE(DspClient::validate(m.data(), m.size(), l, &why));
}

TEST_CASE("a blank control block (service just booted) is 'not set up yet', not an unknown layout") {
    const uint32_t bases[] = {0x234000u, 0x124000u};
    std::vector<uint8_t> blank(DspClient::kRegionBytes, 0);
    CHECK(DspClient::fps_unset(blank.data(), blank.size(), bases));

    // The fps word is set in this active block, so a failed probe is a real layout problem.
    DspClient::Layout l;
    l.ctrl_base = 0x234000;
    auto m = good_block(l);
    CHECK_FALSE(DspClient::fps_unset(m.data(), m.size(), bases));

    // Garbage where the fps word must be is an unknown layout and not "not ready".
    std::vector<uint8_t> junk(DspClient::kRegionBytes, 0);
    for (uint32_t base : bases)
        put32(junk, base + DspClient::Layout{}.fps, 0x41424344);
    CHECK_FALSE(DspClient::fps_unset(junk.data(), junk.size(), bases));
}

TEST_CASE("dsp client: sends its request type after the fds and drains what the service sends") {
    // This fake dsp_service gives a sparse memfd, reads the request type and pushes notifications.
    const std::string name = "babble_test_dsp_" + std::to_string(getpid());
    const int srv = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    REQUIRE(srv >= 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path + 1, name.data(), name.size());
    const socklen_t len = socklen_t(offsetof(sockaddr_un, sun_path) + 1 + name.size());
    REQUIRE(bind(srv, reinterpret_cast<sockaddr *>(&addr), len) == 0);
    REQUIRE(listen(srv, 1) == 0);

    uint32_t request = 0xdeadbeef;
    std::thread service([&] {
        const int c = accept(srv, nullptr, nullptr);
        if (c < 0)
            return;
        const int mem = memfd_create("dsp_test", MFD_CLOEXEC);
        if (mem >= 0 && ftruncate(mem, DspClient::kCtrlOffset + DspClient::kRegionBytes) == 0) {
            char byte = 0;
            iovec iov{&byte, 1};
            alignas(cmsghdr) char cbuf[CMSG_SPACE(sizeof(int))] = {};
            msghdr msg{};
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            msg.msg_control = cbuf;
            msg.msg_controllen = sizeof(cbuf);
            cmsghdr *cm = CMSG_FIRSTHDR(&msg);
            cm->cmsg_level = SOL_SOCKET;
            cm->cmsg_type = SCM_RIGHTS;
            cm->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(cm), &mem, sizeof(int));
            if (sendmsg(c, &msg, 0) == 1 && recv(c, &request, sizeof(request), MSG_WAITALL) == sizeof(request))
                for (int i = 0; i < 100; i++)
                    send(c, "service_notification", 21, MSG_NOSIGNAL);
        }
        if (mem >= 0)
            close(mem);
        // Keep the socket open until the client is done with it.
        char b;
        recv(c, &b, 1, 0);
        close(c);
    });

    DspClient client;
    REQUIRE(client.connect(0, name));
    CHECK(client.connected());
    CHECK(client.ctrl() != nullptr);
    size_t drained = 0;
    for (int i = 0; i < 200 && drained < 2100; i++) {
        drained += client.drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(request == 0);        // Without a subscription, dsp_service sends nothing.
    CHECK(drained == 2100);     // The client reads all data, so the data does not fill the socket.
    CHECK(client.drain() == 0);
    client.disconnect();
    CHECK(client.drain() == 0);   // A drain is safe when the client is not connected.
    service.join();
    close(srv);
}
