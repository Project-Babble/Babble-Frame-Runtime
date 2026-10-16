#include "capture/capture_server.h"

#include "core/log.h"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
const babble::log::Logger LOG("capture");
constexpr size_t kMaxLine = 64 * 1024;      // A longer request line closes the connection.
constexpr size_t kMaxPending = 256 * 1024;  // The server drops a client that has more unread output than this.
}
namespace babble::capture {

using json = nlohmann::json;

Server::~Server() { close(); }

bool Server::listen(const std::filesystem::path &path, std::string *err) {
    close();
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.string().size() >= sizeof(addr.sun_path)) {
        *err = "socket path too long: " + path.string();
        return false;
    }
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        *err = std::string("socket: ") + strerror(errno);
        return false;
    }
    ::unlink(path.c_str());   // Remove a stale socket from a previous run.
    const mode_t old = umask(0077);
    const int rc = bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    umask(old);
    if (rc != 0 || ::listen(fd, 2) != 0) {
        *err = "bind " + path.string() + ": " + strerror(errno);
        ::close(fd);
        return false;
    }
    listen_ = fd;
    path_ = path;
    fds_changed_ = true;
    LOG.info("capture: control socket %s", path.c_str());
    return true;
}

void Server::close() {
    if (client_ >= 0)
        drop_client("closing");
    if (listen_ >= 0) {
        ::close(listen_);
        ::unlink(path_.c_str());
        listen_ = -1;
        fds_changed_ = true;
    }
}

std::vector<int> Server::fds() const {
    std::vector<int> v;
    if (listen_ >= 0)
        v.push_back(listen_);
    if (client_ >= 0)
        v.push_back(client_);
    return v;
}

void Server::on_readable(int fd) {
    if (fd == listen_)
        accept_client();
    else if (fd == client_)
        read_client();
}

void Server::accept_client() {
    for (;;) {
        const int fd = accept4(listen_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0)
            return;
        if (client_ >= 0) {
            const std::string busy = Connection::error(nullptr, "busy", "another app is connected").dump() + "\n";
            (void)!::send(fd, busy.data(), busy.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
            ::close(fd);
            LOG.warn("capture: refused a second client (one is already connected)");
            continue;
        }
        client_ = fd;
        conn_ = Connection();
        in_.clear();
        out_.clear();
        fds_changed_ = true;
        LOG.info("capture: client connected");
    }
}

void Server::drop_client(const char *why) {
    if (client_ < 0)
        return;
    flush();
    ::close(client_);
    client_ = -1;
    fds_changed_ = true;
    LOG.info("capture: client disconnected (%s)", why);
    // A recording must stop when the app crashes or closes.
    if (conn_.greeted())
        backend_.stop(kAppDisconnected);
    conn_ = Connection();
}

void Server::read_client() {
    char buf[4096];
    for (;;) {
        const ssize_t n = ::recv(client_, buf, sizeof(buf), 0);
        if (n == 0)
            return drop_client("closed by the app");
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            return drop_client(strerror(errno));
        }
        in_.append(buf, size_t(n));
        size_t nl;
        while ((nl = in_.find('\n')) != std::string::npos) {
            std::string line = in_.substr(0, nl);
            in_.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty())
                continue;
            auto out = conn_.handle_line(line, backend_);
            for (const auto &m : out.messages)
                out_ += m.dump() + "\n";
            if (out.close) {
                flush();
                return drop_client("protocol error");
            }
        }
        if (in_.size() > kMaxLine) {
            out_ += Connection::error(nullptr, "bad_request", "line too long").dump() + "\n";
            return drop_client("line too long");
        }
    }
    flush();
}

void Server::flush() {
    while (client_ >= 0 && !out_.empty()) {
        const ssize_t n = ::send(client_, out_.data(), out_.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            out_.erase(0, size_t(n));
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        break;   // On EAGAIN, the rest goes with the next send.
    }
}

void Server::send(const json &event) {
    if (client_ < 0 || !conn_.greeted())
        return;
    out_ += event.dump() + "\n";
    flush();
    if (out_.size() > kMaxPending) {
        out_.clear();
        drop_client("not reading its messages");
    }
}

void Server::stopped(json event) {
    event["op"] = "stopped";
    const json &id = conn_.pending_stop_id();
    if (!id.is_null() && !id.is_boolean())
        event["id"] = id;
    conn_.clear_pending_stop();
    send(event);
}

}
