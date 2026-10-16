#include "core/lifecycle.h"

#include <cerrno>
#include <csignal>

#include <sys/signalfd.h>
#include <sys/stat.h>
#include <unistd.h>

namespace babble {

bool ShimLink::present() {
    struct stat st{};
    if (fstat(kFd, &st) != 0)
        return false;
    return S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode);
}

bool ShimLink::drain() {
    char buf[256];
    for (;;) {
        ssize_t n = read(kFd, buf, sizeof(buf));
        if (n == 0)
            return false;
        if (n < 0)
            return errno == EAGAIN || errno == EINTR;
    }
}

int make_signalfd() {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGHUP);
    sigaddset(&mask, SIGUSR1);   // SIGUSR1 writes a diagnostics file.
    if (pthread_sigmask(SIG_BLOCK, &mask, nullptr) != 0)
        return -1;
    signal(SIGPIPE, SIG_IGN);
    return signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
}

}
