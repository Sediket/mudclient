// Critic adversarial test (M3 round 3).
//
// M3-NEW-2 (round 2, major, test-integrity) found that client.reset_timer's
// Lua binding had zero test coverage. The Builder's round-2 fix added
// "ScriptEngine: register_timer/pause_timer/resume_timer/reset_timer/
// kill_timer bindings all work" (tests/test_script_engine.cpp), whose
// reset_timer portion only asserts:
//
//     f.engine.run_string("client.reset_timer('tick')");
//     pump_for(60ms);
//     int ticks_after_reset = report_ticks();
//     CHECK(ticks_after_reset > ticks_after_resume);
//
// against a timer that is already a *repeating* 10ms-interval timer,
// already ticking from the preceding resume_timer step. That assertion
// passes whether or not client.reset_timer does anything at all: the
// timer keeps ticking on its own regardless, so "ticks increased" is
// guaranteed either way. Confirmed by mutation (this round): making the
// C++ reset_timer binding a complete no-op (dropping the
// `asio::post(...timers_.reset(id)...)` call entirely) leaves that whole
// test case passing (13/13 assertions), which is exactly the "if none
// fails, that's a blocking test-coverage finding" case CRITIC.md's
// mutation-check mandate describes.
//
// This test instead mirrors TimerManager's own decisive
// "reset restarts the full interval" unit test
// (tests/test_timer_manager.cpp), but through the client.reset_timer Lua
// binding rather than TimerManager directly: a one-shot timer, reset
// partway through its interval, must NOT fire in the window it would have
// fired without the reset, and must fire once the full (restarted)
// interval has elapsed. A no-op reset_timer binding fails the first
// check (the timer fires in the original window); the real binding does
// not.
#include <catch2/catch_test_macros.hpp>

#include <asio.hpp>
#include <chrono>
#include <future>
#include <thread>

#include "mudclient/event_queue.hpp"
#include "mudclient/network_client.hpp"
#include "mudclient/script_engine.hpp"
#include "mudclient/timer_manager.hpp"

using namespace mudclient;
using namespace std::chrono_literals;

namespace {

struct Fixture {
    asio::io_context server_io;
    asio::ip::tcp::acceptor acceptor{server_io, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0)};
    uint16_t port = acceptor.local_endpoint().port();
    asio::ip::tcp::socket server_socket{server_io};
    std::thread server_thread;

    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> guard = asio::make_work_guard(io);
    std::thread io_thread{[this] { io.run(); }};

    EventQueue events;
    std::shared_ptr<NetworkClient> network;
    TimerManager timers{io, events};

    std::vector<std::string> echoed;
    ScriptEngine engine{*network_ptr(), io, timers, [this](std::string s) { echoed.push_back(std::move(s)); }};

    NetworkClient* network_ptr() {
        network = std::make_shared<NetworkClient>(io, events);
        return network.get();
    }

    Fixture() {
        acceptor.async_accept(server_socket, [](const asio::error_code&) {});
        server_thread = std::thread([this] { server_io.run_for(2s); });
        std::promise<void> connected;
        auto fut = connected.get_future();
        asio::post(io, [this, &connected] {
            network->connect("127.0.0.1", std::to_string(port), 2000ms);
            connected.set_value();
        });
        fut.get();
        auto ev = events.pop_wait(2s);
        REQUIRE(ev.has_value());
    }

    // Drains raw TimerFired events from the network-thread EventQueue for
    // `duration`, counting them -- deliberately *not* calling
    // engine.dispatch_timer() (unlike test_script_engine.cpp's Fixture),
    // since this test only needs to know whether/when the underlying
    // TimerManager fires, not exercise the Lua callback dispatch path.
    int count_fires_within(std::chrono::milliseconds duration) {
        int count = 0;
        auto deadline = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < deadline) {
            auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining <= 0ms) break;
            auto ev = events.pop_wait(std::chrono::duration_cast<std::chrono::milliseconds>(remaining));
            if (ev && std::holds_alternative<TimerFired>(*ev)) {
                ++count;
            }
        }
        return count;
    }

    ~Fixture() {
        asio::post(io, [this] { network->disconnect(); });
        std::this_thread::sleep_for(20ms);
        guard.reset();
        io.stop();
        io_thread.join();
        server_thread.join();
    }
};

} // namespace

TEST_CASE("CRITIC: client.reset_timer's Lua binding actually restarts the interval, not a no-op",
          "[critic][timer][script]") {
    Fixture f;

    // A one-shot 60ms timer, labeled so client.reset_timer can address it
    // exactly as script_engine.cpp's own reset_timer test does.
    f.engine.run_string(R"lua(
        client.register_timer(0.06, function() end, false, "once")
    )lua");

    std::this_thread::sleep_for(40ms);
    f.engine.run_string("client.reset_timer('once')"); // should push the fire out another 60ms from here

    // Without the fix (or with a no-op reset_timer binding), the timer
    // would fire ~20ms from here (60ms - 40ms already elapsed). Give it
    // 40ms of headroom past that and require zero fires.
    CHECK(f.count_fires_within(40ms) == 0);

    // With a real reset, the restarted 60ms interval should still fire
    // within a generous window.
    CHECK(f.count_fires_within(1000ms) == 1);
}
