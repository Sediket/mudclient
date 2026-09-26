#include <catch2/catch_test_macros.hpp>

#include "mudclient/trigger_manager.hpp"

using namespace mudclient;

namespace {
StyledLine plain_line(std::string text, bool prompt = false) {
    StyledLine line;
    line.plain = std::move(text);
    line.is_prompt = prompt;
    return line;
}
} // namespace

TEST_CASE("TriggerManager: basic match invokes action with captures", "[trigger]") {
    TriggerManager triggers;
    std::vector<std::string> seen;
    triggers.add_trigger(R"(^(\w+) (hits|misses) you)", [&](const std::vector<std::string>& caps) {
        seen = caps;
    });
    auto outcome = triggers.process_line(plain_line("Orc hits you for 5"));
    CHECK(outcome.fired_count == 1);
    REQUIRE(seen.size() == 3);
    CHECK(seen[1] == "Orc");
    CHECK(seen[2] == "hits");
}

TEST_CASE("TriggerManager: non-matching line fires nothing", "[trigger]") {
    TriggerManager triggers;
    int fired = 0;
    triggers.add_trigger("^ambush", [&](auto&) { ++fired; });
    triggers.process_line(plain_line("You walk north."));
    CHECK(fired == 0);
}

TEST_CASE("TriggerManager: disabled trigger never fires", "[trigger]") {
    TriggerManager triggers;
    int fired = 0;
    TriggerManager::Options opts;
    opts.enabled = false;
    triggers.add_trigger("hits", [&](auto&) { ++fired; }, opts);
    triggers.process_line(plain_line("Orc hits you"));
    CHECK(fired == 0);
}

TEST_CASE("TriggerManager: match_prompts gates evaluation against prompt lines", "[trigger]") {
    TriggerManager triggers;
    int fired_no_prompt = 0, fired_with_prompt = 0;
    TriggerManager::Options normal;
    TriggerManager::Options wants_prompts;
    wants_prompts.match_prompts = true;
    triggers.add_trigger("HP:", [&](auto&) { ++fired_no_prompt; }, normal);
    triggers.add_trigger("HP:", [&](auto&) { ++fired_with_prompt; }, wants_prompts);

    triggers.process_line(plain_line("HP: 100", /*prompt=*/true));
    CHECK(fired_no_prompt == 0);
    CHECK(fired_with_prompt == 1);
}

TEST_CASE("TriggerManager: gag and recolor outcome flags", "[trigger]") {
    TriggerManager triggers;
    TriggerManager::Options gag_opts;
    gag_opts.gag = true;
    triggers.add_trigger("secret", [](auto&) {}, gag_opts);

    TriggerManager::Options recolor_opts;
    recolor_opts.recolor = true;
    recolor_opts.recolor_style.fg = encode_standard_color(1);
    triggers.add_trigger("danger", [](auto&) {}, recolor_opts);

    auto gagged = triggers.process_line(plain_line("this is a secret message"));
    CHECK(gagged.gag);
    CHECK_FALSE(gagged.recolor);

    auto recolored = triggers.process_line(plain_line("danger ahead"));
    CHECK(recolored.recolor);
    CHECK(recolored.recolor_style.fg == encode_standard_color(1));
}

TEST_CASE("TriggerManager: once removes the trigger after first fire", "[trigger]") {
    TriggerManager triggers;
    int fired = 0;
    TriggerManager::Options once_opts;
    once_opts.once = true;
    uint64_t id = triggers.add_trigger("boom", [&](auto&) { ++fired; }, once_opts);

    triggers.process_line(plain_line("boom goes the dynamite"));
    CHECK(fired == 1);
    CHECK_FALSE(triggers.exists(id));
    triggers.process_line(plain_line("boom again"));
    CHECK(fired == 1); // no second fire; trigger is gone
}

