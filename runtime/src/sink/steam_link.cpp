#include "sink/steam_link.h"

#include "core/clock.h"

#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

#include <arpa/inet.h>

namespace babble {

bool valid_pc_address(const std::string &text) {
    in_addr tmp{};
    return text.empty() || inet_pton(AF_INET, text.c_str(), &tmp) == 1;
}

namespace steam_link {

std::optional<std::string> streaming_pc(std::string_view text) {
    // Each vrlink session logs the PC addresses once, and a '*' marks the address that it uses.
    auto ipv4 = [](std::string_view s) -> std::optional<std::string> {
        const size_t a = s.find_first_of("0123456789");
        if (a == std::string_view::npos)
            return std::nullopt;
        const size_t b = s.find_first_not_of("0123456789.", a);
        std::string ip(s.substr(a, b == std::string_view::npos ? std::string_view::npos : b - a));
        in_addr tmp{};
        return inet_pton(AF_INET, ip.c_str(), &tmp) == 1 ? std::optional(ip) : std::nullopt;
    };
    auto line_at = [&](size_t pos) {
        const size_t end = text.find('\n', pos);
        return text.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
    };
    // Returns the message part of a log line, after "] - ".
    auto message = [](std::string_view line) {
        const size_t m = line.find("] - ");
        return m == std::string_view::npos ? line : line.substr(m + 4);
    };

    const size_t list = text.rfind("UDPServerAddresses:");
    if (list != std::string_view::npos) {
        size_t pos = text.find('\n', list);
        while (pos != std::string_view::npos && pos + 1 < text.size()) {
            pos++;
            const std::string_view msg = message(line_at(pos));
            if (msg.empty() || msg.front() != '\t')
                break;   // This line is after the end of the list.
            if (msg.find('*') != std::string_view::npos)
                if (auto ip = ipv4(msg))
                    return ip;
            pos = text.find('\n', pos);
        }
    }
    // If no entry has a mark, use the peer of the latest SetPeer line.
    const size_t peer = text.rfind("SetPeer ");
    if (peer != std::string_view::npos) {
        const std::string_view line = line_at(peer);
        const size_t arrow = line.find("-> ");
        if (arrow != std::string_view::npos)
            return ipv4(line.substr(arrow + 3));
    }
    return std::nullopt;
}

std::optional<int> session_pid(std::string_view text) {
    const std::string_view key = "startup with PID=";
    const size_t at = text.rfind(key);
    if (at == std::string_view::npos)
        return std::nullopt;
    const int pid = atoi(std::string(text.substr(at + key.size(), 12)).c_str());
    return pid > 0 ? std::optional(pid) : std::nullopt;
}

namespace {

std::filesystem::path default_log_path() {
    const char *home = getenv("HOME");
    return std::filesystem::path(home && *home ? home : "/home/steamos") / ".local/share/Steam/logs/vrlink.txt";
}
//======= Add support for WiVRNn here ========
}

SteamLinkPc::SteamLinkPc(std::filesystem::path log, std::chrono::nanoseconds recheck)
    : log_path_(log.empty() ? default_log_path() : std::move(log)), recheck_ns_(uint64_t(recheck.count())) {}

std::optional<std::string> SteamLinkPc::live() {
    std::lock_guard lock(mutex_);
    const uint64_t now = mono_ns();
    if (checked_ && now - checked_ns_ < recheck_ns_)
        return answer_;   // Another output checked a moment ago.
    checked_ = true;
    checked_ns_ = now;
    answer_ = std::nullopt;

    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(log_path_, ec);
    const auto size = ec ? 0 : std::filesystem::file_size(log_path_, ec);
    if (ec)
        return answer_;   // If there is no Steam Link log, the headset never streamed.
    if (mtime != log_mtime_ || size != log_size_) {
        log_mtime_ = mtime;
        log_size_ = size;
        std::ifstream in(log_path_, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        const std::string contents = text.str();
        log_pc_ = streaming_pc(contents);
        log_pid_ = session_pid(contents);
    }
    // The session is live if its vrlink process still runs.
    if (!log_pc_ || !log_pid_)
        return answer_;
    std::ifstream cmd("/proc/" + std::to_string(*log_pid_) + "/cmdline");
    std::string line;
    std::getline(cmd, line, '\0');
    if (line.find("vrlink") != std::string::npos)
        answer_ = log_pc_;
    return answer_;
}

std::shared_ptr<SteamLinkPc> shared(const std::filesystem::path &log) {
    static std::mutex mutex;
    static std::map<std::filesystem::path, std::weak_ptr<SteamLinkPc>> readers;
    const std::filesystem::path key = log.empty() ? default_log_path() : log;
    std::lock_guard lock(mutex);
    for (auto it = readers.begin(); it != readers.end();)
        it = it->second.expired() ? readers.erase(it) : std::next(it);
    if (auto reader = readers[key].lock())
        return reader;
    auto reader = std::make_shared<SteamLinkPc>(key);
    readers[key] = reader;
    return reader;
}

}
}
