#include "mudclient/alias_manager.hpp"

#include <algorithm>

namespace mudclient {

AliasManager::AliasManager(std::string separator) : separator_(std::move(separator)) {}

uint64_t AliasManager::add_alias(Kind kind, std::string pattern, Action action, Options opts) {
    AliasEntry entry;
    entry.id = next_id_++;
    entry.seq = next_seq_++;
    entry.kind = kind;
    entry.pattern_text = pattern;
    if (kind == Kind::Regex) {
        entry.regex.emplace(pattern, /*full_match=*/true);
    }
    entry.action = std::move(action);
    entry.opts = opts;
    uint64_t id = entry.id;
    aliases_.push_back(std::move(entry));
    resort();
    return id;
}

void AliasManager::remove_alias(uint64_t id) {
    aliases_.erase(std::remove_if(aliases_.begin(), aliases_.end(), [id](const AliasEntry& a) { return a.id == id; }),
                    aliases_.end());
}

bool AliasManager::exists(uint64_t id) const {
    return std::any_of(aliases_.begin(), aliases_.end(), [id](const AliasEntry& a) { return a.id == id; });
}

AliasManager::AliasEntry* AliasManager::find(uint64_t id) {
    for (auto& a : aliases_) {
        if (a.id == id) {
            return &a;
        }
    }
    return nullptr;
}

void AliasManager::resort() {
    std::stable_sort(aliases_.begin(), aliases_.end(), [](const AliasEntry& a, const AliasEntry& b) {
        if (a.opts.priority != b.opts.priority) {
            return a.opts.priority > b.opts.priority;
        }
        return a.seq < b.seq;
    });
}

std::string AliasManager::substitute_template(const std::string& tmpl, const std::vector<std::string>& captures) {
    std::string out;
    out.reserve(tmpl.size());
    for (size_t i = 0; i < tmpl.size(); ++i) {
        char c = tmpl[i];
        if (c == '$' && i + 1 < tmpl.size()) {
            char next = tmpl[i + 1];
            if (next == '$') {
                out.push_back('$');
                ++i;
                continue;
            }
            if (next >= '0' && next <= '9') {
                size_t idx = static_cast<size_t>(next - '0');
                if (idx < captures.size()) {
                    out += captures[idx];
                }
                ++i;
                continue;
            }
        }
        out.push_back(c);
    }
    return out;
}

std::vector<std::string> AliasManager::split_on_separator(const std::string& text) const {
    std::vector<std::string> parts;
    if (separator_.empty()) {
        parts.push_back(text);
        return parts;
    }
    size_t pos = 0;
    while (true) {
        size_t next = text.find(separator_, pos);
        std::string piece = (next == std::string::npos) ? text.substr(pos) : text.substr(pos, next - pos);
        size_t begin = piece.find_first_not_of(" \t");
        if (begin == std::string::npos) {
            piece.clear();
        } else {
            size_t end = piece.find_last_not_of(" \t");
            piece = piece.substr(begin, end - begin + 1);
        }
        if (!piece.empty()) {
            parts.push_back(std::move(piece));
        }
        if (next == std::string::npos) {
            break;
        }
        pos = next + separator_.size();
    }
    return parts;
}

AliasManager::ExpansionResult AliasManager::expand(std::string_view input) {
    ExpansionResult result;
    expand_recursive(std::string(input), 0, result);
    return result;
}

void AliasManager::expand_recursive(std::string command, int depth, ExpansionResult& result) {
    if (result.aborted) {
        return;
    }
    if (depth > kMaxRecursionDepth) {
        result.aborted = true;
        result.error = "alias expansion recursion limit exceeded (" + std::to_string(kMaxRecursionDepth) + ")";
        result.commands.clear();
        return;
    }

    // See TriggerManager::process_line for why this snapshots ids up front
    // and re-looks-up via find() rather than iterating aliases_ directly:
    // a FunctionAction may add/remove aliases while we're still evaluating
    // this same input.
    std::vector<uint64_t> ids;
    ids.reserve(aliases_.size());
    for (auto& a : aliases_) {
        ids.push_back(a.id);
    }

    bool matched_any = false;
    for (uint64_t id : ids) {
        AliasEntry* a = find(id);
        if (!a) {
            continue;
        }
        std::vector<std::string> captures;
        bool matched = false;
        if (a->kind == Kind::Exact) {
            matched = (a->pattern_text == command);
            if (matched) {
                captures = {command};
            }
        } else {
            matched = a->regex->match(command, captures);
        }
        if (!matched) {
            continue;
        }
        matched_any = true;

        // Copy the action and the options we still need out of `a` before
        // invoking it: a FunctionAction may itself add/remove aliases
        // (including this one), which can reallocate aliases_ and leave
        // `a` dangling for the remainder of this iteration.
        Action action = a->action;
        bool fall_through = a->opts.fall_through;

        std::string expansion;
        if (std::holds_alternative<std::string>(action)) {
            expansion = substitute_template(std::get<std::string>(action), captures);
        } else {
            expansion = std::get<FunctionAction>(action)(captures);
        }

        for (auto& cmd : split_on_separator(expansion)) {
            expand_recursive(cmd, depth + 1, result);
            if (result.aborted) {
                return;
            }
        }
        if (!fall_through) {
            return;
        }
    }

    if (!matched_any) {
        result.commands.push_back(std::move(command));
    }
}

} // namespace mudclient