TEST_CASE("TriggerManager: stop_processing halts evaluation of lower-priority triggers", "[trigger]") {
    TriggerManager triggers;
    std::vector<int> order;
    TriggerManager::Options high;
    high.priority = 10;
    high.stop_processing = true;
    TriggerManager::Options low;
    low.priority = 0;

    triggers.add_trigger("^line$", [&](auto&) { order.push_back(1); }, high);
    triggers.add_trigger("^line$", [&](auto&) { order.push_back(2); }, low);

    triggers.process_line(plain_line("line"));
    REQUIRE(order.size() == 1);
    CHECK(order[0] == 1);
}

TEST_CASE("TriggerManager: priority order is highest-first, registration order breaks ties", "[trigger]") {
    TriggerManager triggers;
    std::vector<int> order;
    TriggerManager::Options p0, p5;
    p5.priority = 5;

    triggers.add_trigger("^x$", [&](auto&) { order.push_back(1); }, p0); // registered first, priority 0
    triggers.add_trigger("^x$", [&](auto&) { order.push_back(2); }, p5); // registered second, priority 5
    triggers.add_trigger("^x$", [&](auto&) { order.push_back(3); }, p0); // registered third, priority 0

    triggers.process_line(plain_line("x"));
    REQUIRE(order.size() == 3);
    CHECK(order == std::vector<int>{2, 1, 3}); // priority 5 first, then the two priority-0 in registration order
}

TEST_CASE("TriggerManager: remove_trigger drops it from future matching", "[trigger]") {
    TriggerManager triggers;
    int fired = 0;
    uint64_t id = triggers.add_trigger("gone", [&](auto&) { ++fired; });
    triggers.remove_trigger(id);
    triggers.process_line(plain_line("gone but not forgotten"));
    CHECK(fired == 0);
}

TEST_CASE("TriggerManager: an action that adds/removes triggers mid-line does not corrupt iteration",
          "[trigger][concurrency]") {
    // Regression test for the reentrancy hazard documented in
    // trigger_manager.cpp: an Action (a stand-in for a Lua callback in M3)
    // that mutates the trigger set while process_line() is still iterating
    // must never cause a crash or use-after-free (ASan/UBSan builds will
    // catch that), regardless of what it decides to add or remove.
    TriggerManager triggers;
    std::vector<int> fired;
    uint64_t self_id = 0;
    self_id = triggers.add_trigger("mutator", [&](auto&) {
        fired.push_back(1);
        triggers.remove_trigger(self_id); // remove itself while process_line iterates
        triggers.add_trigger("late", [&](auto&) { fired.push_back(99); }); // and add a new one
    });
    triggers.add_trigger("mutator", [&](auto&) { fired.push_back(2); });

    triggers.process_line(plain_line("mutator line"));
    // Both triggers present at the start of this call fired; the
    // newly-added "late" trigger did not get a chance to match this same
    // line (it wasn't in the snapshot), which is well-defined per the
    // implementation comment.
    CHECK(fired == std::vector<int>{1, 2});
    CHECK_FALSE(triggers.exists(self_id));

    fired.clear();
    triggers.process_line(plain_line("late arrival"));
    CHECK(fired == std::vector<int>{99});
}

TEST_CASE("TriggerManager: prefilter never skips a regex that would actually match", "[trigger][prefilter]") {
    // A pattern with an extractable required literal ("you") combined with
    // a case where the literal is present but positioned such that only a
    // full regex evaluation (not just substring presence) determines the
    // real match -- the prefilter must still let the match proceed
    // correctly whenever the literal is present, and must not fire when
    // the literal truly is absent.
    TriggerManager triggers;
    int fired = 0;
    triggers.add_trigger(R"(^you (win|lose)$)", [&](auto&) { ++fired; });

    triggers.process_line(plain_line("you win"));
    CHECK(fired == 1);
    triggers.process_line(plain_line("you draw")); // "you" present, but regex doesn't match -- must not fire
    CHECK(fired == 1);
    triggers.process_line(plain_line("something else entirely")); // literal absent -- prefiltered out
    CHECK(fired == 1);
}
