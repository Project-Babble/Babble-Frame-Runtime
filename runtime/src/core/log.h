#pragma once

// The runtime's logger.
//
// Each line:
//   2026-09-27 18:43:02.123 [  512.345678] I usb        bbl-face  message
// wall-clock time (to line up with SteamVR's logs and tester reports), the
// time since start, the level, the module and the calling thread's name.
//
// Lines go to runtime.log in the state dir (open_file), which the runtime
// rotates itself, and to stderr until the file is open or when stderr is a
// terminal. The last lines are also kept in memory for crash reports and
// diagnostics files (recent()).
//
// Each source file logs through its own module:
//   namespace { const babble::log::Logger LOG("usb"); }
//   LOG.info("streaming %s", name);
// Levels are global (set_level) with per-module overrides (set_module_level,
// config "log_levels"), and can be raised for a while (verbose_for).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace babble::log {

enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3 };

void set_level(Level level);   // the default for modules without their own level
Level level();
bool parse_level(const char *name, Level *out);
const char *level_name(Level l);

// Per-module level; nullopt goes back to the default. Unknown modules are
// remembered and apply when the module first logs.
void set_module_level(std::string_view module, std::optional<Level> level);
void clear_module_levels();
// Every module at Debug until the given CLOCK_MONOTONIC time (0 = off).
void verbose_until(uint64_t mono_ns);
uint64_t verbose_until();

// Writes the log to `path` from now on, rotating it when it passes max_bytes
// (path.1 ... path.<keep>). False (and stays on stderr) if it can't be opened.
bool open_file(const std::filesystem::path &path, size_t max_bytes = 8u << 20, int keep = 3);
std::filesystem::path file_path();
void close_file();   // back to stderr only (tests)
// Also copy every line to stderr (default: when stderr is a terminal).
void mirror_stderr(bool on);

// The last n lines, oldest first (for diagnostics files).
std::vector<std::string> recent(size_t n);
// Async-signal-safe: writes the kept lines to fd (for the crash handler).
void dump_recent(int fd);
// Async-signal-safe raw write to the log file and stderr.
void write_raw_signal_safe(const char *text);
// The log file's descriptor (-1 if not open), for the crash handler.
int file_fd();

// Re-reads the thread name after pthread_setname_np on the current thread.
void refresh_thread_name();

class Logger {
public:
    explicit Logger(const char *module);
    const char *module() const { return module_; }
    bool enabled(Level l) const;
    bool debug_enabled() const { return enabled(Level::Debug); }

    void debug(const char *fmt, ...) const __attribute__((format(printf, 2, 3)));
    void info(const char *fmt, ...) const __attribute__((format(printf, 2, 3)));
    void warn(const char *fmt, ...) const __attribute__((format(printf, 2, 3)));
    void error(const char *fmt, ...) const __attribute__((format(printf, 2, 3)));
    void write(Level l, const char *fmt, ...) const __attribute__((format(printf, 3, 4)));

private:
    const char *module_;
    int slot_;   // index into the module table
};

// Rate limit for a message that can repeat at frame rate:
//   static log::Every every(5'000'000'000);
//   if (every()) LOG.warn("send failed (%llu more since)", every.suppressed());
class Every {
public:
    explicit Every(uint64_t period_ns) : period_ns_(period_ns) {}
    bool operator()();
    // How many were held back before the last one allowed through.
    unsigned long long suppressed() const { return suppressed_shown_; }

private:
    uint64_t period_ns_;
    std::atomic<uint64_t> next_ns_{0};
    std::atomic<unsigned long long> held_{0};
    unsigned long long suppressed_shown_ = 0;
};

// A size-rotated append-only file (the log, stats.jsonl).
class RotatingFile {
public:
    RotatingFile() = default;
    RotatingFile(const RotatingFile &) = delete;
    RotatingFile &operator=(const RotatingFile &) = delete;
    ~RotatingFile();
    bool open(const std::filesystem::path &path, size_t max_bytes, int keep);
    void close();
    void write(std::string_view text);   // not thread-safe: callers serialize
    int fd() const { return fd_; }
    const std::filesystem::path &path() const { return path_; }

private:
    void rotate();
    std::filesystem::path path_;
    size_t max_bytes_ = 0, size_ = 0;
    int keep_ = 3, fd_ = -1;
};

} // namespace babble::log

// Module "main", for code that hasn't got its own Logger (and tests/tools).
#define LOG_DEBUG(...) ::babble::log::main_logger().debug(__VA_ARGS__)
#define LOG_INFO(...) ::babble::log::main_logger().info(__VA_ARGS__)
#define LOG_WARN(...) ::babble::log::main_logger().warn(__VA_ARGS__)
#define LOG_ERROR(...) ::babble::log::main_logger().error(__VA_ARGS__)

namespace babble::log {
const Logger &main_logger();
}
