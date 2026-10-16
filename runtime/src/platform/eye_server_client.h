#pragma once

#include "core/metrics.h"
#include "core/threading.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace babble {

// The private eye-service shared memory of SteamVR, as konsti219/frameeyeosc documents it (see docs/eye-tracking.md).
namespace eye_server {

// The file size and the record offset are the only differences between the layout versions.
struct Layout {
    uint32_t version;
    size_t file_size, record_offset;
};
inline constexpr Layout kLayouts[] = {{4, 0x4f21a, 0x152}, {5, 0x4f21f, 0x157}};
inline constexpr size_t kMinFileSize = 0x4f21a;   // The size of the smallest known layout.
inline constexpr size_t kVersionOffset = 0x00, kInitializedOffset = 0x04;
inline constexpr size_t kMutexOffset = 0x08;       // A glibc pthread_mutex_t that is process-shared and robust.
inline constexpr size_t kMutexBytes = 0x30;
inline constexpr size_t kSequenceOffset = 0x38;    // The service increments this word and wakes the futex per record.
inline constexpr size_t kRequestedOffset = 0x3c;   // Consumers set this word to 1 to ask for records.
inline constexpr size_t kRecordBytes = 0xebc;

// The offsets inside the packed record.
namespace rec {
inline constexpr size_t kProducerState = 0x00;   // A u32 that is 1 when the record is valid.
inline constexpr size_t kSampleTime = 0x05;      // An f64.
inline constexpr size_t kGaze = 0x0d;            // An f32[2][3] for each eye after stereo fusion.
inline constexpr size_t kGazeCov = 0x25;         // An f32[2][3].
inline constexpr size_t kFixation = 0x3d;        // An f32[3] in metres, relative to the head, with -z forward.
inline constexpr size_t kPreFusionGaze = 0x49;   // An f32[2][3] for each eye before fusion.
inline constexpr size_t kPreFusionCov = 0x61;    // An f32[2][3].
inline constexpr size_t kOpenness = 0x79;        // An f32[2].
}

using Vec3 = std::array<float, 3>;

// In the arrays for each eye, index 0 is the left eye of the wearer and index 1 is the right eye.
inline constexpr int kLeftEye = 0, kRightEye = 1;

// One eye-service sample in head space, with +x right, +y up and -z forward.
struct Sample {
    uint64_t seq = 0;           // The runtime increments this counter for each published sample.
    uint64_t received_ns = 0;   // The mono_ns time when the runtime copied the sample.
    uint32_t layout = 0;        // The layout version of the file.
    double sample_time = 0;     // The time on the clock of the service.
    std::array<Vec3, 2> gaze{}, pre_fusion_gaze{};
    Vec3 fixation{};
    std::array<float, 2> openness{};
};

// Decodes kRecordBytes at rec and returns false if the record is invalid or a value is not finite.
bool parse_record(const uint8_t *rec, Sample *out);

}

// Reads the eye-service shared memory on its own thread, and writes only the mutex and the request flag.
class EyeServerClient {
public:
    enum Consumer : uint32_t { Gaze = 1, Openness = 2, Record = 4 };
    struct Options {
        std::string path = "/dev/shm/eye-server.mmap";
    };

    ~EyeServerClient();
    void start(ThreadSpec thread, Options options, StatsRegistry *stats);
    void stop();

    // Tells the reader that a consumer needs samples or not, and the reader waits when no consumer needs them.
    void want(Consumer c, bool on);
    // Returns true while the file is open and valid.
    bool available() const { return available_.load(); }
    // Returns the newest sample if it arrived within max_age_ns.
    std::optional<eye_server::Sample> latest(uint64_t max_age_ns) const;
    // Sets a function that the reader thread calls for each sample, so the function must be fast.
    void set_listener(std::function<void(const eye_server::Sample &)> fn);
    // Returns the layout version of the open file, or 0 if no file is open.
    uint32_t layout_version() const { return version_.load(); }

private:
    void run();
    bool open_map();
    void close_map();
    bool read_one();   // Requests, waits for and copies one record, and returns true for a new sample.

    ThreadSpec thread_spec_;
    Options options_;
    StatsRegistry *stats_ = nullptr;
    std::thread thread_;
    std::atomic<uint32_t> version_{0};
    size_t map_bytes_ = 0, record_offset_ = 0;
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
    std::mutex listener_mutex_;
    std::function<void(const eye_server::Sample &)> listener_;

    RateStat samples_;
    Counter timeouts_, reopens_;
};

}
