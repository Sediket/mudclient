#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <vector>

#include "mudclient/event_queue.hpp"

using namespace mudclient;
using namespace std::chrono_literals;

TEST_CASE("pop_wait times out on an empty queue without blocking forever", "[event_queue]") {
    EventQueue q;
    auto start = std::chrono::steady_clock::now();
    auto result = q.pop_wait(50ms);
    auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK_FALSE(result.has_value());
    CHECK(elapsed >= 50ms);
    CHECK(elapsed < 2s);
}

TEST_CASE("push then pop_wait returns events in FIFO order", "[event_queue]") {
    EventQueue q;
    q.push(UserInput{"first"});
    q.push(UserInput{"second"});

    auto a = q.pop_wait(1s);
    REQUIRE(a.has_value());
    REQUIRE(std::holds_alternative<UserInput>(*a));
    CHECK(std::get<UserInput>(*a).text == "first");

    auto b = q.pop_wait(1s);
    REQUIRE(b.has_value());
    CHECK(std::get<UserInput>(*b).text == "second");

    CHECK(q.empty());
}

TEST_CASE("multiple producer threads deliver every event exactly once", "[event_queue][concurrency]") {
    EventQueue q;
    constexpr int producers = 4;
    constexpr int per_producer = 500;

    std::vector<std::thread> threads;
    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&q, p] {
            for (int i = 0; i < per_producer; ++i) {
                q.push(UserInput{std::to_string(p) + ":" + std::to_string(i)});
            }
        });
    }

    int received = 0;
    std::atomic<bool> producers_done{false};
    std::thread stopper([&] {
        for (auto& t : threads) t.join();
        producers_done = true;
    });

    while (received < producers * per_producer) {
        auto ev = q.pop_wait(50ms);
        if (ev) {
            ++received;
        } else if (producers_done && q.empty()) {
            break;
        }
    }
    stopper.join();
    CHECK(received == producers * per_producer);
}
