#include "mudclient/command_parser.hpp"

#include <cctype>

namespace mudclient {

std::vector<std::string> tokenize_quoted(std::string_view text) {
    std::vector<std::string> tokens;
    std::string current;
    bool in_token = false;
    size_t i = 0;
    while (i < text.size()) {
        char c = text[i];
        if (!in_token && std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        if (c == '\'' || c == '"') {
            in_token = true;
            char quote = c;
            ++i;
            while (i < text.size() && text[i] != quote) {
                if (text[i] == '\\' && i + 1 < text.size()) {
                    current.push_back(text[i + 1]);
                    i += 2;
                } else {
                    current.push_back(text[i]);
                    ++i;
                }
            }
            if (i < text.size()) {
                ++i; // skip closing quote
            }
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            tokens.push_back(std::move(current));
            current.clear();
            in_token = false;
            ++i;
            continue;
        }
        in_token = true;
        current.push_back(c);
        ++i;
    }
    if (in_token) {
        tokens.push_back(std::move(current));
    }
    return tokens;
}

CommandParser::CommandParser(char prefix) : prefix_(prefix) {}

bool CommandParser::is_command_line(std::string_view line) const {
    size_t p = line.find_first_not_of(" \t");
    return p != std::string_view::npos && line[p] == prefix_;
}

std::variant<ParsedCommand, ParseError> CommandParser::parse(std::string_view line) const {
    size_t p = line.find_first_not_of(" \t");
    if (p == std::string_view::npos || line[p] != prefix_) {
        return ParseError{"not a command line"};
    }
    std::string_view rest = line.substr(p + 1);
    size_t name_end = rest.find_first_of(" \t");
    std::string name(rest.substr(0, name_end));

    std::string raw_args;
    if (name_end != std::string_view::npos) {
        std::string_view after = rest.substr(name_end);
        size_t args_start = after.find_first_not_of(" \t");
        if (args_start != std::string_view::npos) {
            raw_args = std::string(after.substr(args_start));
        }
    }

    std::vector<std::string> tokens = tokenize_quoted(raw_args);

    auto with_subcommand = [&](std::string_view group) -> std::variant<ParsedCommand, ParseError> {
        if (tokens.empty()) {
            return ParseError{"usage: #" + name + " add|del|list ..."};
        }
        std::string sub = tokens[0];
        std::vector<std::string> remaining(tokens.begin() + 1, tokens.end());
        BuiltinCommand cmd;
        size_t min_args = 0;
        if (sub == "add") {
            cmd = group == "alias" ? BuiltinCommand::AliasAdd : BuiltinCommand::TriggerAdd;
            min_args = 2; // pattern + expansion
        } else if (sub == "del") {
            cmd = group == "alias" ? BuiltinCommand::AliasDel : BuiltinCommand::TriggerDel;
            min_args = 1; // id
        } else if (sub == "list") {
            cmd = group == "alias" ? BuiltinCommand::AliasList : BuiltinCommand::TriggerList;
            min_args = 0;
        } else {
            return ParseError{"unknown #" + name + " subcommand: " + sub};
        }
        if (remaining.size() < min_args) {
            return ParseError{"usage: #" + name + " " + sub +
                               (sub == "add" ? " <pattern> <expansion>" : " <id>")};
        }
        return ParsedCommand{cmd, std::move(remaining), raw_args};
    };

    if (name == "connect") {
        if (tokens.size() < 2) {
            return ParseError{"usage: #connect <host> <port>"};
        }
        return ParsedCommand{BuiltinCommand::Connect, tokens, raw_args};
    }
    if (name == "disconnect") {
        return ParsedCommand{BuiltinCommand::Disconnect, tokens, raw_args};
    }
    if (name == "lua") {
        if (raw_args.empty()) {
            return ParseError{"usage: #lua <code>"};
        }
        return ParsedCommand{BuiltinCommand::Lua, {}, raw_args};
    }
    if (name == "alias") {
        return with_subcommand("alias");
    }
    if (name == "trigger") {
        return with_subcommand("trigger");
    }
    if (name == "timer") {
        if (tokens.empty()) {
            return ParseError{"usage: #timer add|list|pause|resume|reset|kill ..."};
        }
        std::string sub = tokens[0];
        std::vector<std::string> remaining(tokens.begin() + 1, tokens.end());
        if (sub == "add") {
            if (remaining.size() < 2) {
                return ParseError{"usage: #timer add <seconds> [repeat] <command>"};
            }
            return ParsedCommand{BuiltinCommand::TimerAdd, std::move(remaining), raw_args};
        }
        if (sub == "list") {
            return ParsedCommand{BuiltinCommand::TimerList, std::move(remaining), raw_args};
        }
        if (sub == "pause" || sub == "resume" || sub == "reset" || sub == "kill") {
            if (remaining.empty()) {
                return ParseError{"usage: #timer " + sub + " <id|label>"};
            }
            BuiltinCommand cmd = sub == "pause"    ? BuiltinCommand::TimerPause
                                  : sub == "resume" ? BuiltinCommand::TimerResume
                                  : sub == "reset"  ? BuiltinCommand::TimerReset
                                                     : BuiltinCommand::TimerKill;
            return ParsedCommand{cmd, std::move(remaining), raw_args};
        }
        return ParseError{"unknown #timer subcommand: " + sub};
    }
    if (name == "quit") {
        return ParsedCommand{BuiltinCommand::Quit, tokens, raw_args};
    }
    return ParseError{"unknown command: #" + name};
}

} // namespace mudclient
