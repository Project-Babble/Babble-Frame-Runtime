#include <doctest/doctest.h>

#include "sink/steam_link.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

using namespace babble;

TEST_CASE("the streaming PC comes from the latest Steam Link session") {
    const std::string log =
        "Tue Sep 29 2026 20:34:29.000 [Info] - UDPServerAddresses:\n"
        "Tue Sep 29 2026 20:34:29.000 [Info] - \t192.168.0.149  \n"
        "Tue Sep 29 2026 20:34:29.000 [Info] - \t10.35.78.25 *\n"
        "Tue Sep 29 2026 20:34:29.100 [Info] - Soft AES PERF: 5.07 MByte/s\n"
        "Tue Sep 29 2026 20:45:05.796 [Info] - UDPServerAddresses:\n"
        "Tue Sep 29 2026 20:45:05.796 [Info] - \t127.0.0.1  \n"
        "Tue Sep 29 2026 20:45:05.796 [Info] - \t192.168.0.149 *\n"
        "Tue Sep 29 2026 20:45:05.796 [Info] - \t10.35.78.25  \n"
        "Tue Sep 29 2026 20:45:05.815 [Info] - Soft AES PERF: 5.07 MByte/s\n"
        "Tue Sep 29 2026 20:45:06.180 [Info] - SVLDataLinkTransferUDP::SetPeer 10.35.78.1 -> 10.35.78.25 (Connect: 0)\n";
    CHECK(steam_link::streaming_pc(log) == "192.168.0.149");

    // Without a marked address, the result is the latest link peer.
    CHECK(steam_link::streaming_pc("x [Info] - UDPServerAddresses:\nx [Info] - \t192.168.0.149  \n"
                                      "x [Info] - SVLDataLinkTransferUDP::SetPeer 10.35.78.1 -> 10.35.78.25 (Connect: 0)\n") ==
          "10.35.78.25");
    CHECK_FALSE(steam_link::streaming_pc("nothing to see here\n"));
    CHECK_FALSE(steam_link::streaming_pc(""));
}

TEST_CASE("the latest Steam Link session's process id is found") {
    CHECK(steam_link::session_pid("x [Info] - vrlink 2.17.10 startup with PID=1711175, config=/x\n"
                                     "y [Info] - vrlink 2.17.10 startup with PID=3121094, config=/x\n") == 3121094);
    CHECK_FALSE(steam_link::session_pid("no sessions\n"));
}

TEST_CASE("the PC address is empty or IPv4") {
    CHECK(valid_pc_address(""));
    CHECK(valid_pc_address("192.168.1.20"));
    CHECK_FALSE(valid_pc_address("192.168.1"));
    CHECK_FALSE(valid_pc_address("my-pc.local"));
}

TEST_CASE("outputs that read the same log share one reader") {
    auto a = steam_link::shared("/nonexistent/a/vrlink.txt");
    CHECK(steam_link::shared("/nonexistent/a/vrlink.txt") == a);
    CHECK(steam_link::shared("/nonexistent/b/vrlink.txt") != a);
    CHECK(steam_link::shared() == steam_link::shared(""));

    // Once every output lets the reader go, the next one gets a fresh reader.
    const std::weak_ptr<steam_link::SteamLinkPc> old = a;
    a.reset();
    CHECK(old.expired());
    const auto fresh = steam_link::shared("/nonexistent/a/vrlink.txt");
    REQUIRE(fresh);
    CHECK_FALSE(fresh->live());
}

TEST_CASE("the reader reuses its answer until the recheck time has passed") {
    const auto dir = std::filesystem::temp_directory_path() / ("babble_steamlink_" + std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    const auto log = dir / "vrlink.txt";
    // Make a live process named vrlink that sleeps.
    const pid_t child = fork();
    if (child == 0) {
        execl("/bin/sleep", "vrlink", "20", static_cast<char *>(nullptr));
        _exit(127);
    }
    REQUIRE(child > 0);
    {
        std::ofstream out(log, std::ios::trunc);
        out << "x [Info] - vrlink 2.17.10 startup with PID=" << child << ", config=/x\n"
            << "x [Info] - UDPServerAddresses:\n"
            << "x [Info] - \t10.0.0.9 *\n"
            << "x [Info] - Soft AES PERF\n";
    }
    // Wait until the child has become the vrlink process.
    std::string cmdline;
    for (int i = 0; i < 100 && cmdline.find("vrlink") == std::string::npos; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::ifstream cmd("/proc/" + std::to_string(child) + "/cmdline");
        std::getline(cmd, cmdline, '\0');
    }

    steam_link::SteamLinkPc reader(log, std::chrono::milliseconds(200));
    CHECK(reader.live() == "10.0.0.9");

    // The session ends, but the reader keeps its answer for the recheck time.
    kill(child, SIGKILL);
    waitpid(child, nullptr, 0);
    CHECK(reader.live() == "10.0.0.9");
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    CHECK(reader.live() == std::nullopt);

    // Many outputs can ask at the same time.
    auto shared = steam_link::shared(log);
    std::atomic<bool> stop{false};
    std::atomic<int> answers{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 2; t++)
        threads.emplace_back([&] {
            while (!stop.load()) {
                if (!shared->live())
                    answers++;
            }
        });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop = true;
    for (auto &t : threads)
        t.join();
    CHECK(answers.load() > 0);
    std::filesystem::remove_all(dir);
}
