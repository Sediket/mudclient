#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Quote-aware command-line tokenizing and built-in command recognition
// (spec section 3, "Built-in commands"). This module only parses; it does
// not execute anything (dispatch into NetworkClient/TimerManager/Lua is the
// engine's job, wired up in M3's main.cpp).
namespace mudclient {

// Splits `text` on whitespace, treating a run of characters inside matching
// single or double quotes as one token (quotes themselves are stripped). A
// backslash inside a quoted section escapes the following character
// (including the quote character itself, allowing it to appear literally).
// A backslash outside any quotes is not special. An unterminated quote
// consumes the rest of the string as its (unterminated) token.
std::vector<std::string> tokenize_quoted(std::string_view text);

enum class BuiltinCommand {
    Connect,
    Disconnect,
    Lua,
    AliasAdd,
    AliasDel,
    AliasList,
    TriggerAdd,
    TriggerDel,
    TriggerList,
    TimerAdd,
    TimerList,
    TimerPause,
    TimerResume,
    TimerReset,
    TimerKill,
    Quit,
};

struct ParsedCommand {
    BuiltinCommand command;
    std::vector<std::string> args; // tokenized, quote-stripped, excluding the command name itself
    std::string raw_args; // the untouched remainder of the line after the command name and one space,
                           // for commands like #lua <code> that must not be destructively re-tokenized
};

struct ParseError {
    std::string message;
};

class CommandParser {
public:
    explicit CommandParser(char prefix = '#');

    bool is_command_line(std::string_view line) const;

    // Parses a line that is_command_line() would accept. Returns a
    // ParseError (never throws) for an unknown command name or missing
    // required arguments; validates arity/shape only, not semantic
    // validity (e.g. it doesn't check the host resolves).
    std::variant<ParsedCommand, ParseError> parse(std::string_view line) const;

private:
    char prefix_;
};

} // namespace mudclient
