#include <doctest/doctest.h>

#include "capture/hevc_encoder.h"
#include "capture/packager.h"
#include "core/sha256.h"
#include "platform/process.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>

#include <unistd.h>

using namespace babble;
using namespace babble::capture;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

void write(const fs::path &p, const std::string &s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

std::string slurp(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

// A complete session: n frames of w x h gray (a moving gradient plus sensor-like
// noise) as lossless x264 in eye.mkv, with stamps, labels and a routine.
bool make_session(const fs::path &dir, uint32_t w, uint32_t h, int n) {
    fs::create_directories(dir);
    std::mt19937 rng(1);
    std::normal_distribution<float> noise(0, 2.3f);
    std::string frames, stamps;
    for (int i = 0; i < n; i++) {
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                frames += char(std::clamp(int(40 + (x + 2 * i) % 160 + y / 4 + noise(rng)), 0, 255));
        for (uint64_t v : {uint64_t(1000000 + i * 13888889ull), uint64_t(i), uint64_t(0)})
            for (int b = 0; b < 8; b++)
                stamps += char(v >> (8 * b));
    }
    write(dir / "eye.gray", frames);
    write(dir / "eye.stamps", stamps);
    write(dir / "labels.jsonl", R"({"label":"eye_closed","start_ns":1,"end_ns":2,"step":0,"phase":"hold"})" "\n");
    write(dir / "routine.json", R"({"steps":[]})");
    std::string out, err;
    if (Process::run({"ffmpeg", "-v", "error", "-f", "rawvideo", "-pix_fmt", "gray", "-s",
                      std::to_string(w) + "x" + std::to_string(h), "-r", "72", "-i", (dir / "eye.gray").string(),
                      "-c:v", "libx264", "-preset", "ultrafast", "-qp", "0", "-pix_fmt", "gray", "-y",
                      (dir / "eye.mkv").string()},
                     &out, &err, 60000) != 0)
        return false;
    fs::remove(dir / "eye.gray");
    json s = {{"format", 2}, {"id", dir.filename().string()}, {"status", "complete"}, {"routine", "eye"},
              {"sources",
               {{"eye",
                 {{"file", "eye.mkv"}, {"stamps", "eye.stamps"}, {"width", w}, {"height", h}, {"nominal_fps", 72.0},
                  {"frames", n}}}}}};
    write(dir / "session.json", s.dump());
    return true;
}

} // namespace

TEST_CASE("packager: refuses what it can't package, and a missing encoder is retried") {
    const fs::path dir = fs::temp_directory_path() / ("babble_pkg_" + std::to_string(getpid()));
    fs::remove_all(dir);
    write(dir / "a" / "session.json", R"({"status":"recording"})");
    auto r = make_package(dir / "a", {});
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.retry);
    CHECK(r.error == "not a complete session");

    write(dir / "b" / "session.json", R"({"status":"complete","sources":{}})");
    r = make_package(dir / "b", {});
    CHECK_FALSE(r.ok);
    CHECK(r.error == "the session recorded no frames");

    if (Process::find_in_path("ffmpeg") && make_session(dir / "c", 64, 32, 10)) {
        PackagerOptions o;
        o.encoder_device = "/dev/null";   // not an encoder: a passing problem, tried again later
        r = make_package(dir / "c", o);
        CHECK_FALSE(r.ok);
        CHECK(r.retry);
        CHECK(r.error.find("hardware encoder") == 0);
        CHECK_FALSE(fs::exists(dir / "c" / "package.tar"));
    }
    CHECK(psnr(reinterpret_cast<const uint8_t *>("abc"), reinterpret_cast<const uint8_t *>("abc"), 3) == 100);
    fs::remove_all(dir);
}

TEST_CASE("packager: a session becomes a verified, hashed H.265 package (hardware encoder only)") {
    if (HevcEncoder::find_device().empty() || !Process::find_in_path("ffmpeg")) {
        MESSAGE("no hardware H.265 encoder: skipped");
        return;
    }
    const fs::path dir = fs::temp_directory_path() / ("babble_pkg_hw_" + std::to_string(getpid())) / "s1";
    fs::remove_all(dir.parent_path());
    const int n = 300;   // a little over 4 s at 72: three stills
    REQUIRE(make_session(dir, 320, 160, n));
    PackagerOptions o;
    double last = 0;
    o.progress = [&](double p) { last = p; };
    const auto r = make_package(dir, o);
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(last == 1.0);
    CHECK(r.tar == dir / "package.tar");
    CHECK_FALSE(fs::exists(dir / "package"));   // built in, then removed

    const json &src = r.package["sources"]["eye"];
    CHECK(src["frames"] == n);
    CHECK(src["still_every"] == 144);
    CHECK(src["still_count"] == 3);
    CHECK(src["still_psnr_min_db"].get<double>() > 40);
    CHECK(r.package["codec"]["qp"] == 20);
    CHECK(r.package["codec"]["driver"].get<std::string>().find("iris") != std::string::npos);

    const fs::path x = dir.parent_path() / "x";
    fs::create_directories(x);
    std::string out, err;
    REQUIRE(Process::run({"tar", "-xf", r.tar.string(), "-C", x.string()}, &out, &err, 60000) == 0);
    for (const auto &f : r.package["files"]) {
        const std::string name = f["name"];
        CHECK_MESSAGE(Sha256::file_hex(x / name).value_or("") == f["sha256"].get<std::string>(), name);
    }
    CHECK(slurp(x / "eye.stamps") == slurp(dir / "eye.stamps"));
    CHECK(slurp(x / "routine.json") == slurp(dir / "routine.json"));
    const json m = json::parse(slurp(x / "session.json"));
    CHECK(m["package"]["files"] == r.package["files"]);
    CHECK(m["routine"] == "eye");
    fs::remove_all(dir.parent_path());
}
