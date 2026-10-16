#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace babble::capture {

// Packages a finished session into <session>/package.tar with H.265 video and lossless FFV1 stills for upload.
struct PackagerOptions {
    int qp = 20;
    double still_every_s = 2.0;
    std::string ffmpeg = "ffmpeg", ffprobe = "ffprobe", tar = "tar";
    // These limits on the still PSNR in dB and on the bits per pixel fail the package if the encoder ignored the QP.
    double min_still_psnr = 40.0;
    double max_bits_per_pixel = 0.6;
    std::string encoder_device;              // The value "" finds the device automatically.
    std::function<bool()> cancelled;         // Returns true to stop the package for now when a recording starts.
    std::function<void(double)> progress;    // Receives the progress from 0 to 1.
};

struct PackageResult {
    bool ok = false;
    bool retry = false;   // The problem is temporary, so try again later.
    std::string error;
    std::filesystem::path tar;
    nlohmann::json package;   // The "package" block for the session.json of the package.
};

PackageResult make_package(const std::filesystem::path &session_dir, const PackagerOptions &options);

// Returns the PSNR in dB of two 8-bit images of the same size, or 100 if they are identical.
double psnr(const uint8_t *a, const uint8_t *b, size_t n);

}
