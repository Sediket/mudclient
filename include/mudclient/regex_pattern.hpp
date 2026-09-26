#pragma once

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

// PCRE2-backed regex matching, JIT-compiled at registration, with a reused
// match-data block (no per-match allocation) and a conservative "required
// literal" extraction heuristic used by TriggerManager's prefilter.
namespace mudclient {

class RegexCompileError : public std::runtime_error {
public:
    explicit RegexCompileError(std::string message) : std::runtime_error(std::move(message)) {}
};

namespace detail {
struct Pcre2CodeDeleter {
    void operator()(pcre2_code* code) const noexcept { pcre2_code_free(code); }
};
struct Pcre2MatchDataDeleter {
    void operator()(pcre2_match_data* data) const noexcept { pcre2_match_data_free(data); }
};
} // namespace detail

using Pcre2CodePtr = std::unique_ptr<pcre2_code, detail::Pcre2CodeDeleter>;
using Pcre2MatchDataPtr = std::unique_ptr<pcre2_match_data, detail::Pcre2MatchDataDeleter>;

// Compiles `pattern` (PCRE2 syntax) and JIT-compiles it. Throws
// RegexCompileError with a human-readable message on invalid syntax.
// `full_match` wraps the pattern in \A(?:...)\z so it must match the whole
// subject (used for aliases); otherwise the pattern may match anywhere in
// the subject (used for triggers), matching each option's documented
// semantics in docs/SPEC.md §3.
class CompiledRegex {
public:
    explicit CompiledRegex(std::string_view pattern, bool full_match = false);

    // Matches against `subject`. On success, fills `captures_out` with
    // capture group 0 (whole match) through the highest numbered group that
    // participated (missing/unset trailing groups are omitted), and returns
    // true. Reuses internal match data across calls (no per-match
    // allocation). Not thread-safe for concurrent calls on the same
    // instance (engine thread only, per the spec's threading model).
    bool match(std::string_view subject, std::vector<std::string>& captures_out) const;

    const std::string& source() const { return source_; }

private:
    std::string source_;
    Pcre2CodePtr code_;
    mutable Pcre2MatchDataPtr match_data_;
};

// Extracts a substring that is *guaranteed* to appear in `pattern`'s subject
// whenever the pattern matches, or std::nullopt if no such substring can be
// established. Used as a prefilter: a line lacking this substring can never
// match the pattern, so the (relatively expensive) regex match can be
// skipped for that line.
//
// This is a deliberately conservative heuristic, not a full regex analyzer:
//   - Literal runs shorter than `min_length` are not reported.
//   - A run inside a group is only kept if the group (and every group
//     enclosing it) is not quantified with a possibly-zero repeat (*, ?, or
//     any {...} -- {m,n} is treated as possibly-zero even when m>=1, which
//     is conservative but simple and always safe).
//   - Any top-level or in-group alternation (|) discards every literal
//     candidate gathered in that group/level, since different branches may
//     share no common substring.
//   - `.`, `^`, `$`, and character classes always break a literal run.
//   - Recognized single-character escapes of punctuation (\. \( etc.) are
//     treated as that literal character; other escapes (\d, \w, \s, \b,
//     backreferences, ...) are not literal and break the run.
//   - Unbalanced parentheses/brackets or any other parsing surprise makes
//     this function give up and return std::nullopt.
// The only correctness property this function must uphold is: it must
// never claim a substring is required when the pattern could match without
// it. When in doubt, it returns std::nullopt (i.e. "always evaluate this
// regex"), which is always safe, just less selective.
std::optional<std::string> extract_required_literal(std::string_view pattern, size_t min_length = 3);

} // namespace mudclient
