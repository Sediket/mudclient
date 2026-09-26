#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include <asio.hpp>

#include "mudclient/event_queue.hpp"

// Owns one asio::steady_timer per active timer. Lives on the network
// thread (per the spec's threading model: the network thread owns every
// asio::steady_timer). Every public method assumes it runs on the network
// thread; the engine thread must reach them only via asio::post(io_context,
// ...), exactly like NetworkClient::send_line. TimerFired events are pushed
// onto the (thread-safe) EventQueue for the engine thread to consume; Lua
// callbacks for a fired timer run on the engine thread only.
//
// Design note: one asio::steady_timer per timer (rather than a timer
// wheel) was chosen because the spec's target load is modest (a handful to
// low hundreds of user-registered timers, not thousands), asio's
// steady_timer is O(log n) per operation via a heap internally, and this
// keeps cancellation/pause/resume/reset trivially correct per-timer instead
// of needing wheel-bucket bookkeeping.
namespace mudclient {

class TimerManager {
public:
    TimerManager(asio::io_context& io, EventQueue& events);

    // Registers a new timer and returns its id. IDs are monotonically
    // increasing and never reused. `label`, if given, must be unique;
    // re-registering an existing label throws std::invalid_argument.
    uint64_t add_timer(std::chrono::milliseconds interval, bool repeating,
                        std::optional<std::string> label = std::nullopt);

    // All of these are no-ops if `id`/`label` doesn't name a live timer
    // (per spec: killing/operating on an already-gone timer is silently
    // ignored, never an error that could crash a Lua callback).
    void kill(uint64_t id);
    void pause(uint64_t id);
    void resume(uint64_t id);
    void reset(uint64_t id);

    // Killing a timer from inside its own TimerFired handler (i.e. a Lua
    // callback that posts kill() back to this thread while processing the
    // TimerFired event this same timer just produced) is safe: the
    // generation check plus by-value id/generation capture in the pending
    // async_wait handler means the handler either hasn't run yet (and will
    // find the timer gone) or has already run and rescheduled under a new
    // generation (and kill() cancels that new wait cleanly).

    std::optional<uint64_t> id_for_label(const std::string& label) const;
    bool exists(uint64_t id) const { return timers_.find(id) != timers_.end(); }
    size_t active_count() const { return timers_.size(); }

private:
    struct TimerEntry {
        explicit TimerEntry(asio::io_context& io) : timer(io) {}
        asio::steady_timer timer;
        std::chrono::milliseconds interval{};
        bool repeating = false;
        bool paused = false;
        uint64_t generation = 0;
        std::chrono::steady_clock::time_point deadline{};
        std::chrono::milliseconds remaining_when_paused{};
        std::optional<std::string> label;
    };

    void schedule(uint64_t id);
    void handle_fire(uint64_t id, uint64_t expected_generation, const asio::error_code& ec);

    asio::io_context& io_;
    EventQueue& events_;
    std::unordered_map<uint64_t, std::unique_ptr<TimerEntry>> timers_;
    std::unordered_map<std::string, uint64_t> labels_;
    uint64_t next_id_ = 1;
};

} // namespace mudclient
