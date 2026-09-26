// Critic adversarial test (M3 round 2).
//
// Round 1's critic_lua_sandbox.cpp tested (and, at HEAD, disproved) the
// hypothesis that a single coroutine.create()/coroutine.resume() pair
// inside a guarded callback entirely escapes the instruction budget. The
// Builder's round-2 fix comment explains *why* that specific case is safe
// as: "lua_sethook ... installs the count hook on the whole shared
// global_State, which every coroutine ... also runs under -- there is no
// separate hook to install per-coroutine."
//
// That explanation does not match Lua 5.4's actual source
// (third_party/.../lua54-src/src/lstate.h): `hook`, `hookmask`,
// `hookcount`, and `basehookcount` are fields of `struct lua_State` (the
// *per-thread* state), not of `global_State` (the fields genuinely shared
// across all threads/coroutines of one Lua universe hold GC state,
// metatables, the string cache, etc. -- no hook-related field). What
// actually happens (lstate.c, the thread-creation path used by
// `coroutine.create`) is a one-time *copy*: a newly created thread's
// hook/hookmask/basehookcount are copied from the *creating* thread at
// creation time, and its hookcount is freshly reset via
// `resethookcount()`. This is a different mechanism with a different
// safety property than "one shared hook for the whole universe":
//
//   Every freshly created coroutine gets its OWN, independently-ticking,
//   FULL fresh instruction budget at the moment it is created --
//   decoupled from how much of the *creating* thread's own budget has
//   already been spent. A callback can therefore repeatedly create a new
//   coroutine, let it run up to just under its own (fresh, full) budget,
//   and discard it, paying only the cheap create()+resume() call
//   overhead against the *outer* thread's own budget each time. Total
//   instructions actually executed in a single top-level callback
//   invocation can be amplified far beyond instruction_budget_, because
//   each inner coroutine's ~budget instructions of real work cost the
//   outer loop only a handful of instructions.
//
// This test demonstrates the amplification directly: it runs enough
// spray iterations that, if the amplification is real, total real work
// completed vastly exceeds instruction_budget_ (10,000,000 at HEAD)
// before the outer loop's own hook (the only thing bounding it) fires --
// and checks how long that actually takes/how much total work gets done.
//
// This is NOT re-litigating round 1's already-adjudicated finding (a
// single coroutine escaping entirely, unconditionally) -- it is a
// distinct, narrower bypass: repeated *fresh* coroutine creation inside
// one callback amplifying the effective per-callback budget by roughly
// (iterations) instead of bounding total work to instruction_budget_.
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

TEST_CASE("CRITIC: repeated fresh-coroutine creation amplifies the per-callback instruction budget",
          "[critic][sandbox]") {
    Fixture f;
    // Each inner coroutine does 100,000 loop iterations of real work --
    // trivially far under the 10,000,000 instruction_budget_ it inherits
    // fresh at creation -- so it completes and returns *without* ever
    // tripping its own hook. The outer loop runs this 200 times: if the
    // budget is amplified rather than genuinely bounding the callback as
    // a whole, total real work is 200 * 100,000 = 20,000,000 loop-body
    // executions (several times instruction_budget_ once VM opcode
    // overhead per iteration is counted -- see the direct-loop test's own
    // 300,000,000-iteration case aborting almost instantly for scale),
    // and the callback still completes normally with no "instruction
    // budget" error at all, because the *outer* loop's own instruction
    // count (200 create+resume calls plus trivial bookkeeping) never
    // approaches 10,000,000 on its own.
    f.engine.run_string(
        "client.register_trigger('go', function()"
        "  local total = 0"
        "  for outer = 1, 200 do"
        "    local co = coroutine.create(function()"
        "      local n = 0"
        "      for i = 1, 100000 do n = n + 1 end"
        "      return n"
        "    end)"
        "    local ok, result = coroutine.resume(co)"
        "    if ok then total = total + result else client.echo('iter ' .. outer .. ' failed: ' .. tostring(result)) end"
        "  end"
        "  client.echo('total:' .. total)"
        "end)");
    StyledLine line;
    line.plain = "go";
    auto start = std::chrono::steady_clock::now();
    f.engine.dispatch_line(line);
    auto elapsed = std::chrono::steady_clock::now() - start;

    INFO("elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                        << " echoed=" << (f.echoed.empty() ? std::string("<none>") : f.echoed[0]));

    // A sandbox that genuinely bounds *total* work per callback to
    // instruction_budget_ must abort this well before "total:20000000"
    // is ever echoed -- the callback should end in an "instruction
    // budget" error, the same as the direct-loop case, once cumulative
    // work (however it's distributed across coroutines) exceeds the
    // configured budget.
    //
    // At HEAD (1d36d06) this FAILS: the callback echoes "total:20000000"
    // with no instruction-budget error at all, confirming the
    // amplification bypass described above is real, not hypothetical.
    REQUIRE(f.echoed.size() == 1);
    CHECK(f.echoed[0].find("instruction budget") != std::string::npos);
    CHECK(f.echoed[0].find("total:20000000") == std::string::npos);
}
