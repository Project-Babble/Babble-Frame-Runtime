#pragma once

#include "capture/capture_protocol.h"

#include <filesystem>
#include <string>
#include <vector>

namespace babble::capture {

// The non-blocking Unix control socket for the capture app, which accepts one client at a time on the main thread.
class Server {
public:
    explicit Server(Backend &backend) : backend_(backend) {}
    ~Server();

    bool listen(const std::filesystem::path &path, std::string *err);
    void close();
    const std::filesystem::path &path() const { return path_; }

    std::vector<int> fds() const;   // Returns the listening socket and the client socket if a client is connected.
    void on_readable(int fd);
    bool connected() const { return client_ >= 0; }
    bool greeted() const { return client_ >= 0 && conn_.greeted(); }

    // Sends an event to the client, or drops it if no client is connected.
    void send(const nlohmann::json &event);
    // Sends the "stopped" event with the id of the stop request after the recording is finished.
    void stopped(nlohmann::json event);

    // Returns true once after the client fd set changes, so that main can update its epoll.
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

}
