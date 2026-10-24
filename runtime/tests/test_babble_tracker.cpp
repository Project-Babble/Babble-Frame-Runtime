// These tests use data from a real Babble face tracker (OpenIris, USB 303a:8000).
#include <doctest/doctest.h>

#include "camera/usb/babble_usb_source.h"
#include "camera/usb/jpeg.h"
#include "camera/usb/uvc.h"
#include "core/metrics.h"
#include "model/babble_mono_face.h"
#include "model/executorch_runner.h"
#include "model/half.h"
#include "model/postprocess/face_post.h"
#include "state/expression_state.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <iterator>
#include <numeric>
#include <string>
#include <vector>

using namespace babble;
using json = nlohmann::json;

namespace {

std::vector<uint8_t> read_file(const std::string &name) {
    std::ifstream in(std::string(BABBLE_TEST_DATA_DIR) + "/" + name, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Reads a binary 8-bit PGM (P5) file and sets the width and height.
std::vector<uint8_t> read_pgm(const std::string &name, int *w, int *h) {
    std::ifstream in(std::string(BABBLE_TEST_DATA_DIR) + "/" + name, std::ios::binary);
    std::string magic;
    int maxval = 0;
    in >> magic >> *w >> *h >> maxval;
    in.get();
    std::vector<uint8_t> px(size_t(*w) * size_t(*h));
    in.read(reinterpret_cast<char *>(px.data()), std::streamsize(px.size()));
    return px;
}

// Makes one UVC payload with a 2-byte header (FID, EOF and ERR bits) and the data.
std::vector<uint8_t> payload(int fid, bool eof, std::vector<uint8_t> data, bool err = false) {
    std::vector<uint8_t> p{2, uint8_t((fid & 1) | (eof ? 0x02 : 0) | (err ? 0x40 : 0) | 0x80)};
    p.insert(p.end(), data.begin(), data.end());
    return p;
}

bool push(uvc::FrameAssembler &a, const std::vector<uint8_t> &p) { return a.push(p.data(), p.size()); }

}

TEST_CASE("UVC descriptors of the Babble tracker parse to its bulk MJPEG stream") {
    const auto all = read_file("babble_tracker_descriptors.bin");
    REQUIRE(all.size() > 18);   // The sysfs file has the device descriptor and then the configuration.
    std::string err;
    auto info = uvc::parse_config(all.data() + 18, all.size() - 18, &err);
    REQUIRE_MESSAGE(info, err);
    CHECK(info->bcd_uvc == 0x0150);
    CHECK(info->control_iface == 2);
    CHECK(info->stream_iface == 3);
    CHECK(info->endpoint == 0x83);
    CHECK(info->bulk);
    CHECK(info->endpoint_max_packet == 64);
    CHECK(info->alts.empty());
    REQUIRE(info->formats.size() == 1);
    CHECK(info->formats[0].kind == uvc::FormatKind::Mjpeg);
    CHECK(info->formats[0].index == 1);
    REQUIRE(info->formats[0].frames.size() == 4);
    for (const auto &f : info->formats[0].frames) {
        CHECK(f.width == 240);
        CHECK(f.height == 240);
        CHECK(f.default_interval == 166666);   // The interval of 166666 is 60 fps.
    }
    const auto pick = uvc::pick_mode(*info, {uvc::FormatKind::Mjpeg, 240, 240, 0});
    REQUIRE(pick);
    CHECK(pick->frame->index == 1);
    CHECK(pick->interval == 166666);
    // The default mode of the source (240x240 at 30 fps) still streams the tracker at its own rate.
    CHECK(uvc::pick_mode(*info, {uvc::FormatKind::Yuy2, 240, 240, 30})->format->kind == uvc::FormatKind::Mjpeg);

    // This is not a video device because it has only the CDC serial part.
    const uint8_t cdc_only[] = {9, 2, 18, 0, 1, 1, 0, 0x80, 50, 9, 4, 0, 0, 1, 2, 2, 0, 0};
    CHECK_FALSE(uvc::parse_config(cdc_only, sizeof(cdc_only), &err));
}

TEST_CASE("UVC descriptors of an isochronous camera parse to its MJPEG and YUY2 modes and its alts") {
    // This is the "Generic HD Camera" 1e45:0209, read from sysfs on the headset.
    const auto all = read_file("hd_camera_1e45_0209_descriptors.bin");
    REQUIRE(all.size() > 18);
    std::string err;
    auto info = uvc::parse_config(all.data() + 18, all.size() - 18, &err);
    REQUIRE_MESSAGE(info, err);
    CHECK(info->bcd_uvc == 0x0100);
    CHECK_FALSE(info->bulk);
    CHECK(info->endpoint == 0x81);
    REQUIRE(info->alts.size() == 7);
    CHECK(info->alts.front().bytes == 128);
    CHECK(info->alts[4].bytes == 2048);    // 2 x 1024, with the high-bandwidth multiplier.
    CHECK(info->alts.back().bytes == 3072);
    CHECK(info->alts.back().alt == 7);
    REQUIRE(info->formats.size() == 2);
    const auto &mjpeg = info->formats[0], &yuy2 = info->formats[1];
    CHECK(mjpeg.kind == uvc::FormatKind::Mjpeg);
    CHECK(yuy2.kind == uvc::FormatKind::Yuy2);
    CHECK(yuy2.index == 2);
    CHECK(yuy2.bits_per_pixel == 16);
    REQUIRE(mjpeg.frames.size() == 5);
    CHECK(mjpeg.frames[0].width == 640);
    CHECK(mjpeg.frames[0].intervals == std::vector<uint32_t>{83333, 166666, 333333, 400000, 500000});
    CHECK(uvc::frame_rates(mjpeg.frames[0]) == std::vector<int>{120, 60, 30, 25, 20});
    REQUIRE(yuy2.frames.size() == 5);
    CHECK(uvc::frame_rates(yuy2.frames[0]) == std::vector<int>{30});

    // Exact mode, nearest size, nearest rate, and a format that the camera does not have.
    auto p = uvc::pick_mode(*info, {uvc::FormatKind::Yuy2, 320, 240, 30});
    REQUIRE(p);
    CHECK(p->format->index == 2);
    CHECK(p->frame->index == 3);
    CHECK(p->interval == 333333);
    p = uvc::pick_mode(*info, {uvc::FormatKind::Mjpeg, 240, 240, 55});
    CHECK(p->frame->width == 320);   // 320x240 is the nearest to 240x240.
    CHECK(p->interval == 166666);    // 60 fps is the nearest to 55 fps.
    p = uvc::pick_mode(*info, {uvc::FormatKind::Nv12, 640, 480, 0});
    CHECK(p->format->kind == uvc::FormatKind::Mjpeg);
    CHECK(p->interval == 83333);     // Without a rate, the camera default is used.
    CHECK(uvc::format_from_key("yuy2") == uvc::FormatKind::Yuy2);
    CHECK(uvc::format_from_key("h264") == uvc::FormatKind::Unknown);
}

TEST_CASE("uncompressed UVC frames convert to grayscale and wrong sizes are rejected") {
    std::vector<uint8_t> out(16);
    int w = 0, h = 0;
    const std::vector<uint8_t> yuy2{10, 1, 20, 2, 30, 3, 40, 4};   // Two rows of two pixels: Y0 U Y1 V.
    REQUIRE(uvc::to_gray(uvc::FormatKind::Yuy2, yuy2.data(), yuy2.size(), 2, 2, out.data(), out.size(), &w, &h));
    CHECK(w == 2);
    CHECK(h == 2);
    CHECK(std::vector<uint8_t>(out.begin(), out.begin() + 4) == std::vector<uint8_t>{10, 20, 30, 40});
    REQUIRE(uvc::to_gray(uvc::FormatKind::Uyvy, yuy2.data(), yuy2.size(), 2, 2, out.data(), out.size(), &w, &h));
    CHECK(std::vector<uint8_t>(out.begin(), out.begin() + 4) == std::vector<uint8_t>{1, 2, 3, 4});
    const std::vector<uint8_t> nv12{5, 6, 7, 8, 99, 99};   // The Y plane, and then one UV pair.
    REQUIRE(uvc::to_gray(uvc::FormatKind::Nv12, nv12.data(), nv12.size(), 2, 2, out.data(), out.size(), &w, &h));
    CHECK(std::vector<uint8_t>(out.begin(), out.begin() + 4) == std::vector<uint8_t>{5, 6, 7, 8});
    REQUIRE(uvc::to_gray(uvc::FormatKind::Grey, nv12.data(), 4, 2, 2, out.data(), out.size(), &w, &h));
    CHECK(out[3] == 8);
    // A frame with missing bytes, or one that does not fit, fails.
    CHECK_FALSE(uvc::to_gray(uvc::FormatKind::Yuy2, yuy2.data(), 6, 2, 2, out.data(), out.size(), &w, &h));
    CHECK_FALSE(uvc::to_gray(uvc::FormatKind::Grey, nv12.data(), 4, 2, 2, out.data(), 3, &w, &h));
    CHECK_FALSE(uvc::to_gray(uvc::FormatKind::Mjpeg, yuy2.data(), yuy2.size(), 2, 2, out.data(), out.size(), &w, &h));
}

TEST_CASE("isochronous packets rebuild a frame: one payload per packet, FID toggles without EOF") {
    uvc::FrameAssembler a(64);
    // Each packet has its own header, and a packet with only a header carries no data.
    CHECK_FALSE(push(a, payload(0, false, {1, 2})));
    CHECK_FALSE(push(a, payload(0, false, {})));
    CHECK_FALSE(push(a, payload(0, false, {3})));
    CHECK(push(a, payload(1, false, {4})));
    CHECK(a.frame() == std::vector<uint8_t>{1, 2, 3});
    CHECK(push(a, payload(1, true, {5})));
    CHECK(a.frame() == std::vector<uint8_t>{4, 5});
}

TEST_CASE("each USB camera has its own settings, with a flip for the Babble tracker by default") {
    json threads = json::object();
    LatestSlot<StereoFrame> slot;
    StatsRegistry stats;
    BabbleUsbSource src(json{{"type", "babble_usb"}, {"enabled", false},
                             {"cameras", {{"1e45:0209@1-1", {{"format", "yuy2"}, {"rotate", 90}}}}}},
                        SourceDeps{"face", threads, slot, stats});
    usb::Found tracker, webcam;
    tracker.vendor = 0x303a;
    tracker.product = 0x8000;
    tracker.serial = "ABC";
    webcam.vendor = 0x1e45;
    webcam.product = 0x0209;
    webcam.sysfs = "/sys/bus/usb/devices/1-1";
    REQUIRE(usb::device_id(webcam) == "1e45:0209@1-1");

    auto t = src.settings_for(tracker);
    CHECK(t.flip_h);
    CHECK_FALSE(t.flip_v);
    CHECK(t.mode == uvc::Mode{uvc::FormatKind::Mjpeg, 240, 240, 30});
    auto w = src.settings_for(webcam);
    CHECK_FALSE(w.flip_h);
    CHECK(w.rotate == 90);
    CHECK(w.mode.format == uvc::FormatKind::Yuy2);

    // One key, or the whole object as the dashboard sends it.
    REQUIRE(src.set_option("cameras/303a:8000:ABC/flip_h", false));
    CHECK_FALSE(src.settings_for(tracker).flip_h);
    REQUIRE(src.set_option("cameras/1e45:0209@1-1",
                           json{{"format", "mjpeg"}, {"width", 640}, {"height", 480}, {"fps", 60}, {"rotate", 0},
                                {"flip_h", true}, {"flip_v", true}}));
    w = src.settings_for(webcam);
    CHECK(w.mode == uvc::Mode{uvc::FormatKind::Mjpeg, 640, 480, 60});
    CHECK(w.flip_h);
    CHECK(w.flip_v);
    CHECK(w.rotate == 0);
    CHECK(BabbleUsbSource::to_json(w)["format"] == "mjpeg");
    // Bad values are refused and change nothing.
    CHECK_FALSE(src.set_option("cameras/1e45:0209@1-1/rotate", 45));
    CHECK_FALSE(src.set_option("cameras/1e45:0209@1-1/format", "h264"));
    CHECK_FALSE(src.set_option("cameras/1e45:0209@1-1", json{{"flip_h", "yes"}}));
    CHECK_FALSE(src.set_option("cameras/1e45:0209@1-1/colour", true));
    CHECK(src.settings_for(webcam).rotate == 0);

    // Without the old mirror, the tracker default can be no flip.
    BabbleUsbSource plain(json{{"type", "babble_usb"}, {"enabled", false}, {"tracker_flip_h", false}},
                          SourceDeps{"face2", threads, slot, stats});
    CHECK_FALSE(plain.settings_for(tracker).flip_h);
}

TEST_CASE("the USB camera transform flips and then turns the image clockwise") {
    // The 3x2 image is   1 2 3
    //                    4 5 6
    auto run = [](int rotate, bool fh, bool fv, int *w, int *h) {
        std::vector<uint8_t> scratch[2];
        scratch[0] = {1, 2, 3, 4, 5, 6};
        std::vector<uint8_t> out(6);
        BabbleUsbSource::transform(scratch, 3, 2, rotate, fh, fv, out.data(), w, h);
        return out;
    };
    int w = 0, h = 0;
    CHECK(run(0, false, false, &w, &h) == std::vector<uint8_t>{1, 2, 3, 4, 5, 6});
    CHECK(run(0, true, false, &w, &h) == std::vector<uint8_t>{3, 2, 1, 6, 5, 4});
    CHECK(run(0, false, true, &w, &h) == std::vector<uint8_t>{4, 5, 6, 1, 2, 3});
    CHECK(run(0, true, true, &w, &h) == std::vector<uint8_t>{6, 5, 4, 3, 2, 1});
    // A clockwise quarter turn makes a 2x3 image:  4 1 / 5 2 / 6 3
    CHECK(run(90, false, false, &w, &h) == std::vector<uint8_t>{4, 1, 5, 2, 6, 3});
    CHECK(w == 2);
    CHECK(h == 3);
    CHECK(run(180, false, false, &w, &h) == std::vector<uint8_t>{6, 5, 4, 3, 2, 1});
    CHECK(w == 3);
    CHECK(run(270, false, false, &w, &h) == std::vector<uint8_t>{3, 6, 2, 5, 1, 4});
    // The flip comes first: a mirror and then a clockwise turn.
    CHECK(run(90, true, false, &w, &h) == std::vector<uint8_t>{6, 3, 5, 2, 4, 1});
}

TEST_CASE("UVC probe blocks have the version's length and round-trip") {
    CHECK(uvc::probe_length(0x0100) == 26);
    CHECK(uvc::probe_length(0x0110) == 34);
    CHECK(uvc::probe_length(0x0150) == 48);
    uvc::Probe p;
    p.format_index = 1;
    p.frame_index = 2;
    p.frame_interval = 166666;
    p.max_video_frame_size = 115200;
    p.max_payload_transfer_size = 64;
    const auto b = uvc::encode_probe(p, 0x0150);
    REQUIRE(b.size() == 48);
    auto back = uvc::decode_probe(b.data(), b.size());
    REQUIRE(back);
    CHECK(back->hint == 1);
    CHECK(back->format_index == 1);
    CHECK(back->frame_index == 2);
    CHECK(back->frame_interval == 166666);
    CHECK(back->max_video_frame_size == 115200);
    CHECK(back->max_payload_transfer_size == 64);
    CHECK_FALSE(uvc::decode_probe(b.data(), 20));
}

TEST_CASE("UVC frames are rebuilt from payloads: EOF, FID toggles, errors, size limits") {
    uvc::FrameAssembler a(16);
    CHECK_FALSE(push(a, payload(0, false, {1, 2})));
    CHECK(push(a, payload(0, true, {3})));
    CHECK(a.frame() == std::vector<uint8_t>{1, 2, 3});

    // If a device never sends EOF, the toggled FID ends the frame.
    CHECK_FALSE(push(a, payload(1, false, {4})));
    CHECK_FALSE(push(a, payload(1, false, {5})));
    CHECK(push(a, payload(0, false, {6})));
    CHECK(a.frame() == std::vector<uint8_t>{4, 5});

    // An error bit drops that frame, and the next frame is good.
    a.reset();
    const uint64_t dropped = a.dropped();
    CHECK_FALSE(push(a, payload(1, false, {7}, true)));
    CHECK_FALSE(push(a, payload(1, true, {8})));
    CHECK(a.dropped() == dropped + 1);
    CHECK(push(a, payload(0, true, {9})));
    CHECK(a.frame() == std::vector<uint8_t>{9});

    // A frame that is larger than the limit is dropped.
    CHECK_FALSE(push(a, payload(1, false, std::vector<uint8_t>(12, 1))));
    CHECK_FALSE(push(a, payload(1, true, std::vector<uint8_t>(8, 2))));

    // A header that claims more bytes than the payload holds is corrupt.
    const std::vector<uint8_t> bad{40, 0x02, 1, 2};
    CHECK_FALSE(push(a, bad));
}

TEST_CASE("a Babble tracker frame decodes to 240x240 grayscale") {
    const auto jpg = read_file("babble_tracker_frame.jpg");
    REQUIRE(uvc::looks_like_jpeg(jpg.data(), jpg.size()));
    std::vector<uint8_t> px(240 * 240);
    int w = 0, h = 0;
    REQUIRE(jpeg::decode_gray(jpg.data(), jpg.size(), px.data(), px.size(), &w, &h));
    CHECK(w == 240);
    CHECK(h == 240);
    const double mean = std::accumulate(px.begin(), px.end(), 0.0) / double(px.size());
    CHECK(mean == doctest::Approx(151.3).epsilon(0.02));
    // The decoder refuses a buffer that is too small and does not overrun it.
    CHECK_FALSE(jpeg::decode_gray(jpg.data(), jpg.size(), px.data(), 100, &w, &h));
    CHECK_FALSE(jpeg::decode_gray(jpg.data(), 10, px.data(), px.size(), &w, &h));
}

TEST_CASE("the mono face model (fp16) matches ONNX Runtime on a tracker frame") {
    ExecuTorchRunner::set_threads(1);
    ExecuTorchRunner r;
    REQUIRE(r.load(std::string(BABBLE_MODELS_DIR) + "/face_mono_224_fp16.pte"));
    REQUIRE(r.inputs().size() == 1);
    CHECK(r.inputs()[0].shape == std::vector<int64_t>{1, 1, 224, 224});
    CHECK(r.inputs()[0].dtype == DType::F16);
    REQUIRE(r.outputs().size() == 1);
    CHECK(r.outputs()[0].elements() == 45);

    int w = 0, h = 0;
    const auto px = read_pgm("face_in224.pgm", &w, &h);
    REQUIRE(w == 224);
    REQUIRE(h == 224);
    std::vector<float> x(px.size());
    for (size_t i = 0; i < px.size(); i++)
        x[i] = float(px[i]) / 255.0f;
    std::vector<uint16_t> xh(x.size()), yh(45);
    to_half(x.data(), xh.data(), x.size());
    std::vector<TensorView> iv{{xh.data(), &r.inputs()[0]}};
    std::vector<TensorView> ov{{yh.data(), &r.outputs()[0]}};
    REQUIRE(r.run(iv, ov));
    std::vector<float> y(45);
    from_half(yh.data(), y.data(), 45);

    std::ifstream f(std::string(BABBLE_TEST_DATA_DIR) + "/face_mono_golden.json");
    const auto golden = nlohmann::json::parse(f)["outputs"].get<std::vector<float>>();
    REQUIRE(golden.size() == 45);
    for (size_t k = 0; k < 45; k++)
        CHECK_MESSAGE(std::fabs(y[k] - golden[k]) <= 0.01f,
                      face::kShapes[k] << ": " << y[k] << " vs " << golden[k]);
}

TEST_CASE("the face One Euro filter passes the first sample and follows a step smoothly") {
    OneEuro f(1, face::kOneEuro);
    std::vector<float> x{0.0f};
    f.apply(x, 0.0);
    CHECK(x[0] == 0.0f);
    float prev = 0.0f;
    for (int i = 0; i < 30; i++) {
        x = {1.0f};
        f.apply(x, 1.0 / 60);
        CHECK(x[0] >= prev - 1e-6f);   // The value increases towards the step, within float rounding.
        CHECK(x[0] <= 1.0f);
        prev = x[0];
    }
    CHECK(prev > 0.9f);   // Fast movement opens the filter because of beta.
    f.reset();
    x = {0.25f};
    f.apply(x, 1.0 / 60);   // After a reset, the filter passes the value through again.
    CHECK(x[0] == 0.25f);
}

TEST_CASE("face calibration: auto scales by the session maximum, raw and manual clamp") {
    face::Calibration a(2, face::CalibMode::Auto);
    std::vector<float> x{0.3f, 0.8f};
    a.apply(x);
    CHECK(x[0] == doctest::Approx(0.3f));   // The value is below the 0.5 seed, so it stays raw.
    CHECK(x[1] == doctest::Approx(1.0f));   // The value is its own maximum.
    x = {0.3f, 0.4f};
    a.apply(x);
    CHECK(x[1] == doctest::Approx(0.5f));   // The result is 0.4 / 0.8.
    x = {0.3f, 0.9f};
    a.apply(x, false);                      // When the headset is not worn, the maximum does not change.
    CHECK(x[1] == doctest::Approx(1.0f));   // The value is clamped.
    a.reset();
    x = {0.3f, 0.4f};
    a.apply(x);
    CHECK(x[1] == doctest::Approx(0.4f));   // The reset removed the old maximum.

    face::Calibration raw(1, face::CalibMode::Raw);
    x = {1.4f};
    raw.apply(x);
    CHECK(x[0] == 1.0f);
    x = {-0.2f};
    raw.apply(x);
    CHECK(x[0] == 0.0f);

    face::Calibration man(1, face::CalibMode::Manual);
    man.set_range(0, 0.2f, 0.6f);
    x = {0.4f};
    man.apply(x);
    CHECK(x[0] == doctest::Approx(0.5f));

    face::CalibMode mode;
    CHECK(face::parse_mode("auto", &mode));
    CHECK(mode == face::CalibMode::Auto);
    CHECK_FALSE(face::parse_mode("magic", &mode));
}

TEST_CASE("the mono face pipeline crops the ROI and rotates before resizing") {
    const nlohmann::json threads = nlohmann::json::object();
    ExpressionState state;
    StatsRegistry stats;
    LatestSlot<StereoFrame> slot;
    auto make = [&](nlohmann::json cfg) {
        cfg["from"] = "face";
        cfg["model"] = "face_mono_224_fp16.pte";
        PipelineDeps deps{"face", threads, [&](const std::string &s) { return s == "face" ? &slot : nullptr; }, state,
                          stats, {}, BABBLE_MODELS_DIR};
        auto p = std::make_unique<BabbleMonoFace>(cfg, std::move(deps));
        REQUIRE(p->init());
        return p;
    };

    // The 240x240 image has a 100x100 square of 200 at (50, 60) and 0 elsewhere.
    std::vector<uint8_t> px(240 * 240, 0);
    for (int y = 60; y < 160; y++)
        for (int x = 50; x < 150; x++)
            px[size_t(y) * 240 + size_t(x)] = 200;
    Frame f;
    f.width = f.height = f.stride = 240;
    f.data = px.data();
    f.size = px.size();
    std::vector<float> in(224 * 224);

    auto roi = make({{"roi", {50, 60, 100, 100}}});
    roi->prepare(f, in.data());
    for (float v : in)
        REQUIRE(v == doctest::Approx(200.0f / 255.0f));

    // A ROI outside the image falls back to the whole image, as in Baballonia.
    auto outside = make({{"roi", {200, 200, 100, 100}}});
    outside->prepare(f, in.data());
    CHECK(in[0] == 0.0f);
    CHECK(in[112 * 224 + 90] == doctest::Approx(200.0f / 255.0f));

    // The left half is 255 and the right half is 0, so a 90 degree counter-clockwise rotation
    // puts the bright half at the bottom.
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 240; x++)
            px[size_t(y) * 240 + size_t(x)] = x < 120 ? 255 : 0;
    auto rot = make({{"rotate", 90}});
    rot->prepare(f, in.data());
    CHECK(in[10 * 224 + 112] == 0.0f);
    CHECK(in[213 * 224 + 112] == 1.0f);

    auto flip = make({{"flip", "h"}});   // The mirror puts the bright half on the right.
    flip->prepare(f, in.data());
    CHECK(in[112 * 224 + 10] == 0.0f);
    CHECK(in[112 * 224 + 213] == 1.0f);

    // The mirror option is gone, because each USB camera has its own flips in the source.
    auto old = make({{"mirror", true}});
    old->prepare(f, in.data());
    CHECK(in[112 * 224 + 10] == 1.0f);
    CHECK_FALSE(old->set_option("mirror", false));
}
