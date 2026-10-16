#include <doctest/doctest.h>

#include "camera/usb/jpeg.h"
#include "core/clock.h"
#include "core/metrics.h"
#include "frame/frame_pool.h"
#include "sink/camera_stream_sink.h"
#include "sink/sink.h"
#include "state/expression_state.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <thread>

using namespace babble;
using json = nlohmann::json;

namespace {

uint64_t be(const uint8_t *p, int bytes) {
    uint64_t v = 0;
    for (int i = 0; i < bytes; i++)
        v = (v << 8) | p[i];
    return v;
}

}

TEST_CASE("camera stream packets follow stream_daemon.py's wire format") {
    const std::vector<uint8_t> a = {1, 2, 3}, b = {9};
    const auto pkt = camera_stream::packet({{camera_stream::EyeL, 0x0102030405060708ull, &a},
                                            {camera_stream::FaceR, 5, &b}});
    REQUIRE(pkt.size() == 5 + 13 + 3 + 13 + 1);
    CHECK(be(pkt.data(), 4) == 0xFEEDFACE);
    CHECK(pkt[4] == 2);
    CHECK(pkt[5] == 2);   // The view id 2 is eye_l.
    CHECK(be(pkt.data() + 6, 8) == 0x0102030405060708ull);
    CHECK(be(pkt.data() + 14, 4) == 3);
    CHECK(memcmp(pkt.data() + 18, a.data(), 3) == 0);
    CHECK(pkt[21] == 1);  // The view id 1 is face_r.
    CHECK(be(pkt.data() + 30, 4) == 1);
    CHECK(pkt[34] == 9);
}

TEST_CASE("eye views are cropped like the streamer's, faces pass through") {
    std::vector<uint8_t> eye(512 * 512);
    for (size_t y = 0; y < 512; y++)
        for (size_t x = 0; x < 512; x++)
            eye[y * 512 + x] = uint8_t((x + 3 * y) & 0xff);
    camera_stream::Image img;
    camera_stream::copy_view(camera_stream::EyeL, eye.data(), 512, 512, 512, &img);
    CHECK(img.width == 400);
    CHECK(img.height == 400);
    CHECK(img.pixels[0] == eye[16 * 512 + 56]);
    CHECK(img.pixels[399 * 400 + 399] == eye[415 * 512 + 455]);
    camera_stream::copy_view(camera_stream::EyeR, eye.data(), 512, 512, 512, &img);
    CHECK(img.pixels[0] == eye[16 * 512 + 0]);

    std::vector<uint8_t> face(400 * 404, 7);   // The stride is 404.
    face[404 + 1] = 200;
    camera_stream::copy_view(camera_stream::FaceL, face.data(), 400, 400, 404, &img);
    CHECK(img.width == 400);
    CHECK(img.pixels[400 + 1] == 200);
}

TEST_CASE("camera stream JPEGs decode back to the image") {
    camera_stream::Image img;
    img.width = 64;
    img.height = 48;
    img.pixels.resize(64 * 48);
    for (size_t i = 0; i < img.pixels.size(); i++)
        img.pixels[i] = uint8_t((i % 64) * 4);
    std::vector<uint8_t> jpg;
    REQUIRE(camera_stream::encode_jpeg(img, 90, &jpg));
    std::vector<uint8_t> back(64 * 48);
    int w = 0, h = 0;
    REQUIRE(jpeg::decode_gray(jpg.data(), jpg.size(), back.data(), back.size(), &w, &h));
    CHECK(w == 64);
    CHECK(h == 48);
    CHECK(std::abs(int(back[10]) - int(img.pixels[10])) < 8);
    camera_stream::Image empty;
    CHECK_FALSE(camera_stream::encode_jpeg(empty, 80, &jpg));
}

