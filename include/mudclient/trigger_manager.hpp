#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "mudclient/events.hpp"
#include "mudclient/regex_pattern.hpp"

// Inbound trigger matching against StyledLine::plain. Engine thread only
// (owns Lua-facing state per the spec's threading model); does no I/O.
//
// Prefilter strategy (spec section 3, "Triggers"): rather than a shared
// Aho-Corasick automaton, each trigger independently stores the required
// literal substring extract_required_literal() derived from its own
// pattern (computed once, at registration/pattern-change time -- there is
// no shared structure to go stale, so there is no separate "rebuild"
// step). At match time, a trigger with a required literal is skipped
// unless that literal is found via std::string_view::find() in the line;
// a trigger with no extractable literal is always evaluated. This is the
// "simpler multi-substring scan" alternative the spec allows: with the
// target load (200 triggers), worst case is 200 substring searches over a
// short line (a MUD line is typically well under 200 bytes), which is a
// few microseconds total -- comfortably inside the 2000 lines/sec budget --
// versus building and maintaining a combined automaton for comparatively
// little benefit at this scale. extract_required_literal() is intentionally
// conservative (see its own doc comment): it never causes a trigger that
// could actually match to be skipped.
namespace mudclient {

// Deliberately a free (non-nested) struct: a nested aggregate type's own
// default member initializers aren't usable in a `= {}` default argument
// on another member function of the *same* enclosing class (the compiler
// needs the enclosing class complete first, but the nested type's default
// member initializers are themselves only available in complete-class
// contexts -- both GCC 13 and Clang 18 reject the nested form).
struct TriggerOptions {
    bool gag = false;
    bool recolor = false;
    TextStyle recolor_style{};
    bool once = false;
    bool enabled = true;
    int priority = 0;
    bool stop_processing = false;
    bool match_prompts = false;
};

class TriggerManager {
public:
    using Options = TriggerOptions;
    using Action = std::function<void(const std::vector<std::string>& captures)>;

    // Throws RegexCompileError if `pattern` is invalid PCRE2 syntax.
    uint64_t add_trigger(std::string pattern, Action action, Options opts = {});
    void remove_trigger(uint64_t id);
    void set_enabled(uint64_t id, bool enabled);
    bool exists(uint64_t id) const;
    size_t count() const { return triggers_.size(); }

    struct MatchOutcome {
        bool gag = false;
        bool recolor = false;
        TextStyle recolor_style{};
        int fired_count = 0;
    };

    // Evaluates all enabled triggers against `line` in priority order
    // (highest priority first; registration order breaks ties), invoking
    // each match's Action, honoring `once` (auto-removes after firing) and
    // `stop_processing` (stops evaluating further triggers on this line).
    MatchOutcome process_line(const StyledLine& line);

private:
    struct TriggerEntry {
        uint64_t id = 0;
        uint64_t seq = 0; // insertion sequence, for a stable priority sort
        CompiledRegex regex;
        Action action;
        Options opts;
        std::optional<std::string> required_literal;
    };

    void resort();
    TriggerEntry* find(uint64_t id);

    std::vector<TriggerEntry> triggers_; // kept sorted by (priority desc, seq asc)
    uint64_t next_id_ = 1;
    uint64_t next_seq_ = 0;
};

} // namespace mudclient
