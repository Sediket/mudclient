#pragma once

#include <array>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>

#include <asio.hpp>

#include "mudclient/event_queue.hpp"
#include "mudclient/telnet_parser.hpp"

// Owns the socket, the telnet parser, and every asio::steady_timer used for
// networking. Runs entirely on the network thread's io_context; the only
// cross-thread interactions are: pushing Event values into the shared
// EventQueue (thread-safe by construction), and accepting work posted onto
// io_context from the engine thread via asio::post (send_line, connect,
// disconnect are all safe to call that way).
namespace mudclient {

class NetworkClient : public std::enable_shared_from_this<NetworkClient> {
public:
    NetworkClient(asio::io_context& io, EventQueue& events);

    // Begins async resolve + connect. Safe to call via asio::post from any
    // thread; must actually run on the network thread (the io_context's
    // thread), which asio::post guarantees.
    void connect(std::string host, std::string port, std::chrono::milliseconds connect_timeout);

    // Queues a line for sending. Appends "\r\n" and escapes any literal
    // 0xFF byte as IAC IAC. Safe to call via asio::post only.
    void send_line(std::string line);

    // Cancels all pending operations and closes the socket. Safe to call
    // via asio::post only. Lifetime: every async handler captures
    // shared_from_this(), so the NetworkClient stays alive until every
    // in-flight handler has run (each observing asio::error::operation_aborted
    // and returning early), even if the last external shared_ptr is dropped
    // immediately after calling disconnect().
    void disconnect();

    bool connected() const { return connected_; }

    // Optional --record sink: called with every raw inbound chunk exactly
    // as read from the socket (before telnet/ANSI parsing, preserving
    // original chunk boundaries) and every outbound line (as passed to
    // send_line, before \r\n termination/0xFF escaping). Set before
    // connect(); safe to call only on the network thread (i.e. via
    // asio::post like every other NetworkClient entry point).
    using Recorder = std::function<void(bool inbound, std::span<const uint8_t> data)>;
    void set_recorder(Recorder recorder) { recorder_ = std::move(recorder); }

private:
    void start_read();
    void do_write();
    void enqueue_raw(std::string bytes);
    void start_idle_timer();
    void dispatch_output_events(std::vector<TelnetParser::OutputEvent>& out_events);
    void fail(const std::string& reason);

    asio::ip::tcp::resolver resolver_;
    asio::ip::tcp::socket socket_;
    asio::steady_timer connect_timer_;
    asio::steady_timer idle_timer_;

    std::array<uint8_t, 16384> read_buffer_{};
    std::deque<std::string> write_queue_;
    bool write_in_progress_ = false;

    TelnetParser parser_;
    EventQueue& events_;
    Recorder recorder_;

    bool connected_ = false;
    bool stopped_ = false;
};

} // namespace mudclient
