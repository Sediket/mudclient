#pragma once

#include <string>

// Small, pure string-splicing helpers used by main.cpp's built-in ("#...")
// command dispatch, which synthesizes snippets of Lua source and runs them
// through the same client.* API a script would use. Header-only and
// dependency-free (no ScriptEngine/Lua types) so they can be unit-tested
// directly, independent of the anonymous-namespace handle_builtin() that
// uses them.
namespace mudclient {

// Produces a single-quoted Lua string literal safe to splice into a
// synthesized snippet of Lua source.
inline std::string lua_quote(const std::string& text) {
    std::string out = "'";
    for (char c : text) {
        if (c == '\\' || c == '\'') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

// True if `s` is safe to splice bare as a Lua numeral (used for ids and
// durations from built-in commands): digits, at most one '.', optional
// leading '-', nothing else. Rejecting anything else (rather than
// splicing it unchecked) is what keeps a crafted argument like
// "1);client.echo('x');(" from running as injected Lua instead of being
// treated as a literal value.
inline bool is_valid_number(const std::string& s) {
    size_t i = 0;
    if (i < s.size() && s[i] == '-') ++i;
    bool seen_digit = false;
    bool seen_dot = false;
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (c == '.' && !seen_dot) {
            seen_dot = true;
        } else if (c >= '0' && c <= '9') {
            seen_digit = true;
        } else {
            return false;
        }
    }
    return seen_digit;
}

// A numeric id/label argument may be either: splice a validated numeral
// bare (so it's read as a Lua number, matching client.*_timer's
// id-vs-label dispatch), or quote it as a string label otherwise.
inline std::string lua_id_or_label(const std::string& text) {
    return is_valid_number(text) ? text : lua_quote(text);
}

} // namespace mudclient
