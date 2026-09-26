#include <catch2/catch_test_macros.hpp>

#include "mudclient/command_parser.hpp"

using namespace mudclient;

TEST_CASE("tokenize_quoted: whitespace-separated tokens", "[command_parser]") {
    CHECK(tokenize_quoted("alpha beta  gamma") == std::vector<std::string>{"alpha", "beta", "gamma"});
}

TEST_CASE("tokenize_quoted: double and single quoted segments become one token", "[command_parser]") {
    CHECK(tokenize_quoted(R"(add "hello world" 'foo bar')") ==
          std::vector<std::string>{"add", "hello world", "foo bar"});
}

TEST_CASE("tokenize_quoted: escaped characters inside quotes", "[command_parser]") {
    // Written as ordinary (non-raw) string literals: MSVC's lexer rejects a
    // raw string literal whose content contains a backslash immediately
    // followed by a quote (R"(...\"...)"), even though it's valid standard
    // C++ (GCC and Clang both accept it) -- portably avoided here rather
    // than relied upon.
    std::string input = "\"she said \\\"hi\\\"\"";
    CHECK(tokenize_quoted(input) == std::vector<std::string>{"she said \"hi\""});
}

TEST_CASE("tokenize_quoted: adjacent quoted/unquoted pieces merge into one token", "[command_parser]") {
    CHECK(tokenize_quoted(R"(foo"bar baz"qux)") == std::vector<std::string>{"foobar bazqux"});
}

TEST_CASE("tokenize_quoted: unterminated quote consumes the rest of the string", "[command_parser]") {
    CHECK(tokenize_quoted(R"(a "b c)") == std::vector<std::string>{"a", "b c"});
}

TEST_CASE("tokenize_quoted: empty input yields no tokens", "[command_parser]") {
    CHECK(tokenize_quoted("").empty());
    CHECK(tokenize_quoted("   ").empty());
}

TEST_CASE("CommandParser: is_command_line recognizes the configured prefix", "[command_parser]") {
    CommandParser parser;
    CHECK(parser.is_command_line("#connect host 1234"));
    CHECK(parser.is_command_line("  #quit"));
    CHECK_FALSE(parser.is_command_line("say hello"));
    CHECK_FALSE(parser.is_command_line(""));
}

TEST_CASE("CommandParser: configurable prefix", "[command_parser]") {
    CommandParser parser('!');
    CHECK(parser.is_command_line("!quit"));
    CHECK_FALSE(parser.is_command_line("#quit"));
}

TEST_CASE("CommandParser: #connect requires host and port", "[command_parser]") {
    CommandParser parser;
    auto ok = parser.parse("#connect mud.example.com 4000");
    REQUIRE(std::holds_alternative<ParsedCommand>(ok));
    auto& cmd = std::get<ParsedCommand>(ok);
    CHECK(cmd.command == BuiltinCommand::Connect);
    CHECK(cmd.args == std::vector<std::string>{"mud.example.com", "4000"});

    auto missing = parser.parse("#connect onlyhost");
    CHECK(std::holds_alternative<ParseError>(missing));
}

TEST_CASE("CommandParser: #disconnect and #quit take no required args", "[command_parser]") {
    CommandParser parser;
    CHECK(std::holds_alternative<ParsedCommand>(parser.parse("#disconnect")));
    CHECK(std::holds_alternative<ParsedCommand>(parser.parse("#quit")));
}

TEST_CASE("CommandParser: #lua preserves raw code untokenized", "[command_parser]") {
    CommandParser parser;
    auto result = parser.parse(R"(#lua print("hello world"))");
    REQUIRE(std::holds_alternative<ParsedCommand>(result));
    auto& cmd = std::get<ParsedCommand>(result);
    CHECK(cmd.command == BuiltinCommand::Lua);
    CHECK(cmd.raw_args == R"(print("hello world"))");

    auto empty = parser.parse("#lua");
    CHECK(std::holds_alternative<ParseError>(empty));
}

TEST_CASE("CommandParser: #alias add|del|list", "[command_parser]") {
    CommandParser parser;
    auto add = parser.parse(R"(#alias add e4 "east;east;east;east")");
    REQUIRE(std::holds_alternative<ParsedCommand>(add));
    CHECK(std::get<ParsedCommand>(add).command == BuiltinCommand::AliasAdd);
    CHECK(std::get<ParsedCommand>(add).args == std::vector<std::string>{"e4", "east;east;east;east"});

    auto del = parser.parse("#alias del 3");
    REQUIRE(std::holds_alternative<ParsedCommand>(del));
    CHECK(std::get<ParsedCommand>(del).command == BuiltinCommand::AliasDel);

    auto list = parser.parse("#alias list");
    REQUIRE(std::holds_alternative<ParsedCommand>(list));
    CHECK(std::get<ParsedCommand>(list).command == BuiltinCommand::AliasList);

    CHECK(std::holds_alternative<ParseError>(parser.parse("#alias add e4"))); // missing expansion
    CHECK(std::holds_alternative<ParseError>(parser.parse("#alias bogus")));
}

TEST_CASE("CommandParser: #trigger add|del|list", "[command_parser]") {
    CommandParser parser;
    auto add = parser.parse(R"(#trigger add "^(\w+) hits you" "echo ouch")");
    REQUIRE(std::holds_alternative<ParsedCommand>(add));
    CHECK(std::get<ParsedCommand>(add).command == BuiltinCommand::TriggerAdd);
    CHECK(std::holds_alternative<ParsedCommand>(parser.parse("#trigger del 1")));
    CHECK(std::holds_alternative<ParsedCommand>(parser.parse("#trigger list")));
}

TEST_CASE("CommandParser: #timer add/list/pause/resume/reset/kill", "[command_parser]") {
    CommandParser parser;
    auto add = parser.parse("#timer add 5 repeat say tick");
    REQUIRE(std::holds_alternative<ParsedCommand>(add));
    auto& add_cmd = std::get<ParsedCommand>(add);
    CHECK(add_cmd.command == BuiltinCommand::TimerAdd);
    CHECK(add_cmd.args == std::vector<std::string>{"5", "repeat", "say", "tick"});

    CHECK(std::holds_alternative<ParsedCommand>(parser.parse("#timer list")));

    auto pause = parser.parse("#timer pause tick");
    REQUIRE(std::holds_alternative<ParsedCommand>(pause));
    CHECK(std::get<ParsedCommand>(pause).command == BuiltinCommand::TimerPause);

    CHECK(std::get<ParsedCommand>(parser.parse("#timer resume tick")).command == BuiltinCommand::TimerResume);
    CHECK(std::get<ParsedCommand>(parser.parse("#timer reset tick")).command == BuiltinCommand::TimerReset);
    CHECK(std::get<ParsedCommand>(parser.parse("#timer kill tick")).command == BuiltinCommand::TimerKill);

    CHECK(std::holds_alternative<ParseError>(parser.parse("#timer add 5"))); // missing command
    CHECK(std::holds_alternative<ParseError>(parser.parse("#timer pause"))); // missing id/label
    CHECK(std::holds_alternative<ParseError>(parser.parse("#timer bogus")));
}

TEST_CASE("CommandParser: unknown command name is a ParseError", "[command_parser]") {
    CommandParser parser;
    CHECK(std::holds_alternative<ParseError>(parser.parse("#frobnicate")));
}

TEST_CASE("CommandParser: parsing a non-command line is a ParseError", "[command_parser]") {
    CommandParser parser;
    CHECK(std::holds_alternative<ParseError>(parser.parse("just talking")));
}
