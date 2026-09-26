#include <catch2/catch_test_macros.hpp>

#include "mudclient/lua_escaping.hpp"

using namespace mudclient;

TEST_CASE("lua_quote: escapes backslashes, quotes, and newlines", "[lua_escaping]") {
    CHECK(lua_quote("hello") == "'hello'");
    CHECK(lua_quote("it's") == "'it\\'s'");
    CHECK(lua_quote("a\\b") == "'a\\\\b'");
    CHECK(lua_quote("a\nb") == "'a\\nb'");
    CHECK(lua_quote("") == "''");
}

TEST_CASE("lua_quote: a crafted string cannot break out of its own quotes", "[lua_escaping]") {
    // The exact shape a round-2 Critic finding was about: something that
    // looks like it could close the string and inject a second statement.
    std::string crafted = "x');client.echo('pwned";
    std::string quoted = lua_quote(crafted);
    // Every embedded ' must be escaped, so the quoted text has exactly two
    // *unescaped* single quotes: the opening and closing ones.
    int unescaped_quotes = 0;
    for (size_t i = 0; i < quoted.size(); ++i) {
        if (quoted[i] == '\'' && (i == 0 || quoted[i - 1] != '\\')) ++unescaped_quotes;
    }
    CHECK(unescaped_quotes == 2);
}

TEST_CASE("is_valid_number: accepts plain integers and decimals", "[lua_escaping]") {
    CHECK(is_valid_number("0"));
    CHECK(is_valid_number("42"));
    CHECK(is_valid_number("3.14"));
    CHECK(is_valid_number("-7"));
    CHECK(is_valid_number("-0.5"));
}

TEST_CASE("is_valid_number: rejects empty, malformed, and injection-shaped input", "[lua_escaping]") {
    CHECK_FALSE(is_valid_number(""));
    CHECK_FALSE(is_valid_number("-"));
    CHECK_FALSE(is_valid_number("."));
    CHECK_FALSE(is_valid_number("1.2.3"));
    CHECK_FALSE(is_valid_number("1e10")); // exponent form not accepted, by design (splice-safety over completeness)
    CHECK_FALSE(is_valid_number("1);client.echo('pwned');("));
    CHECK_FALSE(is_valid_number("3 -- comment"));
    CHECK_FALSE(is_valid_number("nan"));
    CHECK_FALSE(is_valid_number("0x10"));
}

TEST_CASE("lua_id_or_label: numeric text is spliced bare, anything else is quoted", "[lua_escaping]") {
    CHECK(lua_id_or_label("3") == "3");
    CHECK(lua_id_or_label("42") == "42");
    CHECK(lua_id_or_label("tick") == "'tick'");
    // The exact injection shape closed by this helper: without it, a bare
    // numeric-looking label like this would splice unescaped Lua.
    CHECK(lua_id_or_label("1);client.echo('pwned');(") == "'1);client.echo(\\'pwned\\');('");
}
