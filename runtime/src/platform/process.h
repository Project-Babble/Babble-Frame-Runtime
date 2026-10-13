#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <sys/types.h>

namespace babble {

// A child process the runtime drives: the recorder's encoder, curl, tar.
// Started with posix_spawn like AppLauncher (clean signal mask and
// dispositions, fd 3 closed), but in the caller's session, and with optional
// pipes. The child inherits the calling thread's CPU pinning and nice value,
// so spawn it from a thread with the ThreadSpec it should have. Not
// thread-safe; one owner thread.
class Process {
public:
    struct Options {
        bool stdin_pipe = false;     // write with write_stdin(); otherwise /dev/null
        bool stdout_pipe = false;    // read with read_stdout(); otherwise stdout_file or /dev/null
        bool stderr_pipe = false;    // read with read_stderr(); otherwise stderr_file or /dev/null
        std::string stdout_file;     // appended to
        std::string stderr_file;     // appended to
        std::map<std::string, std::string> env;   // added to (or replacing) the runtime's environment
    };

    Process() = default;
    ~Process();   // terminates a child still running (1 s grace)
    Process(const Process &) = delete;
    Process &operator=(const Process &) = delete;

    // argv[0] is looked up on PATH when it has no slash. False (with *err) if
    // it can't be started.
    bool spawn(const std::vector<std::string> &argv, const Options &opt, std::string *err = nullptr);

    pid_t pid() const { return pid_; }
    bool running();   // reaps the child once it exits

    // Writes all of data to the child's stdin, waiting up to timeout_ms in
    // total for room. False on timeout, a closed pipe (the child exited) or no pipe.
    bool write_stdin(const void *data, size_t n, int timeout_ms);
    void close_stdin();

    // Whatever is available now, without waiting ("" when nothing is).
    std::string read_stdout();
    // Exactly n bytes of stdout, waiting up to timeout_ms between bytes
    // arriving. 1 = read, 0 = clean end of output before any byte, -1 = a
    // short read, a timeout or no pipe.
    int read_stdout_exact(void *buf, size_t n, int timeout_ms);
    std::string read_stderr();

    // Waits for the child to exit, up to timeout_ms (-1 = forever). True if it did.
    bool wait(int timeout_ms);
    // SIGTERM, then SIGKILL after grace_ms if it's still there; reaps it.
    void terminate(int grace_ms);

    // The exit code once reaped; nullopt while running, or if a signal ended it.
    std::optional<int> exit_code() const { return exit_code_; }
    int exit_signal() const { return exit_signal_; }

    // The full path of an executable on PATH (or the argument itself if it has
    // a slash and is executable); nullopt if there's none.
    static std::optional<std::string> find_in_path(const std::string &name);

    // Runs argv to completion: stdout captured (up to max_out bytes), stderr
    // into *err_out. Exit code, or -1 if it couldn't start or timed out (killed).
    static int run(const std::vector<std::string> &argv, std::string *out, std::string *err_out, int timeout_ms,
                   size_t max_out = 16u << 20);

private:
    void reaped(int status);
    static std::string drain(int fd);

    pid_t pid_ = -1;
    int in_ = -1, out_ = -1, err_ = -1;
    std::optional<int> exit_code_;
    int exit_signal_ = 0;
};

} // namespace babble
