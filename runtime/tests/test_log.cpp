// The logger: module levels, repeats, rate limits, the file and its rotation.
#include <doctest/doctest.h>

#include "core/clock.h"
#include "core/crash.h"
#include "core/log.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <csignal>

#include <sys/wait.h>
#include <unistd.h>

using namespace babble;

namespace {

std::string slurp(const std::filesystem::path &p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct TempLog {
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("babble_log_test_" + std::to_string(getpid()));
    std::filesystem::path path = dir / "test.log";
    explicit TempLog(size_t max_bytes = 1 << 20, int keep = 2) {
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        REQUIRE(log::open_file(path, max_bytes, keep));
        log::mirror_stderr(false);
    }
    ~TempLog() {
        log::close_file();
        log::clear_module_levels();
        log::set_level(log::Level::Info);
        log::verbose_until(0);
        std::filesystem::remove_all(dir);
    }
};

} // namespace

TEST_CASE("log lines carry wall time, module and level; module levels override the default") {
    TempLog t;
    const log::Logger usb("test.usb"), link("test.link");
    log::set_module_level("test.usb", log::Level::Debug);
    usb.debug("usb detail %d", 1);
    link.debug("link detail");   // default level is Info: dropped
    link.info("link up");
    const std::string text = slurp(t.path);
    CHECK(text.find("usb detail 1") != std::string::npos);
    CHECK(text.find("link detail") == std::string::npos);
    CHECK(text.find(" I test.link ") != std::string::npos);
    CHECK(text.find(" D test.usb ") != std::string::npos);
    CHECK(text.substr(0, 2) == "20");   // starts with the wall-clock date

    // A level set before the module first logs applies once it does.
    log::set_module_level("test.later", log::Level::Error);
    const log::Logger later("test.later");
    CHECK_FALSE(later.enabled(log::Level::Warn));
    CHECK(later.enabled(log::Level::Error));

    // Verbose: everything at debug for a while.
    log::verbose_until(mono_ns() + 1'000'000'000);
    CHECK(later.enabled(log::Level::Debug));
    log::verbose_until(0);
    CHECK_FALSE(later.enabled(log::Level::Debug));
}

TEST_CASE("repeated lines collapse and recent() keeps the latest") {
    TempLog t;
    const log::Logger l("test.repeat");
    for (int i = 0; i < 5; i++)
        l.warn("same thing");
    l.info("something else");
    const std::string text = slurp(t.path);
    size_t count = 0;
    for (size_t at = text.find("same thing"); at != std::string::npos; at = text.find("same thing", at + 1))
        count++;
    CHECK(count == 1);
    CHECK(text.find("W test.repeat") != std::string::npos);
    CHECK(text.find("last message repeated 4 times") != std::string::npos);
    const auto last = log::recent(2);
    REQUIRE(last.size() == 2);
    CHECK(last[1].find("something else") != std::string::npos);
}

TEST_CASE("Every lets one message through per period and counts the rest") {
    log::Every every(50'000'000);   // 50 ms
    CHECK(every());
    CHECK_FALSE(every());
    CHECK_FALSE(every());
    usleep(60'000);
    CHECK(every());
    CHECK(every.suppressed() == 2);
}

TEST_CASE("the log file rotates at its size limit and keeps the newest files") {
    TempLog t(2000, 2);
    const log::Logger l("test.rotate");
    for (int i = 0; i < 100; i++)
        l.info("line %03d with some padding to fill the file faster ................", i);
    CHECK(std::filesystem::exists(t.path));
    CHECK(std::filesystem::exists(t.dir / "test.log.1"));
    CHECK(std::filesystem::exists(t.dir / "test.log.2"));
    CHECK_FALSE(std::filesystem::exists(t.dir / "test.log.3"));   // keep = 2
    CHECK(std::filesystem::file_size(t.path) < 2200);
    CHECK(slurp(t.path).find("line 099") != std::string::npos);
}

TEST_CASE("a crash writes the signal and a backtrace to the log, then dies as usual") {
    const auto dir = std::filesystem::temp_directory_path() / ("babble_crash_test_" + std::to_string(getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto path = dir / "crash.log";
    const pid_t child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        // The child: install the handler, log a line, then crash.
        babble::crash::install();
        log::open_file(path);
        log::mirror_stderr(false);
        const log::Logger l("test.crash");
        l.info("about to crash");
        volatile int *p = nullptr;
        *p = 1;
        _exit(0);   // not reached
    }
    int status = 0;
    REQUIRE(waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status));   // the default action still ran
    CHECK(WTERMSIG(status) == SIGSEGV);
    const std::string text = slurp(path);
    CHECK(text.find("about to crash") != std::string::npos);
    CHECK(text.find("FATAL: signal 11 (SIGSEGV)") != std::string::npos);
    CHECK(text.find("end of backtrace") != std::string::npos);
    std::filesystem::remove_all(dir);
}
