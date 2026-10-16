#include <doctest/doctest.h>

#include "core/clock.h"
#include "core/metrics.h"
#include "frame/frame_pool.h"
#include "sink/record_sink.h"
#include "state/expression_state.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

#include <unistd.h>

using namespace babble;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct Fixture {
    json threads = json::object();
    ExpressionState state;
    StatsRegistry stats;
    fs::path dir = fs::temp_directory_path() / ("babble_record_" + std::to_string(getpid()));
    std::shared_ptr<FramePool> pool = FramePool::create_owned(8, 128 * 128);

    Fixture() { fs::remove_all(dir); }
    ~Fixture() { fs::remove_all(dir); }

    std::unique_ptr<RecordSink> make(json extra = json::object()) {
        // Use a raw passthrough and not ffmpeg, so the "video" is the frames as written.
        json cfg = {{"type", "record"}, {"dir", dir.string()}, {"encoder", {"dd", "of={out}", "status=none"}},
                    {"container", "raw"}, {"min_free_gb", 0}};
        cfg.merge_patch(extra);
        return std::make_unique<RecordSink>(cfg, SinkDeps{"record", threads, state, stats, "V-test", "dev"});
    }

    // Makes a w x h frame with stride w + 8, every pixel equal to value and the padding equal to 0xEE.
    FramePtr frame(FrameOrigin o, uint32_t w, uint32_t h, uint8_t value, uint32_t seq) {
        auto f = pool->acquire();
        REQUIRE(f);
        const uint32_t stride = w + 8;
        uint8_t *d = const_cast<uint8_t *>(f->data);
        memset(d, 0xEE, size_t(stride) * h);
        for (uint32_t y = 0; y < h; y++)
            memset(d + size_t(y) * stride, value, w);
        f->origin = o;
        f->width = w;
        f->height = h;
        f->stride = stride;
        f->size = size_t(stride) * h;
        f->seq = seq;
        return f;
    }
};

