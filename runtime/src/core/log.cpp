#include "core/log.h"

#include "core/clock.h"

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>

#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

namespace babble::log {

namespace {

// The modules are in a fixed table so that a level check is an array read.
constexpr int kMaxModules = 96;
struct ModuleSlot {
    char name[32] = {0};
    std::atomic<int> level{-1};   // The value -1 means the default level.
};

struct State {
    std::mutex mutex;              // This protects the output, the repeats, the ring and the registration.
    std::atomic<Level> level{Level::Info};
    std::atomic<uint64_t> verbose_until{0};
    std::array<ModuleSlot, kMaxModules> modules;
    std::atomic<int> module_count{0};
    std::map<std::string, Level, std::less<>> pending;   // The levels for modules that are not registered yet.
    RotatingFile file;
    bool file_open = false;
    std::atomic<bool> mirror{isatty(2) == 1};
    // Identical lines that repeat become one "repeated N times" line.
    std::string last_key;
    const char *last_module = "";
    Level last_level = Level::Info;
    unsigned long long repeats = 0;
    // The last lines have a fixed size so that the crash handler can read them without a lock.
    static constexpr int kRing = 256, kRingLine = 400;
    char ring[kRing][kRingLine] = {{0}};
    std::atomic<unsigned> ring_next{0};
};

State &st() {
    static State *s = new State;   // Never destroy the state, so that the log works until the end.
    return *s;
}

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

int register_module(const char *module) {
    State &s = st();
    std::lock_guard lock(s.mutex);
    const int n = s.module_count.load();
    for (int i = 0; i < n; i++)
        if (!strcmp(s.modules[size_t(i)].name, module))
            return i;
    if (n >= kMaxModules)
        return -1;   // The table is full, so this module uses the default level.
    ModuleSlot &slot = s.modules[size_t(n)];
    snprintf(slot.name, sizeof(slot.name), "%s", module);
    if (auto it = s.pending.find(std::string_view(module)); it != s.pending.end())
        slot.level = int(it->second);
    s.module_count = n + 1;
    return n;
}

// Call this only with the mutex held.
void emit(State &s, const char *line, size_t len) {
    if (s.file_open)
        s.file.write(std::string_view(line, len));
    if (!s.file_open || s.mirror.load(std::memory_order_relaxed))
        fwrite(line, 1, len, stderr);
    const unsigned i = s.ring_next.fetch_add(1) % State::kRing;
    const size_t n = std::min(len, size_t(State::kRingLine - 1));
    memcpy(s.ring[i], line, n);
    s.ring[i][n] = 0;
}

void write_line(const char *module, Level lvl, const char *fmt, va_list args) {
    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, args);

    timespec wall{};
    clock_gettime(CLOCK_REALTIME, &wall);
    tm local{};
    localtime_r(&wall.tv_sec, &local);
    char when[32];
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &local);
    const double t = double(mono_ns() - g_t0) / 1e9;
    const char *thread = thread_name();

    State &s = st();
    std::lock_guard lock(s.mutex);
    // Count a line again if it has the same module, level and text.
    std::string key = std::string(module) + '\x1f' + level_char(lvl) + '\x1f' + msg;
    if (key == s.last_key) {
        s.repeats++;
        return;
    }
    char line[1280];
    if (s.repeats) {
        const int n = snprintf(line, sizeof(line), "%s.%03ld [%11.6f] %c %-10s %-9s (last message repeated %llu times)\n",
                               when, wall.tv_nsec / 1000000, t, level_char(s.last_level), s.last_module, thread,
                               s.repeats);
        emit(s, line, size_t(std::min(n, int(sizeof(line)) - 1)));
        s.repeats = 0;
    }
    s.last_key = std::move(key);
    s.last_module = module;
    s.last_level = lvl;
    const int n = snprintf(line, sizeof(line), "%s.%03ld [%11.6f] %c %-10s %-9s %s\n", when, wall.tv_nsec / 1000000,
                           t, level_char(lvl), module, thread, msg);
    emit(s, line, size_t(std::min(n, int(sizeof(line)) - 1)));
}

}

void set_level(Level level) { st().level.store(level, std::memory_order_relaxed); }
Level level() { return st().level.load(std::memory_order_relaxed); }

bool parse_level(const char *name, Level *out) {
    if (!strcmp(name, "debug")) *out = Level::Debug;
    else if (!strcmp(name, "info")) *out = Level::Info;
    else if (!strcmp(name, "warn") || !strcmp(name, "warning")) *out = Level::Warn;
    else if (!strcmp(name, "error")) *out = Level::Error;
    else return false;
    return true;
}

const char *level_name(Level l) {
    switch (l) {
    case Level::Debug: return "debug";
    case Level::Info: return "info";
    case Level::Warn: return "warn";
    case Level::Error: return "error";
    }
    return "?";
}

void set_module_level(std::string_view module, std::optional<Level> level) {
    State &s = st();
    std::lock_guard lock(s.mutex);
    if (level)
        s.pending[std::string(module)] = *level;
    else if (auto it = s.pending.find(module); it != s.pending.end())
        s.pending.erase(it);
    const int n = s.module_count.load();
    for (int i = 0; i < n; i++)
        if (module == s.modules[size_t(i)].name)
            s.modules[size_t(i)].level = level ? int(*level) : -1;
}

