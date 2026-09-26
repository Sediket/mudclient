// Integration tests for NetworkClient against a real loopback TCP socket
// (a minimal in-process fake server), exercising connect/read/write/
// disconnect and the negotiation/idle-flush paths end to end.
#include <catch2/catch_test_macros.hpp>

#include <asio.hpp>
#include <chrono>
#include <thread>

#include "mudclient/event_queue.hpp"
#include "mudclient/network_client.hpp"

using namespace mudclient;
using namespace std::chrono_literals;

namespace {

// Runs an io_context on a background thread until stopped; used to host the
// NetworkClient under test, mirroring the real network-thread setup.
struct IoThread {
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> guard = asio::make_work_guard(io);
    std::thread thread{[this] { io.run(); }};

    ~IoThread() {
        guard.reset();
        io.stop();
        thread.join();
    }
};

std::optional<Event> wait_for_event(EventQueue& q, std::chrono::milliseconds timeout = 2s) {
    return q.pop_wait(timeout);
}

} // namespace

TEST_CASE("NetworkClient connects, exchanges lines, and reports disconnect", "[network_client]") {
    asio::io_context server_io;
    asio::ip::tcp::acceptor acceptor(server_io, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
    uint16_t port = acceptor.local_endpoint().port();

    std::string received_from_client;
    asio::ip::tcp::socket server_socket(server_io);
    acceptor.async_accept(server_socket, [&](const asio::error_code&) {
        auto buf = std::make_shared<asio::streambuf>();
        asio::async_read_until(server_socket, *buf, "\r\n",
                               [&, buf](const asio::error_code& ec, size_t n) {
                                   if (!ec) {
                                       std::istream is(buf.get());
                                       std::string line(n, '\0');
                                       is.read(line.data(), static_cast<std::streamsize>(n));
                                       received_from_client = line;
                                   }
                                   asio::write(server_socket, asio::buffer(std::string("hello there\r\n")));
                               });
    });
    std::thread server_thread([&] { server_io.run(); });

    EventQueue events;
    IoThread client_io;
    auto client = std::make_shared<NetworkClient>(client_io.io, events);
    asio::post(client_io.io, [&] { client->connect("127.0.0.1", std::to_string(port), 2000ms); });

    auto connected = wait_for_event(events);
    REQUIRE(connected.has_value());
    REQUIRE(std::holds_alternative<Connected>(*connected));

    asio::post(client_io.io, [&] { client->send_line("hi\xFF" "server"); });

    auto line_event = wait_for_event(events);
    REQUIRE(line_event.has_value());
    REQUIRE(std::holds_alternative<LineReceived>(*line_event));
    CHECK(std::get<LineReceived>(*line_event).line.plain == "hello there");

    asio::post(client_io.io, [&] { client->disconnect(); });
    auto disconnected = wait_for_event(events);
    REQUIRE(disconnected.has_value());
    REQUIRE(std::holds_alternative<Disconnected>(*disconnected));

    server_io.stop();
    server_thread.join();
    CHECK(received_from_client == std::string("hi\xFF\xFF" "server\r\n"));
}

TEST_CASE("NetworkClient reports Disconnected within the connect timeout bound", "[network_client]") {
    // 192.0.2.0/24 is TEST-NET-1 (RFC 5737): guaranteed unroutable. Sandboxed
    // network stacks vary in whether this hangs (until our timer fires) or
    // fails fast (e.g. "network unreachable"); either way NetworkClient must
    // report a single Disconnected well within the bound, never hang.
    EventQueue events;
    IoThread client_io;
    auto client = std::make_shared<NetworkClient>(client_io.io, events);
    auto start = std::chrono::steady_clock::now();
    asio::post(client_io.io, [&] { client->connect("192.0.2.1", "1", 200ms); });

    auto result = wait_for_event(events, 5s);
    auto elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE(result.has_value());
    REQUIRE(std::holds_alternative<Disconnected>(*result));
    CHECK_FALSE(std::get<Disconnected>(*result).reason.empty());
    CHECK(elapsed < 5s);
}

TEST_CASE("NetworkClient survives disconnect racing with in-flight I/O", "[network_client][concurrency]") {
    // Regression guard for use-after-free: disconnect() is posted
    // immediately after connect governs, so handlers for the connect,
    // read, and idle timers are all potentially in flight when the socket
    // is torn down. shared_from_this() in every handler must keep the
    // object alive until asio finishes delivering operation_aborted.
    asio::io_context server_io;
    asio::ip::tcp::acceptor acceptor(server_io, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
    uint16_t port = acceptor.local_endpoint().port();
    asio::ip::tcp::socket server_socket(server_io);
    acceptor.async_accept(server_socket, [](const asio::error_code&) {});
    std::thread server_thread([&] { server_io.run_for(500ms); });

    for (int i = 0; i < 20; ++i) {
        EventQueue events;
        IoThread client_io;
        auto client = std::make_shared<NetworkClient>(client_io.io, events);
        asio::post(client_io.io, [&] { client->connect("127.0.0.1", std::to_string(port), 500ms); });
        asio::post(client_io.io, [&] { client->disconnect(); });
        wait_for_event(events, 1s);
    }

    server_thread.join();
    SUCCEED("no crash/UAF across repeated connect+immediate-disconnect races");
}
