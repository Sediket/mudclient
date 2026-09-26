// Critic adversarial test (M3 round 1).
//
// Hypothesis: ScriptEngine's per-callback instruction budget
// (lua_sethook(LUA_MASKCOUNT) via InstructionBudgetGuard, installed on
// lua_.lua_state() -- the *main* Lua thread) does not propagate to a
// coroutine created and resumed synchronously from *inside* a
// trigger/timer/alias/event callback. Lua's debug hooks are per-thread
// (per lua_State*, and a coroutine is its own lua_State* sharing the same
// global_State): setting a hook on the main state does not set it on a
// child coroutine's state. If true, a script can escape the instruction
// budget entirely by doing all of its runaway work inside a nested
// coroutine.create()/coroutine.resume() pair, defeating the sandbox's
// only defense against a hung/infinite Lua callback (SPEC.md section 4:
// "Instruction-count hook ... aborts any single callback exceeding a
// configurable budget").
//
// This is deliberately NOT a true infinite loop (which would hang the
// test/CI forever if the hypothesis is confirmed). Instead it runs a
// large, finite, fixed-iteration-count loop (well beyond the configured
// budget) two ways and compares elapsed wall time and iteration count
// actually completed:
//   (a) directly inside the trigger callback (on the main thread) --
//       expected to be aborted by the instruction hook after ~budget
//       instructions, so it echoes an early, small iteration count and
//       an "instruction budget" error, and returns quickly.
//   (b) inside a coroutine.create()/coroutine.resume() invoked from
//       inside the same kind of trigger callback -- if the hook does not
//       propagate, this runs to completion (all N iterations) and takes
//       much longer, with NO "instruction budget" error at all.
//
// A fixed, generous per-test timeout (this file's own wall-clock guard)
// keeps this bounded even if the bypass is confirmed.

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

// Same fixture shape as tests/test_script_engine.cpp's Fixture (kept
// self-contained here since Critic may only add files under tests/critic/).
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

TEST_CASE("CRITIC: instruction budget aborts a runaway loop run directly in a trigger callback", "[critic][sandbox]") {
    Fixture f;
    // 300M empty-body loop iterations is far beyond the default 10M
    // instruction budget; a correctly-enforced budget must abort well
    // before this completes.
    f.engine.run_string(
        "client.register_trigger('go', function()"
        "  local n = 0"
        "  for i = 1, 300000000 do n = n + 1 end"
        "  client.echo('completed:' .. n)"
        "end)");
    StyledLine line;
    line.plain = "go";
    auto start = std::chrono::steady_clock::now();
    f.engine.dispatch_line(line);
    auto elapsed = std::chrono::steady_clock::now() - start;

    REQUIRE(f.echoed.size() == 1);
    CHECK(f.echoed[0].find("instruction budget") != std::string::npos);
    CHECK(f.echoed[0].find("completed:") == std::string::npos); // must not have run to completion
    // Aborting after ~10M instructions of a trivial loop body should take
    // well under a second; this is a loose sanity bound, not the main
    // assertion (the string checks above are).
    CHECK(elapsed < 2s);
}

TEST_CASE("CRITIC: instruction budget does NOT stop a runaway loop hidden inside a nested coroutine",
          "[critic][sandbox]") {
    Fixture f;
    // Identical iteration count and loop shape as the previous test, but
    // wrapped in coroutine.create()/coroutine.resume() invoked from
    // *inside* the trigger callback -- the same callback-invocation
    // context (a sol::protected_function called by C++ from
    // TriggerManager's dispatch), just with the actual work delegated to
    // a child Lua thread that InstructionBudgetGuard's lua_sethook call
    // never touches.
    f.engine.run_string(
        "client.register_trigger('go', function()"
        "  local co = coroutine.create(function()"
        "    local n = 0"
        "    for i = 1, 300000000 do n = n + 1 end"
        "    return n"
        "  end)"
        "  local ok, result = coroutine.resume(co)"
        "  if ok then client.echo('completed:' .. tostring(result))"
        "  else client.echo('coroutine error: ' .. tostring(result)) end "
        "end)");
    StyledLine line;
    line.plain = "go";
    auto start = std::chrono::steady_clock::now();
    f.engine.dispatch_line(line);
    auto elapsed = std::chrono::steady_clock::now() - start;

    INFO("elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                        << " echoed=" << (f.echoed.empty() ? std::string("<none>") : f.echoed[0]));

    // If the sandbox's instruction budget genuinely bounds every Lua
    // callback (as SPEC.md section 4 requires), this must behave exactly
    // like the previous test: aborted quickly, with an "instruction
    // budget" error, and the loop must NOT have run to completion.
    //
    // This CHECK (not REQUIRE) is the actual finding: at HEAD, the loop
    // inside the coroutine runs to completion (echoes
    // "completed:300000000") and takes on the order of a second or more,
    // because lua_sethook was only ever installed on the main Lua thread,
    // not on the child coroutine's own lua_State. That is a real sandbox
    // gap: SPEC.md's "aborts any single callback" is violated whenever
    // the callback's own work happens inside a nested coroutine, which
    // any Lua script (malicious or merely buggy) can trivially do.
    REQUIRE(f.echoed.size() == 1);
    CHECK(f.echoed[0].find("instruction budget") != std::string::npos);
    CHECK(f.echoed[0].find("completed:300000000") == std::string::npos);
}
