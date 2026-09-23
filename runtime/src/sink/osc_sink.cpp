#include "sink/osc_sink.h"

#include "core/clock.h"
#include "core/log.h"
#include "sink/osc.h"
#include "state/expression_state.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

namespace babble {

OscSink::OscSink(const nlohmann::json &cfg, SinkDeps deps)
    : name_(deps.name), thread_spec_(thread_spec(deps.threads, cfg.value("thread", "osc"), "bbl-osc")),
      state_(deps.state), stats_(deps.stats), host_(cfg.value("host", "127.0.0.1")),
      prefix_(cfg.value("prefix", "")), port_(cfg.value("port", 8888)), max_rate_hz_(cfg.value("max_rate_hz", 0.0)),
      test_pattern_(cfg.value("test_pattern", false)) {
    if (cfg.contains("rename") && cfg["rename"].is_object())
        for (auto &[k, v] : cfg["rename"].items())
            if (v.is_string())
                rename_[k] = v.get<std::string>();
    if (cfg.contains("exclude") && cfg["exclude"].is_array())
        for (const auto &e : cfg["exclude"])
            if (e.is_string())
                exclude_.push_back(e.get<std::string>());
    stats_.add(name_ + ".msgs_per_s", &messages_);
    stats_.add(name_ + ".send_errors", &send_errors_);
}

OscSink::~OscSink() {
    stop();
    stats_.remove_prefix(name_ + ".");
}

bool OscSink::start() {
    dest_ = {};
    dest_.sin_family = AF_INET;
    dest_.sin_port = htons(uint16_t(port_));
    if (inet_pton(AF_INET, host_.c_str(), &dest_.sin_addr) != 1) {
        LOG_ERROR("%s: '%s' is not an IPv4 address", name_.c_str(), host_.c_str());
        return false;
    }
    sock_ = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (sock_ < 0) {
        LOG_ERROR("%s: socket failed: %s", name_.c_str(), strerror(errno));
        return false;
    }
    stop_ = false;
    thread_ = start_thread(thread_spec_, [this] { run(); });
    LOG_INFO("%s: sending OSC to %s:%d%s%s", name_.c_str(), host_.c_str(), port_,
             prefix_.empty() ? "" : (" prefix " + prefix_).c_str(), test_pattern_ ? " (TEST PATTERN)" : "");
    return true;
}

void OscSink::stop() {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
    if (sock_ >= 0)
        close(sock_);
    sock_ = -1;
}

void OscSink::send(const std::string &address, float value) {
    uint8_t buf[256];
    size_t n = osc::encode_float(address, value, buf, sizeof(buf));
    if (!n)
        return;
    if (sendto(sock_, buf, n, MSG_DONTWAIT | MSG_NOSIGNAL, reinterpret_cast<const sockaddr *>(&dest_),
               sizeof(dest_)) == ssize_t(n)) {
        messages_.tick();
        return;
    }
    // Nobody listening, or the socket buffer is full: drop it; the next update supersedes it.
    send_errors_.add();
    uint64_t now = mono_ns();
    if (now - last_error_log_ns_ > 5000000000ull) {
        LOG_WARN("%s: send to %s:%d failed: %s", name_.c_str(), host_.c_str(), port_, strerror(errno));
        last_error_log_ns_ = now;
    }
}

void OscSink::run() {
    const uint64_t min_period_ns = max_rate_hz_ > 0 ? uint64_t(1e9 / max_rate_hz_) : 0;
    uint64_t seq = 0, last_batch_ns = 0;
    std::vector<uint64_t> last_sent_t;   // per signal: t_ns of the value last sent
    std::vector<std::string> addresses;  // per signal: final OSC address, "" = excluded

    while (!stop_) {
        if (test_pattern_) {
            // ~120 Hz of smooth, distinct motion per signal, independent of tracking.
            sleep_ns(8333333);
        } else {
            seq = state_.wait_update(seq, std::chrono::milliseconds(200));
        }
        if (stop_)
            break;
        uint64_t now = mono_ns();
        if (min_period_ns && now - last_batch_ns < min_period_ns)
            continue;
        last_batch_ns = now;

        auto snap = state_.snapshot();
        if (addresses.size() != snap.names.size()) {
            // Signals are registered before streaming starts; rebuild the address table if that changes.
            addresses.clear();
            for (const auto &nm : snap.names) {
                if (std::find(exclude_.begin(), exclude_.end(), nm) != exclude_.end()) {
                    addresses.emplace_back();
                    continue;
                }
                auto r = rename_.find(nm);
                addresses.push_back(prefix_ + "/" + (r != rename_.end() ? r->second : nm));
            }
            last_sent_t.assign(snap.names.size(), 0);
        }

        for (size_t i = 0; i < snap.signals.size(); i++) {
            if (addresses[i].empty())
                continue;
            if (test_pattern_) {
                double t = double(now) / 1e9;
                float v = float(0.5 + 0.5 * std::sin(t * 1.5 + double(i) * 0.7));
                // Gaze-style signals (…EyeX / …EyeY) span -1..1; everything else 0..1.
                const std::string &nm = snap.names[i];
                if (nm.size() > 4 && (nm.ends_with("EyeX") || nm.ends_with("EyeY")))
                    v = v * 2.0f - 1.0f;
                send(addresses[i], v);
                continue;
            }
            const Signal &s = snap.signals[i];
            if (s.t_ns == 0 || s.t_ns == last_sent_t[i])
                continue;   // never set, or unchanged since the last send
            last_sent_t[i] = s.t_ns;
            send(addresses[i], s.value);
        }
    }
}

} // namespace babble
