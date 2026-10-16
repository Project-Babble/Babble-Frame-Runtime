#include "platform/process.h"

#include "core/clock.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace babble {

namespace {

void close_fd(int &fd) {
    if (fd >= 0)
        ::close(fd);
    fd = -1;
}

bool executable(const std::string &path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(path.c_str(), X_OK) == 0;
}

}

std::optional<std::string> Process::find_in_path(const std::string &name) {
    if (name.empty())
        return std::nullopt;
    if (name.find('/') != std::string::npos)
        return executable(name) ? std::optional<std::string>(name) : std::nullopt;
    const char *path = getenv("PATH");
    std::string dirs = path && *path ? path : "/usr/local/bin:/usr/bin:/bin";
    size_t start = 0;
    while (start <= dirs.size()) {
        size_t end = dirs.find(':', start);
        if (end == std::string::npos)
            end = dirs.size();
        std::string dir = dirs.substr(start, end - start);
        if (!dir.empty()) {
            std::string full = dir + "/" + name;
            if (executable(full))
                return full;
        }
        start = end + 1;
    }
    return std::nullopt;
}

Process::~Process() {
    if (running())
        terminate(1000);
    close_fd(in_);
    close_fd(out_);
    close_fd(err_);
}

bool Process::spawn(const std::vector<std::string> &argv, const Options &opt, std::string *err) {
    auto fail = [&](const std::string &why) {
        if (err)
            *err = why;
        return false;
    };
    if (pid_ > 0)
        return fail("already running");
    if (opt.stdin_pipe)
        signal(SIGPIPE, SIG_IGN);   // If the child exits early, the write must fail and not kill the runtime.
    if (argv.empty())
        return fail("no command");
    const auto exe = find_in_path(argv[0]);
    if (!exe)
        return fail(argv[0] + ": not found");

    int in[2] = {-1, -1}, out[2] = {-1, -1}, er[2] = {-1, -1};
    auto close_all = [&] {
        for (int *p : {in, out, er})
            for (int i = 0; i < 2; i++)
                if (p[i] >= 0)
                    ::close(p[i]);
    };
    if ((opt.stdin_pipe && pipe2(in, O_CLOEXEC) != 0) || (opt.stdout_pipe && pipe2(out, O_CLOEXEC) != 0) ||
        (opt.stderr_pipe && pipe2(er, O_CLOEXEC) != 0)) {
        const int e = errno;
        close_all();
        return fail(std::string("pipe: ") + strerror(e));
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (opt.stdin_pipe)
        posix_spawn_file_actions_adddup2(&fa, in[0], STDIN_FILENO);
    else
        posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    auto output = [&](int fd, bool piped, int pipe_w, const std::string &file) {
        if (piped)
            posix_spawn_file_actions_adddup2(&fa, pipe_w, fd);
        else if (!file.empty())
            posix_spawn_file_actions_addopen(&fa, fd, file.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        else
            posix_spawn_file_actions_addopen(&fa, fd, "/dev/null", O_WRONLY, 0);
    };
    output(STDOUT_FILENO, opt.stdout_pipe, out[1], opt.stdout_file);
    output(STDERR_FILENO, opt.stderr_pipe, er[1], opt.stderr_file);
    posix_spawn_file_actions_addclose(&fa, 3);   // Close fd 3 because only the runtime can use the shim link.

    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    sigset_t none, defaults;
    sigemptyset(&none);
    sigemptyset(&defaults);
    for (int sig : {SIGTERM, SIGINT, SIGHUP, SIGPIPE, SIGCHLD})
        sigaddset(&defaults, sig);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &defaults);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    // Use the environment of the runtime, and let opt.env replace or add entries.
    std::vector<std::string> env_store;
    for (char **e = environ; e && *e; e++) {
        std::string kv = *e;
        const std::string key = kv.substr(0, kv.find('='));
        if (!opt.env.count(key))
            env_store.push_back(std::move(kv));
    }
    for (const auto &[k, v] : opt.env)
        env_store.push_back(k + "=" + v);
    std::vector<char *> envp;
    for (auto &kv : env_store)
        envp.push_back(kv.data());
    envp.push_back(nullptr);

    std::vector<std::string> args = argv;
    std::vector<char *> cargv;
    for (auto &a : args)
        cargv.push_back(a.data());
    cargv.push_back(nullptr);

    pid_t pid = -1;
    const int rc = posix_spawn(&pid, exe->c_str(), &fa, &at, cargv.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    // Close the pipe ends that belong to the child.
    for (int fd : {in[0], out[1], er[1]})
        if (fd >= 0)
            ::close(fd);
    if (rc != 0) {
        for (int fd : {in[1], out[0], er[0]})
            if (fd >= 0)
                ::close(fd);
        return fail(argv[0] + ": " + strerror(rc));
    }
    pid_ = pid;
    in_ = in[1];
    out_ = out[0];
    err_ = er[0];
    for (int fd : {in_, out_, err_})
        if (fd >= 0)
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    exit_code_.reset();
    exit_signal_ = 0;
    return true;
}

void Process::reaped(int status) {
    pid_ = -1;
    if (WIFEXITED(status))
        exit_code_ = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        exit_signal_ = WTERMSIG(status);
}

bool Process::running() {
    if (pid_ <= 0)
        return false;
    int status = 0;
    const pid_t r = waitpid(pid_, &status, WNOHANG);
    if (r == pid_) {
        reaped(status);
        return false;
    }
    if (r < 0 && errno == ECHILD) {
        pid_ = -1;   // Another caller reaped the child.
        return false;
    }
    return true;
}

bool Process::write_stdin(const void *data, size_t n, int timeout_ms) {
    if (in_ < 0)
        return false;
    const auto *p = static_cast<const uint8_t *>(data);
    const uint64_t deadline = mono_ns() + uint64_t(timeout_ms) * 1000000ull;
    while (n > 0) {
        const ssize_t w = ::write(in_, p, n);
        if (w > 0) {
            p += w;
            n -= size_t(w);
            continue;
        }
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0 && errno != EAGAIN)
            return false;   // EPIPE means that the child closed its stdin or exited.
        const uint64_t now = mono_ns();
        if (now >= deadline)
            return false;
        pollfd pfd{in_, POLLOUT, 0};
        poll(&pfd, 1, int((deadline - now) / 1000000ull) + 1);
    }
    return true;
}

void Process::close_stdin() { close_fd(in_); }

std::string Process::drain(int fd) {
    std::string s;
    if (fd < 0)
        return s;
    char buf[4096];
    for (;;) {
        const ssize_t r = ::read(fd, buf, sizeof(buf));
        if (r > 0) {
            s.append(buf, size_t(r));
            continue;
        }
        if (r < 0 && errno == EINTR)
            continue;
        break;
    }
    return s;
}

std::string Process::read_stdout() { return drain(out_); }

int Process::read_stdout_exact(void *buf, size_t n, int timeout_ms) {
    if (out_ < 0)
        return -1;
    auto *p = static_cast<uint8_t *>(buf);
    size_t got = 0;
    while (got < n) {
        const ssize_t r = ::read(out_, p + got, n - got);
        if (r > 0) {
            got += size_t(r);
            continue;
        }
        if (r == 0)
            return got == 0 ? 0 : -1;   // The child closed its stdout.
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN)
            return -1;
        pollfd pfd{out_, POLLIN, 0};
        if (poll(&pfd, 1, timeout_ms) == 0)
            return -1;
    }
    return 1;
}
std::string Process::read_stderr() { return drain(err_); }

