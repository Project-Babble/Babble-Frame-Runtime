#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <sys/types.h>

namespace babble {

// A child process that gets the CPU pinning and nice value of the calling thread, so spawn it from the correct thread.
class Process {
public:
    struct Options {
        bool stdin_pipe = false;     // Use write_stdin() with the pipe, or the child gets /dev/null.
        bool stdout_pipe = false;    // Use read_stdout() with the pipe, or the output goes to stdout_file or /dev/null.
        bool stderr_pipe = false;    // Use read_stderr() with the pipe, or the output goes to stderr_file or /dev/null.
        std::string stdout_file;     // The child appends to this file.
        std::string stderr_file;     // The child appends to this file.
        std::map<std::string, std::string> env;   // These entries replace or add to the environment of the runtime.
    };

    Process() = default;
    ~Process();   // Terminates a child that still runs, with a grace time of 1 s.
    Process(const Process &) = delete;
    Process &operator=(const Process &) = delete;

    // Starts argv and looks up argv[0] on PATH if it has no slash, and returns false with *err on failure.
    bool spawn(const std::vector<std::string> &argv, const Options &opt, std::string *err = nullptr);

    pid_t pid() const { return pid_; }
    bool running();   // Also reaps the child after it exits.

    // Writes all of data to the stdin of the child within timeout_ms, or returns false.
    bool write_stdin(const void *data, size_t n, int timeout_ms);
    void close_stdin();

    // Returns the output that is available now, and does not wait.
    std::string read_stdout();
    // Reads n bytes and returns 1 on success, 0 at a clean end of output, or -1 on any failure.
    int read_stdout_exact(void *buf, size_t n, int timeout_ms);
    std::string read_stderr();

    // Waits up to timeout_ms, or forever for -1, and returns true if the child exited.
    bool wait(int timeout_ms);
    // Sends SIGTERM, then sends SIGKILL after grace_ms if the child still runs, and reaps the child.
    void terminate(int grace_ms);

    // Returns the exit code after the reap, or nullopt if the child runs or a signal stopped it.
    std::optional<int> exit_code() const { return exit_code_; }
    int exit_signal() const { return exit_signal_; }

    // Returns the full path of an executable on PATH, or nullopt if there is no such executable.
    static std::optional<std::string> find_in_path(const std::string &name);

    // Runs argv until it exits and returns the exit code, or -1 if it did not start or did not finish in time.
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

}
