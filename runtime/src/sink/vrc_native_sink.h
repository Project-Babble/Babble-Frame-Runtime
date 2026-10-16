#pragma once

#include "core/metrics.h"
#include "core/threading.h"
#include "sink/sink.h"

#include <netinet/in.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace babble {

class ExpressionState;

namespace vrc_native {

// The native face tracking input of VRChat is one 102-byte OSC blob, with each value scaled from 0..1 to 0..255.
constexpr size_t kBlobSize = 102;
constexpr std::string_view kAddress = "/tracking/face/v1";
using Blob = std::array<uint8_t, kBlobSize>;

// The position of a signal in the blob, with the same signal names as the osc sink.
struct Target {
    enum Kind : uint8_t { None, Shapes, LeftX, LeftY, RightX, RightY, LeftOpen, RightOpen };
    Kind kind = None;
    std::vector<uint8_t> shapes;   // The Unified Expressions indices for the Shapes kind.
};
Target target_of(std::string_view signal);

// Makes the blob from the values, where a nullopt value gives open eyes or 0 for the other shapes.
Blob encode(const std::vector<Target> &targets, const std::vector<std::optional<float>> &values);

// Finds the address of the PC that the headset streams from in the Steam Link log of SteamVR.
std::optional<std::string> streaming_pc(std::string_view log_text);
// Finds the PID of the vrlink process of the latest session in the log.
std::optional<int> session_pid(std::string_view log_text);

}

// Config type "vrc_native" sends the native face tracking blob of VRChat directly to VRChat without VRCFaceTracking.
class VrcNativeSink : public ISink {
public:
    VrcNativeSink(const nlohmann::json &cfg, SinkDeps deps);
    ~VrcNativeSink() override;

    bool start() override;
    void stop() override;

    // The destination of the blob, where an empty kind means that there is no destination yet.
    struct Status {
        std::string kind, address;   // The kind is "pc", "frame" or "host".
        uint64_t sent = 0;            // The number of blobs that the sink sent.
    };
    Status status() const;

private:
    void run();
    // Finds VRChat again every few seconds and returns true if dest_ is usable.
    bool refresh_destination(uint64_t now);
    // Returns the Steam Link PC while its session is live, and reads the log again when it changes.
    std::optional<std::string> live_pc();
    void send(const vrc_native::Blob &blob);

    std::string name_;
    ThreadSpec thread_spec_;
    const ExpressionState &state_;
    StatsRegistry &stats_;

    std::string host_, target_;
    int port_ = 9000;
    std::filesystem::path log_path_;
    double max_rate_hz_ = 0;

    int sock_ = -1;
    sockaddr_in dest_{};
    bool have_dest_ = false;
    std::string dest_ip_;
    uint64_t next_check_ns_ = 0;
    std::filesystem::file_time_type log_mtime_{};
    uintmax_t log_size_ = 0;
    std::optional<std::string> log_pc_;   // The PC from the last read of the log.
    std::optional<int> log_pid_;
    mutable std::mutex status_mutex_;
    Status status_;

    std::thread thread_;
    std::atomic<bool> stop_{false};

    RateStat messages_;
    Counter send_errors_;
    uint64_t last_error_log_ns_ = 0;
};

}
