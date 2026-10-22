#include "capture/packager.h"

#include "capture/hevc_encoder.h"
#include "core/clock.h"
#include "core/log.h"
#include "core/sha256.h"
#include "platform/process.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <vector>

#include <sys/utsname.h>

namespace {
const babble::log::Logger LOG("package");
}

namespace babble::capture {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

json read_json(const fs::path &p) {
    std::ifstream f(p);
    if (!f)
        return json();
    json j = json::parse(f, nullptr, false);
    return j.is_discarded() ? json() : j;
}

bool write_json(const fs::path &p, const json &j) {
    std::ofstream f(p);
    f << j.dump(2) << '\n';
    return bool(f);
}

std::string num(double v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%g", v);
    return buf;
}

std::string kernel_release() {
    utsname u{};
    return uname(&u) == 0 ? u.release : "";
}

// One recorded source to package.
struct Job {
    std::string name, file, stamps;
    uint32_t w = 0, h = 0;
    double fps = 30;
    uint64_t records = 0;   // The number of stamps records, which is the frame count that the video must have.
};

}

double psnr(const uint8_t *a, const uint8_t *b, size_t n) {
    double se = 0;
    for (size_t i = 0; i < n; i++) {
        const double d = double(a[i]) - double(b[i]);
        se += d * d;
    }
    if (se == 0 || n == 0)
        return 100;
    return 10 * std::log10(255.0 * 255.0 / (se / double(n)));
}

