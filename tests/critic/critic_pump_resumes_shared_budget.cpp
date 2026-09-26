// Critic adversarial test (M3 round 4).
//
// Prompted specifically by round 4's review of the round-3 fix for
// M3-NEW-4 (ScriptEngine::pump_resumes() previously had no
// InstructionBudgetGuard at all; round 3 added one, matching every other
// dispatch_* entry point's pattern).
//
// Every OTHER guarded entry point gives each individual callback
// invocation its own fresh, full instruction_budget_: call_event_handlers
// constructs a new InstructionBudgetGuard inside its for-loop, once per
// handler; dispatch_timer's fn() call and register_trigger/
// register_alias's action lambdas each get their own guard per firing.
//
// pump_resumes() does NOT follow that pattern. It wraps its ENTIRE
// while-loop (client._pump_resumes(), which can drain an arbitrary
// number of queued coroutine.resume() calls -- one per pending
// client.wait_for/client.sleep -- queued during a single dispatch_line/
// dispatch_gmcp/dispatch_timer/etc. call) in exactly ONE
// InstructionBudgetGuard, constructed once before the loop starts. That
// guard resets the shared counter (instruction_budget_remaining_) to a
// single fresh instruction_budget_ for the WHOLE drain, not per resumed
// coroutine.
//
// Consequence: if one incoming line/event causes N separate,
// independently-legitimate wait_for/sleep continuations to all become
// resumable at once (e.g. several `client.run_test` coroutines all
// wait_for-ing the same trigger pattern, which is an ordinary, spec-legal
// way to write concurrent test scripts, not an attack), they are drained
// together in FIFO order under ONE shared budget. Each individual
// continuation's post-resume work can be entirely reasonable and well
// under instruction_budget_ on its own, yet a later-queued (but equally
// legitimate) continuation can still be aborted with "instruction budget
// exceeded" purely because an earlier continuation in the *same drain*
// already spent most of the shared allowance -- something that would NOT
// happen had the same two continuations instead been resumed from two
// separate dispatch_* calls (each of which gets its own fresh guard/
// budget).
//
// This is not a sandbox-safety regression (total work per drain is still
// correctly bounded, which is what M3-NEW-4 asked for) -- it is a
// correctness/spec-deviation risk in the other direction: a legitimate
// script combining ordinary concurrency (multiple simultaneous
// client.wait_for calls on a shared pattern) with any non-trivial
// per-continuation work can see spurious, order-dependent
// "instruction budget exceeded" failures that have nothing to do with
// that continuation's own cost. Whether this is acceptable depends on
// how large instruction_budget_ (10,000,000) is relative to realistic
// script workloads; flagged for the Builder's/human's judgment on
// whether pump_resumes should instead give each drained item its own
// fresh guard (mirroring call_event_handlers), not fixed as part of this
// review's own action.
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

TEST_CASE("CRITIC: pump_resumes shares one instruction budget across all resumes drained "
          "in the same dispatch, not one fresh budget per resumed continuation",
          "[critic][sandbox][script]") {
    Fixture f;

    // Three independent client.run_test coroutines, each wait_for-ing the
    // SAME pattern (an ordinary way to write several concurrent waiters
    // in one script) and then doing 4,500,000 loop-body iterations of
    // entirely ordinary, non-malicious work. Calibrated (see
    // tests/critic/critic_calibrate.cpp, not committed/wired -- a
    // throwaway measurement) against this exact build: a single fresh
    // instruction_budget_ (10,000,000) comfortably permits one such loop
    // run in isolation (a plain `for i=1,N do n=n+1 end` loop first fails
    // only once N reaches 5,000,000, i.e. ~2 VM instructions per source
    // iteration) -- so 4,500,000 is individually, comfortably
    // budget-compliant. Three of them (13,500,000 total) is not, if they
    // share one budget for the whole drain.
    f.engine.run_string(R"lua(
        client.run_test(function()
            client.wait_for('go', 5)
            local n = 0
            for i = 1, 4500000 do n = n + 1 end
            client.echo('first:' .. n)
        end)
        client.run_test(function()
            client.wait_for('go', 5)
            local n = 0
            for i = 1, 4500000 do n = n + 1 end
            client.echo('second:' .. n)
        end)
        client.run_test(function()
            client.wait_for('go', 5)
            local n = 0
            for i = 1, 4500000 do n = n + 1 end
            client.echo('third:' .. n)
        end)
    )lua");

    StyledLine line;
    line.plain = "go";
    // All three wait_for triggers match this single incoming line within
    // one triggers_.process_line() call, queuing all three resumes; a
    // single pump_resumes() call at the end of dispatch_line then drains
    // all three under one shared guard/budget.
    f.engine.dispatch_line(line);

    INFO("echoed: " << [&] {
        std::string all;
        for (auto& s : f.echoed) all += s + " | ";
        return all;
    }());

    // Each continuation's 4,500,000-iteration loop is legitimate,
    // individually-budget-compliant work (see the calibration comment
    // above): resumed on its own with a fresh full guard, it always
    // completes. If pump_resumes gave each drained resume its own fresh
    // budget (matching every other entry point's per-callback pattern),
    // all three would complete and echo their "first:"/"second:"/
    // "third:" result. Instead, this documents the actual behavior at
    // HEAD (2de5016): with only one ~10,000,000 budget shared across the
    // whole drain (13,500,000 total real work requested), at least one
    // of the three does not complete -- aborted by the shared,
    // already-partially-spent counter, purely because of what else was
    // queued in the *same* dispatch, not because of its own cost.
    int completed_count = 0;
    bool budget_error_seen = false;
    for (const auto& msg : f.echoed) {
        if (msg.rfind("first:", 0) == 0) ++completed_count;
        if (msg.rfind("second:", 0) == 0) ++completed_count;
        if (msg.rfind("third:", 0) == 0) ++completed_count;
        if (msg.find("instruction budget") != std::string::npos) budget_error_seen = true;
    }

    // Not itself asserted as a pass/fail verdict on the design question
    // (see reviews/m3-round4.json finding M3-NEW-5 for the disposition):
    // this documents, reproducibly, that batching independently-
    // legitimate continuations into one pump_resumes() drain can make a
    // later one fail purely due to what ran before it in the same drain.
    // At HEAD: completed_count == 2 (first and second complete: 2 *
    // 4,500,000 = 9,000,000 fits under the one shared 10,000,000-ish
    // budget; third does not, and budget_error_seen is true), NOT 3.
    CHECK(completed_count < 3);
    CHECK(budget_error_seen);
    // Documents that the two that DID complete were not simply the
    // sandbox rejecting all of them outright -- the shared-budget
    // interaction is real and partial, not "everything fails".
    CHECK(completed_count > 0);
}
