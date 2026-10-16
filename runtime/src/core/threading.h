#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace babble {

struct ThreadSpec {
    std::string name;       // The pthread name, with a maximum of 15 characters.
    std::vector<int> cpus;  // An empty list keeps the process mask.
    int nice = 0;
};

// Reads config["threads"][key], or gives the default name with no pinning if the entry is missing.
ThreadSpec thread_spec(const nlohmann::json &threads_cfg, const std::string &key, const char *default_name);

// Applies the name, affinity and nice to the calling thread, and only logs a failure.
void apply_thread_spec(const ThreadSpec &spec);

// Starts a thread that applies the spec before it runs fn.
std::thread start_thread(ThreadSpec spec, std::function<void()> fn);

}
