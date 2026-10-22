#include "sink/link_sink.h"

#include "babble_version.h"
#include "core/clock.h"
#include "core/log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
const babble::log::Logger LOG("link");
}

namespace babble {

namespace link {

std::string encode(const Message &m) {
    nlohmann::json j = {{"proto", "bfr-link"}, {"v", 1}, {"type", m.type}};
    for (const auto &[key, value] : {std::pair{"sid", &m.sid}, {"mid", &m.mid}, {"name", &m.name},
                                     {"version", &m.version}, {"path", &m.path}})
        if (!value->empty())
            j[key] = *value;
    if (m.type == "ping" || m.type == "pong")
        j["seq"] = m.seq;
    if (m.type == "ping")
        j["active"] = m.active;
    if (m.type == "pong" && m.rx)
        j["rx"] = *m.rx;
    if (m.type == "offer")
        j["busy"] = m.busy;
    if (m.eye)
        j["eye"] = *m.eye;
    if (m.face)
        j["face"] = *m.face;
    if (m.restart)
        j["restart"] = *m.restart;
    return j.dump();
}

std::optional<Message> decode(const char *data, size_t size) {
    const auto j = nlohmann::json::parse(data, data + size, nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("proto", "") != "bfr-link" || !j.contains("v") ||
        !j["v"].is_number_integer() || j["v"].get<int>() != 1 || !j.contains("type") || !j["type"].is_string())
        return std::nullopt;
    Message m;
    try {
        m.type = j["type"].get<std::string>();
        m.sid = j.value("sid", "");
        m.mid = j.value("mid", "");
        m.name = j.value("name", "");
        m.version = j.value("version", "");
        m.path = j.value("path", "");
        m.seq = j.value("seq", uint64_t(0));
        if (j.contains("rx"))
            m.rx = j["rx"].get<uint64_t>();
        m.busy = j.value("busy", false);
        m.active = j.value("active", false);
        if (j.contains("eye"))
            m.eye = j["eye"].get<bool>();
        if (j.contains("face"))
            m.face = j["face"].get<bool>();
        if (j.contains("restart"))
            m.restart = j["restart"].get<bool>();
    } catch (const nlohmann::json::exception &) {
        return std::nullopt;   // A field has the wrong type.
    }
    return m;
}

std::vector<Interface> system_interfaces() {
    std::vector<Interface> out;
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) != 0)
        return out;
    for (ifaddrs *i = list; i; i = i->ifa_next) {
        // Use only IPv4 interfaces that are up, have a carrier and can broadcast, but not loopback.
        const unsigned want = IFF_UP | IFF_RUNNING | IFF_BROADCAST;
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & want) != want ||
            (i->ifa_flags & IFF_LOOPBACK) || !i->ifa_broadaddr || !i->ifa_netmask)
            continue;
        Interface f;
        f.name = i->ifa_name;
        f.address = ntohl(reinterpret_cast<sockaddr_in *>(i->ifa_addr)->sin_addr.s_addr);
        f.broadcast = ntohl(reinterpret_cast<sockaddr_in *>(i->ifa_broadaddr)->sin_addr.s_addr);
        f.netmask = ntohl(reinterpret_cast<sockaddr_in *>(i->ifa_netmask)->sin_addr.s_addr);
        out.push_back(f);
    }
    freeifaddrs(list);
    return out;
}

std::vector<uint32_t> on_subnet(const Interface &iface, const std::vector<uint32_t> &candidates) {
    std::vector<uint32_t> out;
    for (uint32_t a : candidates)
        if ((a & iface.netmask) == (iface.address & iface.netmask) && a != iface.address && a != iface.broadcast)
            out.push_back(a);
    return out;
}

}

