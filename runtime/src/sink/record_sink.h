#pragma once

#include "camera/tracking_source.h"
#include "core/threading.h"
#include "platform/eye_server_client.h"
#include "sink/sink.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace babble {

class Process;

// Config type "record": records the runtime's own camera sources for the
// dataset capture app. A session writes, per source, a video (raw 8-bit gray
// frames piped into the configured encoder, ffmpeg/x264 by default) and a
// timestamp file, plus the app's labels and a session.json manifest, into
// <dir>/<YYYYmmdd-HHMMSS>/. Frames come from the sources' frame taps (every
// camera frame, before tracking-rate subsampling); stereo pairs are recorded
// side by side (left | right).
//
// Threads: the taps copy frames into per-source queues (never blocking: a
// full queue drops and counts); one writer thread feeds the encoders and
// writes the stamps. Sessions are started, labelled and stopped from the main
// thread; stopping finishes asynchronously, reported by take_finished().
//
// Per frame, <source>.stamps gets a 24-byte little-endian record:
// u64 capture_ns (CLOCK_MONOTONIC), u64 seq, i64 skew_ns.
//
// Format 2 (docs/dataset-format.md) adds routine.json (the routine the app
// ran) and valve_eye.bin: SteamVR's eye-service samples, forwarded by main
// through add_eye_sample(), one 88-byte little-endian record each.
class RecordSink : public ISink, public IFrameTap {
public:
    struct SessionSpec {
        std::string routine;
        nlohmann::json meta = nlohmann::json::object();
        nlohmann::json consent;                              // null = none given
        std::map<std::string, double> source_fps;            // nominal rate per source, for the encoder
        nlohmann::json context = nlohmann::json::object();   // recorded as-is (camera rate, display refresh...)
        nlohmann::json routine_def;                          // the app's routine definition; null = none
        bool valve_eye = false;                              // record Valve's eye-service samples
    };
    struct StartResult {
        bool ok = false;
        std::string error;     // busy | unavailable | disk | io
        std::string message;
        std::string id, dir;
    };
    struct SourceCounts {
        uint64_t frames = 0, dropped = 0, size_changed = 0;
    };
    struct Status {
        bool recording = false;   // a session is open (including while it finishes)
        bool finishing = false;
        std::string id;
        uint64_t started_ns = 0;
        std::map<std::string, SourceCounts> sources;
    };
    struct Finished {
        std::string id, dir;
        std::string status;   // complete | failed
        std::string error;         // why it failed ("" if it didn't)
        std::string stop_reason;   // why it stopped: the app, the runtime, low disk
        std::map<std::string, SourceCounts> sources;
    };

    RecordSink(const nlohmann::json &cfg, SinkDeps deps);
    ~RecordSink() override;

    bool start() override;
    void stop() override;

    // IFrameTap: capture threads.
    bool tap_wanted() const override { return accepting_.load(std::memory_order_relaxed); }
    void tap_frames(const std::string &source, const StereoFrame &frames) override;

    // Main thread.
    bool available() const;   // the encoder can be found
    StartResult start_session(const SessionSpec &spec);
    // extra: more fields for the label's line (step, phase).
    bool add_label(const std::string &label, uint64_t start_ns, uint64_t end_ns, std::string *err,
                   const nlohmann::json &extra = nlohmann::json::object());
    bool stop_session(const std::string &reason = "");   // false if no session is open
    std::optional<Finished> take_finished();
    Status status() const;
    const std::filesystem::path &dir() const { return dir_; }

    // Any thread (the eye-service reader): queues a Valve sample for
    // valve_eye.bin while a session that wants them is recording.
    void add_eye_sample(const eye_server::Sample &s);
    bool wants_eye_samples() const { return eye_accepting_.load(std::memory_order_relaxed); }

    static constexpr size_t kEyeRecordBytes = 88;
    static void encode_eye_record(const eye_server::Sample &s, uint8_t out[kEyeRecordBytes]);

private:
    struct Stamp {
        uint64_t capture_ns = 0, seq = 0;
        int64_t skew_ns = 0;
    };
    struct Item {
        size_t buf = 0;
        Stamp stamp;
    };
    struct Channel;

    Channel *channel(const std::string &source);
    void run();
    bool write_items(Channel &ch);
    void write_eye_samples();
    bool open_encoder(Channel &ch);
    void finish_session(int encoder_wait_ms);
    void write_manifest(const std::string &status, const std::string &error, const std::string &stop_reason);
    bool low_disk() const;

    std::string name_;
    ThreadSpec thread_spec_;
    std::filesystem::path dir_;
    std::vector<std::string> encoder_;
    std::string container_;
    size_t queue_frames_ = 64;
    double min_free_gb_ = 5;
    std::string version_, profile_;
    nlohmann::json device_;   // headset model and OS build, read once

    std::thread thread_;
    std::atomic<bool> stop_thread_{false};
    std::atomic<bool> accepting_{false};   // taps copy frames
    std::mutex cv_mutex_;
    std::condition_variable cv_;

    // Session state. Written on the main thread under session_mutex_; the
    // writer reads it while a session is open.
    mutable std::mutex session_mutex_;
    bool open_ = false;
    bool stop_requested_ = false;
    std::string stop_reason_;
    std::string id_;
    std::filesystem::path session_dir_;
    SessionSpec spec_;
    uint64_t started_ns_ = 0, ended_ns_ = 0;
    int64_t realtime_offset_ns_ = 0;
    std::optional<Finished> finished_;
    std::string routine_sha256_;   // of routine.json; "" if the app sent no routine

    // Valve eye samples: queued by add_eye_sample, written by the writer.
    std::atomic<bool> eye_accepting_{false};
    std::atomic<uint32_t> eye_layout_{0};   // layout version of the samples seen; 0 = none yet
    std::mutex eye_mutex_;
    std::vector<eye_server::Sample> eye_pending_;
    uint64_t eye_dropped_ = 0;   // under eye_mutex_
    FILE *eye_file_ = nullptr;   // writer thread
    uint64_t eye_records_ = 0;   // writer thread

    mutable std::mutex channels_mutex_;
    std::map<std::string, std::unique_ptr<Channel>> channels_;
};

} // namespace babble
