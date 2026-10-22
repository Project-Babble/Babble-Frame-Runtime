#include "sink/babble_osc_sink.h"

#include "core/clock.h"
#include "core/log.h"

#include <arpa/inet.h>

namespace {
const babble::log::Logger LOG("babbleosc");
}

namespace babble {

BabbleOscSink::BabbleOscSink(const nlohmann::json &cfg, SinkDeps deps)
    : name_(deps.name), thread_spec_(thread_spec(deps.threads, cfg.value("thread", "osc"), "bbl-bosc")),
      host_(cfg.value("host", "")), pc_address_(cfg.value("pc_address", "")), port_(cfg.value("port", 8888)),
      steam_link_(steam_link::shared(cfg.value("steam_link_log", ""))), destination_(std::make_shared<OscDestination>()) {
    for (std::string *a : {&host_, &pc_address_})
        if (!valid_pc_address(*a)) {
            LOG.warn("%s: ignoring '%s' (not an IPv4 address)", name_.c_str(), a->c_str());
            a->clear();
        }
    data_ = std::make_unique<OscSink>(cfg, std::move(deps), destination_);
}

BabbleOscSink::~BabbleOscSink() { stop(); }

std::optional<std::pair<std::string, std::string>> BabbleOscSink::choose(const std::string &host,
                                                                         const std::string &pc_address,
                                                                         const std::optional<std::string> &steam_link) {
    if (!host.empty())
        return std::make_pair(host, std::string("host"));
    if (!pc_address.empty())
        return std::make_pair(pc_address, std::string("pc_address"));
    if (steam_link)
        return std::make_pair(*steam_link, std::string("steam_link"));
    return std::nullopt;
}

bool BabbleOscSink::start() {
    if (port_ <= 0 || port_ > 65535) {
        LOG.error("%s: port %d is not a UDP port", name_.c_str(), port_);
        return false;
    }
    destination_->clear();
    {
        std::lock_guard lock(status_mutex_);
        status_ = {};
        waiting_ = false;
    }
    if (!data_->start())
        return false;
    stop_ = false;
    thread_ = start_thread(thread_spec_, [this] { run(); });
    LOG.info("%s: sending Baballonia OSC on port %d", name_.c_str(), port_);
    return true;
}

void BabbleOscSink::stop() {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
    data_->stop();
}

BabbleOscSink::Status BabbleOscSink::status() const {
    std::lock_guard lock(status_mutex_);
    Status s = status_;
    s.sent = data_->sent();
    return s;
}

void BabbleOscSink::refresh() {
    const auto pick = choose(host_, pc_address_, host_.empty() && pc_address_.empty() ? steam_link_->live()
                                                                                      : std::nullopt);
    std::lock_guard lock(status_mutex_);
    if (!pick) {
        if (!waiting_)
            LOG.info("%s: no PC address and no Steam Link session; waiting for one", name_.c_str());
        waiting_ = true;
        destination_->clear();
        status_ = {};
        return;
    }
    waiting_ = false;
    if (pick->first == status_.address && pick->second == status_.kind)
        return;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(uint16_t(port_));
    inet_pton(AF_INET, pick->first.c_str(), &to.sin_addr);
    destination_->set(to);
    LOG.info("%s: sending to %s:%d (%s)", name_.c_str(), pick->first.c_str(), port_,
             pick->second == "host" ? "fixed host" : pick->second == "pc_address" ? "PC address" : "Steam Link PC");
    status_.kind = pick->second;
    status_.address = pick->first;
    status_.port = port_;
}

void BabbleOscSink::run() {
    uint64_t next_check = 0;
    while (!stop_) {
        const uint64_t now = mono_ns();
        if (now >= next_check) {   // Look for the Steam Link PC again every few seconds.
            refresh();
            next_check = now + 3'000'000'000ull;
        }
        sleep_ns(200'000'000);
    }
}

}
