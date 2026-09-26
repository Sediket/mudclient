#include <catch2/catch_test_macros.hpp>

#include <asio.hpp>
#include <thread>

#include "mudclient/event_queue.hpp"
#include "mudclient/network_client.hpp"
#include "mudclient/script_engine.hpp"
#include "mudclient/timer_manager.hpp"

using namespace mudclient;
using namespace std::chrono_literals;

namespace {

// A minimal fixture giving ScriptEngine a real (connected, loopback)
// NetworkClient and a TimerManager, both living on a background
// network-thread io_context, matching the real threading model. A local
// TCP echo-ish acceptor stands in for a MUD server; tests only care that
// sends don't error, not about what (if anything) comes back.
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
        auto ev = events.pop_wait(2s); // Connected
        REQUIRE(ev.has_value());
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

TEST_CASE("ScriptEngine: sandbox excludes io/os/debug/package and dangerous globals", "[script]") {
    Fixture f;
    f.engine.run_string("result = (io == nil) and (debug == nil) and (package == nil) and "
                         "(dofile == nil) and (loadfile == nil) and (require == nil) and (string.dump == nil)");
    // run_string doesn't expose globals directly; verify via echo side channel instead.
    f.engine.run_string("if not ((io==nil) and (debug==nil) and (package==nil) and (dofile==nil) and "
                         "(loadfile==nil) and (require==nil) and (string.dump==nil)) then "
                         "client.echo('SANDBOX LEAK') end");
    CHECK(f.echoed.empty());
}

TEST_CASE("ScriptEngine: os provides only time/clock/date", "[script]") {
    Fixture f;
    f.engine.run_string("if type(os.time) ~= 'function' or type(os.clock) ~= 'function' or "
                         "type(os.date) ~= 'function' or os.execute ~= nil or os.remove ~= nil then "
                         "client.echo('OS LEAK') end");
    CHECK(f.echoed.empty());
}

TEST_CASE("ScriptEngine: load is text-only and rejects binary chunks", "[script]") {
    Fixture f;
    // A precompiled bytecode chunk starts with the Lua signature byte
    // 0x1B ('\x1bLua'); forcing mode "t" must reject it rather than
    // silently executing arbitrary bytecode (the classic load() sandbox
    // escape).
    f.engine.run_string("local fn, err = load('\\27Lua fake bytecode', 'x')\n"
                         "if fn ~= nil then client.echo('BYTECODE ACCEPTED') end");
    CHECK(f.echoed.empty());
}

TEST_CASE("ScriptEngine: load accepts and runs plain text chunks", "[script]") {
    Fixture f;
    f.engine.run_string("local fn = load('return 1 + 2')\n"
                         "if fn() ~= 3 then client.echo('LOAD BROKEN') end");
    CHECK(f.echoed.empty());
}

TEST_CASE("ScriptEngine: collectgarbage is restricted to count", "[script]") {
    Fixture f;
    f.engine.run_string("local n = collectgarbage('count')\n"
                         "if type(n) ~= 'number' then client.echo('COUNT BROKEN') end\n"
                         "collectgarbage('stop')\n" // must be a harmless no-op, not actually stop the GC
                         "collectgarbage('collect')");
    CHECK(f.echoed.empty());
}

TEST_CASE("ScriptEngine: instruction budget aborts a runaway loop", "[script]") {
    Fixture f;
    f.engine.run_string("while true do end");
    REQUIRE(f.echoed.size() == 1);
    CHECK(f.echoed[0].find("instruction budget") != std::string::npos);
}

TEST_CASE("ScriptEngine: client.send runs input through alias expansion and logs the result", "[script]") {
    Fixture f;
    f.engine.run_string("client.register_alias('e4', 'east;east;east;east')");
    f.engine.run_string("client.send('e4')");
    CHECK(f.engine.sent_log() == std::vector<std::string>{"east", "east", "east", "east"});
}

TEST_CASE("ScriptEngine: register_trigger fires on matching lines with captures", "[script]") {
    Fixture f;
    f.engine.run_string("client.register_trigger([[^(\\w+) hits you]], function(caps) "
                         "client.echo('hit by ' .. caps[1]) end)");
    StyledLine line;
    line.plain = "Orc hits you for 5";
    f.engine.dispatch_line(line);
    REQUIRE(f.echoed.size() == 1);
    CHECK(f.echoed[0] == "hit by Orc");
}

TEST_CASE("ScriptEngine: on('line') handlers receive plain/is_prompt/spans", "[script]") {
    Fixture f;
    f.engine.run_string("client.on('line', function(line) "
                         "client.echo(line.plain .. '|' .. tostring(line.is_prompt)) end)");
    StyledLine line;
    line.plain = "hello";
    line.is_prompt = true;
    f.engine.dispatch_line(line);
    REQUIRE(f.echoed.size() == 1);
    CHECK(f.echoed[0] == "hello|true");
}

TEST_CASE("ScriptEngine: gmcp cache updates and get_gmcp reads dotted paths", "[script]") {
    Fixture f;
    f.engine.dispatch_gmcp("Char.Vitals", nlohmann::json{{"hp", 10}, {"maxhp", 20}});
    f.engine.run_string("local hp = client.get_gmcp('Char.Vitals.hp')\n"
                         "if hp ~= 10 then client.echo('GMCP MISSING: ' .. tostring(hp)) end");
    CHECK(f.echoed.empty());
}

TEST_CASE("ScriptEngine: on('gmcp') fires with package and decoded table", "[script]") {
    Fixture f;
    f.engine.run_string("client.on('gmcp', function(package, data) "
                         "client.echo(package .. '=' .. tostring(data.hp)) end)");
    f.engine.dispatch_gmcp("Char.Vitals", nlohmann::json{{"hp", 42}});
    REQUIRE(f.echoed.size() == 1);
    CHECK(f.echoed[0] == "Char.Vitals=42");
}

TEST_CASE("ScriptEngine: client.exit sets exit_requested and exit_code", "[script]") {
    Fixture f;
    CHECK_FALSE(f.engine.exit_requested());
    f.engine.run_string("client.exit(3)");
    CHECK(f.engine.exit_requested());
    CHECK(f.engine.exit_code() == 3);
}

TEST_CASE("ScriptEngine: run_test + wait_for resolves on a matching trigger with captures", "[script]") {
    Fixture f;
    f.engine.run_string(R"lua(
        client.run_test(function()
            local caps = client.wait_for([[^(\w+) arrives]], 5)
            if caps == nil then
                client.echo('TIMED OUT')
            else
                client.echo('arrived: ' .. caps[1])
            end
        end)
    )lua");
    CHECK(f.echoed.empty()); // still waiting, nothing echoed yet

    StyledLine line;
    line.plain = "Goblin arrives";
    f.engine.dispatch_line(line);

    REQUIRE(f.echoed.size() == 1);
    CHECK(f.echoed[0] == "arrived: Goblin");
}

TEST_CASE("ScriptEngine: run_test + wait_for times out when nothing matches", "[script]") {
    Fixture f;
    f.engine.run_string(R"lua(
        client.run_test(function()
            local caps = client.wait_for('will never match this', 0.05)
            client.echo(caps == nil and 'TIMED OUT' or 'unexpected match')
        end)
    )lua");
    // Poll the engine's timer dispatch until the wait_for timeout fires.
    for (int i = 0; i < 50 && f.echoed.empty(); ++i) {
        auto ev = f.events.pop_wait(50ms);
        if (ev && std::holds_alternative<TimerFired>(*ev)) {
            f.engine.dispatch_timer(std::get<TimerFired>(*ev).id);
        }
    }
    REQUIRE(f.echoed.size() == 1);
    CHECK(f.echoed[0] == "TIMED OUT");
}

TEST_CASE("ScriptEngine: sleep suspends and resumes a test coroutine via a timer", "[script]") {
    Fixture f;
    f.engine.run_string(R"lua(
        client.run_test(function()
            client.sleep(0.02)
            client.echo('resumed')
        end)
    )lua");
    CHECK(f.echoed.empty());
    for (int i = 0; i < 50 && f.echoed.empty(); ++i) {
        auto ev = f.events.pop_wait(50ms);
        if (ev && std::holds_alternative<TimerFired>(*ev)) {
            f.engine.dispatch_timer(std::get<TimerFired>(*ev).id);
        }
    }
    REQUIRE(f.echoed.size() == 1);
    CHECK(f.echoed[0] == "resumed");
}

// Regression coverage for the client.register_timer/pause_timer/
// resume_timer/reset_timer/kill_timer Lua bindings themselves (as opposed
// to TimerManager's own, already-thorough unit tests): a Critic finding
// (M3-NEW-2) pointed out these had no coverage at the Lua-binding layer at
// all, including client.reset_timer(), added the same round to close
// M3-F4 (#timer reset was previously unwired).
TEST_CASE("ScriptEngine: register_timer/pause_timer/resume_timer/reset_timer/kill_timer bindings all work",
          "[script]") {
    Fixture f;

    auto pump_for = [&](std::chrono::milliseconds duration) {
        auto deadline = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < deadline) {
            auto ev = f.events.pop_wait(5ms);
            if (ev && std::holds_alternative<TimerFired>(*ev)) {
                f.engine.dispatch_timer(std::get<TimerFired>(*ev).id);
            }
        }
    };
    auto report_ticks = [&] {
        f.echoed.clear();
        f.engine.run_string("client.echo(tostring(ticks))");
        REQUIRE(f.echoed.size() == 1);
        return std::stoi(f.echoed[0]);
    };

