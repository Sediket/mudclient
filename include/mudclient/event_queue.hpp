#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>

#include "mudclient/events.hpp"

namespace mudclient {

// Thread-safe MPSC queue of Event values.
//
// Producers: the network thread (Connected/Disconnected/LineReceived/
// GmcpReceived/TimerFired) and the stdin thread (UserInput). Both may call
// push() concurrently.
//
// Consumer: the engine thread only, via pop_wait(). It owns every Event
// once popped; producers must not retain references to pushed values.
class EventQueue {
public:
    void push(Event event) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(event));
        }
        cv_.notify_one();
    }

    // Blocks up to `timeout` for an event to arrive; returns std::nullopt on
    // timeout so the engine loop never busy-spins but can still service
    // periodic bookkeeping. Engine thread only.
    std::optional<Event> pop_wait(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, timeout, [this] { return !queue_.empty(); })) {
            return std::nullopt;
        }
        Event event = std::move(queue_.front());
        queue_.pop_front();
        return event;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Event> queue_;
};

} // namespace mudclient
