#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace babble {

// A running total, which set() changes into a level, for example a rate in fps.
class Counter {
public:
    void add(uint64_t n = 1) { v_.fetch_add(n, std::memory_order_relaxed); }
    void set(uint64_t n) { v_.store(n, std::memory_order_relaxed); }
    uint64_t get() const { return v_.load(std::memory_order_relaxed); }

private:
    std::atomic<uint64_t> v_{0};
};

// The number of events per second between snapshots.
class RateStat {
public:
    void tick(uint64_t n = 1) { total_.fetch_add(n, std::memory_order_relaxed); }
    double snapshot_hz();   // This resets the window.
    uint64_t total() const { return total_.load(std::memory_order_relaxed); }   // The total never resets.

private:
    std::atomic<uint64_t> total_{0};
    uint64_t last_total_ = 0;
    uint64_t last_ns_ = 0;
};

// Keeps the last kWindow duration samples in nanoseconds for percentiles.
class LatencyStat {
public:
    struct Snapshot {
        uint64_t count = 0;
        double mean_us = 0, p50_us = 0, p99_us = 0, max_us = 0;
    };
    void add(uint64_t ns);
    Snapshot snapshot_and_reset();

private:
    static constexpr size_t kWindow = 512;
    std::mutex mutex_;
    std::vector<uint64_t> samples_;
    uint64_t count_ = 0;
    uint64_t sum_ = 0;
    uint64_t max_ = 0;
};

// Named stats as one JSON line per interval, where each owner must stay alive while its raw pointer is registered.
class StatsRegistry {
public:
    void add(const std::string &name, Counter *c);
    void add(const std::string &name, RateStat *r);
    void add(const std::string &name, LatencyStat *l);
    void remove_prefix(const std::string &prefix);
    std::string render();

private:
    struct Entry {
        std::string name;
        Counter *counter = nullptr;
        RateStat *rate = nullptr;
        LatencyStat *latency = nullptr;
    };
    std::mutex mutex_;
    std::vector<Entry> entries_;
};

}