    f.engine.run_string(R"lua(
        ticks = 0
        client.register_timer(0.01, function() ticks = ticks + 1 end, true, "tick")
    )lua");

    pump_for(60ms);
    int ticks_before_pause = report_ticks();
    CHECK(ticks_before_pause > 0);

    f.engine.run_string("client.pause_timer('tick')");
    pump_for(60ms);
    int ticks_while_paused = report_ticks();
    // Same inherent asio cancel-vs-already-dispatched race documented for
    // TimerManager's own pause/kill tests: at most one stray tick racing
    // pause(), never a cascade (nothing reschedules a paused timer, so it
    // can't repeat). Observed as an occasional single leaked tick on a
    // loaded Windows CI runner.
    CHECK(ticks_while_paused <= ticks_before_pause + 1);

    f.engine.run_string("client.resume_timer('tick')");
    pump_for(60ms);
    int ticks_after_resume = report_ticks();
    CHECK(ticks_after_resume > ticks_while_paused); // ticking again

    // reset_timer restarts the interval rather than stopping it -- the
    // binding working correctly means ticks keep incrementing afterward
    // (mirroring TimerManager's own "reset restarts the full interval"
    // unit test, but exercised through the Lua binding this time).
    f.engine.run_string("client.reset_timer('tick')");
    pump_for(60ms);
    int ticks_after_reset = report_ticks();
    CHECK(ticks_after_reset > ticks_after_resume);

    f.engine.run_string("client.kill_timer('tick')");
    pump_for(60ms);
    int ticks_after_kill = report_ticks();
    // Same inherent asio cancel-vs-already-dispatched race documented for
    // TimerManager::kill() directly (tests/test_timer_manager.cpp): at
    // most one stray tick, never a cascade.
    CHECK(ticks_after_kill <= ticks_after_reset + 1);
    pump_for(60ms);
    CHECK(report_ticks() == ticks_after_kill); // definitely silent now
}
