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
    // Same inherent race documented on the pause/resume generation test
    // below: asio's cancel() cannot un-queue a completion that was already
    // dispatched internally before kill()'s posted task got a turn on the
    // io_context thread, so a single stray TimerFired can still arrive.
    // kill() erases the timer entry outright (unlike pause, which only
    // bumps a generation counter), so there's nothing left to reschedule
    // from -- at most one stray tick, never a cascade. Observed as an
    // occasional single leaked tick on a loaded Windows CI runner.
    CHECK(count_fires_within(events, 100ms) <= 1);
}

TEST_CASE("TimerManager: pause suppresses ticks, resume continues", "[timer]") {
    // Pauses immediately after registration, well before the first
    // deadline, rather than letting some ticks happen first and then
    // pausing: asio's cancel() cannot un-queue a completion that has
    // already been dispatched, so pausing a moment before a tick was about
    // to fire is an inherent (and otherwise-harmless) race that showed up
    // as an intermittent extra tick on a loaded CI runner. Pausing before
    // any tick can possibly have fired removes that race from this test
    // entirely, while still exercising the same pause/resume behavior.
    EventQueue events;
    IoThread io;
    auto timers = post_and_wait(io, [&] { return std::make_unique<TimerManager>(io.io, events); });
    uint64_t id = post_and_wait(io, [&] { return timers->add_timer(60ms, true); });

    post_and_wait(io, [&] { timers->pause(id); });
    CHECK(count_fires_within(events, 150ms) == 0); // paused: no ticks, even past when the first would have fired

    post_and_wait(io, [&] { timers->resume(id); });
    auto ev = events.pop_wait(1s);
    REQUIRE(ev.has_value());
    REQUIRE(std::holds_alternative<TimerFired>(*ev));

    post_and_wait(io, [&] { timers->kill(id); });
}

TEST_CASE("TimerManager: generation counter suppresses stale completions racing pause()",
          "[timer][concurrency]") {
    // Regression test for a mutation-testing gap: the previous
    // pause/resume test paused immediately after registration (to remove
    // a CI flake, see NOTES.md), which incidentally meant no test ever
    // raced pause() against a timer's own about-to-fire deadline, so
    // deleting the generation check in handle_fire passed the whole suite.
    //
    // asio's cancel() cannot un-queue a completion that's already been
    // dispatched internally -- when that happens, the handler still runs
    // with ec=success (not operation_aborted). Without the generation
    // check, such a stale completion would push a TimerFired *and*
    // reschedule itself via schedule(id), starting a runaway cascade of
    // ticks from a timer the caller believes is paused. With the check,
    // that single stale completion is recognized as belonging to a
    // superseded generation and dropped without rescheduling.
    //
    // This deliberately races pause() against a very short repeating
    // timer's deadline, then watches for a good while afterward (many
    // multiples of the tick interval). A single legitimate stray
    // completion (asio's documented cancel-vs-already-dispatched race)
    // never repeats -- there's nothing left to reschedule it, since
    // handle_fire's early return (correct behavior) or its buggy
    // reschedule-anyway (mutant behavior) is a one-time fork in the road
    // for that specific completion. So a correct implementation produces
    // at most one tick per race and then silence, however slowly or
    // quickly the test happens to run; a mutant with the generation check
    // removed produces a *continuous* stream of ticks at the timer's
    // normal interval for as long as we keep watching, because the
    // rescheduled wait fires again, and again. Watching for many interval
    // lengths (not just one) turns "0 or 1" vs. "keeps going" into a
    // difference of orders of magnitude that holds regardless of
    // execution speed (checked stable under both the `release` and `asan`
    // presets, the latter being substantially slower per operation).
    EventQueue events;
    IoThread io;
    auto timers = post_and_wait(io, [&] { return std::make_unique<TimerManager>(io.io, events); });

    constexpr int trials = 20;
    constexpr auto interval = 2ms;
    int leaked = 0;
    for (int t = 0; t < trials; ++t) {
        uint64_t id = post_and_wait(io, [&] { return timers->add_timer(interval, true); });
        std::this_thread::sleep_for(interval - 200us); // land close to the deadline
        post_and_wait(io, [&] { timers->pause(id); });
        leaked += count_fires_within(events, interval * 8); // watch for 8 intervals' worth of potential cascade
        post_and_wait(io, [&] { timers->kill(id); });
        while (events.pop_wait(0ms)) {
        } // drain between trials
    }
    // Correct: at most one stray tick per race (bounded by `trials`).
    // Mutant (generation check removed): a cascade of roughly (8 intervals
    // / 1 interval) = ~8 ticks per race that actually lands, overshooting
    // this bound by close to an order of magnitude.
    CHECK(leaked <= trials);
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
