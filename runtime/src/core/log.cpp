#include "core/log.h"

#include "core/clock.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <pthread.h>
#include <unistd.h>

namespace babble::log {

namespace {

std::atomic<Level> g_level{Level::Info};
std::mutex g_mutex;
const uint64_t g_t0 = mono_ns();

thread_local char t_name[16] = {0};

const char *thread_name() {
    if (!t_name[0]) {
        pthread_getname_np(pthread_self(), t_name, sizeof(t_name));
        if (!t_name[0])
            snprintf(t_name, sizeof(t_name), "tid%ld", long(gettid()));
    }
    return t_name;
}

char level_char(Level l) {
    switch (l) {
    case Level::Debug: return 'D';
    case Level::Info: return 'I';
    case Level::Warn: return 'W';
    case Level::Error: return 'E';
    }
    return '?';
}

} // namespace

void set_level(Level level) { g_level.store(level, std::memory_order_relaxed); }
Level level() { return g_level.load(std::memory_order_relaxed); }

bool parse_level(const char *name, Level *out) {
    if (!strcmp(name, "debug")) *out = Level::Debug;
    else if (!strcmp(name, "info")) *out = Level::Info;
    else if (!strcmp(name, "warn") || !strcmp(name, "warning")) *out = Level::Warn;
    else if (!strcmp(name, "error")) *out = Level::Error;
    else return false;
    return true;
}

void refresh_thread_name() {
    t_name[0] = 0;
    thread_name();
}

void write(Level lvl, const char *fmt, ...) {
    if (lvl < level())
        return;
    char msg[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    double t = double(mono_ns() - g_t0) / 1e9;
    const char *name = thread_name();
    std::lock_guard<std::mutex> lock(g_mutex);
    fprintf(stderr, "[%11.6f] %c %-9s %s\n", t, level_char(lvl), name, msg);
}

} // namespace babble::log
