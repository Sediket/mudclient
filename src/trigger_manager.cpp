#include "mudclient/trigger_manager.hpp"

#include <algorithm>

namespace mudclient {

uint64_t TriggerManager::add_trigger(std::string pattern, Action action, Options opts) {
    TriggerEntry entry{next_id_++, next_seq_++, CompiledRegex(pattern, /*full_match=*/false), std::move(action),
                       opts, extract_required_literal(pattern)};
    uint64_t id = entry.id;
    triggers_.push_back(std::move(entry));
    resort();
    return id;
}

void TriggerManager::remove_trigger(uint64_t id) {
    triggers_.erase(std::remove_if(triggers_.begin(), triggers_.end(), [id](const TriggerEntry& t) { return t.id == id; }),
                     triggers_.end());
}

void TriggerManager::set_enabled(uint64_t id, bool enabled) {
    for (auto& t : triggers_) {
        if (t.id == id) {
            t.opts.enabled = enabled;
            return;
        }
    }
}

bool TriggerManager::exists(uint64_t id) const {
    return std::any_of(triggers_.begin(), triggers_.end(), [id](const TriggerEntry& t) { return t.id == id; });
}

TriggerManager::TriggerEntry* TriggerManager::find(uint64_t id) {
    for (auto& t : triggers_) {
        if (t.id == id) {
            return &t;
        }
    }
    return nullptr;
}

void TriggerManager::resort() {
    std::stable_sort(triggers_.begin(), triggers_.end(), [](const TriggerEntry& a, const TriggerEntry& b) {
        if (a.opts.priority != b.opts.priority) {
            return a.opts.priority > b.opts.priority;
        }
        return a.seq < b.seq;
    });
}

TriggerManager::MatchOutcome TriggerManager::process_line(const StyledLine& line) {
    MatchOutcome outcome;
    std::vector<uint64_t> to_remove;

    // Snapshot the ids to consider, in priority order, before running any
    // action. An Action may (via a Lua callback, in M3) add or remove
    // triggers -- including itself or others -- while we're still
    // iterating; re-looking-up each id through find() rather than holding
    // a reference/iterator across the action() call means such a mutation
    // can never leave us touching freed or reallocated memory. A trigger
    // added mid-line by such a callback simply doesn't get a chance to
    // match this same line, which is well-defined and safe; a trigger
    // removed mid-line is skipped via the find() == nullptr check below.
    std::vector<uint64_t> ids;
    ids.reserve(triggers_.size());
    for (auto& t : triggers_) {
        ids.push_back(t.id);
    }

    for (uint64_t id : ids) {
        TriggerEntry* t = find(id);
        if (!t || !t->opts.enabled) {
            continue;
        }
        if (line.is_prompt && !t->opts.match_prompts) {
            continue;
        }
        if (t->required_literal && line.plain.find(*t->required_literal) == std::string::npos) {
            continue; // prefiltered: this literal must appear if the regex could match, and it doesn't
        }
        std::vector<std::string> captures;
        if (!t->regex.match(line.plain, captures)) {
            continue;
        }

        ++outcome.fired_count;
        if (t->opts.gag) {
            outcome.gag = true;
        }
        if (t->opts.recolor) {
            outcome.recolor = true;
            outcome.recolor_style = t->opts.recolor_style;
        }
        bool once = t->opts.once;
        bool stop_processing = t->opts.stop_processing;
        Action action = t->action; // copy: `t` may dangle after action() mutates triggers_

        if (action) {
            action(captures);
        }
        if (once) {
            to_remove.push_back(id);
        }
        if (stop_processing) {
            break;
        }
    }

    for (uint64_t id : to_remove) {
        remove_trigger(id);
    }
    return outcome;
}

} // namespace mudclient
