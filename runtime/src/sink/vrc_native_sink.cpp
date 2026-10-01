#include "sink/vrc_native_sink.h"

#include "core/clock.h"
#include "core/log.h"
#include "sink/osc.h"
#include "state/expression_state.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
const babble::log::Logger LOG("vrcnative");
} // namespace

namespace babble {

namespace vrc_native {

namespace {

// Unified Expressions (VRCFaceTracking.Core/Params/Expressions/UnifiedExpressions.cs).
enum Shape : uint8_t {
    EyeSquintRight, EyeSquintLeft, EyeWideRight, EyeWideLeft, BrowPinchRight, BrowPinchLeft, BrowLowererRight,
    BrowLowererLeft, BrowInnerUpRight, BrowInnerUpLeft, BrowOuterUpRight, BrowOuterUpLeft, NasalDilationRight,
    NasalDilationLeft, NasalConstrictRight, NasalConstrictLeft, CheekSquintRight, CheekSquintLeft, CheekPuffRight,
    CheekPuffLeft, CheekSuckRight, CheekSuckLeft, JawOpen, JawRight, JawLeft, JawForward, JawBackward, JawClench,
    JawMandibleRaise, MouthClosed, LipSuckUpperRight, LipSuckUpperLeft, LipSuckLowerRight, LipSuckLowerLeft,
    LipSuckCornerRight, LipSuckCornerLeft, LipFunnelUpperRight, LipFunnelUpperLeft, LipFunnelLowerRight,
    LipFunnelLowerLeft, LipPuckerUpperRight, LipPuckerUpperLeft, LipPuckerLowerRight, LipPuckerLowerLeft,
    MouthUpperUpRight, MouthUpperUpLeft, MouthUpperDeepenRight, MouthUpperDeepenLeft, NoseSneerRight, NoseSneerLeft,
    MouthLowerDownRight, MouthLowerDownLeft, MouthUpperRight, MouthUpperLeft, MouthLowerRight, MouthLowerLeft,
    MouthCornerPullRight, MouthCornerPullLeft, MouthCornerSlantRight, MouthCornerSlantLeft, MouthFrownRight,
    MouthFrownLeft, MouthStretchRight, MouthStretchLeft, MouthDimpleRight, MouthDimpleLeft, MouthRaiserUpper,
    MouthRaiserLower, MouthPressRight, MouthPressLeft, MouthTightenerRight, MouthTightenerLeft, TongueOut, TongueUp,
    TongueDown, TongueRight, TongueLeft, TongueRoll, TongueBendDown, TongueCurlUp, TongueSquish, TongueFlat,
    TongueTwistRight, TongueTwistLeft, SoftPalateClose, ThroatSwallow, NeckFlexRight, NeckFlexLeft, kShapeCount
};
static_assert(kShapeCount == 88);
constexpr size_t kEyeBytes = 14;
static_assert(kEyeBytes + kShapeCount == kBlobSize);

const std::map<std::string_view, std::vector<uint8_t>> &shape_table() {
    static const std::map<std::string_view, std::vector<uint8_t>> t = {
        {"cheekPuffLeft", {CheekPuffLeft}},
        {"cheekPuffRight", {CheekPuffRight}},
        {"cheekSuckLeft", {CheekSuckLeft}},
        {"cheekSuckRight", {CheekSuckRight}},
        {"jawOpen", {JawOpen}},
        {"jawForward", {JawForward}},
        {"jawLeft", {JawLeft}},
        {"jawRight", {JawRight}},
        {"noseSneerLeft", {NoseSneerLeft}},
        {"noseSneerRight", {NoseSneerRight}},
        {"mouthFunnel", {LipFunnelLowerLeft, LipFunnelLowerRight, LipFunnelUpperLeft, LipFunnelUpperRight}},
        {"mouthPucker", {LipPuckerLowerLeft, LipPuckerLowerRight, LipPuckerUpperLeft, LipPuckerUpperRight}},
        {"mouthLeft", {MouthUpperLeft, MouthLowerLeft}},
        {"mouthRight", {MouthUpperRight, MouthLowerRight}},
        {"mouthRollUpper", {LipSuckUpperLeft, LipSuckUpperRight}},
        {"mouthRollLower", {LipSuckLowerLeft, LipSuckLowerRight}},
        {"mouthShrugUpper", {MouthRaiserUpper}},
        {"mouthShrugLower", {MouthRaiserLower}},
        {"mouthClose", {MouthClosed}},
        {"mouthSmileLeft", {MouthCornerPullLeft}},
        {"mouthSmileRight", {MouthCornerPullRight}},
        {"mouthFrownLeft", {MouthFrownLeft}},
        {"mouthFrownRight", {MouthFrownRight}},
        {"mouthDimpleLeft", {MouthDimpleLeft}},
        {"mouthDimpleRight", {MouthDimpleRight}},
        {"mouthUpperUpLeft", {MouthUpperUpLeft}},
        {"mouthUpperUpRight", {MouthUpperUpRight}},
        {"mouthLowerDownLeft", {MouthLowerDownLeft}},
        {"mouthLowerDownRight", {MouthLowerDownRight}},
        {"mouthPressLeft", {MouthPressLeft}},
        {"mouthPressRight", {MouthPressRight}},
        {"mouthStretchLeft", {MouthStretchLeft}},
        {"mouthStretchRight", {MouthStretchRight}},
        {"tongueOut", {TongueOut}},
        {"tongueUp", {TongueUp}},
        {"tongueDown", {TongueDown}},
        {"tongueLeft", {TongueLeft}},
        {"tongueRight", {TongueRight}},
        {"tongueRoll", {TongueRoll}},
        {"tongueBendDown", {TongueBendDown}},
        {"tongueCurlUp", {TongueCurlUp}},
        {"tongueSquish", {TongueSquish}},
        {"tongueFlat", {TongueFlat}},
        {"tongueTwistLeft", {TongueTwistLeft}},
        {"tongueTwistRight", {TongueTwistRight}},
        {"LeftEyeWiden", {EyeWideLeft}},
        {"RightEyeWiden", {EyeWideRight}},
        {"CombinedEyeWiden", {EyeWideLeft, EyeWideRight}},
        {"LeftEyeSquint", {EyeSquintLeft}},
        {"RightEyeSquint", {EyeSquintRight}},
        {"CombinedEyeSquint", {EyeSquintLeft, EyeSquintRight}},
        {"LeftEyeBrow", {BrowLowererLeft}},
        {"RightEyeBrow", {BrowLowererRight}},
        {"CombinedEyeBrow", {BrowLowererLeft, BrowLowererRight}},
    };
    return t;
}

// VRCFT: (byte)(Math.Clamp(v, 0f, 1f) * 255), a truncating cast.
uint8_t to_byte(float v) { return uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f); }

} // namespace

Target target_of(std::string_view signal) {
    Target t;
    if (signal == "LeftEyeX") t.kind = Target::LeftX;
    else if (signal == "LeftEyeY") t.kind = Target::LeftY;
    else if (signal == "RightEyeX") t.kind = Target::RightX;
    else if (signal == "RightEyeY") t.kind = Target::RightY;
    else if (signal == "LeftEyeLid") t.kind = Target::LeftOpen;
    else if (signal == "RightEyeLid") t.kind = Target::RightOpen;
    else if (auto it = shape_table().find(signal); it != shape_table().end()) {
        t.kind = Target::Shapes;
        t.shapes = it->second;
    }
    return t;
}

Blob encode(const std::vector<Target> &targets, const std::vector<std::optional<float>> &values) {
    float lx = 0, ly = 0, rx = 0, ry = 0, lopen = 1, ropen = 1;
    std::array<float, kShapeCount> shapes{};
    for (size_t i = 0; i < targets.size() && i < values.size(); i++) {
        if (!values[i])
            continue;
        const float v = *values[i];
        switch (targets[i].kind) {
        case Target::None: break;
        case Target::LeftX: lx = v; break;
        case Target::LeftY: ly = v; break;
        case Target::RightX: rx = v; break;
        case Target::RightY: ry = v; break;
        case Target::LeftOpen: lopen = v; break;
        case Target::RightOpen: ropen = v; break;
        case Target::Shapes:
            for (uint8_t s : targets[i].shapes)
                shapes[s] = v;
            break;
        }
    }
    Blob b{};
    b[0] = to_byte(rx);
    b[1] = to_byte(-rx);
    b[2] = to_byte(ry);
    b[3] = to_byte(-ry);
    b[4] = to_byte(-lx);
    b[5] = to_byte(lx);
    b[6] = to_byte(ly);
    b[7] = to_byte(-ly);
    b[8] = to_byte(1.0f - ropen);
    b[9] = to_byte(1.0f - lopen);
    for (size_t s = 0; s < kShapeCount; s++)
        b[kEyeBytes + s] = to_byte(shapes[s]);
    return b;
}

std::optional<std::string> streaming_pc(std::string_view text) {
    // Each vrlink session logs its PC's addresses once, the chosen one marked:
    //   ... [Info] - UDPServerAddresses:
    //   ... [Info] - \t192.168.0.149
    //   ... [Info] - \t10.35.78.25 *
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
    // The message part of a log line (after "] - ").
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
                break;   // past the list
            if (msg.find('*') != std::string_view::npos)
                if (auto ip = ipv4(msg))
                    return ip;
            pos = text.find('\n', pos);
        }
    }
    // No marked entry: the peer of the latest link transfer.
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

} // namespace vrc_native

