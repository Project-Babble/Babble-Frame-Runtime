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

// VRChat's native ("full") face tracking input, as VRCFaceTracking 6 sends
// it (VRCFaceTracking.Core/Services/BinaryFaceDataSender.cs and
// Params/Data/UnifiedData.cs, CopyTo): one OSC blob at /tracking/face/v1 of
// 102 bytes, each a value clamped to 0..1 and scaled to 0..255:
//   0..3    right gaze +x, -x, +y, -y
//   4..7    left gaze  -x, +x, +y, -y
//   8, 9    right, left eye closed (1 - openness)
//   10..13  unused (0)
//   14..101 the 88 Unified Expressions shapes, in enum order
constexpr size_t kBlobSize = 102;
constexpr std::string_view kAddress = "/tracking/face/v1";
using Blob = std::array<uint8_t, kBlobSize>;

// Where each signal goes in the blob. Signal names are the osc sink's (the
// Baballonia module's spellings); the shapes they drive follow that module's
// BabbleOSC.cs.
struct Target {
    enum Kind : uint8_t { None, Shapes, LeftX, LeftY, RightX, RightY, LeftOpen, RightOpen };
    Kind kind = None;
    std::vector<uint8_t> shapes;   // Unified Expressions indices, for Shapes
};
Target target_of(std::string_view signal);

// Builds the blob from values per signal (targets[i] for values[i]; a value
// of nullopt is unset: eyes open, everything else 0).
Blob encode(const std::vector<Target> &targets, const std::vector<std::optional<float>> &values);

// The address of the PC that the headset streams from, per SteamVR's Steam
// Link log (vrlink.txt): the entry marked '*' in the latest session's
// "UDPServerAddresses:" list, else the latest "SetPeer <us> -> <pc>".
std::optional<std::string> streaming_pc(std::string_view log_text);
// The latest session's vrlink process ("vrlink ... startup with PID=N").
std::optional<int> session_pid(std::string_view log_text);

} // namespace vrc_native

// Config type "vrc_native": sends VRChat's native face tracking blob
// (vrc_native::encode) straight to VRChat, skipping VRCFaceTracking. VRChat
// runs either on the PC the headset streams from (Steam Link) or on the
// Frame itself; with target "auto" the blob goes to the PC while a Steam Link
// session is live, else to this headset.
//
// Config:
//   output         the Output setting that runs this module ("vrc_native")
//   target         "auto", "pc" (the Steam Link PC), "frame" (127.0.0.1)
//   host           a fixed address instead (overrides target)
//   port           VRChat's OSC input (default 9000)
//   steam_link_log default ~/.local/share/Steam/logs/vrlink.txt
//   max_rate_hz    cap on sends per second (0 = every update)
class VrcNativeSink : public ISink {
public:
    VrcNativeSink(const nlohmann::json &cfg, SinkDeps deps);
    ~VrcNativeSink() override;

    bool start() override;
    void stop() override;

    // For the debug overlay: where the blob goes ("" kind = nowhere yet).
    struct Status {
        std::string kind, address;   // kind: "pc", "frame" or "host"
        uint64_t sent = 0;            // blobs sent so far
    };
    Status status() const;

private:
    void run();
    // Re-checks where VRChat is (every few seconds); true if dest_ is usable.
    bool refresh_destination(uint64_t now);
    // The Steam Link PC while its session is live, re-reading the log when it changed.
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
    std::optional<std::string> log_pc_;   // from the last read of the log
    std::optional<int> log_pid_;
    mutable std::mutex status_mutex_;
    Status status_;

    std::thread thread_;
    std::atomic<bool> stop_{false};

    RateStat messages_;
    Counter send_errors_;
    uint64_t last_error_log_ns_ = 0;
};

} // namespace babble
