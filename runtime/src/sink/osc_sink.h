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

// A UDP socket that threads share, which closes when the last owner releases it.
struct SharedSocket {
    explicit SharedSocket(int f) : fd(f) {}
    ~SharedSocket();
    SharedSocket(const SharedSocket &) = delete;
    SharedSocket &operator=(const SharedSocket &) = delete;
    int fd = -1;
};

// A destination that can change while an OscSink runs, and each set() makes the sink send all values again.
class OscDestination {
public:
    void set(const sockaddr_in &to, std::shared_ptr<SharedSocket> via = nullptr);
    void clear();
    // Returns false if no destination is set, and the socket in `via` can be null.
    bool get(sockaddr_in *to, uint64_t *generation, std::shared_ptr<SharedSocket> *via = nullptr) const;

private:
    mutable std::mutex mutex_;
    bool valid_ = false;
    sockaddr_in to_{};
    std::shared_ptr<SharedSocket> via_;
    uint64_t generation_ = 0;
};

// Config type "osc" sends each changed signal as an OSC float over UDP, and a missing receiver costs nothing.
class OscSink : public ISink {
public:
    // If `destination` is not null, the sink ignores host and port and sends to the destination.
    OscSink(const nlohmann::json &cfg, SinkDeps deps, std::shared_ptr<OscDestination> destination = nullptr);
    ~OscSink() override;

    bool start() override;
    void stop() override;

    uint64_t sent() const { return messages_.total(); }   // The number of values that the sink sent.

private:
    void run();
    void send(const std::string &address, float value);
    void send_datagram(const uint8_t *data, size_t n, size_t values);
    void flush();   // Sends the pending bundle.

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
    std::vector<uint8_t> pending_;   // The bundle that the sink fills before it sends it.
    size_t pending_values_ = 0;

    int sock_ = -1;
    sockaddr_in dest_{};
    std::shared_ptr<OscDestination> destination_;   // The changeable target, or null to use host and port.
    std::shared_ptr<SharedSocket> via_;             // The socket of the destination for this batch, if it has one.
    std::thread thread_;
    std::atomic<bool> stop_{false};

    RateStat messages_;
    Counter send_errors_;
    uint64_t last_error_log_ns_ = 0;
};

}
