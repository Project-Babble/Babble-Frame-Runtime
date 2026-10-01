#pragma once

#include "core/metrics.h"
#include "core/threading.h"
#include "sink/sink.h"

#include <netinet/in.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace babble {

class ExpressionState;

// A UDP socket shared between threads; closed when the last owner lets go.
struct SharedSocket {
    explicit SharedSocket(int f) : fd(f) {}
    ~SharedSocket();
    SharedSocket(const SharedSocket &) = delete;
    SharedSocket &operator=(const SharedSocket &) = delete;
    int fd = -1;
};

// A destination that changes while an OscSink runs (set by the PC link,
// sink/link_sink.h). Unset means: send nothing. Every set() starts a new
// generation, after which the sink resends every signal's current value, so a
// new receiver (or one reached over a new path) gets the full state. With a
// socket, the sink sends from it (the link's path socket, so the data shares
// the firewall-approved pair its pings use) instead of its own.
class OscDestination {
public:
    void set(const sockaddr_in &to, std::shared_ptr<SharedSocket> via = nullptr);
    void clear();
    // False while unset; otherwise the address, its generation and the socket to send from (may be null).
    bool get(sockaddr_in *to, uint64_t *generation, std::shared_ptr<SharedSocket> *via = nullptr) const;

private:
    mutable std::mutex mutex_;
    bool valid_ = false;
    sockaddr_in to_{};
    std::shared_ptr<SharedSocket> via_;
    uint64_t generation_ = 0;
};

// Config type "osc". Sends tracked signals as OSC over UDP, one message per
// datagram with a single float, which is what the Baballonia VRCFT module
// (VRCFaceTracking.Baballonia/BabbleOSC.cs) expects: address
// "<prefix>/<signal name>", default target 127.0.0.1:8888.
//
// The module waits on ExpressionState updates and sends only the signals
// that changed since its last send, so latency is one wakeup. Sends are
// non-blocking and unconnected: a missing receiver costs nothing and never
// stalls anything.
//
// Config:
//   host, port        target (default 127.0.0.1:8888)
//   prefix            prepended to every address (default "")
//   rename            {"signalName": "oscName"} for receivers with other names
//   exclude           ["signalName", ...] never sent (nor are internal
//                     signals, whose names start with '_')
//   max_rate_hz       cap on send batches per second (0 = every update)
//   bundle            pack each batch into OSC bundles of up to ~1400 bytes
//                     instead of a datagram per signal (the PC link does)
//   test_pattern      true: send slowly moving values for every signal
//                     instead of tracking data (to test the receiving end)
class OscSink : public ISink {
public:
    // With `destination`, host/port are ignored and the sink sends to wherever
    // the destination currently points (nothing while it's unset).
    OscSink(const nlohmann::json &cfg, SinkDeps deps, std::shared_ptr<OscDestination> destination = nullptr);
    ~OscSink() override;

    bool start() override;
    void stop() override;

    uint64_t sent() const { return messages_.total(); }   // values sent so far

private:
    void run();
    void send(const std::string &address, float value);
    void send_datagram(const uint8_t *data, size_t n, size_t values);
    void flush();   // the pending bundle

    std::string name_;
    ThreadSpec thread_spec_;
    const ExpressionState &state_;
    StatsRegistry &stats_;

    std::string host_, prefix_;
    int port_ = 8888;
    std::map<std::string, std::string> rename_;
    std::vector<std::string> exclude_;
    double max_rate_hz_ = 0;
    bool test_pattern_ = false;
    bool bundle_ = false;
    std::vector<uint8_t> pending_;   // the bundle being filled
    size_t pending_values_ = 0;

    int sock_ = -1;
    sockaddr_in dest_{};
    std::shared_ptr<OscDestination> destination_;   // dynamic target, or null for host/port
    std::shared_ptr<SharedSocket> via_;             // the destination's socket for this batch, if any
    std::thread thread_;
    std::atomic<bool> stop_{false};

    RateStat messages_;
    Counter send_errors_;
    uint64_t last_error_log_ns_ = 0;
};

} // namespace babble
