#include <catch2/catch_test_macros.hpp>

#include "mudclient/alias_manager.hpp"

using namespace mudclient;

TEST_CASE("AliasManager: exact alias expands to a single command", "[alias]") {
    AliasManager aliases;
    aliases.add_alias(AliasManager::Kind::Exact, "n", std::string("north"));
    auto result = aliases.expand("n");
    CHECK_FALSE(result.aborted);
    REQUIRE(result.commands.size() == 1);
    CHECK(result.commands[0] == "north");
}

TEST_CASE("AliasManager: unmatched input passes through unchanged", "[alias]") {
    AliasManager aliases;
    aliases.add_alias(AliasManager::Kind::Exact, "n", std::string("north"));
    auto result = aliases.expand("say hello");
    CHECK_FALSE(result.aborted);
    REQUIRE(result.commands.size() == 1);
    CHECK(result.commands[0] == "say hello");
}

TEST_CASE("AliasManager: expansion splits on the configured separator", "[alias]") {
    AliasManager aliases; // default separator ";"
    aliases.add_alias(AliasManager::Kind::Exact, "e4", std::string("east;east;east;east"));
    auto result = aliases.expand("e4");
    CHECK_FALSE(result.aborted);
    CHECK(result.commands == std::vector<std::string>{"east", "east", "east", "east"});
}

TEST_CASE("AliasManager: custom separator", "[alias]") {
    AliasManager aliases("|");
    aliases.add_alias(AliasManager::Kind::Exact, "combo", std::string("north|kill orc|loot"));
    auto result = aliases.expand("combo");
    CHECK(result.commands == std::vector<std::string>{"north", "kill orc", "loot"});
}

TEST_CASE("AliasManager: regex alias with $1-$9 and $0 template substitution", "[alias]") {
    AliasManager aliases;
    aliases.add_alias(AliasManager::Kind::Regex, R"(^kill (\w+)$)", std::string("attack $1 with sword ($0)"));
    auto result = aliases.expand("kill orc");
    REQUIRE(result.commands.size() == 1);
    CHECK(result.commands[0] == "attack orc with sword (kill orc)");
}

TEST_CASE("AliasManager: $$ produces a literal dollar sign", "[alias]") {
    AliasManager aliases;
    aliases.add_alias(AliasManager::Kind::Exact, "price", std::string("say it costs $$5"));
    auto result = aliases.expand("price");
    REQUIRE(result.commands.size() == 1);
    CHECK(result.commands[0] == "say it costs $5");
}

TEST_CASE("AliasManager: regex full-match semantics -- partial input does not match", "[alias]") {
    AliasManager aliases;
    aliases.add_alias(AliasManager::Kind::Regex, "^east$", std::string("go east"));
    auto matched = aliases.expand("east");
    REQUIRE(matched.commands.size() == 1);
    CHECK(matched.commands[0] == "go east");

    auto not_matched = aliases.expand("east wall"); // must NOT trigger the alias
    REQUIRE(not_matched.commands.size() == 1);
    CHECK(not_matched.commands[0] == "east wall");
}

TEST_CASE("AliasManager: first match wins without fall_through", "[alias]") {
    AliasManager aliases;
    aliases.add_alias(AliasManager::Kind::Exact, "go", std::string("first"));
    aliases.add_alias(AliasManager::Kind::Exact, "go", std::string("second"));
    auto result = aliases.expand("go");
    REQUIRE(result.commands.size() == 1);
    CHECK(result.commands[0] == "first");
}

TEST_CASE("AliasManager: fall_through lets later aliases also fire", "[alias]") {
    AliasManager aliases;
    AliasManager::Options fall_through;
    fall_through.fall_through = true;
    aliases.add_alias(AliasManager::Kind::Exact, "go", std::string("first"), fall_through);
    aliases.add_alias(AliasManager::Kind::Exact, "go", std::string("second"));
    auto result = aliases.expand("go");
    CHECK(result.commands == std::vector<std::string>{"first", "second"});
}

TEST_CASE("AliasManager: priority order, ties broken by registration order", "[alias]") {
    AliasManager aliases;
    AliasManager::Options high;
    high.priority = 5;
    high.fall_through = true;
    AliasManager::Options low;
    low.fall_through = true;
    aliases.add_alias(AliasManager::Kind::Exact, "go", std::string("low1"), low);
    aliases.add_alias(AliasManager::Kind::Exact, "go", std::string("high"), high);
    aliases.add_alias(AliasManager::Kind::Exact, "go", std::string("low2"), low);
    auto result = aliases.expand("go");
    CHECK(result.commands == std::vector<std::string>{"high", "low1", "low2"});
}

TEST_CASE("AliasManager: recursive expansion -- an alias expanding to another alias", "[alias]") {
    AliasManager aliases;
    aliases.add_alias(AliasManager::Kind::Exact, "e4", std::string("e;e;e;e"));
    aliases.add_alias(AliasManager::Kind::Exact, "e", std::string("east"));
    auto result = aliases.expand("e4");
    CHECK(result.commands == std::vector<std::string>{"east", "east", "east", "east"});
}

TEST_CASE("AliasManager: runaway recursive expansion aborts at depth 10", "[alias]") {
    AliasManager aliases;
    // "loop" expands to "loop", forever.
    aliases.add_alias(AliasManager::Kind::Exact, "loop", std::string("loop"));
    auto result = aliases.expand("loop");
    CHECK(result.aborted);
    CHECK_FALSE(result.error.empty());
    CHECK(result.commands.empty());
}

TEST_CASE("AliasManager: function action receives captures and returns the expansion", "[alias]") {
    AliasManager aliases;
    aliases.add_alias(AliasManager::Kind::Regex, R"(^cast (\w+)$)",
                       AliasManager::FunctionAction([](const std::vector<std::string>& caps) {
                           return "invoke spell:" + caps[1];
                       }));
    auto result = aliases.expand("cast fireball");
    REQUIRE(result.commands.size() == 1);
    CHECK(result.commands[0] == "invoke spell:fireball");
}

TEST_CASE("AliasManager: an action that adds/removes aliases mid-expansion does not corrupt iteration",
          "[alias][concurrency]") {
    AliasManager aliases;
    uint64_t self_id = 0;
    self_id = aliases.add_alias(AliasManager::Kind::Exact, "mutate",
                                 AliasManager::FunctionAction([&](const std::vector<std::string>&) {
                                     aliases.remove_alias(self_id);
                                     aliases.add_alias(AliasManager::Kind::Exact, "late", std::string("late-cmd"));
                                     return std::string("first-cmd");
                                 }));
    auto result = aliases.expand("mutate");
    REQUIRE(result.commands.size() == 1);
    CHECK(result.commands[0] == "first-cmd");
    CHECK_FALSE(aliases.exists(self_id));

    auto later = aliases.expand("late");
    REQUIRE(later.commands.size() == 1);
    CHECK(later.commands[0] == "late-cmd");
}
