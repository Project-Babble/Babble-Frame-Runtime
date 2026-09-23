#pragma once

// Minimal logger. Writes to stderr, which the driver shim redirects to
// $XDG_STATE_HOME/babble-frame-runtime/runtime.log. Each line carries the
// monotonic time since start, the level and the calling thread's name.

namespace babble::log {

enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3 };

void set_level(Level level);
Level level();
bool parse_level(const char *name, Level *out);

// Re-reads the thread name after pthread_setname_np on the current thread.
void refresh_thread_name();

void write(Level level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

} // namespace babble::log

#define LOG_DEBUG(...) ::babble::log::write(::babble::log::Level::Debug, __VA_ARGS__)
#define LOG_INFO(...) ::babble::log::write(::babble::log::Level::Info, __VA_ARGS__)
#define LOG_WARN(...) ::babble::log::write(::babble::log::Level::Warn, __VA_ARGS__)
#define LOG_ERROR(...) ::babble::log::write(::babble::log::Level::Error, __VA_ARGS__)
