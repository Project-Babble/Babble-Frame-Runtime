#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace babble {

// Returns true if text is empty or an IPv4 address, which are the values that the shared pc_address can have.
bool valid_pc_address(const std::string &text);

namespace steam_link {

// Finds the address of the PC that the headset streams from in the Steam Link log of SteamVR.
std::optional<std::string> streaming_pc(std::string_view log_text);
// Finds the PID of the vrlink process of the latest session in the log.
std::optional<int> session_pid(std::string_view log_text);

// Follows the Steam Link log of SteamVR to find the PC that the headset streams from, and reads it only when it changes.
// More than one thread can call live().
class SteamLinkPc {
public:
    // An empty path uses the log in the Steam folder. live() checks again only after recheck has passed.
    explicit SteamLinkPc(std::filesystem::path log = {}, std::chrono::nanoseconds recheck = std::chrono::seconds(1));
    // Returns the PC while its Steam Link session is live.
    std::optional<std::string> live();

private:
    std::mutex mutex_;
    std::filesystem::path log_path_;
    uint64_t recheck_ns_;
    uint64_t checked_ns_ = 0;
    bool checked_ = false;
    std::optional<std::string> answer_;   // The result of the last check.
    std::filesystem::file_time_type log_mtime_{};
    uintmax_t log_size_ = 0;
    std::optional<std::string> log_pc_;   // The PC from the last read of the log.
    std::optional<int> log_pid_;
};

// Returns the reader for this log, which every output that asks for the same log shares.
// An empty path is the log in the Steam folder.
std::shared_ptr<SteamLinkPc> shared(const std::filesystem::path &log = {});

}
}
