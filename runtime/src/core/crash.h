#pragma once

namespace babble::crash {

// On a fatal signal (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT) write the
// signal, the thread and a backtrace to the log and stderr, then let the
// default action run (so the shim still sees the crash). std::terminate logs
// the exception's message first. Call once, early in main.
void install();

} // namespace babble::crash
