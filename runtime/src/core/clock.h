#pragma once

#include <cstdint>
#include <ctime>
#include <sys/time.h>

namespace babble {

// All runtime timestamps are CLOCK_MONOTONIC nanoseconds.
inline uint64_t mono_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

// Returns the CLOCK_REALTIME time for records that must have a date.
inline uint64_t wall_ns() {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

inline uint64_t mono_raw_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

inline uint64_t timeval_ns(const timeval &tv) {
    return uint64_t(tv.tv_sec) * 1000000000ull + uint64_t(tv.tv_usec) * 1000ull;
}

inline uint64_t timespec_ns(const timespec &ts) {
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

inline void sleep_ns(uint64_t ns) {
    timespec ts{time_t(ns / 1000000000ull), long(ns % 1000000000ull)};
    while (clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, &ts) != 0) {
    }
}

}
