#include "mudclient/timer_manager.hpp"

#include <stdexcept>

namespace mudclient {

TimerManager::TimerManager(asio::io_context& io, EventQueue& events) : io_(io), events_(events) {}

uint64_t TimerManager::add_timer(std::chrono::milliseconds interval, bool repeating,
                                  std::optional<std::string> label) {
    if (label && labels_.contains(*label)) {
        throw std::invalid_argument("timer label already in use: " + *label);
    }
    uint64_t id = next_id_++;
    auto entry = std::make_unique<TimerEntry>(io_);
    entry->interval = interval;
    entry->repeating = repeating;
    entry->deadline = std::chrono::steady_clock::now() + interval;
    entry->label = label;
    if (label) {
        labels_.emplace(*label, id);
    }
    timers_.emplace(id, std::move(entry));
    schedule(id);
    return id;
}

void TimerManager::schedule(uint64_t id) {
    auto it = timers_.find(id);
    if (it == timers_.end()) {
        return;
    }
    TimerEntry& entry = *it->second;
    uint64_t generation = entry.generation;
    entry.timer.expires_at(entry.deadline);
    entry.timer.async_wait([this, id, generation](const asio::error_code& ec) { handle_fire(id, generation, ec); });
}

void TimerManager::handle_fire(uint64_t id, uint64_t expected_generation, const asio::error_code& ec) {
    if (ec == asio::error::operation_aborted) {
        return; // cancelled by pause/resume/reset/kill; that call already did whatever was needed
    }
    auto it = timers_.find(id);
    if (it == timers_.end()) {
        return; // killed before this handler ran
    }
    TimerEntry& entry = *it->second;
    if (entry.generation != expected_generation) {
        return; // superseded by a pause/resume/reset since this wait was scheduled
    }

    events_.push(TimerFired{id});

    if (!entry.repeating) {
        timers_.erase(it);
        return;
    }

    // Repeating: schedule from the previous deadline, not now(), so ticks
    // don't drift. If the callback (or event-queue backpressure) made us
    // miss one or more entire intervals, skip ahead to the next deadline
    // that is still in the future rather than bursting out every missed
    // tick back-to-back.
    auto now = std::chrono::steady_clock::now();
    entry.deadline += entry.interval;
    if (entry.deadline <= now && entry.interval.count() > 0) {
        auto behind = now - entry.deadline;
        auto periods_missed = behind / entry.interval + 1;
        entry.deadline += entry.interval * periods_missed;
    }
    schedule(id);
}

void TimerManager::kill(uint64_t id) {
    auto it = timers_.find(id);
    if (it == timers_.end()) {
        return;
    }
    if (it->second->label) {
        labels_.erase(*it->second->label);
    }
    it->second->timer.cancel(); // pending handler will see operation_aborted (or find the id gone below)
    timers_.erase(it);
}

void TimerManager::pause(uint64_t id) {
    auto it = timers_.find(id);
    if (it == timers_.end()) {
        return;
    }
    TimerEntry& entry = *it->second;
    if (entry.paused) {
        return;
    }
    auto now = std::chrono::steady_clock::now();
    entry.remaining_when_paused = entry.deadline > now
                                       ? std::chrono::duration_cast<std::chrono::milliseconds>(entry.deadline - now)
                                       : std::chrono::milliseconds{0};
    entry.paused = true;
    ++entry.generation;
    entry.timer.cancel();
}

void TimerManager::resume(uint64_t id) {
    auto it = timers_.find(id);
    if (it == timers_.end()) {
        return;
    }
    TimerEntry& entry = *it->second;
    if (!entry.paused) {
        return;
    }
    entry.paused = false;
    entry.deadline = std::chrono::steady_clock::now() + entry.remaining_when_paused;
    ++entry.generation;
    schedule(id);
}

void TimerManager::reset(uint64_t id) {
    auto it = timers_.find(id);
    if (it == timers_.end()) {
        return;
    }
    TimerEntry& entry = *it->second;
    entry.paused = false;
    entry.deadline = std::chrono::steady_clock::now() + entry.interval;
    ++entry.generation;
    schedule(id);
}

std::optional<uint64_t> TimerManager::id_for_label(const std::string& label) const {
    auto it = labels_.find(label);
    if (it == labels_.end()) {
        return std::nullopt;
    }
    return it->second;
}

} // namespace mudclient
