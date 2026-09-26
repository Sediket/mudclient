#include "mudclient/regex_pattern.hpp"

#include <cctype>

namespace mudclient {

namespace {
std::string wrap_full_match(std::string_view pattern) {
    std::string wrapped;
    wrapped.reserve(pattern.size() + 8);
    wrapped += "\\A(?:";
    wrapped += pattern;
    wrapped += ")\\z";
    return wrapped;
}
} // namespace

CompiledRegex::CompiledRegex(std::string_view pattern, bool full_match)
    : source_(pattern) {
    std::string compiled_source = full_match ? wrap_full_match(pattern) : source_;

    int error_code = 0;
    PCRE2_SIZE error_offset = 0;
    pcre2_code* raw = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(compiled_source.data()), compiled_source.size(),
                                     PCRE2_UTF | PCRE2_UCP, &error_code, &error_offset, nullptr);
    if (!raw) {
        PCRE2_UCHAR buffer[256];
        pcre2_get_error_message(error_code, buffer, sizeof(buffer));
        throw RegexCompileError("PCRE2 compile error at offset " + std::to_string(error_offset) + ": " +
                                 reinterpret_cast<const char*>(buffer));
    }
    code_.reset(raw);

    // JIT is a pure optimization: pcre2_match() transparently falls back to
    // the interpreter if JIT compilation isn't available on this platform
    // or for this pattern, so a failure here is not an error.
    pcre2_jit_compile(code_.get(), PCRE2_JIT_COMPLETE);

    match_data_.reset(pcre2_match_data_create_from_pattern(code_.get(), nullptr));
}

bool CompiledRegex::match(std::string_view subject, std::vector<std::string>& captures_out) const {
    int rc = pcre2_match(code_.get(), reinterpret_cast<PCRE2_SPTR>(subject.data()), subject.size(), 0, 0,
                          match_data_.get(), nullptr);
    if (rc < 0) {
        return false; // PCRE2_ERROR_NOMATCH or another failure; treat as no match
    }
    captures_out.clear();
    PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(match_data_.get());
    uint32_t pair_count = pcre2_get_ovector_count(match_data_.get());
    // rc == 0 means the ovector was too small for all groups; in that case
    // pcre2_get_ovector_count() still reports the (larger) allocated
    // capacity, so clamp to what pcre2_match() actually reported when > 0.
    uint32_t used_pairs = rc > 0 ? static_cast<uint32_t>(rc) : pair_count;
    for (uint32_t i = 0; i < used_pairs && i < pair_count; ++i) {
        PCRE2_SIZE start = ovector[2 * i];
        PCRE2_SIZE end = ovector[2 * i + 1];
        if (start == PCRE2_UNSET || end == PCRE2_UNSET) {
            captures_out.emplace_back(); // group didn't participate
        } else {
            captures_out.emplace_back(subject.substr(start, end - start));
        }
    }
    return true;
}

namespace {

struct LiteralFrame {
    std::vector<std::string> candidates;
    bool has_alternation = false;
};

// Removes the last UTF-8 codepoint (1-4 bytes) from `s`, not just its last
// byte. Patterns are compiled with PCRE2_UTF, so a "single character"
// quantified with */?/{ (not guaranteed present) can be a multi-byte
// sequence; popping only one byte would leave its leading byte(s)
// misclassified as part of a "required" literal, which is exactly the
// property extract_required_literal() must never get wrong.
void pop_last_utf8_codepoint(std::string& s) {
    if (s.empty()) {
        return;
    }
    size_t i = s.size() - 1;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) {
        --i; // continuation byte (10xxxxxx): keep walking back to the lead byte
    }
    s.resize(i);
}

} // namespace

std::optional<std::string> extract_required_literal(std::string_view pattern, size_t min_length) {
    std::vector<LiteralFrame> stack;
    stack.emplace_back();
    std::string current_run;
    bool malformed = false;

    auto flush_run = [&] {
        if (current_run.size() >= min_length) {
            stack.back().candidates.push_back(current_run);
        }
        current_run.clear();
    };

    bool in_class = false;
    size_t i = 0;
    while (i < pattern.size() && !malformed) {
        char c = pattern[i];

        if (in_class) {
            if (c == '\\' && i + 1 < pattern.size()) {
                i += 2;
                continue;
            }
            if (c == ']') {
                in_class = false;
            }
            ++i;
            continue;
        }

        if (c == '\\') {
            if (i + 1 >= pattern.size()) {
                flush_run();
                malformed = true;
                break;
            }
            char escaped = pattern[i + 1];
            // PCRE2 (like most regex flavors): a backslash followed by a
            // non-alphanumeric character always means that character
            // taken literally (e.g. \. \$ \\ \:), so it continues the
            // current run rather than breaking it. A backslash followed by
            // a letter or digit is a class/escape/backreference (\d \w \s
            // \b \1 ...), which is not literal and breaks the run.
            if (!std::isalnum(static_cast<unsigned char>(escaped))) {
                current_run.push_back(escaped);
            } else {
                flush_run();
            }
            i += 2;
            continue;
        }

        if (c == '[') {
            flush_run();
            in_class = true;
            ++i;
            // A ']' immediately after '[' or '[^' is a literal member of
            // the class, not its closing bracket.
            if (i < pattern.size() && pattern[i] == '^') ++i;
            if (i < pattern.size() && pattern[i] == ']') ++i;
            continue;
        }

        if (c == '(') {
            flush_run();
            stack.emplace_back();
            ++i;
            continue;
        }

        if (c == ')') {
            flush_run();
            if (stack.size() <= 1) {
                malformed = true; // unbalanced
                break;
            }
            LiteralFrame closed = std::move(stack.back());
            stack.pop_back();

            bool zero_min = false;
            if (i + 1 < pattern.size()) {
                char q = pattern[i + 1];
                if (q == '*' || q == '?' || q == '{') {
                    zero_min = true; // conservative: {m,n} treated as possibly-zero too
                }
            }
            if (!closed.has_alternation && !zero_min) {
                for (auto& candidate : closed.candidates) {
                    stack.back().candidates.push_back(std::move(candidate));
                }
            }
            ++i;
            continue;
        }

        if (c == '|') {
            flush_run();
            stack.back().has_alternation = true;
            stack.back().candidates.clear();
            ++i;
            continue;
        }

        if (c == '*' || c == '+' || c == '?' || c == '{') {
            if (!current_run.empty()) {
                if (c != '+') {
                    pop_last_utf8_codepoint(current_run); // not guaranteed present
                }
                flush_run();
            }
            if (c == '{') {
                size_t close = pattern.find('}', i + 1);
                if (close == std::string_view::npos) {
                    malformed = true;
                    break;
                }
                i = close + 1;
            } else {
                ++i;
            }
            continue;
        }

        if (c == '.' || c == '^' || c == '$') {
            flush_run();
            ++i;
            continue;
        }

        current_run.push_back(c);
        ++i;
    }
    flush_run();

    if (malformed || in_class || stack.size() != 1 || stack.front().has_alternation) {
        return std::nullopt;
    }

    std::optional<std::string> best;
    for (auto& candidate : stack.front().candidates) {
        if (!best || candidate.size() > best->size()) {
            best = candidate;
        }
    }
    return best;
}

} // namespace mudclient
