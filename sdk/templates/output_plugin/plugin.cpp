// Template output plugin: "udp_json".
//
// Sends the tracked values that changed, as one JSON line per update, to a
// UDP port:  {"seq":123,"values":{"jawOpen":0.25,"LeftEyeLid":0.97}}
//
// It is small on purpose: it shows the whole plugin contract (the ABI in
// sdk/include/babble/output_plugin.h) and is a working starting point.
// Replace send_update() with your own output.
//
// Config (the "config" object of the sinks entry):
//   "host": "127.0.0.1", "port": 9500, "max_rate_hz": 0 (0 = every update)
#include <babble/output_plugin.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

// Minimal config reading for the template's two flat keys. A real plugin
// would use a JSON library (the runtime uses nlohmann/json).
std::string json_string(const std::string &json, const char *key, const std::string &fallback) {
    const std::string k = std::string("\"") + key + "\"";
    auto p = json.find(k);
    if (p == std::string::npos || (p = json.find('"', json.find(':', p) + 1)) == std::string::npos)
        return fallback;
    auto e = json.find('"', p + 1);
    return e == std::string::npos ? fallback : json.substr(p + 1, e - p - 1);
}

double json_number(const std::string &json, const char *key, double fallback) {
    const std::string k = std::string("\"") + key + "\"";
    auto p = json.find(k);
    if (p == std::string::npos)
        return fallback;
    p = json.find(':', p);
    return p == std::string::npos ? fallback : std::strtod(json.c_str() + p + 1, nullptr);
}

struct Plugin {
    const babble_host_v1 *host = nullptr;
    std::string instance;
    sockaddr_in target{};
    int sock = -1;
    double max_rate_hz = 0;

    std::thread thread;
    std::atomic<bool> stop{false};

    void log(int level, const std::string &msg) const { host->log(host->ctx, level, msg.c_str()); }

    // The plugin's thread: wait for new values, send what changed.
    void run() {
        uint64_t seq = 0;
        std::vector<uint32_t> ids;
        std::vector<float> values, last;
        std::vector<std::string> names;
        auto last_send = std::chrono::steady_clock::time_point{};
        while (!stop) {
            seq = host->wait_update(host->ctx, seq, 200);   // wakes on each update, or every 200 ms
            if (stop)
                break;
            // Signals are registered by the runtime's pipelines as they start:
            // pick up new ones (ids only ever grow).
            const uint32_t n = host->signal_count(host->ctx);
            while (ids.size() < n) {
                const uint32_t id = uint32_t(ids.size());
                ids.push_back(id);
                const char *name = host->signal_name(host->ctx, id);
                names.emplace_back(name ? name : "");
                last.push_back(NAN);   // forces a first send
            }
            if (ids.empty())
                continue;
            if (max_rate_hz > 0 && std::chrono::steady_clock::now() - last_send <
                                       std::chrono::duration<double>(1.0 / max_rate_hz))
                continue;
            values.resize(ids.size());
            const uint64_t at = host->read(host->ctx, ids.data(), uint32_t(ids.size()), values.data(), nullptr);
            send_update(at, names, values, last);
            last_send = std::chrono::steady_clock::now();
        }
    }

    void send_update(uint64_t at, const std::vector<std::string> &names, const std::vector<float> &values,
                     std::vector<float> &last) {
        std::string line = "{\"seq\":" + std::to_string(at) + ",\"values\":{";
        bool any = false;
        char num[32];
        for (size_t i = 0; i < values.size(); i++) {
            if (values[i] == last[i])
                continue;
            last[i] = values[i];
            std::snprintf(num, sizeof(num), "%.6g", double(values[i]));
            line += (any ? ",\"" : "\"") + names[i] + "\":" + num;
            any = true;
        }
        if (!any)
            return;
        line += "}}\n";
        sendto(sock, line.data(), line.size(), MSG_DONTWAIT, reinterpret_cast<const sockaddr *>(&target),
               sizeof(target));
    }
};

void *create(const babble_host_v1 *host, const char *instance, const char *config_json) {
    auto *p = new Plugin;
    p->host = host;
    p->instance = instance;
    const std::string cfg = config_json ? config_json : "{}";
    const std::string ip = json_string(cfg, "host", "127.0.0.1");
    const int port = int(json_number(cfg, "port", 9500));
    p->max_rate_hz = json_number(cfg, "max_rate_hz", 0);
    p->target.sin_family = AF_INET;
    p->target.sin_port = htons(uint16_t(port));
    if (inet_pton(AF_INET, ip.c_str(), &p->target.sin_addr) != 1) {
        p->log(BABBLE_LOG_ERROR, "bad host " + ip);
        delete p;
        return nullptr;
    }
    return p;
}

int start(void *self) {
    auto *p = static_cast<Plugin *>(self);
    p->sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (p->sock < 0)
        return -1;
    p->stop = false;
    p->thread = std::thread([p] { p->run(); });
    char buf[64];
    inet_ntop(AF_INET, &p->target.sin_addr, buf, sizeof(buf));
    p->log(BABBLE_LOG_INFO, std::string("sending JSON to ") + buf + ":" + std::to_string(ntohs(p->target.sin_port)));
    return 0;
}

void stop(void *self) {
    auto *p = static_cast<Plugin *>(self);
    p->stop = true;   // the thread notices within one wait_update timeout (200 ms)
    if (p->thread.joinable())
        p->thread.join();
    if (p->sock >= 0)
        close(p->sock);
    p->sock = -1;
}

void destroy(void *self) { delete static_cast<Plugin *>(self); }

const babble_output_v1 kPlugin = {BABBLE_OUTPUT_ABI, "udp_json", "1.0", create, start, stop, destroy};

} // namespace

extern "C" BABBLE_PLUGIN_EXPORT const babble_output_v1 *babble_output_plugin_v1(void) { return &kPlugin; }
