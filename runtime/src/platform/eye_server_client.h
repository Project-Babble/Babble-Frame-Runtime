#pragma once

#include "core/metrics.h"
#include "core/threading.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace babble {

// SteamVR's private eye-service shared memory, /dev/shm/eye-server.mmap.
// Beyond the fused gaze the public input API gives, it carries each eye's
// gaze before stereo fusion and Valve's own lid openness. Layout and access
// protocol as documented by konsti219/frameeyeosc (MIT), for layout version 4
// (Steam Frame, SteamVR 2.17.10).
namespace eye_server {

inline constexpr size_t kMinFileSize = 0x4f21a;
inline constexpr size_t kVersionOffset = 0x00, kInitializedOffset = 0x04;
inline constexpr size_t kMutexOffset = 0x08;       // glibc pthread_mutex_t, process-shared + robust
inline constexpr size_t kMutexBytes = 0x30;
inline constexpr size_t kSequenceOffset = 0x38;    // bumped (and futex-woken) per new record
inline constexpr size_t kRequestedOffset = 0x3c;   // consumers set 1 to ask for records
inline constexpr size_t kRecordOffset = 0x152;
inline constexpr size_t kRecordBytes = 0xebc;

// Offsets inside the packed record.
namespace rec {
inline constexpr size_t kProducerState = 0x00;   // u32, 1 = valid
inline constexpr size_t kSampleTime = 0x05;      // f64
inline constexpr size_t kGaze = 0x0d;            // f32[2][3], per eye after stereo fusion
inline constexpr size_t kGazeCov = 0x25;         // f32[2][3]
inline constexpr size_t kFixation = 0x3d;        // f32[3], head-relative, -z forward, metres
inline constexpr size_t kPreFusionGaze = 0x49;   // f32[2][3], per eye before fusion
inline constexpr size_t kPreFusionCov = 0x61;    // f32[2][3]
inline constexpr size_t kOpenness = 0x79;        // f32[2]
} // namespace rec

using Vec3 = std::array<float, 3>;

// Per-eye arrays: index 0 is the wearer's left eye, 1 the right, as
// frameeyeosc reads them ("left, right"). Confirmed in VRChat for gaze
// (convergence) and closed eyes. The mirrored visualizer shows the sides
// swapped; judge sides in game, not there.
inline constexpr int kLeftEye = 0, kRightEye = 1;

// One eye-service sample; per-eye arrays indexed kRightEye / kLeftEye.
// Directions: +x right, +y up, -z forward (head space).
struct Sample {
    uint64_t seq = 0;           // ours: increments per published sample
    uint64_t received_ns = 0;   // mono_ns when we copied it
    double sample_time = 0;     // the service's clock
    std::array<Vec3, 2> gaze{}, pre_fusion_gaze{};
    Vec3 fixation{};
    std::array<float, 2> openness{};
};

// Decodes a record (kRecordBytes at rec). False if the producer marked it
// invalid or any value isn't finite.
bool parse_record(const uint8_t *rec, Sample *out);

} // namespace eye_server

// Reads the eye-service shared memory on its own thread while any consumer
// wants it. Validates the file (size, layout version, initialized) before
// touching it and never writes anything but the consumer protocol: the shared
// mutex and the metadata_requested flag. An unknown layout is left alone
// (logged once) and retried every few seconds; so is a missing file.
class EyeServerClient {
public:
    enum Consumer : uint32_t { Gaze = 1, Openness = 2 };
    struct Options {
        std::string path = "/dev/shm/eye-server.mmap";
        uint32_t version = 4;
    };

    ~EyeServerClient();
    void start(ThreadSpec thread, Options options, StatsRegistry *stats);
    void stop();

    // A consumer starts or stops needing samples; the reader idles when none do.
    void want(Consumer c, bool on);
    // True while the file is open and validated.
    bool available() const { return available_.load(); }
    // The newest sample if it arrived within max_age_ns.
    std::optional<eye_server::Sample> latest(uint64_t max_age_ns) const;

private:
    void run();
    bool open_map();
    void close_map();
    bool read_one();   // one request/wait/copy round; true if a new sample was published

    ThreadSpec thread_spec_;
    Options options_;
    StatsRegistry *stats_ = nullptr;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<uint32_t> wanted_{0};
    std::atomic<bool> available_{false};

    int fd_ = -1;
    uint8_t *base_ = nullptr;
    std::string last_problem_;
    bool announced_ = false;
    uint64_t seq_ = 0;

    mutable std::mutex mutex_;
    std::optional<eye_server::Sample> latest_;

    RateStat samples_;
    Counter timeouts_, reopens_;
};

} // namespace babble