namespace {

std::filesystem::path default_log_path() {
    const char *home = getenv("HOME");
    return std::filesystem::path(home && *home ? home : "/home/steamos") / ".local/share/Steam/logs/vrlink.txt";
}

} // namespace

VrcNativeSink::VrcNativeSink(const nlohmann::json &cfg, SinkDeps deps)
    : name_(deps.name), thread_spec_(thread_spec(deps.threads, cfg.value("thread", "osc"), "bbl-vrcnat")),
      state_(deps.state), stats_(deps.stats), host_(cfg.value("host", "")), target_(cfg.value("target", "auto")),
      port_(cfg.value("port", 9000)), max_rate_hz_(cfg.value("max_rate_hz", 0.0)) {
    if (target_ != "auto" && target_ != "pc" && target_ != "frame") {
        LOG.warn("%s: unknown target '%s'; using auto", name_.c_str(), target_.c_str());
        target_ = "auto";
    }
    const std::string log = cfg.value("steam_link_log", "");
    log_path_ = log.empty() ? default_log_path() : std::filesystem::path(log);
    stats_.add(name_ + ".msgs_per_s", &messages_);
    stats_.add(name_ + ".send_errors", &send_errors_);
}

VrcNativeSink::~VrcNativeSink() {
    stop();
    stats_.remove_prefix(name_ + ".");
}

