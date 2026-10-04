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

// BFR Link, protocol v1: finds the Baballonia VRCFT module on a PC, keeps a
// heartbeat to it over every network path that reaches it, and streams the
// tracked values (as the osc sink does) over the best live path.
//
// The Frame can reach a PC over the streaming dongle (its own AP, wlanap),
// over USB (usb0) and over the home LAN (wlan0), in any combination.
// Everything travels as UDP datagrams to the module's port 6767: control
// messages are small JSON objects, data is OSC.
//
//   beacon  headset -> broadcast + unicast   {sid, name, version}
//   query   module  -> broadcast              {mid}        (answered by a beacon)
//   offer   module  -> beacon's source        {mid, name, busy}
//   ping    headset -> module, per path, 1 Hz {sid, seq, path, active}
//   pong    module  -> ping's source          {mid, seq, rx}
//   bye     either side, on clean shutdown
//   data    headset -> module, active path    OSC bundles of the osc sink's floats
//
// Every control message also has "proto":"bfr-link","v":1 and "type"; the
// module tells the two apart by the first byte ('{' or '#'/'/'). Beacons,
// pings and data go out of one socket per interface, so the module's offers
// and pongs come back on that path, and its address is their source address
// (never a field). Data shares the pings' address pair on purpose: the
// module's pongs keep that pair open in the PC's firewall, so data needs no
// inbound rule there. rx in a pong counts the data datagrams the module has
// received, which shows whether data actually arrives (status().data).
// A path is dead after timeout_s without a pong; the best live path in
// path_order is used, and switching back to a better path waits for 3 good
// pongs.
//
// Config (sinks entry, type "vrcft_link"; also the osc sink's prefix, rename,
// exclude, max_rate_hz, thread):
//   control_port   6767
//   path_order     ["dongle", "usb", "lan"]
//   interfaces     {"wlanap": "dongle", "usb0": "usb"}; others are "lan"
//   hosts          ["192.168.1.50", ...] extra unicast beacon targets
//   beacon_s       [2, 5] beacon period while searching / while connected
//   ping_s         1
//   timeout_s      3
//   name           shown by the module (default: the host name)
namespace link {

struct Message {
    std::string type;                 // beacon, query, offer, ping, pong, bye
    std::string sid, mid, name, version, path;
    uint64_t seq = 0;
    std::optional<uint64_t> rx;       // pong: data datagrams received (absent from older modules)
    bool busy = false, active = false;
};
std::string encode(const Message &m);
std::optional<Message> decode(const char *data, size_t size);

// One IPv4 interface that can carry the link.
struct Interface {
    std::string name;
    uint32_t address = 0, broadcast = 0, netmask = 0;   // host byte order
};
std::vector<Interface> system_interfaces();
// The addresses among `candidates` on the interface's own subnet (ARP entries
// can name a host's addresses from other networks too).
std::vector<uint32_t> on_subnet(const Interface &iface, const std::vector<uint32_t> &candidates);

} // namespace link

class LinkSink : public ISink {
public:
    LinkSink(const nlohmann::json &cfg, SinkDeps deps);
    ~LinkSink() override;

    bool start() override;
    void stop() override;

    // For tests: replaces the interface scan. Call before start().
    void set_interface_source(std::function<std::vector<link::Interface>()> f) { interfaces_fn_ = std::move(f); }

    // For tests and status: the active path's name ("" while searching).
    std::string active_path() const;

    // For the debug overlay.
    enum class Data { Unknown, Arriving, Missing };   // Unknown: not sending, or the module doesn't say
    struct Status {
        bool linked = false;
        std::string path, module, address;   // while linked: path kind, PC name, PC address
        double rtt_ms = -1;                  // the active path's last round trip, -1 = none yet
        Data data = Data::Unknown;
    };
    Status status() const;

private:
    struct Path {
        link::Interface iface;
        std::string kind;              // dongle / usb / lan
        std::shared_ptr<SharedSocket> sock;   // shared with the data sender while active
        bool has_peer = false;
        sockaddr_in peer{};            // the module's address on this path (control and data)
        std::string mid;
        uint64_t last_pong_ns = 0;
        uint64_t failing_since_ns = 0;  // sends refused (EAGAIN/ENOBUFS) since, 0 = sending fine
        int good_pongs = 0;            // consecutive
        std::map<uint64_t, uint64_t> pings;   // seq -> sent time
    };

    void run();
    void rescan(uint64_t now);
    void beacon(uint64_t now);
    void ping(uint64_t now);
    void receive(Path *path, int fd, uint64_t now);
    void handle(Path *path, const link::Message &m, const sockaddr_in &from, uint64_t now);
    void choose(uint64_t now);
    bool send(int fd, const sockaddr_in &to, const link::Message &m);   // false if the socket refused it
    void send_on(Path &p, const sockaddr_in &to, const link::Message &m);   // tracks a path that stops sending
    bool open_socket(Path &p);   // (re)binds the path's socket to its interface address
    void check_data(uint64_t now);   // 1 Hz: are we sending, and does the module see it?
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
    int listen_sock_ = -1;             // :control_port, for the module's queries
    std::vector<std::unique_ptr<Path>> paths_;
    std::string module_mid_;           // the module we're linked to ("" = none yet)
    std::string module_name_;
    Path *active_ = nullptr;
    uint64_t seq_ = 0, next_beacon_ns_ = 0, next_ping_ns_ = 0, next_scan_ns_ = 0, last_query_beacon_ns_ = 0;

    // Data arriving at the module: its rx count rising while we send.
    uint64_t sent_last_ = 0, last_sent_ns_ = 0, sending_since_ns_ = 0, linked_ns_ = 0;
    std::optional<uint64_t> rx_last_;
    uint64_t rx_seen_ns_ = 0;
    Data data_state_ = Data::Unknown;

    mutable std::mutex status_mutex_;
    std::string active_name_;
    Status status_;

    Counter switches_;
    std::map<std::string, std::unique_ptr<LatencyStat>> rtt_;   // per path kind
};

} // namespace babble
