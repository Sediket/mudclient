#include <catch2/catch_test_macros.hpp>

#include <asio.hpp>
#include <future>
#include <thread>

#include "mudclient/event_queue.hpp"
#include "mudclient/timer_manager.hpp"

using namespace mudclient;
using namespace std::chrono_literals;

namespace {

// Runs an io_context on a background thread, mirroring the real network
// thread that owns every TimerManager instance. Every TimerManager method
// call in these tests goes through post_and_wait() below rather than being
// called directly from the test thread, matching the documented contract
// that TimerManager is only ever touched from the network thread.
struct IoThread {
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> guard = asio::make_work_guard(io);
    std::thread thread{[this] { io.run(); }};
    ~IoThread() {
        guard.reset();
        io.stop();
        thread.join();
    }
};

template <typename Fn>
auto post_and_wait(IoThread& io, Fn&& fn) -> decltype(fn()) {
    using Result = decltype(fn());
    std::promise<Result> promise;
    auto future = promise.get_future();
    asio::post(io.io, [&fn, &promise] {
        try {
            if constexpr (std::is_void_v<Result>) {
                fn();
                promise.set_value();
            } else {
                promise.set_value(fn());
            }
        } catch (...) {
            promise.set_exception(std::current_exception());
        }
    });
    return future.get(); // rethrows here if the posted call threw
}

int count_fires_within(EventQueue& q, std::chrono::milliseconds window) {
    auto deadline = std::chrono::steady_clock::now() + window;
    int count = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        auto remaining = deadline - std::chrono::steady_clock::now();
        auto ev = q.pop_wait(std::chrono::duration_cast<std::chrono::milliseconds>(remaining));
        if (ev && std::holds_alternative<TimerFired>(*ev)) {
            ++count;
        }
    }
    return count;
}

} // namespace

TEST_CASE("TimerManager: one-shot timer fires exactly once", "[timer]") {
    EventQueue events;
    IoThread io;
    auto timers = post_and_wait(io, [&] { return std::make_unique<TimerManager>(io.io, events); });

    uint64_t id = post_and_wait(io, [&] { return timers->add_timer(50ms, false); });

    auto ev = events.pop_wait(1s);
    REQUIRE(ev.has_value());
    REQUIRE(std::holds_alternative<TimerFired>(*ev));
    CHECK(std::get<TimerFired>(*ev).id == id);

    CHECK(count_fires_within(events, 150ms) == 0); // no further fires from a one-shot
    CHECK_FALSE(post_and_wait(io, [&] { return timers->exists(id); }));
}

TEST_CASE("TimerManager: repeating timer fires multiple times, killed timer stops", "[timer]") {
    EventQueue events;
    IoThread io;
    auto timers = post_and_wait(io, [&] { return std::make_unique<TimerManager>(io.io, events); });
    uint64_t id = post_and_wait(io, [&] { return timers->add_timer(20ms, true); });

    int fires = 0;
    auto deadline = std::chrono::steady_clock::now() + 130ms;
    while (std::chrono::steady_clock::now() < deadline) {
        auto ev = events.pop_wait(20ms);
        if (ev && std::holds_alternative<TimerFired>(*ev)) ++fires;
    }
    CHECK(fires >= 4); // ~130ms / 20ms, allow scheduling slack

    post_and_wait(io, [&] { timers->kill(id); });
    CHECK(count_fires_within(events, 100ms) == 0);
}

TEST_CASE("TimerManager: pause suppresses ticks, resume continues", "[timer]") {
    EventQueue events;
    IoThread io;
    auto timers = post_and_wait(io, [&] { return std::make_unique<TimerManager>(io.io, events); });
    uint64_t id = post_and_wait(io, [&] { return timers->add_timer(20ms, true); });

    std::this_thread::sleep_for(50ms); // let it tick a couple times
    while (events.pop_wait(1ms)) {
    } // drain

    post_and_wait(io, [&] { timers->pause(id); });
    CHECK(count_fires_within(events, 100ms) == 0); // paused: no ticks

    post_and_wait(io, [&] { timers->resume(id); });
    auto ev = events.pop_wait(1s);
    REQUIRE(ev.has_value());
    REQUIRE(std::holds_alternative<TimerFired>(*ev));

    post_and_wait(io, [&] { timers->kill(id); });
}

TEST_CASE("TimerManager: reset restarts the full interval", "[timer]") {
    EventQueue events;
    IoThread io;
    auto timers = post_and_wait(io, [&] { return std::make_unique<TimerManager>(io.io, events); });
    uint64_t id = post_and_wait(io, [&] { return timers->add_timer(60ms, false); });

    std::this_thread::sleep_for(40ms);
    post_and_wait(io, [&] { timers->reset(id); }); // pushes the fire out another 60ms from here
    CHECK(count_fires_within(events, 40ms) == 0); // would have fired ~20ms from now without the reset

    auto ev = events.pop_wait(1s);
    REQUIRE(ev.has_value());
    REQUIRE(std::holds_alternative<TimerFired>(*ev));
}

TEST_CASE("TimerManager: kill from inside its own fired callback is safe", "[timer][concurrency]") {
    // Simulates the engine dispatching a TimerFired to a Lua callback that
    // itself calls client.kill_timer(id), which posts kill() back to the
    // network thread -- exactly the scenario the header documents as safe.
    EventQueue events;
    IoThread io;
    auto timers = post_and_wait(io, [&] { return std::make_unique<TimerManager>(io.io, events); });
    uint64_t id = post_and_wait(io, [&] { return timers->add_timer(15ms, true); });

    auto ev = events.pop_wait(1s);
    REQUIRE(ev.has_value());
    REQUIRE(std::get<TimerFired>(*ev).id == id);

    // "Callback" reacts to the fire by killing its own timer, exactly as it
    // would be posted from the engine thread in response to processing the
    // TimerFired event above.
    post_and_wait(io, [&] { timers->kill(id); });
    CHECK(count_fires_within(events, 100ms) == 0);
    CHECK_FALSE(post_and_wait(io, [&] { return timers->exists(id); }));
}

TEST_CASE("TimerManager: labels resolve to ids and are freed on kill", "[timer]") {
    EventQueue events;
    IoThread io;
    auto timers = post_and_wait(io, [&] { return std::make_unique<TimerManager>(io.io, events); });
    uint64_t id = post_and_wait(io, [&] { return timers->add_timer(1s, true, std::string("tick")); });
    CHECK(post_and_wait(io, [&] { return timers->id_for_label("tick"); }) == id);
    post_and_wait(io, [&] { timers->kill(id); });
    CHECK_FALSE(post_and_wait(io, [&] { return timers->id_for_label("tick"); }).has_value());
}

TEST_CASE("TimerManager: registering a duplicate label throws", "[timer]") {
    EventQueue events;
    IoThread io;
    auto timers = post_and_wait(io, [&] { return std::make_unique<TimerManager>(io.io, events); });
    post_and_wait(io, [&] { timers->add_timer(1s, true, std::string("dup")); });
    CHECK_THROWS_AS(post_and_wait(io, [&] { timers->add_timer(1s, true, std::string("dup")); }),
                    std::invalid_argument);
}

TEST_CASE("TimerManager: operations on a nonexistent id are silently ignored", "[timer]") {
    EventQueue events;
    IoThread io;
    auto timers = post_and_wait(io, [&] { return std::make_unique<TimerManager>(io.io, events); });
    post_and_wait(io, [&] {
        timers->kill(9999);
        timers->pause(9999);
        timers->resume(9999);
        timers->reset(9999);
    });
    CHECK_FALSE(post_and_wait(io, [&] { return timers->exists(9999); }));
}
