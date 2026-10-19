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

namespace {
const babble::log::Logger LOG("osc");
}

namespace babble {

namespace {
constexpr size_t kMaxDatagram = 1400;   // This is less than a 1500-byte MTU, so the network does not fragment it.
}

SharedSocket::~SharedSocket() {
    if (fd >= 0)
        close(fd);
}

void OscDestination::set(const sockaddr_in &to, std::shared_ptr<SharedSocket> via) {
    std::lock_guard lock(mutex_);
    valid_ = true;
    to_ = to;
    via_ = std::move(via);
    generation_++;
}

void OscDestination::clear() {
    std::lock_guard lock(mutex_);
    valid_ = false;
    via_.reset();
}

bool OscDestination::get(sockaddr_in *to, uint64_t *generation, std::shared_ptr<SharedSocket> *via) const {
    std::lock_guard lock(mutex_);
    if (!valid_)
        return false;
    *to = to_;
    *generation = generation_;
    if (via)
        *via = via_;
    return true;
}

OscSink::OscSink(const nlohmann::json &cfg, SinkDeps deps, std::shared_ptr<OscDestination> destination)
    : name_(deps.name), thread_spec_(thread_spec(deps.threads, cfg.value("thread", "osc"), "bbl-osc")),
      state_(deps.state), stats_(deps.stats), host_(cfg.value("host", "127.0.0.1")),
      prefix_(cfg.value("prefix", "")), port_(cfg.value("port", 8888)), max_rate_hz_(cfg.value("max_rate_hz", 0.0)),
      test_pattern_(cfg.value("test_pattern", false)), bundle_(cfg.value("bundle", false)),
      destination_(std::move(destination)) {
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
    if (!destination_ && inet_pton(AF_INET, host_.c_str(), &dest_.sin_addr) != 1) {
        LOG.error("%s: '%s' is not an IPv4 address", name_.c_str(), host_.c_str());
        return false;
    }
    sock_ = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (sock_ < 0) {
        LOG.error("%s: socket failed: %s", name_.c_str(), strerror(errno));
        return false;
    }
    stop_ = false;
    thread_ = start_thread(thread_spec_, [this] { run(); });
    if (!destination_)
        LOG.info("%s: sending OSC to %s:%d%s%s", name_.c_str(), host_.c_str(), port_,
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
    if (!bundle_) {
        send_datagram(buf, n, 1);
        return;
    }
    if (!pending_.empty() && pending_.size() + 4 + n > kMaxDatagram)
        flush();
    if (pending_.empty()) {
        pending_.resize(osc::kBundleHeaderSize);
        osc::write_bundle_header(pending_.data());
    }
    const size_t at = pending_.size();
    pending_.resize(at + 4 + n);
    osc::write_element_size(pending_.data() + at, n);
    memcpy(pending_.data() + at + 4, buf, n);
    pending_values_++;
}

void OscSink::flush() {
    if (pending_values_)
        send_datagram(pending_.data(), pending_.size(), pending_values_);
    pending_.clear();
    pending_values_ = 0;
}

void OscSink::send_datagram(const uint8_t *buf, size_t n, size_t values) {
    const int fd = via_ ? via_->fd : sock_;
    if (sendto(fd, buf, n, MSG_DONTWAIT | MSG_NOSIGNAL, reinterpret_cast<const sockaddr *>(&dest_), sizeof(dest_)) ==
        ssize_t(n)) {
        messages_.tick(values);
        return;
    }
    // Drop the datagram if it fails, because the next update replaces it.
    send_errors_.add();
    uint64_t now = mono_ns();
    if (now - last_error_log_ns_ > 5000000000ull) {
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &dest_.sin_addr, ip, sizeof(ip));
        LOG.warn("%s: send to %s:%d failed: %s", name_.c_str(), ip, ntohs(dest_.sin_port), strerror(errno));
        last_error_log_ns_ = now;
    }
}

void OscSink::run() {
    const uint64_t min_period_ns = max_rate_hz_ > 0 ? uint64_t(1e9 / max_rate_hz_) : 0;
    uint64_t seq = 0, last_batch_ns = 0, generation = 0;
    std::vector<uint64_t> last_sent_t;   // The t_ns of the last value that the sink sent for each signal.
    std::vector<std::string> addresses;  // The OSC address for each signal, or "" if the signal is excluded.
    ExpressionState::Snapshot snap;

    while (!stop_) {
        if (test_pattern_) {
            // Send the test pattern at about 120 Hz.
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

        bool resend_all = false;
        if (destination_) {
            uint64_t g = 0;
            if (!destination_->get(&dest_, &g, &via_))
                continue;   // Send nothing if there is no receiver.
            resend_all = g != generation;
            generation = g;
        }

        state_.snapshot(snap);
        if (addresses.size() != snap.names.size()) {
            // Make the address table again if the number of signals changes.
            addresses.clear();
            for (const auto &nm : snap.names) {
                // Never send a signal whose name starts with '_', because it is an internal signal.
                if (nm.starts_with('_') || std::find(exclude_.begin(), exclude_.end(), nm) != exclude_.end()) {
                    addresses.emplace_back();
                    continue;
                }
                auto r = rename_.find(nm);
                addresses.push_back(prefix_ + "/" + (r != rename_.end() ? r->second : nm));
            }
            last_sent_t.assign(snap.names.size(), 0);
        }
        if (resend_all)   // Send each current value once to a new receiver or path.
            std::fill(last_sent_t.begin(), last_sent_t.end(), 0);

        for (size_t i = 0; i < snap.signals.size(); i++) {
            if (addresses[i].empty())
                continue;
            if (test_pattern_) {
                double t = double(now) / 1e9;
                float v = float(0.5 + 0.5 * std::sin(t * 1.5 + double(i) * 0.7));
                // The EyeX and EyeY signals use the range -1 to 1, and the other signals use 0 to 1.
                const std::string &nm = snap.names[i];
                if (nm.size() > 4 && (nm.ends_with("EyeX") || nm.ends_with("EyeY")))
                    v = v * 2.0f - 1.0f;
                send(addresses[i], v);
                continue;
            }
            const Signal &s = snap.signals[i];
            if (s.t_ns == 0 || s.t_ns == last_sent_t[i])
                continue;   // The signal has no value or did not change after the last send.
            last_sent_t[i] = s.t_ns;
            send(addresses[i], s.value);
        }
        flush();
        via_.reset();   // Do not keep the socket of a path open between batches.
    }
}

}