void clear_module_levels() {
    State &s = st();
    std::lock_guard lock(s.mutex);
    s.pending.clear();
    const int n = s.module_count.load();
    for (int i = 0; i < n; i++)
        s.modules[size_t(i)].level = -1;
}

void verbose_until(uint64_t t) { st().verbose_until.store(t); }
uint64_t verbose_until() { return st().verbose_until.load(); }

bool open_file(const std::filesystem::path &path, size_t max_bytes, int keep) {
    State &s = st();
    std::lock_guard lock(s.mutex);
    s.file_open = s.file.open(path, max_bytes, keep);
    return s.file_open;
}

void close_file() {
    State &s = st();
    std::lock_guard lock(s.mutex);
    s.file.close();
    s.file_open = false;
}

std::filesystem::path file_path() {
    State &s = st();
    std::lock_guard lock(s.mutex);
    return s.file_open ? s.file.path() : std::filesystem::path();
}

void mirror_stderr(bool on) { st().mirror = on; }

std::vector<std::string> recent(size_t n) {
    State &s = st();
    std::lock_guard lock(s.mutex);
    std::vector<std::string> out;
    const unsigned next = s.ring_next.load();
    n = std::min<size_t>(n, std::min<unsigned>(next, State::kRing));
    for (size_t k = n; k > 0; k--)
        out.emplace_back(s.ring[(next - k) % State::kRing]);
    return out;
}

void dump_recent(int fd) {
    State &s = st();
    const unsigned next = s.ring_next.load();
    const unsigned n = std::min<unsigned>(next, State::kRing);
    for (unsigned k = n; k > 0; k--) {
        const char *line = s.ring[(next - k) % State::kRing];
        (void)!::write(fd, line, strnlen(line, State::kRingLine));
    }
}

void write_raw_signal_safe(const char *text) {
    State &s = st();
    const size_t n = strlen(text);
    if (s.file_open && s.file.fd() >= 0)
        (void)!::write(s.file.fd(), text, n);
    (void)!::write(2, text, n);
}

int file_fd() {
    State &s = st();
    return s.file_open ? s.file.fd() : -1;
}

void refresh_thread_name() {
    t_name[0] = 0;
    thread_name();
}

Logger::Logger(const char *module) : module_(module), slot_(register_module(module)) {}

bool Logger::enabled(Level l) const {
    State &s = st();
    const uint64_t verbose = s.verbose_until.load(std::memory_order_relaxed);
    if (verbose && mono_ns() < verbose)
        return true;
    int lvl = slot_ >= 0 ? s.modules[size_t(slot_)].level.load(std::memory_order_relaxed) : -1;
    if (lvl < 0)
        lvl = int(s.level.load(std::memory_order_relaxed));
    return int(l) >= lvl;
}

#define BABBLE_LOG_BODY(lvl)                                                                                       \
    if (!enabled(lvl))                                                                                             \
        return;                                                                                                    \
    va_list args;                                                                                                  \
    va_start(args, fmt);                                                                                           \
    write_line(module_, lvl, fmt, args);                                                                           \
    va_end(args)

void Logger::debug(const char *fmt, ...) const { BABBLE_LOG_BODY(Level::Debug); }
void Logger::info(const char *fmt, ...) const { BABBLE_LOG_BODY(Level::Info); }
void Logger::warn(const char *fmt, ...) const { BABBLE_LOG_BODY(Level::Warn); }
void Logger::error(const char *fmt, ...) const { BABBLE_LOG_BODY(Level::Error); }
void Logger::write(Level l, const char *fmt, ...) const { BABBLE_LOG_BODY(l); }

#undef BABBLE_LOG_BODY

const Logger &main_logger() {
    static const Logger l("main");
    return l;
}

bool Every::operator()() {
    const uint64_t now = mono_ns();
    uint64_t next = next_ns_.load();
    if (now >= next && next_ns_.compare_exchange_strong(next, now + period_ns_)) {
        suppressed_shown_ = held_.exchange(0);
        return true;
    }
    held_++;
    return false;
}

RotatingFile::~RotatingFile() { close(); }

void RotatingFile::close() {
    if (fd_ >= 0)
        ::close(fd_);
    fd_ = -1;
    size_ = 0;
}

bool RotatingFile::open(const std::filesystem::path &path, size_t max_bytes, int keep) {
    if (fd_ >= 0)
        ::close(fd_);
    path_ = path;
    max_bytes_ = max_bytes;
    keep_ = std::max(1, keep);
    fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd_ < 0)
        return false;
    struct stat sb{};
    size_ = fstat(fd_, &sb) == 0 ? size_t(sb.st_size) : 0;
    if (max_bytes_ && size_ >= max_bytes_)
        rotate();
    return fd_ >= 0;
}

void RotatingFile::rotate() {
    ::close(fd_);
    std::error_code ec;
    for (int i = keep_ - 1; i >= 1; i--)
        std::filesystem::rename(path_.string() + "." + std::to_string(i), path_.string() + "." + std::to_string(i + 1),
                                ec);
    std::filesystem::rename(path_, path_.string() + ".1", ec);
    fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_TRUNC, 0644);
    size_ = 0;
}

void RotatingFile::write(std::string_view text) {
    if (fd_ < 0)
        return;
    const ssize_t n = ::write(fd_, text.data(), text.size());
    if (n > 0)
        size_ += size_t(n);
    if (max_bytes_ && size_ >= max_bytes_)
        rotate();
}

}
