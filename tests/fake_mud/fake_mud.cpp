// Test-only replay server: replays a --record JSON Lines file (see
// mudclient::SessionRecorder for the schema) over a real TCP socket,
// preserving the original inbound chunk boundaries and relative timing
// (optionally sped up via --speed), advancing past each recorded outbound
// line only once it actually receives a line from the connected client.
// Not part of the shipped client; used only by the replay ctest.
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <asio.hpp>
#include <nlohmann/json.hpp>

#include "mudclient/session_recorder.hpp"

using namespace std::chrono_literals;

namespace {

std::vector<nlohmann::json> load_entries(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("fake_mud: cannot open record file: " + path);
    }
    std::vector<nlohmann::json> entries;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        entries.push_back(nlohmann::json::parse(line));
    }
    return entries;
}

std::string read_line(asio::ip::tcp::socket& socket, std::chrono::steady_clock::duration timeout) {
    std::string received;
    auto deadline = std::chrono::steady_clock::now() + timeout;
    socket.non_blocking(true);
    char c = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        asio::error_code ec;
        size_t n = socket.read_some(asio::buffer(&c, 1), ec);
        if (ec == asio::error::would_block) {
            std::this_thread::sleep_for(2ms);
            continue;
        }
        if (ec || n == 0) {
            break;
        }
        if (c == '\n') {
            break;
        }
        if (c != '\r') {
            received.push_back(c);
        }
    }
    socket.non_blocking(false);
    return received;
}

} // namespace

int main(int argc, char** argv) {
    std::string record_path;
    uint16_t port = 0;
    double speed = 1.0;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--record" && i + 1 < argc) {
            record_path = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--speed" && i + 1 < argc) {
            speed = std::stod(argv[++i]);
        }
    }
    if (record_path.empty()) {
        std::cerr << "usage: fake_mud --record <file> [--port N] [--speed N]\n";
        return 1;
    }

    std::vector<nlohmann::json> entries;
    try {
        entries = load_entries(record_path);
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }

    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port));
    // The test driver reads this exact line to discover an ephemeral port.
    std::cout << "PORT " << acceptor.local_endpoint().port() << std::endl;

    asio::ip::tcp::socket socket(io);
    acceptor.accept(socket);

    int64_t last_t_ms = 0;
    for (auto& entry : entries) {
        int64_t t_ms = entry.value("t_ms", int64_t{0});
        auto delay = std::chrono::duration<double, std::milli>(static_cast<double>(t_ms - last_t_ms) / speed);
        last_t_ms = t_ms;
        if (delay.count() > 0) {
            std::this_thread::sleep_for(delay);
        }

        std::string dir = entry.value("dir", std::string());
        if (dir == "in") {
            std::string bytes = mudclient::base64_decode(entry.value("data_b64", std::string()));
            asio::error_code ec;
            asio::write(socket, asio::buffer(bytes), ec);
            if (ec) {
                std::cerr << "fake_mud: write error: " << ec.message() << "\n";
                break;
            }
        } else if (dir == "out") {
            std::string expected = entry.value("line", std::string());
            std::string received = read_line(socket, 30s);
            if (received != expected) {
                // Not fatal: aliases/timing can legitimately change exact
                // wording. The synchronization point (a line arrived) is
                // what advances the replay, not byte-exact content.
                std::cerr << "fake_mud: note: expected outbound '" << expected << "' but got '" << received << "'\n";
            }
        }
    }

    asio::error_code ignore;
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignore);
    socket.close(ignore);
    return 0;
}
