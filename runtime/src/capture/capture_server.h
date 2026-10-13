#pragma once

#include "capture/capture_protocol.h"

#include <filesystem>
#include <string>
#include <vector>

namespace babble::capture {

// The dataset capture app's control socket: a Unix stream socket speaking
// JSON lines (capture_protocol.h), one client at a time. Main thread,
// non-blocking: main adds fds() to its epoll and calls on_readable(); a second
// client is told "busy" and closed. If the client goes away mid-recording the
// recording is stopped.
class Server {
public:
    explicit Server(Backend &backend) : backend_(backend) {}
    ~Server();

    bool listen(const std::filesystem::path &path, std::string *err);
    void close();
    const std::filesystem::path &path() const { return path_; }

    std::vector<int> fds() const;   // the listening socket, and the client's if connected
    void on_readable(int fd);
    bool connected() const { return client_ >= 0; }
    bool greeted() const { return client_ >= 0 && conn_.greeted(); }

    // An event for the client (status, stopped); dropped if none is connected.
    void send(const nlohmann::json &event);
    // The recording finished: sends "stopped" (with the stop request's id).
    void stopped(nlohmann::json event);

    // Main loop hook: called after the client fd set changes (connect or
    // disconnect), so main can update its epoll.
    bool take_fds_changed() {
        const bool c = fds_changed_;
        fds_changed_ = false;
        return c;
    }

private:
    void accept_client();
    void drop_client(const char *why);
    void read_client();
    void flush();

    Backend &backend_;
    std::filesystem::path path_;
    int listen_ = -1, client_ = -1;
    Connection conn_;
    std::string in_, out_;
    bool fds_changed_ = false;
};

} // namespace babble::capture
