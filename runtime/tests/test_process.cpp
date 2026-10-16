#include <doctest/doctest.h>

#include "platform/process.h"

#include <string>

using namespace babble;

TEST_CASE("process: run captures output and the exit code") {
    std::string out, err;
    CHECK(Process::run({"sh", "-c", "echo hello; echo oops >&2; exit 3"}, &out, &err, 5000) == 3);
    CHECK(out == "hello\n");
    CHECK(err == "oops\n");
    CHECK(Process::run({"/nonexistent/thing"}, &out, &err, 1000) == -1);
    CHECK(Process::run({"sleep", "5"}, nullptr, nullptr, 200) == -1);   // The process timed out and was killed.
}

TEST_CASE("process: stdin pipe, environment and PATH lookup") {
    CHECK(Process::find_in_path("sh"));
    CHECK_FALSE(Process::find_in_path("definitely-not-a-command-babble"));

    Process p;
    Process::Options o;
    o.stdin_pipe = true;
    o.stdout_pipe = true;
    o.env["BABBLE_TEST_VAR"] = "42";
    REQUIRE(p.spawn({"sh", "-c", "read line; echo \"$line $BABBLE_TEST_VAR\""}, o));
    const std::string in = "ping\n";
    REQUIRE(p.write_stdin(in.data(), in.size(), 1000));
    p.close_stdin();
    REQUIRE(p.wait(5000));
    CHECK(p.exit_code() == 0);
    CHECK(p.read_stdout() == "ping 42\n");
}

TEST_CASE("process: terminate stops a child that ignores its input") {
    Process p;
    REQUIRE(p.spawn({"sleep", "30"}, {}));
    CHECK(p.running());
    p.terminate(1000);
    CHECK_FALSE(p.running());
    CHECK_FALSE(p.exit_code());   // A signal ended the process.
}
