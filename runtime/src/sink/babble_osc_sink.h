#pragma once

#include "core/threading.h"
#include "sink/osc_sink.h"
#include "sink/sink.h"
#include "sink/steam_link.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace babble {

// Config type "babble_osc" sends OSC in the Baballonia format to a PC, for the Babble module in VRCFaceTracking or
// any other app that reads Baballonia's OSC. The receiver is the host, else this headset for target "frame", else the
// shared PC address, else the Steam Link PC, else this headset for target "auto".
class BabbleOscSink : public ISink {
public:
    BabbleOscSink(const nlohmann::json &cfg, SinkDeps deps);
    ~BabbleOscSink() override;

    bool start() override;
    void stop() override;

    // The destination, where an empty kind means that there is no destination yet.
    struct Status {
        std::string kind, address;   // The kind is "host", "frame", "pc_address" or "steam_link".
        int port = 0;
        uint64_t sent = 0;            // The number of values that the sink sent.
    };
    Status status() const;

    // Finds the destination from the settings and the Steam Link PC, which is public for the tests.
    // The target is "auto", "pc" or "frame".
    static std::optional<std::pair<std::string, std::string>> choose(const std::string &host,
                                                                     const std::string &pc_address,
                                                                     const std::optional<std::string> &steam_link,
                                                                     const std::string &target = "pc");

private:
    void run();
    void refresh();

    std::string name_;
    ThreadSpec thread_spec_;
    std::string host_, pc_address_, target_;
    int port_ = 8888;
    std::shared_ptr<steam_link::SteamLinkPc> steam_link_;
    std::shared_ptr<OscDestination> destination_;
    std::unique_ptr<OscSink> data_;

    std::thread thread_;
    std::atomic<bool> stop_{false};
    mutable std::mutex status_mutex_;
    Status status_;
    bool waiting_ = false;   // True after the log said that there is no destination.
};

}
