#include "core/threading.h"

#include "core/log.h"

#include <cerrno>
#include <cstring>

#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>

namespace {
const babble::log::Logger LOG("core");
}

namespace babble {

ThreadSpec thread_spec(const nlohmann::json &threads_cfg, const std::string &key, const char *default_name) {
    ThreadSpec spec;
    spec.name = default_name;
    if (!threads_cfg.is_object() || !threads_cfg.contains(key))
        return spec;
    const auto &t = threads_cfg[key];
    if (t.contains("name") && t["name"].is_string())
        spec.name = t["name"].get<std::string>();
    if (t.contains("cpus") && t["cpus"].is_array())
        for (const auto &c : t["cpus"])
            if (c.is_number_integer())
                spec.cpus.push_back(c.get<int>());
    if (t.contains("nice") && t["nice"].is_number_integer())
        spec.nice = t["nice"].get<int>();
    return spec;
}

void apply_thread_spec(const ThreadSpec &spec) {
    std::string name = spec.name.substr(0, 15);
    pthread_setname_np(pthread_self(), name.c_str());
    log::refresh_thread_name();

    if (!spec.cpus.empty()) {
        cpu_set_t set;
        CPU_ZERO(&set);
        for (int c : spec.cpus)
            if (c >= 0 && c < CPU_SETSIZE)
                CPU_SET(c, &set);
        if (sched_setaffinity(0, sizeof(set), &set) != 0)
            LOG.warn("%s: sched_setaffinity failed: %s", name.c_str(), strerror(errno));
    }
    if (spec.nice < 0) {
        // The SteamVR service gives a hard RLIMIT_NICE above the soft one, so no privilege is necessary.
        rlimit rl{};
        if (getrlimit(RLIMIT_NICE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
            rl.rlim_cur = rl.rlim_max;
            setrlimit(RLIMIT_NICE, &rl);
        }
    }
    if (spec.nice != 0) {
        // PRIO_PROCESS with the kernel tid changes the nice value of only this thread.
        if (setpriority(PRIO_PROCESS, gettid(), spec.nice) != 0)
            LOG.warn("%s: setpriority(%d) refused: %s", name.c_str(), spec.nice, strerror(errno));
    }
}

std::thread start_thread(ThreadSpec spec, std::function<void()> fn) {
    return std::thread([spec = std::move(spec), fn = std::move(fn)] {
        apply_thread_spec(spec);
        fn();
    });
}

}
