#pragma once

#include "core/metrics.h"
#include "core/threading.h"
#include "sink/osc_sink.h"
#include "sink/sink.h"

#include <netinet/in.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace babble {

// BFR Link v1 finds the Baballonia VRCFT module on a PC and sends the tracked values over the best live network path.
namespace link {

struct Message {
    std::string type;                 // The message type, for example beacon or pong.
    std::string sid, mid, name, version, path;
    uint64_t seq = 0;
    std::optional<uint64_t> rx;       // A pong gives the number of data datagrams that the module received.
    bool busy = false, active = false;
};
std::string encode(const Message &m);
std::optional<Message> decode(const char *data, size_t size);

// One IPv4 interface that can carry the link.
struct Interface {
    std::string name;
    uint32_t address = 0, broadcast = 0, netmask = 0;   // These values use host byte order.
};
std::vector<Interface> system_interfaces();
// Returns the addresses in `candidates` that are on the subnet of the interface.
std::vector<uint32_t> on_subnet(const Interface &iface, const std::vector<uint32_t> &candidates);

}

class LinkSink : public ISink {
public:
    LinkSink(const nlohmann::json &cfg, SinkDeps deps);
    ~LinkSink() override;

    bool start() override;
    void stop() override;

    // Tests use this to replace the interface scan, and they must call it before start().
    void set_interface_source(std::function<std::vector<link::Interface>()> f) { interfaces_fn_ = std::move(f); }

    // Returns the name of the active path, or "" if no path is active.
    std::string active_path() const;

    enum class Data { Unknown, Arriving, Missing };   // Unknown means that the sink cannot tell.
    struct Status {
        bool linked = false;
        std::string path, module, address;   // The path kind, PC name and PC address of the link.
        double rtt_ms = -1;                  // The last round trip of the active path, or -1 if none.
        Data data = Data::Unknown;
    };
    Status status() const;

private:
    struct Path {
        link::Interface iface;
        std::string kind;              // The kind is dongle, usb or lan.
        std::shared_ptr<SharedSocket> sock;   // The data sender also uses this socket when the path is active.
        bool has_peer = false;
        sockaddr_in peer{};            // The address of the module on this path.
        std::string mid;
        uint64_t last_pong_ns = 0;
        uint64_t failing_since_ns = 0;  // The time when the socket started to refuse sends, or 0 if sends work.
        int good_pongs = 0;            // The number of good pongs in sequence.
        std::map<uint64_t, uint64_t> pings;   // This map gives the send time for each seq.
    };

    void run();
    void rescan(uint64_t now);
    void beacon(uint64_t now);
    void ping(uint64_t now);
    void receive(Path *path, int fd, uint64_t now);
    void handle(Path *path, const link::Message &m, const sockaddr_in &from, uint64_t now);
    void choose(uint64_t now);
    bool send(int fd, const sockaddr_in &to, const link::Message &m);   // It fails if the socket refuses.
    void send_on(Path &p, const sockaddr_in &to, const link::Message &m);   // Records when a path stops sending.
    bool open_socket(Path &p);   // Binds the socket of the path to the address of its interface.
    void check_data(uint64_t now);   // Once each second, finds out if the module gets the data.
    link::Message base(const std::string &type) const;
    bool alive(const Path &p, uint64_t now) const;
    int rank(const std::string &kind) const;

    std::string name_;
    ThreadSpec thread_spec_;
    StatsRegistry &stats_;
    std::shared_ptr<OscDestination> destination_;
    std::unique_ptr<OscSink> data_;

    uint16_t control_port_ = 6767;
    std::vector<std::string> path_order_;
    std::map<std::string, std::string> interface_kinds_;
    std::vector<uint32_t> hosts_;
    uint64_t beacon_searching_ns_ = 2'000'000'000, beacon_connected_ns_ = 5'000'000'000;
    uint64_t ping_ns_ = 1'000'000'000, timeout_ns_ = 3'000'000'000;
    std::string display_name_, sid_;
    std::function<std::vector<link::Interface>()> interfaces_fn_;

    std::thread thread_;
    std::atomic<bool> stop_{false};
    int listen_sock_ = -1;             // The socket on control_port for the queries of the module.
    std::vector<std::unique_ptr<Path>> paths_;
    std::string module_mid_;           // The linked module, or "" if there is none.
    std::string module_name_;
    Path *active_ = nullptr;
    uint64_t seq_ = 0, next_beacon_ns_ = 0, next_ping_ns_ = 0, next_scan_ns_ = 0, last_query_beacon_ns_ = 0;

    // The sink knows that data arrives when the rx count of the module increases.
    uint64_t sent_last_ = 0, last_sent_ns_ = 0, sending_since_ns_ = 0, linked_ns_ = 0;
    std::optional<uint64_t> rx_last_;
    uint64_t rx_seen_ns_ = 0;
    Data data_state_ = Data::Unknown;

    mutable std::mutex status_mutex_;
    std::string active_name_;
    Status status_;

    Counter switches_;
    std::map<std::string, std::unique_ptr<LatencyStat>> rtt_;   // There is one stat for each path kind.
};

}
