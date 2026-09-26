#include "mudclient/network_client.hpp"

#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace mudclient {

namespace {
// A partial line left unterminated this long is flushed as a prompt.
constexpr auto kIdleFlushThreshold = std::chrono::milliseconds(250);
} // namespace

// Lifetime strategy: every async operation's completion handler captures
// `self = shared_from_this()`, so this object cannot be destroyed while any
// handler is still queued or running on the io_context. disconnect()/fail()
// set `stopped_` and cancel/close every I/O object; the resulting handlers
// complete with operation_aborted (or another error) and return without
// touching any state beyond reading `stopped_`. All members are touched
// only on the network thread (the io_context's run() thread).

NetworkClient::NetworkClient(asio::io_context& io, EventQueue& events)
    : resolver_(io), socket_(io), connect_timer_(io), idle_timer_(io), events_(events) {}

void NetworkClient::connect(std::string host, std::string port, std::chrono::milliseconds connect_timeout) {
    auto self = shared_from_this();

    connect_timer_.expires_after(connect_timeout);
    connect_timer_.async_wait([this, self](const asio::error_code& ec) {
        if (ec == asio::error::operation_aborted || stopped_) {
            return;
        }
        fail("connect timeout");
        asio::error_code ignored;
        resolver_.cancel();
        socket_.close(ignored);
    });

    resolver_.async_resolve(
        host, port, [this, self](const asio::error_code& ec, asio::ip::tcp::resolver::results_type results) {
            if (stopped_) {
                return;
            }
            if (ec) {
                connect_timer_.cancel();
                fail("resolve failed: " + ec.message());
                return;
            }
            asio::async_connect(socket_, results,
                                [this, self](const asio::error_code& connect_ec, const asio::ip::tcp::endpoint&) {
                                    if (stopped_) {
                                        return;
                                    }
                                    connect_timer_.cancel();
                                    if (connect_ec) {
                                        fail("connect failed: " + connect_ec.message());
                                        return;
                                    }
                                    connected_ = true;
                                    // Thread handoff: Connected is pushed from the network
                                    // thread; the engine thread owns it after pop.
                                    events_.push(Connected{});
                                    start_read();
                                });
        });
}

void NetworkClient::send_line(std::string line) {
    std::string escaped;
    escaped.reserve(line.size() + 2);
    for (char c : line) {
        if (static_cast<uint8_t>(c) == 0xFF) {
            escaped.push_back(c); // IAC IAC escapes a literal 0xFF
        }
        escaped.push_back(c);
    }
    escaped += "\r\n";
    enqueue_raw(std::move(escaped));
}

void NetworkClient::disconnect() {
    if (stopped_) {
        return;
    }
    stopped_ = true;
    connected_ = false;
    asio::error_code ignored;
    connect_timer_.cancel();
    idle_timer_.cancel();
    resolver_.cancel();
    socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);
    events_.push(Disconnected{"disconnected by user"});
}

void NetworkClient::fail(const std::string& reason) {
    if (stopped_) {
        return;
    }
    stopped_ = true;
    connected_ = false;
    asio::error_code ignored;
    idle_timer_.cancel();
    socket_.close(ignored);
    events_.push(Disconnected{reason});
}

void NetworkClient::start_read() {
    auto self = shared_from_this();
    socket_.async_read_some(asio::buffer(read_buffer_), [this, self](const asio::error_code& ec, size_t n) {
        if (stopped_) {
            return;
        }
        if (ec) {
            fail(ec == asio::error::eof ? "connection closed by server" : "read error: " + ec.message());
            return;
        }

        std::vector<TelnetParser::OutputEvent> out_events;
        std::string out_to_send;
        parser_.feed(std::span<const uint8_t>(read_buffer_.data(), n), out_events, out_to_send);
        dispatch_output_events(out_events);
        if (!out_to_send.empty()) {
            enqueue_raw(std::move(out_to_send));
        }
        start_idle_timer();
        start_read();
    });
}

void NetworkClient::dispatch_output_events(std::vector<TelnetParser::OutputEvent>& out_events) {
    for (auto& ev : out_events) {
        std::visit(
            [this](auto&& value) {
                using T = std::decay_t<decltype(value)>;
                // Thread handoff: moved into the queue from the network
                // thread; owned by the engine thread after pop.
                if constexpr (std::is_same_v<T, StyledLine>) {
                    events_.push(LineReceived{std::move(value)});
                } else if constexpr (std::is_same_v<T, GmcpMessage>) {
                    events_.push(GmcpReceived{std::move(value.package), std::move(value.json)});
                }
            },
            ev);
    }
}

void NetworkClient::start_idle_timer() {
    if (!parser_.has_pending_partial_line()) {
        idle_timer_.cancel();
        return;
    }
    // expires_after() cancels any wait already pending on this timer, so
    // each new chunk of data restarts the 250ms window.
    auto self = shared_from_this();
    idle_timer_.expires_after(kIdleFlushThreshold);
    idle_timer_.async_wait([this, self](const asio::error_code& ec) {
        if (ec == asio::error::operation_aborted || stopped_) {
            return;
        }
        std::vector<TelnetParser::OutputEvent> out_events;
        if (parser_.flush_idle(out_events)) {
            dispatch_output_events(out_events);
        }
    });
}

void NetworkClient::enqueue_raw(std::string bytes) {
    if (stopped_) {
        return;
    }
    write_queue_.push_back(std::move(bytes));
    if (!write_in_progress_) {
        do_write();
    }
}

void NetworkClient::do_write() {
    if (write_queue_.empty()) {
        write_in_progress_ = false;
        return;
    }
    write_in_progress_ = true;
    auto self = shared_from_this();
    asio::async_write(socket_, asio::buffer(write_queue_.front()), [this, self](const asio::error_code& ec, size_t) {
        if (stopped_) {
            return;
        }
        if (ec) {
            fail("write error: " + ec.message());
            return;
        }
        write_queue_.pop_front();
        do_write();
    });
}

} // namespace mudclient