TEST_CASE("the camera stream serves frames to a TCP client only while active") {
    json threads = json::object();
    ExpressionState state;
    StatsRegistry stats;
    // Find a free ephemeral port first.
    int probe = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(bind(probe, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    REQUIRE(getsockname(probe, reinterpret_cast<sockaddr *>(&addr), &len) == 0);
    close(probe);

    auto sink = make_sink(json{{"type", "camera_stream"}, {"port", ntohs(addr.sin_port)}, {"fps", 100}},
                          SinkDeps{"camera_stream", threads, state, stats});
    REQUIRE(sink);
    CHECK(sink->wants_frames());
    REQUIRE(sink->start());
    auto *cam = static_cast<CameraStreamSink *>(sink.get());

    auto connect_client = [&] {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        return connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0 ? fd : (close(fd), -1);
    };
    CHECK(connect_client() < 0);   // The sink is not active, so it does not listen.

    CHECK_FALSE(sink->set_option("active", 1));
    REQUIRE(sink->set_option("active", true));
    const int fd = connect_client();
    REQUIRE(fd >= 0);
    for (int i = 0; i < 50 && cam->clients() == 0; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(cam->clients() == 1);

    // This eye frame comes from the simulated pipeline.
    auto pool = FramePool::create_owned(2, 512 * 512);
    auto f = pool->acquire();
    REQUIRE(f);
    memset(const_cast<uint8_t *>(f->data), 128, 512 * 512);
    f->origin = FrameOrigin::EyeR;
    f->fmt = PixelFormat::Gray8;
    f->width = f->height = f->stride = 512;
    f->size = 512 * 512;
    f->capture_ns = mono_ns();
    StereoFrame sf;
    sf.right = std::move(f);
    sink->offer_frames("eye", sf);
    // The mouth cameras give raw Bayer frames, and the sink sends them as gray.
    auto m = pool->acquire();
    REQUIRE(m);
    memset(const_cast<uint8_t *>(m->data), 60, 400 * 400);
    m->origin = FrameOrigin::MouthL;
    m->fmt = PixelFormat::Bayer8;
    m->width = m->height = m->stride = 400;
    m->size = 400 * 400;
    m->capture_ns = mono_ns();
    StereoFrame mouth;
    mouth.left = std::move(m);
    sink->offer_frames("mouth", mouth);

    uint8_t head[18];
    size_t got = 0;
    while (got < sizeof(head)) {
        pollfd p{fd, POLLIN, 0};
        REQUIRE(poll(&p, 1, 2000) == 1);
        const ssize_t n = recv(fd, head + got, sizeof(head) - got, 0);
        REQUIRE(n > 0);
        got += size_t(n);
    }
    CHECK(be(head, 4) == 0xFEEDFACE);
    // The first packet has both views or only the eye view, because both frames can arrive before the next client tick.
    REQUIRE((head[4] == 1 || head[4] == 2));
    CHECK((head[5] == camera_stream::EyeR || head[5] == camera_stream::FaceL));
    const uint64_t stamp = be(head + 6, 8);
    CHECK(stamp > 1'700'000'000'000'000'000ull);   // The stamp is in wall-clock nanoseconds.
    CHECK(be(head + 14, 4) > 100);                 // The payload is a JPEG.

    // The mouth view arrives in this packet or in a later packet.
    bool face_seen = head[5] == camera_stream::FaceL;
    std::vector<uint8_t> rest;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!face_seen && std::chrono::steady_clock::now() < until) {
        uint8_t buf[4096];
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, 200) != 1)
            continue;
        const ssize_t n = recv(fd, buf, sizeof(buf), 0);
        REQUIRE(n > 0);
        rest.insert(rest.end(), buf, buf + n);
        // A view header has id 0 (face_l) after the magic and count or after a previous view.
        for (size_t i = 0; i + 5 < rest.size() && !face_seen; i++)
            face_seen = be(rest.data() + i, 4) == 0xFEEDFACE && rest[i + 5] == camera_stream::FaceL;
    }
    CHECK(face_seen);

    // When the sink is off, it drops the client and closes the port.
    REQUIRE(sink->set_option("active", false));
    CHECK(cam->clients() == 0);
    CHECK(connect_client() < 0);
    close(fd);
    sink->stop();
}
