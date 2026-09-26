#include <catch2/catch_test_macros.hpp>

#include "mudclient/regex_pattern.hpp"

using namespace mudclient;

TEST_CASE("CompiledRegex: substring match with captures", "[regex]") {
    CompiledRegex re(R"(^(\w+) (hits|misses) you)");
    std::vector<std::string> captures;
    REQUIRE(re.match("Orc hits you for 5 damage", captures));
    REQUIRE(captures.size() == 3);
    CHECK(captures[0] == "Orc hits you");
    CHECK(captures[1] == "Orc");
    CHECK(captures[2] == "hits");
}

TEST_CASE("CompiledRegex: no match returns false and leaves captures alone semantics", "[regex]") {
    CompiledRegex re(R"(^nomatch$)");
    std::vector<std::string> captures;
    CHECK_FALSE(re.match("something else", captures));
}

TEST_CASE("CompiledRegex: full_match requires the whole subject to match", "[regex]") {
    CompiledRegex re("east", /*full_match=*/true);
    std::vector<std::string> captures;
    CHECK(re.match("east", captures));
    CHECK_FALSE(re.match("east wall", captures));
    CHECK_FALSE(re.match("go east", captures));
}

TEST_CASE("CompiledRegex: non-full-match finds a substring anywhere", "[regex]") {
    CompiledRegex re("east");
    std::vector<std::string> captures;
    CHECK(re.match("go east now", captures));
}

TEST_CASE("CompiledRegex: invalid pattern throws RegexCompileError", "[regex]") {
    CHECK_THROWS_AS(CompiledRegex("(unterminated"), RegexCompileError);
}

TEST_CASE("CompiledRegex: a non-participating group before a participating one reports as empty", "[regex]") {
    // PCRE2's ovector count (pcre2_match's return code) only extends up to
    // the highest-numbered group that actually participated; a *trailing*
    // non-participating group is simply not reported at all (rc excludes
    // it), but one sandwiched before a group that did participate must
    // come back as an empty string rather than being silently dropped.
    CompiledRegex re(R"(^a(b)?(c)$)", true);
    std::vector<std::string> captures;
    REQUIRE(re.match("ac", captures));
    REQUIRE(captures.size() == 3);
    CHECK(captures[1].empty()); // group 1 (b) did not participate
    CHECK(captures[2] == "c");
}

TEST_CASE("CompiledRegex: a trailing non-participating group is not reported", "[regex]") {
    CompiledRegex re(R"(^a(b)?c$)", true);
    std::vector<std::string> captures;
    REQUIRE(re.match("ac", captures));
    REQUIRE(captures.size() == 1); // only the whole match; group 1 never participated and is trailing
}

TEST_CASE("extract_required_literal: plain trailing literal outside any group", "[prefilter]") {
    auto lit = extract_required_literal(R"(^(\w+) (hits|misses) you)");
    REQUIRE(lit.has_value());
    CHECK(*lit == " you");
}

TEST_CASE("extract_required_literal: no literal at all", "[prefilter]") {
    CHECK_FALSE(extract_required_literal(R"(^(\w+)$)").has_value());
    CHECK_FALSE(extract_required_literal(R"(\d+)").has_value());
}

TEST_CASE("extract_required_literal: top-level alternation discards candidates", "[prefilter]") {
    // Neither "hello" nor "goodbye" is required since either branch alone
    // can match.
    CHECK_FALSE(extract_required_literal("hello|goodbye").has_value());
}

TEST_CASE("extract_required_literal: literal followed by * or ? is trimmed", "[prefilter]") {
    auto lit = extract_required_literal("abcd?");
    REQUIRE(lit.has_value());
    CHECK(*lit == "abc"); // trailing 'd' not guaranteed
}

TEST_CASE("extract_required_literal: literal followed by + keeps the whole run", "[prefilter]") {
    auto lit = extract_required_literal("abcd+");
    REQUIRE(lit.has_value());
    CHECK(*lit == "abcd"); // 'd' guaranteed at least once
}

TEST_CASE("extract_required_literal: group with alternation inside is never promoted", "[prefilter]") {
    // "(hits|misses)" alone can't supply a required literal, but the
    // literal " you" after the group still can.
    auto lit = extract_required_literal(R"((hits|misses) you)");
    REQUIRE(lit.has_value());
    CHECK(*lit == " you");
}

TEST_CASE("extract_required_literal: literal inside a group quantified with * is not promoted", "[prefilter]") {
    // This is the case a naive implementation gets wrong: "roomdesc" here
    // is NOT required, since the whole group (and therefore "roomdesc")
    // may occur zero times.
    auto lit = extract_required_literal("(roomdesc)*end");
    REQUIRE(lit.has_value());
    CHECK(*lit == "end"); // only the literal *outside* the optional group is required
}

TEST_CASE("extract_required_literal: literal inside a group quantified with + is promoted", "[prefilter]") {
    // The group occurs at least once, so its literal content is guaranteed.
    auto lit = extract_required_literal("(roomdescription)+end");
    REQUIRE(lit.has_value());
    // Longest candidate is picked; "roomdescription" (15 chars) beats "end" (3).
    CHECK(*lit == "roomdescription");
}

TEST_CASE("extract_required_literal: nested optional group discards inner literal even under an outer +",
          "[prefilter]") {
    // The inner "abcdefgh" is inside a group quantified with '?', so it
    // must never be the reported literal -- "abcdefgh" appearing in the
    // pattern's source text is not the property under test; what matters
    // is that whatever literal IS reported is actually required. "xyz" and
    // "tail" are each independently required here (the outer group has no
    // deflating quantifier); the implementation may report either one (it
    // picks the longer), but never "abcdefgh".
    auto lit = extract_required_literal("((abcdefgh)?xyz)tail");
    REQUIRE(lit.has_value());
    CHECK(*lit != "abcdefgh");
    CHECK((*lit == "xyz" || *lit == "tail"));
}

TEST_CASE("extract_required_literal: escaped metacharacters are literal", "[prefilter]") {
    auto lit = extract_required_literal(R"(cost\: \$100 exactly)");
    REQUIRE(lit.has_value());
    CHECK(*lit == "cost: $100 exactly");
}

TEST_CASE("extract_required_literal: character class breaks a run and contributes nothing", "[prefilter]") {
    auto lit = extract_required_literal(R"(hit[sz] you hard)");
    REQUIRE(lit.has_value());
    CHECK(*lit == " you hard"); // longest of "hit" (3) and " you hard" (9)
}

TEST_CASE("extract_required_literal: unbalanced parens falls back to no literal", "[prefilter]") {
    CHECK_FALSE(extract_required_literal("abc(def").has_value());
    CHECK_FALSE(extract_required_literal("abc)def").has_value());
}

TEST_CASE("extract_required_literal: short runs below min_length are not reported", "[prefilter]") {
    CHECK_FALSE(extract_required_literal("ab", 3).has_value());
    auto lit = extract_required_literal("ab", 2);
    REQUIRE(lit.has_value());
    CHECK(*lit == "ab");
}

TEST_CASE("extract_required_literal: dot and anchors break runs but surrounding text still counts", "[prefilter]") {
    auto lit = extract_required_literal("^longprefixtext.*longsuffixtext$");
    REQUIRE(lit.has_value());
    // Both "longprefixtext" and "longsuffixtext" are valid candidates (14
    // chars each); either is an acceptable answer as long as it's actually
    // required. Confirm whichever was picked really is one of them.
    CHECK((*lit == "longprefixtext" || *lit == "longsuffixtext"));
}