bool VrcNativeSink::start() {
    if (!host_.empty()) {
        in_addr tmp{};
        if (inet_pton(AF_INET, host_.c_str(), &tmp) != 1) {
            LOG.error("%s: '%s' is not an IPv4 address", name_.c_str(), host_.c_str());
            return false;
        }
    }
    sock_ = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (sock_ < 0) {
        LOG.error("%s: socket failed: %s", name_.c_str(), strerror(errno));
        return false;
    }
    stop_ = false;
    have_dest_ = false;
    dest_ip_.clear();
    next_check_ns_ = 0;
    log_size_ = 0;
    thread_ = start_thread(thread_spec_, [this] { run(); });
    LOG.info("%s: sending VRChat native face tracking (target %s, port %d)", name_.c_str(),
             host_.empty() ? target_.c_str() : host_.c_str(), port_);
    return true;
}

void VrcNativeSink::stop() {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
    if (sock_ >= 0)
        close(sock_);
    sock_ = -1;
    std::lock_guard lock(status_mutex_);
    status_ = {};
}

VrcNativeSink::Status VrcNativeSink::status() const {
    std::lock_guard lock(status_mutex_);
    Status s = status_;
    s.sent = messages_.total();
    return s;
}

std::optional<std::string> VrcNativeSink::live_pc() {
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(log_path_, ec);
    const auto size = ec ? 0 : std::filesystem::file_size(log_path_, ec);
    if (ec)
        return std::nullopt;   // no Steam Link log: never streamed
    if (mtime != log_mtime_ || size != log_size_) {
        log_mtime_ = mtime;
        log_size_ = size;
        std::ifstream in(log_path_, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        log_pc_ = vrc_native::streaming_pc(text.str());
        log_pid_ = vrc_native::session_pid(text.str());
    }
    // Live: the session's vrlink process still runs.
    if (!log_pc_ || !log_pid_)
        return std::nullopt;
    std::ifstream cmd("/proc/" + std::to_string(*log_pid_) + "/cmdline");
    std::string line;
    std::getline(cmd, line, '\0');
    return line.find("vrlink") != std::string::npos ? log_pc_ : std::nullopt;
}

bool VrcNativeSink::refresh_destination(uint64_t now) {
    if (now < next_check_ns_)
        return have_dest_;
    next_check_ns_ = now + 3'000'000'000ull;

    std::string ip, kind;
    if (!host_.empty()) {
        ip = host_;
        kind = "host";
    } else if (target_ == "frame") {
        ip = "127.0.0.1";
        kind = "frame";
    } else if (auto pc = live_pc()) {
        ip = *pc;
        kind = "pc";
    } else if (target_ == "auto") {
        ip = "127.0.0.1";   // not streaming: VRChat runs on the Frame
        kind = "frame";
    } else {
        if (have_dest_ || dest_ip_ != "?")
            LOG.info("%s: no live Steam Link session; waiting for one", name_.c_str());
        have_dest_ = false;
        dest_ip_ = "?";
        std::lock_guard lock(status_mutex_);
        status_ = {};
        return false;
    }
    if (ip != dest_ip_ || !have_dest_) {
        dest_ = {};
        dest_.sin_family = AF_INET;
        dest_.sin_port = htons(uint16_t(port_));
        inet_pton(AF_INET, ip.c_str(), &dest_.sin_addr);
        LOG.info("%s: sending %.*s to %s:%d (%s)", name_.c_str(), int(vrc_native::kAddress.size()),
                 vrc_native::kAddress.data(), ip.c_str(), port_,
                 kind == "pc" ? "VRChat on the PC Steam Link streams from"
                              : kind == "frame" ? "VRChat on this headset" : "fixed host");
        dest_ip_ = ip;
        std::lock_guard lock(status_mutex_);
        status_.kind = kind;
        status_.address = ip;
    }
    have_dest_ = true;
    return true;
}

void VrcNativeSink::send(const vrc_native::Blob &blob) {
    uint8_t buf[160];
    const size_t n = osc::encode_blob(vrc_native::kAddress, blob.data(), blob.size(), buf, sizeof(buf));
    if (!n)
        return;
    if (sendto(sock_, buf, n, MSG_DONTWAIT | MSG_NOSIGNAL, reinterpret_cast<const sockaddr *>(&dest_),
               sizeof(dest_)) == ssize_t(n)) {
        messages_.tick();
        return;
    }
    send_errors_.add();
    const uint64_t now = mono_ns();
    if (now - last_error_log_ns_ > 5'000'000'000ull) {
        LOG.warn("%s: send to %s:%d failed: %s", name_.c_str(), dest_ip_.c_str(), port_, strerror(errno));
        last_error_log_ns_ = now;
    }
}

void VrcNativeSink::run() {
    const uint64_t min_period_ns = max_rate_hz_ > 0 ? uint64_t(1e9 / max_rate_hz_) : 0;
    uint64_t seq = 0, last_send_ns = 0;
    std::vector<vrc_native::Target> targets;
    std::vector<std::optional<float>> values;

    while (!stop_) {
        seq = state_.wait_update(seq, std::chrono::milliseconds(200));
        if (stop_)
            break;
        const uint64_t now = mono_ns();
        if (!refresh_destination(now))
            continue;
        if (min_period_ns && now - last_send_ns < min_period_ns)
            continue;
        last_send_ns = now;

        const auto snap = state_.snapshot();
        if (targets.size() != snap.names.size()) {
            targets.clear();
            for (const auto &nm : snap.names)
                targets.push_back(vrc_native::target_of(nm));
        }
        values.assign(snap.signals.size(), std::nullopt);
        for (size_t i = 0; i < snap.signals.size(); i++)
            if (snap.signals[i].t_ns != 0)
                values[i] = snap.signals[i].value;
        send(vrc_native::encode(targets, values));
    }
}

} // namespace babble
