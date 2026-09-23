#include <doctest/doctest.h>

#include "frame/mailbox.h"

#include <chrono>
#include <thread>

using namespace babble;

TEST_CASE("LatestSlot keeps only the newest value and counts drops") {
    LatestSlot<int> slot;
    CHECK_FALSE(slot.take().has_value());
    slot.put(1);
    slot.put(2);
    slot.put(3);
    CHECK(slot.dropped() == 2);
    auto v = slot.take();
    REQUIRE(v.has_value());
    CHECK(*v == 3);
    CHECK_FALSE(slot.take().has_value());
}

TEST_CASE("LatestSlot wait_take wakes on put and on close") {
    LatestSlot<int> slot;
    std::thread producer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        slot.put(7);
    });
    auto v = slot.wait_take(std::chrono::seconds(2));
    producer.join();
    REQUIRE(v.has_value());
    CHECK(*v == 7);

    std::thread closer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        slot.close();
    });
    auto t0 = std::chrono::steady_clock::now();
    CHECK_FALSE(slot.wait_take(std::chrono::seconds(2)).has_value());
    closer.join();
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1));
}
