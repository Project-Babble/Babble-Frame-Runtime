#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace babble::capture {

constexpr int kProtocol = 2;

// The reasons for a stop that session.json and the stopped event report.
inline constexpr const char *kStoppedByApp = "stopped by the app";
inline constexpr const char *kAppDisconnected = "the app disconnected";

// The result of a request, which is ok with reply data or an error code with a message.
struct Result {
    bool ok = true;
    std::string code, message;
    nlohmann::json data = nlohmann::json::object();

    static Result fail(std::string code, std::string message) { return {false, std::move(code), std::move(message), {}}; }
};

// One labelled time span from the app, with an optional routine step index and phase.
struct Segment {
    std::string label;
    uint64_t start_ns = 0, end_ns = 0;
    std::optional<int64_t> step;
    std::string phase;   // The value "" means that the app did not give a phase.
};

// The runtime side of the requests from the capture app, which main implements.
class Backend {
public:
    virtual ~Backend() = default;
    virtual nlohmann::json hello_info() = 0;   // The connection merges this into the hello reply.
    virtual Result start(const nlohmann::json &request) = 0;   // The reply data holds session and dir.
    virtual Result segment(const Segment &segment) = 0;
    // Finishes later and sends a "stopped" event, with reason kStoppedByApp or kAppDisconnected.
    virtual Result stop(const std::string &reason) = 0;
    virtual nlohmann::json status() = 0;
    virtual Result queue_upload(const std::string &session) = 0;
};

// A Backend made of functions, so that main can give it lambdas.
struct FnBackend : Backend {
    std::function<nlohmann::json()> on_hello;
    std::function<Result(const nlohmann::json &)> on_start;
    std::function<Result(const Segment &)> on_segment;
    std::function<Result(const std::string &)> on_stop;
    std::function<nlohmann::json()> on_status;
    std::function<Result(const std::string &)> on_upload;

    nlohmann::json hello_info() override { return on_hello(); }
    Result start(const nlohmann::json &r) override { return on_start(r); }
    Result segment(const Segment &s) override { return on_segment(s); }
    Result stop(const std::string &reason) override { return on_stop(reason); }
    nlohmann::json status() override { return on_status(); }
    Result queue_upload(const std::string &s) override { return on_upload(s); }
};

// The protocol state of one client connection, without socket I/O so that tests can use it directly.
class Connection {
public:
    struct Output {
        std::vector<nlohmann::json> messages;
        bool close = false;
    };

    Output handle_line(const std::string &line, Backend &backend);
    bool greeted() const { return greeted_; }
    // The id of the stop request that still waits for its "stopped" event, or null.
    const nlohmann::json &pending_stop_id() const { return stop_id_; }
    void clear_pending_stop() { stop_id_ = nullptr; }

    static nlohmann::json error(const nlohmann::json &id, const std::string &code, const std::string &message);

private:
    bool greeted_ = false;
    nlohmann::json stop_id_;
};

}
