#include "core/metrics.h"

#include "core/clock.h"

#include <algorithm>
#include <cstdio>

namespace babble {

double RateStat::snapshot_hz() {
    uint64_t now = mono_ns();
    uint64_t total = total_.load(std::memory_order_relaxed);
    uint64_t n = total - last_total_;
    last_total_ = total;
    double hz = 0;
    if (last_ns_ != 0 && now > last_ns_)
        hz = double(n) * 1e9 / double(now - last_ns_);
    last_ns_ = now;
    return hz;
}

void LatencyStat::add(uint64_t ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (samples_.size() < kWindow)
        samples_.push_back(ns);
    else
        samples_[count_ % kWindow] = ns;
    count_++;
    sum_ += ns;
    max_ = std::max(max_, ns);
}

LatencyStat::Snapshot LatencyStat::snapshot_and_reset() {
    Snapshot s;
    std::vector<uint64_t> samples;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        s.count = count_;
        if (count_)
            s.mean_us = double(sum_) / double(count_) / 1e3;
        s.max_us = double(max_) / 1e3;
        samples.swap(samples_);
        count_ = 0;
        sum_ = 0;
        max_ = 0;
    }
    if (!samples.empty()) {
        std::sort(samples.begin(), samples.end());
        s.p50_us = double(samples[samples.size() / 2]) / 1e3;
        s.p99_us = double(samples[std::min(samples.size() - 1, samples.size() * 99 / 100)]) / 1e3;
    }
    return s;
}

void StatsRegistry::add(const std::string &name, Counter *c) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.push_back({name, c, nullptr, nullptr});
}
void StatsRegistry::add(const std::string &name, RateStat *r) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.push_back({name, nullptr, r, nullptr});
}
void StatsRegistry::add(const std::string &name, LatencyStat *l) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.push_back({name, nullptr, nullptr, l});
}

void StatsRegistry::remove_prefix(const std::string &prefix) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&](const Entry &e) { return e.name.rfind(prefix, 0) == 0; }),
                   entries_.end());
}

std::string StatsRegistry::render() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string out = "{";
    char buf[160];
    bool first = true;
    for (auto &e : entries_) {
        if (!first)
            out += ", ";
        first = false;
        if (e.counter) {
            snprintf(buf, sizeof(buf), "\"%s\": %llu", e.name.c_str(), (unsigned long long)e.counter->get());
        } else if (e.rate) {
            snprintf(buf, sizeof(buf), "\"%s\": %.1f", e.name.c_str(), e.rate->snapshot_hz());
        } else {
            auto s = e.latency->snapshot_and_reset();
            snprintf(buf, sizeof(buf), "\"%s\": {\"n\": %llu, \"mean_us\": %.0f, \"p50_us\": %.0f, \"p99_us\": %.0f, \"max_us\": %.0f}",
                     e.name.c_str(), (unsigned long long)s.count, s.mean_us, s.p50_us, s.p99_us, s.max_us);
        }
        out += buf;
    }
    out += "}";
    return out;
}

} // namespace babble