namespace {

sockaddr_in endpoint(uint32_t ip, uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(ip);
    a.sin_port = htons(port);
    return a;
}

std::string ip_string(const sockaddr_in &a) {
    char buf[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &a.sin_addr, buf, sizeof(buf));
    return buf;
}

// Returns the complete ARP entries of one interface, for unicast beacons if the dongle drops broadcasts.
std::vector<uint32_t> arp_neighbours(const std::string &iface) {
    std::vector<uint32_t> out;
    std::ifstream arp("/proc/net/arp");
    std::string line;
    std::getline(arp, line);   // Skip the header line.
    while (std::getline(arp, line)) {
        std::istringstream in(line);
        std::string ip, hw_type, flags, mac, mask, dev;
        if (!(in >> ip >> hw_type >> flags >> mac >> mask >> dev) || dev != iface || flags == "0x0")
            continue;
        in_addr a{};
        if (inet_pton(AF_INET, ip.c_str(), &a) == 1)
            out.push_back(ntohl(a.s_addr));
    }
    return out;
}

std::string random_id() {
    std::random_device rd;
    std::mt19937_64 rng((uint64_t(rd()) << 32) ^ rd() ^ mono_ns());
    char buf[17];
    snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(rng()));
    return buf;
}

uint64_t seconds_ns(double s) { return uint64_t(std::max(0.01, s) * 1e9); }

}

LinkSink::LinkSink(const nlohmann::json &cfg, SinkDeps deps)
    : name_(deps.name), thread_spec_(thread_spec(deps.threads, cfg.value("thread", "osc"), "bbl-link")),
      stats_(deps.stats), destination_(std::make_shared<OscDestination>()),
      control_port_(uint16_t(cfg.value("control_port", 6767))), sid_(random_id()),
      interfaces_fn_(link::system_interfaces) {
    // The data is the values of the osc sink in bundles, sent from the socket of the active path.
    nlohmann::json data_cfg = cfg;
    data_cfg["bundle"] = true;
    data_ = std::make_unique<OscSink>(data_cfg, deps, destination_);

    path_order_ = cfg.value("path_order", std::vector<std::string>{"dongle", "usb", "lan"});
    interface_kinds_ = cfg.value("interfaces", std::map<std::string, std::string>{{"wlanap", "dongle"}, {"usb0", "usb"}});
    // The PC address of the Outputs page is one more host, for networks where the broadcasts do not arrive.
    auto hosts = cfg.value("hosts", std::vector<std::string>{});
    if (const std::string pc = cfg.value("pc_address", ""); !pc.empty())
        hosts.push_back(pc);
    for (const auto &h : hosts) {
        in_addr a{};
        if (inet_pton(AF_INET, h.c_str(), &a) != 1)
            LOG.warn("%s: ignoring host '%s' (not an IPv4 address)", name_.c_str(), h.c_str());
        else if (std::find(hosts_.begin(), hosts_.end(), ntohl(a.s_addr)) == hosts_.end())
            hosts_.push_back(ntohl(a.s_addr));
    }
    const auto beacon_s = cfg.value("beacon_s", std::vector<double>{2, 5});
    if (beacon_s.size() == 2) {
        beacon_searching_ns_ = seconds_ns(beacon_s[0]);
        beacon_connected_ns_ = seconds_ns(beacon_s[1]);
    }
    ping_ns_ = seconds_ns(cfg.value("ping_s", 1.0));
    timeout_ns_ = seconds_ns(cfg.value("timeout_s", 3.0));

    display_name_ = cfg.value("name", "");
    if (display_name_.empty()) {
        char host[256] = {};
        gethostname(host, sizeof(host) - 1);
        display_name_ = host[0] ? host : "Steam Frame";
    }

    stats_.add(name_ + ".switches", &switches_);
    for (const auto &kind : path_order_) {
        rtt_[kind] = std::make_unique<LatencyStat>();
        stats_.add(name_ + ".rtt_" + kind, rtt_[kind].get());
    }
}

LinkSink::~LinkSink() {
    stop();
    stats_.remove_prefix(name_ + ".");
}

bool LinkSink::start() {
    listen_sock_ = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    const sockaddr_in any = endpoint(INADDR_ANY, control_port_);
    if (listen_sock_ < 0 || bind(listen_sock_, reinterpret_cast<const sockaddr *>(&any), sizeof(any)) != 0) {
        // This is not fatal because the module still finds the headset through the beacons.
        LOG.warn("%s: cannot listen on udp %u for module queries: %s", name_.c_str(), control_port_, strerror(errno));
        if (listen_sock_ >= 0)
            close(listen_sock_);
        listen_sock_ = -1;
    }
    if (!data_->start())
        return false;
    stop_ = false;
    thread_ = start_thread(thread_spec_, [this] { run(); });
    LOG.info("%s: looking for the VRCFT module (control port %u, paths %s)", name_.c_str(), control_port_,
             [&] {
                 std::string s;
                 for (const auto &k : path_order_)
                     s += (s.empty() ? "" : " > ") + k;
                 return s;
             }()
                 .c_str());
    return true;
}

