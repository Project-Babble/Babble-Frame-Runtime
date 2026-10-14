#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace babble::capture {

// Packages a finished recording session for upload (docs/dataset-format.md,
// "Package"): each source's lossless video becomes H.265 at constant QP from
// the hardware encoder (HevcEncoder) plus lossless FFV1 stills every
// still_every_s, verified; the stamps, labels, routine and Valve eye samples
// are copied; everything is hashed into the package's session.json; and the
// lot is tarred (ustar, uncompressed) into <session>/package.tar. The
// package/ folder it was built in is removed afterwards.
//
// Runs ffmpeg and tar as children, so call it from a thread pinned where the
// work should run. Takes a while: about 2000 fps for the encode, and the
// verification decodes the result once more on the CPU.
struct PackagerOptions {
    int qp = 20;
    double still_every_s = 2.0;
    std::string ffmpeg = "ffmpeg", ffprobe = "ffprobe", tar = "tar";
    // Checks that the encoder did what it was asked (QP ignored fails the package):
    // - stills decoded from the H.265 file match the lossless ones at this PSNR
    //   or better (dB). QP 20 measured 43-44.5 on eye and stereo mouth footage,
    //   about 50 on the clean mono USB mouth camera.
    // - the H.265 file uses at most this many bits per pixel. QP 20 measured
    //   0.38 on eye footage and 0.07 on the mono mouth camera; the encoder's own
    //   rate control at 20 Mbit/s took about 0.87 on eye footage.
    double min_still_psnr = 40.0;
    double max_bits_per_pixel = 0.6;
    std::string encoder_device;              // "" = find it
    std::function<bool()> cancelled;         // true: give up for now (a recording started)
    std::function<void(double)> progress;    // 0..1
};

struct PackageResult {
    bool ok = false;
    bool retry = false;   // a passing problem (encoder missing or busy, cancelled): try again later
    std::string error;
    std::filesystem::path tar;
    nlohmann::json package;   // the "package" block written into the package's session.json
};

PackageResult make_package(const std::filesystem::path &session_dir, const PackagerOptions &options);

// PSNR (dB) of two same-sized 8-bit images; 100 when identical.
double psnr(const uint8_t *a, const uint8_t *b, size_t n);

} // namespace babble::capture
