#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace babble {

struct ThreadSpec {
    std::string name;       // pthread name, at most 15 characters
    std::vector<int> cpus;  // empty = inherit the process mask
    int nice = 0;
};

// Reads config["threads"][key]; missing entries yield the default name with no pinning.
ThreadSpec thread_spec(const nlohmann::json &threads_cfg, const std::string &key, const char *default_name);

// Applies name, affinity and nice to the calling thread. Failures are logged, never fatal.
void apply_thread_spec(const ThreadSpec &spec);

// Starts a thread that applies the spec before running fn.
std::thread start_thread(ThreadSpec spec, std::function<void()> fn);

} // namespace babble