void LinkSink::stop() {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
    if (data_)
        data_->stop();
    paths_.clear();   // Each socket closes when its last owner releases it.
    active_ = nullptr;
    destination_->clear();
    if (listen_sock_ >= 0)
        close(listen_sock_);
    listen_sock_ = -1;
    // Clear the link so that the next start begins a new search.
    next_scan_ns_ = next_beacon_ns_ = next_ping_ns_ = 0;
    rx_last_.reset();
    data_state_ = Data::Unknown;
    std::lock_guard lock(status_mutex_);
    active_name_.clear();
    status_ = {};
}

std::string LinkSink::active_path() const {
    std::lock_guard lock(status_mutex_);
    return active_name_;
}

LinkSink::Status LinkSink::status() const {
    std::lock_guard lock(status_mutex_);
    Status s = status_;
    if (s.linked) {
        s.claims_eye = claims_eye_;
        s.claims_face = claims_face_;
        s.restart = restart_;
    }
    return s;
}

void LinkSink::set_mode(const link::Message &m) {
    if (!m.eye || !m.face)
        return;   // An older module does not report its claims.
    const bool restart = m.restart.value_or(false);
    std::lock_guard lock(status_mutex_);
    if (claims_eye_ == m.eye && claims_face_ == m.face && restart_ == restart)
        return;
    claims_eye_ = m.eye;
    claims_face_ = m.face;
    const char *eye = *m.eye ? "on" : "off", *face = *m.face ? "on" : "off";
    if (restart)
        LOG.warn("%s: the VRCFaceTracking module claims eye tracking %s and face tracking %s; restart VRCFaceTracking "
                 "to match the tracking that is on", name_.c_str(), eye, face);
    else
        LOG.info("%s: the VRCFaceTracking module claims eye tracking %s and face tracking %s", name_.c_str(), eye, face);
    restart_ = restart;
}

void LinkSink::set_features(bool eye, bool face) {
    const bool changed = eye_.exchange(eye) != eye;
    if (face_.exchange(face) != face || changed)
        LOG.info("%s: tells the module: eye tracking %s, face tracking %s", name_.c_str(), eye ? "on" : "off",
                 face ? "on" : "off");
}

link::Message LinkSink::base(const std::string &type) const {
    link::Message m;
    m.type = type;
    m.sid = sid_;
    if (type == "beacon" || type == "ping") {
        m.eye = eye_.load();
        m.face = face_.load();
    }
    return m;
}

bool LinkSink::send(int fd, const sockaddr_in &to, const link::Message &m) {
    const std::string text = link::encode(m);
    if (LOG.debug_enabled() && m.type != "ping")   // Do not log pings because the pong log shows them.
        LOG.debug("%s: -> %s:%u %s", name_.c_str(), ip_string(to).c_str(), ntohs(to.sin_port), text.c_str());
    if (sendto(fd, text.data(), text.size(), MSG_DONTWAIT | MSG_NOSIGNAL, reinterpret_cast<const sockaddr *>(&to),
               sizeof(to)) >= 0)
        return true;
    // A full send buffer means that the socket is stuck, not that the host is unreachable.
    return !(errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS);
}

void LinkSink::send_on(Path &p, const sockaddr_in &to, const link::Message &m) {
    if (send(p.sock->fd, to, m))
        p.failing_since_ns = 0;
    else if (!p.failing_since_ns)
        p.failing_since_ns = mono_ns();
}