PackageResult make_package(const fs::path &sdir, const PackagerOptions &o) {
    PackageResult res;
    auto fail = [&](const std::string &why, bool retry = false) {
        res.ok = false;
        res.error = why;
        res.retry = retry;
        return res;
    };
    const json session = read_json(sdir / "session.json");
    if (!session.is_object() || session.value("status", "") != "complete")
        return fail("not a complete session");
    std::error_code ec;
    const fs::path pdir = sdir / "package", logs = sdir / "package-logs";
    const fs::path tar = sdir / "package.tar", part = sdir / "package.tar.part";
    fs::remove_all(pdir, ec);
    fs::remove(tar, ec);
    fs::remove(part, ec);
    if (!fs::create_directories(pdir, ec) || (fs::create_directories(logs, ec), ec))
        return fail("cannot create " + pdir.string());

    std::vector<Job> jobs;
    uint64_t total_frames = 0;
    const json recorded = session.value("sources", json::object());
    for (const auto &[name, s] : recorded.items()) {
        if (!s.is_object() || s.value("frames", uint64_t(0)) == 0)
            continue;
        Job j;
        j.name = name;
        j.file = s.value("file", "");
        j.stamps = s.value("stamps", "");
        j.w = s.value("width", 0u);
        j.h = s.value("height", 0u);
        j.fps = s.value("nominal_fps", 0.0);
        if (j.fps <= 0)
            j.fps = s.value("measured_fps", 0.0);
        if (j.fps <= 0)
            j.fps = 30;
        if (j.file.empty() || j.stamps.empty() || j.w == 0 || j.h == 0 || j.w % 2 || j.h % 2)
            return fail("source " + name + " isn't described well enough to package");
        const uint64_t sb = fs::file_size(sdir / j.stamps, ec);
        if (ec || !fs::exists(sdir / j.file, ec))
            return fail("source " + name + ": " + j.file + " or " + j.stamps + " is missing");
        j.records = sb / 24;
        total_frames += 2 * j.records;   // Each frame is encoded and then decoded again for the check.
        jobs.push_back(j);
    }
    if (jobs.empty())
        return fail("the session recorded no frames");

    uint64_t done_frames = 0;
    auto tick = [&]() {
        done_frames++;
        if ((done_frames & 63) == 0) {
            if (o.progress)
                o.progress(double(done_frames) / double(total_frames));
            if (o.cancelled && o.cancelled())
                return false;
        }
        return true;
    };

    json sources = json::object();
    std::string device, driver;
    for (const Job &j : jobs) {
        const size_t frame_bytes = size_t(j.w) * j.h;
        const uint64_t still_every = std::max<uint64_t>(1, uint64_t(std::llround(o.still_every_s * j.fps)));
        const uint64_t stills_total = (j.records + still_every - 1) / still_every;
        const uint64_t check_every = std::max<uint64_t>(1, stills_total / 16);   // Keep about 16 stills for the check.
        const std::string out_name = j.name + ".hevc.mkv", ref_name = j.name + ".ref.mkv";
        const fs::path out = pdir / out_name, ref = pdir / ref_name;
        std::string err;

        Process dec, mux, stills;
        Process::Options dopt, mopt, sopt;
        dopt.stdout_pipe = true;
        dopt.stderr_file = (logs / ("decode-" + j.name + ".log")).string();
        mopt.stdin_pipe = true;
        mopt.stderr_file = (logs / ("mux-" + j.name + ".log")).string();
        sopt.stdin_pipe = true;
        sopt.stderr_file = (logs / ("stills-" + j.name + ".log")).string();
        if (!dec.spawn({o.ffmpeg, "-hide_banner", "-nostdin", "-v", "error", "-i", (sdir / j.file).string(), "-map",
                        "0:v:0", "-f", "rawvideo", "-pix_fmt", "gray", "-fps_mode", "passthrough", "-"},
                       dopt, &err) ||
            !mux.spawn({o.ffmpeg, "-hide_banner", "-nostdin", "-v", "error", "-f", "hevc", "-r", num(j.fps), "-i", "-",
                        "-c", "copy", "-y", out.string()},
                       mopt, &err) ||
            !stills.spawn({o.ffmpeg, "-hide_banner", "-nostdin", "-v", "error", "-f", "rawvideo", "-pix_fmt", "gray",
                           "-s", std::to_string(j.w) + "x" + std::to_string(j.h), "-r",
                           num(j.fps / double(still_every)), "-i", "-", "-c:v", "ffv1", "-level", "3", "-g", "1", "-y",
                           ref.string()},
                          sopt, &err))
            return fail(o.ffmpeg + ": " + err);

        HevcEncoder enc;
        HevcEncoder::Settings es;
        es.width = j.w;
        es.height = j.h;
        es.fps = j.fps;
        es.qp = o.qp;
        if (!enc.open(es, [&](const uint8_t *p, size_t n) { return mux.write_stdin(p, n, 10000); }, &err,
                      o.encoder_device))
            return fail("hardware encoder: " + err, true);
        device = enc.device();
        driver = enc.driver();

        std::vector<uint8_t> frame(frame_bytes);
        std::map<uint64_t, std::vector<uint8_t>> kept;   // This maps a frame index to its lossless still.
        uint64_t n = 0;
        for (;;) {
            const int r = dec.read_stdout_exact(frame.data(), frame_bytes, 30000);
            if (r == 0)
                break;
            if (r < 0)
                return fail("decoding " + j.file + " failed (see package-logs)");
            if (!enc.encode(frame.data(), &err))
                return fail("hardware encoder: " + err, true);
            if (n % still_every == 0) {
                if (!stills.write_stdin(frame.data(), frame_bytes, 10000))
                    return fail("writing the stills failed (see package-logs)");
                if ((n / still_every) % check_every == 0)
                    kept[n] = frame;
            }
            n++;
            if (!tick())
                return fail("cancelled", true);
        }
        if (!enc.finish(&err))
            return fail("hardware encoder: " + err, true);
        const uint64_t packets = enc.packets_out();
        enc.close();
        mux.close_stdin();
        stills.close_stdin();
        for (Process *p : {&dec, &mux, &stills})
            if (!p->wait(120000) || p->exit_code().value_or(-1) != 0)
                return fail("ffmpeg failed while packaging " + j.name + " (see package-logs)");
        if (n != j.records)
            return fail(j.file + " has " + std::to_string(n) + " frames but " + j.stamps + " has " +
                        std::to_string(j.records) + " records");
        if (packets != n)
            return fail("the encoder returned " + std::to_string(packets) + " packets for " + std::to_string(n) +
                        " frames");

        // Make sure that the file is full range and has every frame and every still.
        std::string range, perr;
        Process::run({o.ffprobe, "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=color_range", "-of",
                      "default=nw=1:nk=1", out.string()},
                     &range, &perr, 60000);
        range.erase(std::remove_if(range.begin(), range.end(), [](char c) { return c == '\n' || c == '\r'; }),
                    range.end());
        if (range != "pc")
            return fail(out_name + " isn't tagged full range (" + (range.empty() ? perr : range) + ")");
        Process vdec;
        Process::Options vopt;
        vopt.stdout_pipe = true;
        vopt.stderr_file = (logs / ("verify-" + j.name + ".log")).string();
        if (!vdec.spawn({o.ffmpeg, "-hide_banner", "-nostdin", "-v", "error", "-i", out.string(), "-map", "0:v:0",
                         "-f", "rawvideo", "-pix_fmt", "gray", "-fps_mode", "passthrough", "-"},
                        vopt, &err))
            return fail(o.ffmpeg + ": " + err);
        uint64_t m = 0;
        double worst = 1000, sum = 0;
        for (;;) {
            const int r = vdec.read_stdout_exact(frame.data(), frame_bytes, 30000);
            if (r == 0)
                break;
            if (r < 0)
                return fail("decoding " + out_name + " failed (see package-logs)");
            if (auto it = kept.find(m); it != kept.end()) {
                const double db = psnr(frame.data(), it->second.data(), frame_bytes);
                worst = std::min(worst, db);
                sum += db;
            }
            m++;
            if (!tick())
                return fail("cancelled", true);
        }
        vdec.wait(10000);
        if (m != n)
            return fail(out_name + " decodes to " + std::to_string(m) + " frames, not " + std::to_string(n));
        const double mean = kept.empty() ? 0 : sum / double(kept.size());
        if (kept.empty() || worst < o.min_still_psnr)
            return fail(out_name + ": stills match at " + num(std::round(worst * 10) / 10) + " dB, below " +
                        num(o.min_still_psnr) + " (did the encoder ignore QP " + std::to_string(o.qp) + "?)");
        std::error_code size_ec;
        const uint64_t out_bytes = fs::file_size(out, size_ec);
        const double bpp = n ? double(out_bytes) * 8 / (double(frame_bytes) * double(n)) : 0;
        if (size_ec || bpp > o.max_bits_per_pixel)
            return fail(out_name + ": " + num(std::round(bpp * 1000) / 1000) + " bits per pixel, above " +
                        num(o.max_bits_per_pixel) + " (did the encoder ignore QP " + std::to_string(o.qp) + "?)");
        LOG.info("package: %s: %s frames, %s stills, still PSNR min %.1f mean %.1f dB, %.3f bits per pixel",
                 j.name.c_str(), std::to_string(n).c_str(), std::to_string(stills_total).c_str(), worst, mean, bpp);
        sources[j.name] = {{"file", out_name},
                           {"stills", ref_name},
                           {"stamps", j.stamps},
                           {"frames", n},
                           {"still_every", still_every},
                           {"still_count", stills_total},
                           {"stills_checked", kept.size()},
                           {"still_psnr_min_db", std::round(worst * 100) / 100},
                           {"still_psnr_mean_db", std::round(mean * 100) / 100},
                           {"bits_per_pixel", std::round(bpp * 1000) / 1000}};
    }

    // Copy the rest of the session as it was recorded.
    std::vector<std::string> copy;
    for (const Job &j : jobs)
        copy.push_back(j.stamps);
    for (const char *f : {"labels.jsonl", "routine.json", "valve_eye.bin", "gaze.bin"})
        copy.push_back(f);
    for (const auto &f : copy)
        if (fs::exists(sdir / f, ec) && !fs::copy_file(sdir / f, pdir / f, fs::copy_options::overwrite_existing, ec))
            return fail("cannot copy " + f);

    std::vector<std::string> names;
    for (const auto &e : fs::directory_iterator(pdir, ec))
        if (e.is_regular_file())
            names.push_back(e.path().filename().string());
    std::sort(names.begin(), names.end());
    json files = json::array();
    for (const auto &name : names) {
        const auto h = Sha256::file_hex(pdir / name);
        if (!h)
            return fail("cannot read " + name);
        files.push_back({{"name", name}, {"bytes", fs::file_size(pdir / name, ec)}, {"sha256", *h}});
    }
    json pkg = {{"format", 2},
                {"made_unix_ns", wall_ns()},
                {"codec",
                 {{"name", "hevc"},
                  {"qp", o.qp},
                  {"gop_s", 2},
                  {"encoder", "v4l2 stateful (hardware)"},
                  {"device", device},
                  {"driver", driver},
                  {"kernel", kernel_release()},
                  {"decode", "full range: scale=in_range=pc:out_range=pc,format=gray"}}},
                {"stills", {{"codec", "ffv1"}, {"every_s", o.still_every_s}}},
                {"sources", sources},
                {"files", files}};
    json manifest = session;
    manifest["package"] = pkg;
    if (!write_json(pdir / "session.json", manifest))
        return fail("cannot write the package's session.json");

    std::vector<std::string> targv = {o.tar, "--format=ustar", "--owner=0", "--group=0", "--numeric-owner", "-cf",
                                      part.string(), "-C", pdir.string(),
                                      "session.json"};
    targv.insert(targv.end(), names.begin(), names.end());
    std::string tout, terr;
    if (Process::run(targv, &tout, &terr, 600000) != 0)
        return fail("tar failed: " + terr);
    fs::rename(part, tar, ec);
    if (ec)
        return fail("cannot rename the package: " + ec.message());
    fs::remove_all(pdir, ec);
    if (o.progress)
        o.progress(1.0);
    res.ok = true;
    res.tar = tar;
    res.package = pkg;
    return res;
}

}
