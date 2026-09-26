#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "mudclient/regex_pattern.hpp"

// Outbound alias evaluation, run on user input before sending. Engine
// thread only.
namespace mudclient {

// Free (non-nested) struct: see the comment on TriggerOptions in
// trigger_manager.hpp for why this can't be nested inside AliasManager.
struct AliasOptions {
    int priority = 0;
    bool fall_through = false;
};

class AliasManager {
public:
    enum class Kind { Exact, Regex };
    using Options = AliasOptions;

    // A command-template string (with $0-$9 / $$ substitution, see below)
    // or a callback invoked with the match's captures, returning the
    // command(s) to send as an already-expanded string (still subject to
    // the same $-recursion/separator splitting as a template result).
    using FunctionAction = std::function<std::string(const std::vector<std::string>& captures)>;
    using Action = std::variant<std::string, FunctionAction>;

    explicit AliasManager(std::string separator = ";");

    // Throws RegexCompileError if kind == Regex and pattern is invalid.
    uint64_t add_alias(Kind kind, std::string pattern, Action action, Options opts = {});
    void remove_alias(uint64_t id);
    bool exists(uint64_t id) const;
    size_t count() const { return aliases_.size(); }

    struct ExpansionResult {
        bool aborted = false;
        std::string error; // set iff aborted
        std::vector<std::string> commands; // final, fully-expanded literal commands to send, in order
    };

    // Evaluates `input` against all aliases (priority order, then
    // registration order), recursively re-evaluating expansions (so an
    // alias's expansion can itself trigger another alias) up to a depth of
    // 10; exceeding that aborts the whole expansion (nothing is sent) and
    // sets ExpansionResult::error.
    ExpansionResult expand(std::string_view input);

    static constexpr int kMaxRecursionDepth = 10;

private:
    struct AliasEntry {
        uint64_t id = 0;
        uint64_t seq = 0;
        Kind kind;
        std::string pattern_text;
        std::optional<CompiledRegex> regex; // engaged iff kind == Regex
        Action action;
        Options opts;
    };

    void resort();
    AliasEntry* find(uint64_t id);
    void expand_recursive(std::string command, int depth, ExpansionResult& result);
    static std::string substitute_template(const std::string& tmpl, const std::vector<std::string>& captures);
    std::vector<std::string> split_on_separator(const std::string& text) const;

    std::string separator_;
    std::vector<AliasEntry> aliases_;
    uint64_t next_id_ = 1;
    uint64_t next_seq_ = 0;
};

} // namespace mudclient
