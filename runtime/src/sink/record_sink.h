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

// Config type "record" records the camera sources of the runtime for the dataset capture app.
class RecordSink : public ISink, public IFrameTap {
public:
    struct SessionSpec {
        std::string routine;
        nlohmann::json meta = nlohmann::json::object();
        nlohmann::json consent;                              // A null value means that the user gave no consent.
        std::map<std::string, double> source_fps;            // The nominal rate of each source, for the encoder.
        nlohmann::json context = nlohmann::json::object();   // The sink records this context without changes.
        nlohmann::json routine_def;                          // The routine definition of the app, or null if none.
        bool valve_eye = false;                              // If true, record the samples of the Valve eye service.
    };
    struct StartResult {
        bool ok = false;
        std::string error;     // The error is busy, unavailable, disk or io.
        std::string message;
        std::string id, dir;
    };
    struct SourceCounts {
        uint64_t frames = 0, dropped = 0, size_changed = 0;
    };
    struct Status {
        bool recording = false;   // A session is open, which includes when it finishes.
        bool finishing = false;
        std::string id;
        uint64_t started_ns = 0;
        std::map<std::string, SourceCounts> sources;
    };
    struct Finished {
        std::string id, dir;
        std::string status;   // The status is complete or failed.
        std::string error;         // The cause of the failure, or "" if it did not fail.
        std::string stop_reason;   // The cause of the stop, for example the app or low disk.
        std::map<std::string, SourceCounts> sources;
    };

    RecordSink(const nlohmann::json &cfg, SinkDeps deps);
    ~RecordSink() override;

    bool start() override;
    void stop() override;

    // The capture threads call these IFrameTap functions.
    bool tap_wanted() const override { return accepting_.load(std::memory_order_relaxed); }
    void tap_frames(const std::string &source, const StereoFrame &frames) override;

    // Call these functions only on the main thread.
    bool available() const;   // Returns true if the sink can find the encoder.
    StartResult start_session(const SessionSpec &spec);
    // The `extra` object holds more fields for the line of the label.
    bool add_label(const std::string &label, uint64_t start_ns, uint64_t end_ns, std::string *err,
                   const nlohmann::json &extra = nlohmann::json::object());
    bool stop_session(const std::string &reason = "");   // Returns false if no session is open.
    std::optional<Finished> take_finished();
    Status status() const;
    const std::filesystem::path &dir() const { return dir_; }

    // Any thread can call this to queue a Valve sample for valve_eye.bin.
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
    nlohmann::json device_;   // The headset model and OS build, which the sink reads once.

    std::thread thread_;
    std::atomic<bool> stop_thread_{false};
    std::atomic<bool> accepting_{false};   // If true, the taps copy frames.
    std::mutex cv_mutex_;
    std::condition_variable cv_;

    // The main thread writes the session state under session_mutex_, and the writer reads it while a session is open.
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
    std::string routine_sha256_;   // The SHA-256 of routine.json, or "" if the app sent no routine.

    // add_eye_sample() queues the Valve eye samples and the writer thread writes them.
    std::atomic<bool> eye_accepting_{false};
    std::atomic<uint32_t> eye_layout_{0};   // The layout version of the samples, or 0 if none arrived.
    std::mutex eye_mutex_;
    std::vector<eye_server::Sample> eye_pending_;
    uint64_t eye_dropped_ = 0;   // eye_mutex_ protects this value.
    FILE *eye_file_ = nullptr;   // Only the writer thread uses this.
    uint64_t eye_records_ = 0;   // Only the writer thread uses this.

    mutable std::mutex channels_mutex_;
    std::map<std::string, std::unique_ptr<Channel>> channels_;
};

}