bool LinkSink::open_socket(Path &p) {
    p.sock = std::make_shared<SharedSocket>(socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    p.failing_since_ns = 0;
    int one = 1;
    setsockopt(p.sock->fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    const sockaddr_in local = endpoint(p.iface.address, 0);
    if (p.sock->fd < 0 || bind(p.sock->fd, reinterpret_cast<const sockaddr *>(&local), sizeof(local)) != 0) {
        LOG.warn("%s: cannot use interface %s: %s", name_.c_str(), p.iface.name.c_str(), strerror(errno));
        return false;
    }
    return true;
}

int LinkSink::rank(const std::string &kind) const {
    auto it = std::find(path_order_.begin(), path_order_.end(), kind);
    return it == path_order_.end() ? int(path_order_.size()) : int(it - path_order_.begin());
}

bool LinkSink::alive(const Path &p, uint64_t now) const {
    return p.has_peer && !module_mid_.empty() && p.mid == module_mid_ && now - p.last_pong_ns < timeout_ns_;
}

void LinkSink::rescan(uint64_t now) {
    const auto found = interfaces_fn_();
    // Remove the paths whose interface is gone or has a different address.
    for (auto it = paths_.begin(); it != paths_.end();) {
        const Path &p = **it;
        const bool present = std::any_of(found.begin(), found.end(), [&](const link::Interface &f) {
            return f.name == p.iface.name && f.address == p.iface.address;
        });
        if (present) {
            ++it;
            continue;
        }
        LOG.info("%s: interface %s (%s) is gone", name_.c_str(), p.iface.name.c_str(), p.kind.c_str());
        if (active_ == it->get())
            active_ = nullptr;
        it = paths_.erase(it);
    }
    // Open a new socket for a path that refused all sends for 5 s, because its send buffer is stuck.
    for (auto &p : paths_) {
        if (!p->failing_since_ns || now - p->failing_since_ns < 5'000'000'000ull)
            continue;
        LOG.warn("%s: interface %s (%s) has refused every send for %.0f s; reopening its socket", name_.c_str(),
                 p->iface.name.c_str(), p->kind.c_str(), double(now - p->failing_since_ns) / 1e9);
        if (open_socket(*p) && active_ == p.get())
            destination_->set(p->peer, p->sock);   // The data moves to the new socket and sends all values again.
    }
    for (const auto &f : found) {
        const bool known = std::any_of(paths_.begin(), paths_.end(), [&](const auto &p) {
            return p->iface.name == f.name && p->iface.address == f.address;
        });
        if (known)
            continue;
        auto p = std::make_unique<Path>();
        p->iface = f;
        auto kind = interface_kinds_.find(f.name);
        p->kind = kind != interface_kinds_.end() ? kind->second : "lan";
        if (!open_socket(*p))
            continue;
        const sockaddr_in local = endpoint(f.address, 0);
        LOG.info("%s: interface %s (%s, %s)", name_.c_str(), f.name.c_str(), p->kind.c_str(),
                 ip_string(local).c_str());
        paths_.push_back(std::move(p));
        next_beacon_ns_ = now;   // Send a beacon on the new path immediately.
    }
    choose(now);
}

void LinkSink::beacon(uint64_t now) {
    link::Message m = base("beacon");
    m.name = display_name_;
    m.version = BABBLE_VERSION_DISPLAY;
    for (auto &p : paths_) {
        send_on(*p, endpoint(p->iface.broadcast, control_port_), m);
        if (p->kind == "dongle")
            for (uint32_t station : link::on_subnet(p->iface, arp_neighbours(p->iface.name)))
                send_on(*p, endpoint(station, control_port_), m);
    }
    // Send to each manual host from the path on its subnet, or from all paths if none matches.
    for (uint32_t host : hosts_) {
        bool sent = false;
        for (auto &p : paths_)
            if ((host & p->iface.netmask) == (p->iface.address & p->iface.netmask)) {
                send_on(*p, endpoint(host, control_port_), m);
                sent = true;
            }
        if (!sent)
            for (auto &p : paths_)
                send_on(*p, endpoint(host, control_port_), m);
    }
    next_beacon_ns_ = now + (active_ ? beacon_connected_ns_ : beacon_searching_ns_);
}

void LinkSink::ping(uint64_t now) {
    for (auto &p : paths_) {
        if (!p->has_peer || p->mid != module_mid_)
            continue;
        link::Message m = base("ping");
        m.seq = ++seq_;
        m.path = p->kind;
        m.active = p.get() == active_;
        send_on(*p, p->peer, m);
        p->pings[m.seq] = now;
        while (p->pings.size() > 16)   // Keep only the 16 most recent unanswered pings.
            p->pings.erase(p->pings.begin());
    }
    next_ping_ns_ = now + ping_ns_;
    check_data(now);
}

void LinkSink::check_data(uint64_t now) {
    const uint64_t grace = std::max<uint64_t>(3'000'000'000ull, 3 * ping_ns_);
    const uint64_t sent = data_->sent();
    if (sent != sent_last_) {
        if (now - last_sent_ns_ >= grace)
            sending_since_ns_ = now;   // The sink sends again after a pause.
        last_sent_ns_ = now;
    }
    sent_last_ = sent;
    Data state = Data::Unknown;
    if (active_ && rx_last_ && now - last_sent_ns_ < grace) {
        // The rx count of the module must increase within the grace period after each start.
        const uint64_t since = std::max({rx_seen_ns_, linked_ns_, sending_since_ns_});
        if (now - since < grace)
            state = Data::Arriving;
        else
            state = Data::Missing;
    }
    if (state != data_state_) {
        if (state == Data::Missing)
            LOG.warn("%s: sending to \"%s\", but none of it arrives (a firewall, or a network that drops it?)",
                     name_.c_str(), module_name_.c_str());
        else if (state == Data::Arriving && data_state_ == Data::Missing)
            LOG.info("%s: data reaches \"%s\" again", name_.c_str(), module_name_.c_str());
        data_state_ = state;
    }
    std::lock_guard lock(status_mutex_);
    status_.data = active_ ? state : Data::Unknown;
}

void LinkSink::handle(Path *path, const link::Message &m, const sockaddr_in &from, uint64_t now) {
    if (!path) {   // On the listening socket, only the queries of the module are important.
        if (m.type == "query" && now - last_query_beacon_ns_ > 500'000'000) {
            last_query_beacon_ns_ = now;
            next_beacon_ns_ = now;
        }
        return;
    }
    if (m.type == "offer") {
        if (m.mid.empty())
            return;
        if (m.busy) {
            LOG.debug("%s: module %s at %s is busy with another headset", name_.c_str(), m.name.c_str(),
                      ip_string(from).c_str());
            return;
        }
        // Keep the current module while it is reachable.
        const bool current_alive =
            std::any_of(paths_.begin(), paths_.end(), [&](const auto &p) { return alive(*p, now); });
        if (m.mid != module_mid_) {
            if (current_alive)
                return;
            module_mid_ = m.mid;
            module_name_ = m.name;
            {
                std::lock_guard lock(status_mutex_);
                claims_eye_.reset();
                claims_face_.reset();
                restart_ = false;
            }
            LOG.info("%s: found VRCFT module \"%s\" at %s via %s", name_.c_str(), m.name.c_str(),
                     ip_string(from).c_str(), path->kind.c_str());
        }
        const bool fresh = !path->has_peer || path->mid != m.mid || path->peer.sin_addr.s_addr != from.sin_addr.s_addr;
        path->has_peer = true;
        path->peer = from;
        path->mid = m.mid;
        set_mode(m);
        if (fresh) {
            path->last_pong_ns = now;   // The offer is also an answer.
            path->good_pongs = 0;
        }
        choose(now);
        return;
    }
    if (m.type == "pong") {
        auto it = path->pings.find(m.seq);
        if (it == path->pings.end() || !path->has_peer)
            return;
        if (auto rtt = rtt_.find(path->kind); rtt != rtt_.end())
            rtt->second->add(now - it->second);
        if (path == active_) {
            std::lock_guard lock(status_mutex_);
            status_.rtt_ms = double(now - it->second) / 1e6;
        }
        path->pings.erase(it);
        if (m.rx && path == active_) {
            // An increase of the rx count of the module shows that the data arrives.
            if (!rx_last_ || *m.rx > *rx_last_)
                rx_seen_ns_ = rx_last_ ? now : rx_seen_ns_;
            rx_last_ = m.rx;
        }
        if (m.mid != path->mid) {
            // A new mid at the same address means that the module restarted.
            LOG.info("%s: VRCFT module restarted", name_.c_str());
            for (auto &p : paths_)
                if (p->mid == module_mid_)
                    p->mid = m.mid;
            module_mid_ = m.mid;
            rx_last_.reset();   // The new instance starts a new count.
            if (active_ == path)
                destination_->set(path->peer, path->sock);   // Send all values again to the new instance.
        }
        if (now - path->last_pong_ns >= timeout_ns_)
            path->good_pongs = 0;   // The path was dead, so it must prove itself again.
        path->last_pong_ns = now;
        path->good_pongs++;
        set_mode(m);
        choose(now);
        return;
    }
    if (m.type == "bye" && m.mid == module_mid_) {
        LOG.info("%s: VRCFT module \"%s\" closed", name_.c_str(), module_name_.c_str());
        for (auto &p : paths_)
            p->has_peer = false;
        choose(now);
    }
}

void LinkSink::receive(Path *path, int fd, uint64_t now) {
    char buf[2048];
    for (;;) {
        sockaddr_in from{};
        socklen_t len = sizeof(from);
        const ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&from), &len);
        if (n <= 0)
            return;
        if (auto m = link::decode(buf, size_t(n))) {
            if (LOG.debug_enabled() && m->type != "beacon")   // The beacons of the headset come back to the listener.
                LOG.debug("%s: <- %s:%u (%s) %.*s", name_.c_str(), ip_string(from).c_str(), ntohs(from.sin_port),
                          path ? path->kind.c_str() : "listener", int(n), buf);
            handle(path, *m, from, now);
        }
    }
}

void LinkSink::choose(uint64_t now) {
    Path *best = nullptr;
    for (auto &p : paths_)
        if (alive(*p, now) && (!best || rank(p->kind) < rank(best->kind)))
            best = p.get();
    Path *next = active_;
    if (next && !alive(*next, now))
        next = nullptr;
    if (!next)
        next = best;
    else if (best && best != next && rank(best->kind) < rank(next->kind) && best->good_pongs >= 3)
        next = best;   // Move back to a better path after it proves itself.
    if (next == active_)
        return;

    const Path *previous = active_;
    active_ = next;
    std::string status;
    if (active_) {
        destination_->set(active_->peer, active_->sock);
        status = active_->kind;
        linked_ns_ = now;
        LOG.info("%s: %s \"%s\" via %s (%s, %s:%d)", name_.c_str(), previous ? "switched to" : "linked to",
                 module_name_.c_str(), active_->kind.c_str(), active_->iface.name.c_str(),
                 ip_string(active_->peer).c_str(), ntohs(active_->peer.sin_port));
        if (previous)
            switches_.add();
        next_beacon_ns_ = std::min(next_beacon_ns_, now + beacon_connected_ns_);
    } else {
        destination_->clear();
        LOG.info("%s: link to \"%s\" lost; searching", name_.c_str(), module_name_.c_str());
        next_beacon_ns_ = now;
    }
    std::lock_guard lock(status_mutex_);
    active_name_ = status;
    status_ = {};
    if (active_) {
        status_.linked = true;
        status_.path = active_->kind;
        status_.module = module_name_;
        status_.address = ip_string(active_->peer);
    }
}

void LinkSink::run() {
    const uint64_t scan_ns = 5'000'000'000;
    std::vector<pollfd> fds;
    std::vector<Path *> owners;
    while (!stop_) {
        uint64_t now = mono_ns();
        if (now >= next_scan_ns_) {
            rescan(now);
            next_scan_ns_ = now + scan_ns;
        }
        if (now >= next_beacon_ns_)
            beacon(now);
        if (now >= next_ping_ns_)
            ping(now);
        choose(now);

        fds.clear();
        owners.clear();
        if (listen_sock_ >= 0) {
            fds.push_back({listen_sock_, POLLIN, 0});
            owners.push_back(nullptr);
        }
        for (auto &p : paths_) {
            fds.push_back({p->sock->fd, POLLIN, 0});
            owners.push_back(p.get());
        }
        const uint64_t next = std::min({next_scan_ns_, next_beacon_ns_, next_ping_ns_});
        const int wait_ms = int(std::clamp<int64_t>((int64_t(next) - int64_t(now)) / 1'000'000, 1, 100));
        if (poll(fds.data(), nfds_t(fds.size()), wait_ms) <= 0)
            continue;
        now = mono_ns();
        for (size_t i = 0; i < fds.size(); i++)
            if (fds[i].revents & POLLIN)
                receive(owners[i], fds[i].fd, now);
    }
    // Send a bye so that the module resets immediately and not after a timeout.
    if (active_) {
        link::Message bye = base("bye");
        send(active_->sock->fd, active_->peer, bye);
    }
}

}
