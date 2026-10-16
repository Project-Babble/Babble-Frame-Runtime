#pragma once

// The runtime logger, which writes to runtime.log and keeps the last lines in memory for crash reports.

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

void set_level(Level level);   // Sets the default for modules that have no level of their own.
Level level();
bool parse_level(const char *name, Level *out);
const char *level_name(Level l);

// Sets the level of one module, and nullopt sets the module back to the default.
void set_module_level(std::string_view module, std::optional<Level> level);
void clear_module_levels();
// Sets every module to Debug until the given CLOCK_MONOTONIC time, where 0 means off.
void verbose_until(uint64_t mono_ns);
uint64_t verbose_until();

// Writes the log to `path` and rotates it at max_bytes, or returns false if it cannot open the file.
bool open_file(const std::filesystem::path &path, size_t max_bytes = 8u << 20, int keep = 3);
std::filesystem::path file_path();
void close_file();   // The tests use this to go back to stderr only.
// Copies every line to stderr, which is the default when stderr is a terminal.
void mirror_stderr(bool on);

// Returns the last n lines with the oldest first.
std::vector<std::string> recent(size_t n);
// Writes the kept lines to fd, and is async-signal-safe for the crash handler.
void dump_recent(int fd);
// Writes raw text to the log file and stderr, and is async-signal-safe.
void write_raw_signal_safe(const char *text);
// Returns the descriptor of the log file for the crash handler, or -1 if the file is not open.
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
    int slot_;   // The index into the module table.
};

// A rate limit for a message that can repeat at the frame rate.
class Every {
public:
    explicit Every(uint64_t period_ns) : period_ns_(period_ns) {}
    bool operator()();
    // The number of messages that it held back before the last message that it let through.
    unsigned long long suppressed() const { return suppressed_shown_; }

private:
    uint64_t period_ns_;
    std::atomic<uint64_t> next_ns_{0};
    std::atomic<unsigned long long> held_{0};
    unsigned long long suppressed_shown_ = 0;
};

// An append-only file that rotates at a maximum size.
class RotatingFile {
public:
    RotatingFile() = default;
    RotatingFile(const RotatingFile &) = delete;
    RotatingFile &operator=(const RotatingFile &) = delete;
    ~RotatingFile();
    bool open(const std::filesystem::path &path, size_t max_bytes, int keep);
    void close();
    void write(std::string_view text);   // This is not thread-safe, so the callers must serialize their calls.
    int fd() const { return fd_; }
    const std::filesystem::path &path() const { return path_; }

private:
    void rotate();
    std::filesystem::path path_;
    size_t max_bytes_ = 0, size_ = 0;
    int keep_ = 3, fd_ = -1;
};

}

// These macros use the "main" module for code that has no Logger of its own.
#define LOG_DEBUG(...) ::babble::log::main_logger().debug(__VA_ARGS__)
#define LOG_INFO(...) ::babble::log::main_logger().info(__VA_ARGS__)
#define LOG_WARN(...) ::babble::log::main_logger().warn(__VA_ARGS__)
#define LOG_ERROR(...) ::babble::log::main_logger().error(__VA_ARGS__)

namespace babble::log {
const Logger &main_logger();
}
