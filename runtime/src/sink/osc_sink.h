#pragma once

#include "core/metrics.h"
#include "core/threading.h"
#include "sink/sink.h"

#include <netinet/in.h>

#include <atomic>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace babble {

class ExpressionState;

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
//   exclude           ["signalName", ...] never sent
//   max_rate_hz       cap on send batches per second (0 = every update)
//   test_pattern      true: send slowly moving values for every signal
//                     instead of tracking data (to test the receiving end)
class OscSink : public ISink {
public:
    OscSink(const nlohmann::json &cfg, SinkDeps deps);
    ~OscSink() override;

    bool start() override;
    void stop() override;

private:
    void run();
    void send(const std::string &address, float value);

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

    int sock_ = -1;
    sockaddr_in dest_{};
    std::thread thread_;
    std::atomic<bool> stop_{false};

    RateStat messages_;
    Counter send_errors_;
    uint64_t last_error_log_ns_ = 0;
};

} // namespace babble
