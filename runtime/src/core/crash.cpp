#include "core/crash.h"

#include "core/log.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <typeinfo>

#include <execinfo.h>
#include <pthread.h>
#include <unistd.h>

namespace {
const babble::log::Logger LOG("crash");
} // namespace

namespace babble::crash {

namespace {

const char *signal_name(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGABRT: return "SIGABRT";
    }
    return "?";
}

// Everything in the handler must be async-signal-safe: no allocation, no
// locks; fixed buffers and write(2) only.
void on_fatal(int sig) {
    char name[16] = "?";
    pthread_getname_np(pthread_self(), name, sizeof(name));
    char head[160];
    snprintf(head, sizeof(head), "\n*** FATAL: signal %d (%s) in thread %s; backtrace:\n", sig, signal_name(sig), name);
    log::write_raw_signal_safe(head);
    void *frames[64];
    const int n = backtrace(frames, 64);
    const int fd = log::file_fd();
    if (fd >= 0)
        backtrace_symbols_fd(frames, n, fd);
    backtrace_symbols_fd(frames, n, 2);
    log::write_raw_signal_safe("*** end of backtrace\n");
    // SA_RESETHAND restored the default action: re-raise to crash as usual.
    raise(sig);
}

void on_terminate() {
    if (auto e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception &ex) {
            LOG.error("terminate: uncaught %s: %s", typeid(ex).name(), ex.what());
        } catch (...) {
            LOG.error("terminate: uncaught non-standard exception");
        }
    } else {
        LOG.error("terminate called without an exception");
    }
    std::abort();   // -> on_fatal(SIGABRT): the backtrace
}

} // namespace

void install() {
    // backtrace() loads libgcc on first use, which isn't safe inside a
    // handler: do it now.
    void *warm[1];
    backtrace(warm, 1);

    // Its own stack, so a stack overflow can still be reported.
    static char alt[64 * 1024];
    stack_t ss{};
    ss.ss_sp = alt;
    ss.ss_size = sizeof(alt);
    sigaltstack(&ss, nullptr);

    struct sigaction sa{};
    sa.sa_handler = on_fatal;
    sa.sa_flags = SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT})
        sigaction(sig, &sa, nullptr);
    std::set_terminate(on_terminate);
}

} // namespace babble::crash
