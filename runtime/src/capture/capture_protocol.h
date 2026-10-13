#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace babble::capture {

constexpr int kProtocol = 2;

// Why a recording stopped, as session.json and the stopped event report it.
inline constexpr const char *kStoppedByApp = "stopped by the app";
inline constexpr const char *kAppDisconnected = "the app disconnected";

// What a request did: ok with data for the reply, or an error code
// (busy | no_source | disk | unavailable | bad_request | protocol | io) and message.
struct Result {
    bool ok = true;
    std::string code, message;
    nlohmann::json data = nlohmann::json::object();

    static Result fail(std::string code, std::string message) { return {false, std::move(code), std::move(message), {}}; }
};

// One labelled span from the app. step and phase are optional (protocol 2):
// the routine step index and its phase (practice, hold, alternate_on...).
struct Segment {
    std::string label;
    uint64_t start_ns = 0, end_ns = 0;
    std::optional<int64_t> step;
    std::string phase;   // "" = not given
};

// The runtime side of the dataset capture app's requests (main implements it).
class Backend {
public:
    virtual ~Backend() = default;
    virtual nlohmann::json hello_info() = 0;   // merged into the hello reply
    virtual Result start(const nlohmann::json &request) = 0;   // data: session, dir
    virtual Result segment(const Segment &segment) = 0;
    // Finishes asynchronously: "stopped" is sent as an event later. reason is
    // kStoppedByApp or kAppDisconnected.
    virtual Result stop(const std::string &reason) = 0;
    virtual nlohmann::json status() = 0;
    virtual Result queue_upload(const std::string &session) = 0;
};

// A Backend made of functions (main wires its lambdas in).
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

// One client connection's protocol state. JSON lines in, JSON objects out;
// pure, so it's tested without sockets (capture_server.h does the I/O).
//
// Requests: hello (first), start, segment, stop, status, upload. start may
// carry routine_def (the routine the app runs, saved as routine.json). Replies echo
// the request's "id". hello with another protocol number is refused and the
// connection closed.
class Connection {
public:
    struct Output {
        std::vector<nlohmann::json> messages;
        bool close = false;
    };

    Output handle_line(const std::string &line, Backend &backend);
    bool greeted() const { return greeted_; }
    // The id of the stop request whose "stopped" event is still due (null if none).
    const nlohmann::json &pending_stop_id() const { return stop_id_; }
    void clear_pending_stop() { stop_id_ = nullptr; }

    static nlohmann::json error(const nlohmann::json &id, const std::string &code, const std::string &message);

private:
    bool greeted_ = false;
    nlohmann::json stop_id_;
};

} // namespace babble::capture