bool Process::wait(int timeout_ms) {
    if (pid_ <= 0)
        return true;
    if (timeout_ms < 0) {
        int status = 0;
        while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
        reaped(status);
        return true;
    }
    const uint64_t deadline = mono_ns() + uint64_t(timeout_ms) * 1000000ull;
    while (running()) {
        if (mono_ns() >= deadline)
            return false;
        sleep_ns(5000000ull);
    }
    return true;
}

void Process::terminate(int grace_ms) {
    if (!running())
        return;
    kill(pid_, SIGTERM);
    if (!wait(grace_ms)) {
        kill(pid_, SIGKILL);
        wait(-1);
    }
}

int Process::run(const std::vector<std::string> &argv, std::string *out, std::string *err_out, int timeout_ms,
                 size_t max_out) {
    Process p;
    Options o;
    o.stdout_pipe = out != nullptr;
    o.stderr_pipe = err_out != nullptr;
    std::string err;
    if (!p.spawn(argv, o, &err)) {
        if (err_out)
            *err_out = err;
        return -1;
    }
    const uint64_t deadline = mono_ns() + uint64_t(timeout_ms) * 1000000ull;
    for (;;) {
        pollfd fds[2];
        int n = 0;
        if (p.out_ >= 0)
            fds[n++] = {p.out_, POLLIN, 0};
        if (p.err_ >= 0)
            fds[n++] = {p.err_, POLLIN, 0};
        const uint64_t now = mono_ns();
        if (now >= deadline) {
            p.terminate(500);
            return -1;
        }
        if (n > 0)
            poll(fds, nfds_t(n), int(std::min<uint64_t>((deadline - now) / 1000000ull + 1, 100)));
        else
            sleep_ns(5000000ull);
        if (out) {
            *out += p.read_stdout();
            if (out->size() > max_out) {
                p.terminate(500);
                return -1;
            }
        }
        if (err_out)
            *err_out += p.read_stderr();
        if (!p.running()) {
            if (out)
                *out += p.read_stdout();
            if (err_out)
                *err_out += p.read_stderr();
            return p.exit_code_ ? *p.exit_code_ : -1;
        }
    }
}

}