template <typename Pred> bool wait_for(Pred p, int ms = 5000) {
    for (int i = 0; i < ms / 10; i++) {
        if (p())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return p();
}

std::string slurp(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

}

TEST_CASE("record: a session writes each source's frames, stamps, labels and a manifest") {
    Fixture fx;
    auto rec = fx.make();
    REQUIRE(rec->available());
    REQUIRE(rec->start());
    CHECK_FALSE(rec->tap_wanted());   // The sink copies nothing until a session starts.

    RecordSink::SessionSpec spec;
    spec.routine = "eye";
    spec.consent = {{"version", "t1"}, {"accepted_ns", 1}};
    spec.source_fps = {{"eye", 90.0}, {"face", 30.0}};
    spec.context = {{"eye_camera_fps", 90}};
    auto r = rec->start_session(spec);
    REQUIRE_MESSAGE(r.ok, r.message);
    CHECK(rec->tap_wanted());
    CHECK(rec->start_session(spec).error == "busy");

    const int n = 10;
    for (int i = 0; i < n; i++) {
        StereoFrame eye;
        eye.left = fx.frame(FrameOrigin::EyeL, 16, 8, uint8_t(i), uint32_t(i));
        eye.right = fx.frame(FrameOrigin::EyeR, 16, 8, uint8_t(100 + i), uint32_t(i));
        eye.capture_ns = 1000 + uint64_t(i) * 11;
        rec->tap_frames("eye", eye);
        StereoFrame face;
        face.left = fx.frame(FrameOrigin::Face, 12, 10, 7, uint32_t(i));
        face.capture_ns = 5000 + uint64_t(i);
        rec->tap_frames("face", face);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::string err;
    CHECK(rec->add_label("JawOpen", 1000, 1050, &err));
    CHECK_FALSE(rec->add_label("Bad", 10, 5, &err));
    REQUIRE(rec->stop_session());
    CHECK_FALSE(rec->tap_wanted());
    std::optional<RecordSink::Finished> fin;
    REQUIRE(wait_for([&] { return (fin = rec->take_finished()).has_value(); }));
    CHECK(fin->status == "complete");
    CHECK(fin->sources["eye"].frames == n);
    CHECK(fin->sources["face"].frames == n);
    CHECK_FALSE(rec->add_label("Late", 1, 2, &err));   // The sink does not record now.

    const fs::path sdir = r.dir;
    const std::string eye = slurp(sdir / "eye.raw");
    REQUIRE(eye.size() == size_t(n) * 32 * 8);
    // The frames are side by side without stride padding, so row 0 of frame 3 is 16 x 3 and then 16 x 103.
    CHECK(uint8_t(eye[3 * 256 + 0]) == 3);
    CHECK(uint8_t(eye[3 * 256 + 15]) == 3);
    CHECK(uint8_t(eye[3 * 256 + 16]) == 103);
    CHECK(uint8_t(eye[3 * 256 + 31]) == 103);
    CHECK(slurp(sdir / "face.raw").size() == size_t(n) * 12 * 10);

    const std::string stamps = slurp(sdir / "eye.stamps");
    REQUIRE(stamps.size() == size_t(n) * 24);
    uint64_t cap = 0, seq = 0;
    memcpy(&cap, stamps.data() + 24 * 4, 8);
    memcpy(&seq, stamps.data() + 24 * 4 + 8, 8);
    CHECK(cap == 1000 + 4 * 11);
    CHECK(seq == 4);

    CHECK(slurp(sdir / "labels.jsonl") == R"({"end_ns":1050,"label":"JawOpen","start_ns":1000})" "\n");

    const json m = json::parse(slurp(sdir / "session.json"));
    CHECK(m["format"] == 2);
    CHECK(m["stop_reason"].is_null());   // The session stopped without a reason.
    CHECK(m["device"].is_object());
    CHECK_FALSE(m.contains("routine_file"));   // The session has no routine_def.
    CHECK_FALSE(m.contains("valve_eye"));
    CHECK(m["status"] == "complete");
    CHECK(m["routine"] == "eye");
    CHECK(m["consent"]["version"] == "t1");
    CHECK(m["context"]["eye_camera_fps"] == 90);
    CHECK(m["runtime"]["version"] == "V-test");
    CHECK(m["sources"]["eye"]["layout"] == "stereo_lr");
    CHECK(m["sources"]["eye"]["width"] == 32);
    CHECK(m["sources"]["eye"]["frames"] == n);
    CHECK(m["sources"]["eye"]["timestamp_kind"] == "detected");
    CHECK(m["sources"]["eye"]["bytes"] == eye.size());
    CHECK(m["sources"]["face"]["layout"] == "mono");
    CHECK(m["sources"]["face"]["timestamp_kind"] == "arrival");
    CHECK(m["ended_ns"].is_number());
    rec->stop();
}

TEST_CASE("record format 2: routine.json, step/phase labels and Valve eye samples") {
    Fixture fx;
    auto rec = fx.make();
    REQUIRE(rec->start());
    eye_server::Sample early;
    rec->add_eye_sample(early);   // The sink ignores a sample when there is no session.
    RecordSink::SessionSpec spec;
    spec.routine = "eye";
    spec.routine_def = {{"steps", {{{"log", "eye_closed"}, {"pose", {{"EyeClosed", 1.0}}}}}}};
    spec.valve_eye = true;
    auto r = rec->start_session(spec);
    REQUIRE_MESSAGE(r.ok, r.message);
    CHECK(rec->wants_eye_samples());
    for (int i = 0; i < 5; i++) {
        eye_server::Sample s;
        s.received_ns = 1000 + uint64_t(i);
        s.layout = 5;
        s.sample_time = 2.5 + i;
        s.gaze[eye_server::kLeftEye] = {0.1f, 0.2f, -1.0f};
        s.pre_fusion_gaze[eye_server::kRightEye] = {0.3f, 0.4f, -0.9f};
        s.fixation = {0.0f, 0.0f, -0.7f};
        s.openness = {0.25f, float(i) / 4};
        rec->add_eye_sample(s);
    }
    std::string err;
    CHECK(rec->add_label("eye_closed", 10, 20, &err, {{"step", 0}, {"phase", "hold"}, {"label", "ignored"}}));
    StereoFrame sf;
    sf.left = fx.frame(FrameOrigin::EyeL, 8, 8, 1, 1);
    sf.right = fx.frame(FrameOrigin::EyeR, 8, 8, 2, 1);
    rec->tap_frames("eye", sf);
    REQUIRE(rec->stop_session("stopped by the app"));
    CHECK_FALSE(rec->wants_eye_samples());
    rec->add_eye_sample(early);   // The sink ignores a sample after the stop.
    REQUIRE(wait_for([&] { return rec->take_finished().has_value(); }));

    const fs::path sdir = r.dir;
    const json routine = json::parse(slurp(sdir / "routine.json"));
    CHECK(routine == spec.routine_def);
    CHECK(slurp(sdir / "labels.jsonl") ==
          R"({"end_ns":20,"label":"eye_closed","phase":"hold","start_ns":10,"step":0})" "\n");

    const std::string v = slurp(sdir / "valve_eye.bin");
    REQUIRE(v.size() == 5 * RecordSink::kEyeRecordBytes);
    const char *rec3 = v.data() + 3 * RecordSink::kEyeRecordBytes;
    uint64_t rx;
    double t;
    float gx, prz, fz, open_r;
    uint32_t flags;
    memcpy(&rx, rec3 + 0, 8);
    memcpy(&t, rec3 + 8, 8);
    memcpy(&gx, rec3 + 16, 4);                // This field is gaze[left].x.
    memcpy(&prz, rec3 + 40 + 4 * 5, 4);       // This field is pre_fusion_gaze[right].z.
    memcpy(&fz, rec3 + 64 + 8, 4);            // This field is fixation.z.
    memcpy(&open_r, rec3 + 80, 4);            // This field is openness[right].
    memcpy(&flags, rec3 + 84, 4);
    CHECK(rx == 1003);
    CHECK(t == 5.5);
    CHECK(gx == 0.1f);
    CHECK(prz == -0.9f);
    CHECK(fz == -0.7f);
    CHECK(open_r == 0.75f);
    CHECK(flags == 1);

    const json m = json::parse(slurp(sdir / "session.json"));
    CHECK(m["format"] == 2);
    CHECK(m["stop_reason"] == "stopped by the app");
    CHECK(m["routine_file"] == "routine.json");
    CHECK(m["routine_sha256"].get<std::string>().size() == 64);
    CHECK(m["valve_eye"]["records"] == 5);
    CHECK(m["valve_eye"]["layout"] == 5);
    CHECK(m["valve_eye"]["dropped"] == 0);
    rec->stop();
}

TEST_CASE("record: a slow encoder drops frames instead of blocking the camera, and size changes are dropped") {
    Fixture fx;
    auto rec = fx.make({{"queue_frames", 1}, {"encoder", {"sh", "-c", "sleep 1; cat > \"$0\"", "{out}"}}});
    REQUIRE(rec->start());
    auto r = rec->start_session({});
    REQUIRE(r.ok);
    const uint64_t t0 = mono_ns();
    for (int i = 0; i < 200; i++) {
        StereoFrame sf;
        sf.left = fx.frame(FrameOrigin::Face, 64, 64, 1, uint32_t(i));
        rec->tap_frames("face", sf);
    }
    StereoFrame odd;
    odd.left = fx.frame(FrameOrigin::Face, 32, 32, 1, 999);
    rec->tap_frames("face", odd);
    CHECK(mono_ns() - t0 < 500000000ull);   // The sink never waited for the encoder.
    const auto st = rec->status();
    REQUIRE(st.sources.count("face"));
    CHECK(st.sources.at("face").dropped > 0);
    CHECK(st.sources.at("face").size_changed == 1);
    rec->stop_session();
    REQUIRE(wait_for([&] { return rec->take_finished().has_value(); }, 15000));
    rec->stop();
}

TEST_CASE("record: unavailable without an encoder, and a broken encoder fails the session") {
    Fixture fx;
    auto none = fx.make({{"encoder", {"definitely-not-an-encoder"}}});
    REQUIRE(none->start());
    CHECK_FALSE(none->available());
    CHECK(none->start_session({}).error == "unavailable");
    none->stop();

    auto bad = fx.make({{"encoder", {"sh", "-c", "exit 1", "{out}"}}});
    REQUIRE(bad->start());
    auto r = bad->start_session({});
    REQUIRE(r.ok);
    for (int i = 0; i < 5; i++) {
        StereoFrame sf;
        sf.left = fx.frame(FrameOrigin::Face, 64, 64, 1, uint32_t(i));
        bad->tap_frames("face", sf);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    bad->stop_session();
    std::optional<RecordSink::Finished> fin;
    REQUIRE(wait_for([&] { return (fin = bad->take_finished()).has_value(); }));
    CHECK(fin->status == "failed");
    CHECK_FALSE(fin->error.empty());
    bad->stop();
}

TEST_CASE("record: shutting down mid-session finishes it") {
    Fixture fx;
    auto rec = fx.make();
    REQUIRE(rec->start());
    auto r = rec->start_session({});
    REQUIRE(r.ok);
    StereoFrame sf;
    sf.left = fx.frame(FrameOrigin::Face, 8, 8, 1, 1);
    rec->tap_frames("face", sf);
    rec->stop();
    const json m = json::parse(slurp(fs::path(r.dir) / "session.json"));
    CHECK(m["status"] == "complete");
    CHECK(m["stop_reason"] == "the runtime stopped");
    CHECK_FALSE(m.contains("error"));
}
