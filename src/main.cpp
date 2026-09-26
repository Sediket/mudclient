#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <string>
#include <thread>

#include <asio.hpp>

#include "mudclient/command_parser.hpp"
#include "mudclient/event_queue.hpp"
#include "mudclient/network_client.hpp"
#include "mudclient/script_engine.hpp"
#include "mudclient/session_recorder.hpp"
#include "mudclient/timer_manager.hpp"
#include "platform/console.hpp"

#ifndef MUDCLIENT_VERSION
#error "MUDCLIENT_VERSION must be defined by the build system"
#endif

using namespace mudclient;
using namespace std::chrono_literals;

namespace {

struct Options {
    bool show_version = false;
    bool test_mode = false;
    std::string test_script;
    double timeout_seconds = 120.0;
    std::string record_path;
    std::string log_transcript_path;
    std::string host;
    std::string port;
};

Options parse_args(int argc, char** argv) {
    Options opts;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--version") {
            opts.show_version = true;
        } else if (arg == "--test" && i + 1 < argc) {
            opts.test_script = argv[++i];
            opts.test_mode = true;
        } else if (arg == "--timeout" && i + 1 < argc) {
            opts.timeout_seconds = std::atof(argv[++i]);
        } else if (arg == "--record" && i + 1 < argc) {
            opts.record_path = argv[++i];
        } else if (arg == "--log-transcript" && i + 1 < argc) {
            opts.log_transcript_path = argv[++i];
        } else if (!arg.empty() && arg[0] != '-') {
            positional.push_back(arg);
        }
    }
    if (!positional.empty()) opts.host = positional[0];
    if (positional.size() > 1) opts.port = positional[1];

    // Test mode allows overriding host/port via environment, per spec.
    if (opts.test_mode) {
        if (const char* env_host = std::getenv("MUD_HOST")) opts.host = env_host;
        if (const char* env_port = std::getenv("MUD_PORT")) opts.port = env_port;
    }
    return opts;
}

// Produces a single-quoted Lua string literal safe to splice into a
// synthesized snippet of Lua source (used only for built-in commands
// like #alias/#trigger/#timer, which need to hand user-typed text to the
// same Lua-level registration functions the scripting API uses).
std::string lua_quote(const std::string& text) {
    std::string out = "'";
    for (char c : text) {
        if (c == '\\' || c == '\'') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

std::string join(const std::vector<std::string>& parts, size_t from) {
    std::string out;
    for (size_t i = from; i < parts.size(); ++i) {
        if (i > from) out += ' ';
        out += parts[i];
    }
    return out;
}

// Handles a parsed built-in ("#...") command. Everything here is
// deliberately implemented by synthesizing a small snippet of Lua and
// running it through the same client.* API a script would use, rather
// than adding a parallel C++ dispatch path for each command -- this
// reuses already-tested registration/sandboxing logic directly.
void handle_builtin(const ParsedCommand& cmd, ScriptEngine& engine, NetworkClient& network, asio::io_context& network_io,
                     bool& quit_requested) {
    switch (cmd.command) {
    case BuiltinCommand::Connect:
        asio::post(network_io, [&network, host = cmd.args[0], port = cmd.args[1]] {
            network.connect(host, port, 5000ms);
        });
        break;
    case BuiltinCommand::Disconnect:
        asio::post(network_io, [&network] { network.disconnect(); });
        break;
    case BuiltinCommand::Lua:
        engine.run_string(cmd.raw_args);
        break;
    case BuiltinCommand::Quit:
        quit_requested = true;
        break;
    case BuiltinCommand::AliasAdd:
        engine.run_string("client.register_alias(" + lua_quote(cmd.args[0]) + ", " + lua_quote(cmd.args[1]) +
                           ", { exact = true })");
        break;
    case BuiltinCommand::AliasDel:
        engine.run_string("client.remove_alias(" + cmd.args[0] + ")");
        break;
    case BuiltinCommand::AliasList:
        std::printf("%zu alias(es) registered\n", engine.aliases().count());
        break;
    case BuiltinCommand::TriggerAdd:
        engine.run_string("client.register_trigger(" + lua_quote(cmd.args[0]) + ", function() client.send(" +
                           lua_quote(cmd.args[1]) + ") end)");
        break;
    case BuiltinCommand::TriggerDel:
        engine.run_string("client.remove_trigger(" + cmd.args[0] + ")");
        break;
    case BuiltinCommand::TriggerList:
        std::printf("%zu trigger(s) registered\n", engine.triggers().count());
        break;
    case BuiltinCommand::TimerAdd: {
        bool repeating = cmd.args.size() >= 2 && cmd.args[1] == "repeat";
        std::string command_text = join(cmd.args, repeating ? 2 : 1);
        engine.run_string("client.register_timer(" + cmd.args[0] + ", function() client.send(" +
                           lua_quote(command_text) + ") end, " + (repeating ? "true" : "false") + ")");
        break;
    }
    case BuiltinCommand::TimerList:
        std::printf("(use client.on/register_timer return values to track individual timers)\n");
        break;
    case BuiltinCommand::TimerPause:
        engine.run_string("client.pause_timer(" + lua_quote(cmd.args[0]) + ")");
        break;
    case BuiltinCommand::TimerResume:
        engine.run_string("client.resume_timer(" + lua_quote(cmd.args[0]) + ")");
        break;
    case BuiltinCommand::TimerReset:
        std::printf("#timer reset is not yet wired to a client.* primitive; use #timer kill and #timer add\n");
        break;
    case BuiltinCommand::TimerKill:
        engine.run_string("client.kill_timer(" + lua_quote(cmd.args[0]) + ")");
        break;
    }
}

} // namespace

int main(int argc, char** argv) {
    platform::init_console();
    Options opts = parse_args(argc, argv);

    if (opts.show_version) {
        std::printf("mudclient %s\n", MUDCLIENT_VERSION);
        return 0;
    }

    // ---- Network thread: owns io_context, NetworkClient, TimerManager ----
    asio::io_context network_io;
    auto network_work_guard = asio::make_work_guard(network_io);
    EventQueue events;
    auto network = std::make_shared<NetworkClient>(network_io, events);
    TimerManager timers(network_io, events);

    std::unique_ptr<SessionRecorder> recorder;
    if (!opts.record_path.empty()) {
        recorder = std::make_unique<SessionRecorder>(opts.record_path);
        SessionRecorder* recorder_ptr = recorder.get();
        network->set_recorder([recorder_ptr](bool inbound, std::span<const uint8_t> data) {
            if (inbound) {
                recorder_ptr->record_inbound(data);
            } else {
                recorder_ptr->record_outbound_line(
                    std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
            }
        });
    }

    std::thread network_thread([&network_io] { network_io.run(); });

    // ---- Engine thread (this thread): owns the sol::state and every Lua binding ----
    std::ofstream transcript;
    if (!opts.log_transcript_path.empty()) {
        transcript.open(opts.log_transcript_path, std::ios::trunc);
    }

    ScriptEngine engine(*network, network_io, timers, [&transcript](std::string text) {
        std::fputs(text.c_str(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
        if (transcript.is_open()) {
            transcript << text << '\n';
            transcript.flush();
        }
    });
    if (transcript.is_open()) {
        engine.set_transcript_sink([&transcript](const std::string& line) {
            transcript << line << '\n';
            transcript.flush();
        });
    }

    CommandParser command_parser;
    bool quit_requested = false;

    if (opts.test_mode) {
        engine.load_file(opts.test_script);
    } else {
        std::ifstream init_check("scripts/init.lua");
        if (init_check.good()) {
            engine.load_file("scripts/init.lua");
        }
    }

    if (!opts.host.empty()) {
        std::string port = opts.port.empty() ? "23" : opts.port;
        asio::post(network_io, [&network, host = opts.host, port] { network->connect(host, port, 5000ms); });
    }

    // ---- Stdin thread: only in interactive mode, per spec ----
    std::thread stdin_thread;
    std::atomic<bool> stdin_stop{false};
    if (!opts.test_mode) {
        stdin_thread = std::thread([&events, &stdin_stop] {
            std::string line;
            while (!stdin_stop && std::getline(std::cin, line)) {
                events.push(UserInput{line});
            }
        });
    }

    // ---- Engine loop: drains events, dispatches to ScriptEngine/built-ins ----
    auto watchdog_deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(opts.timeout_seconds);
    int exit_code = 0;
    bool timed_out = false;

    while (!quit_requested && !engine.exit_requested()) {
        if (opts.test_mode && std::chrono::steady_clock::now() >= watchdog_deadline) {
            std::fprintf(stderr, "mudclient: --test watchdog expired after %.1fs (pending step unknown; script never called client.exit)\n",
                         opts.timeout_seconds);
            timed_out = true;
            break;
        }
        auto event = events.pop_wait(200ms);
        if (!event) {
            continue;
        }
        std::visit(
            [&](auto&& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, Connected>) {
                    engine.dispatch_connect();
                } else if constexpr (std::is_same_v<T, Disconnected>) {
                    engine.dispatch_disconnect(value.reason);
                } else if constexpr (std::is_same_v<T, LineReceived>) {
                    engine.dispatch_line(value.line);
                } else if constexpr (std::is_same_v<T, GmcpReceived>) {
                    engine.dispatch_gmcp(value.package, value.json);
                } else if constexpr (std::is_same_v<T, UserInput>) {
                    if (command_parser.is_command_line(value.text)) {
                        auto parsed = command_parser.parse(value.text);
                        if (std::holds_alternative<ParsedCommand>(parsed)) {
                            handle_builtin(std::get<ParsedCommand>(parsed), engine, *network, network_io,
                                           quit_requested);
                        } else {
                            std::printf("%s\n", std::get<ParseError>(parsed).message.c_str());
                        }
                    } else {
                        engine.dispatch_input(value.text);
                    }
                } else if constexpr (std::is_same_v<T, TimerFired>) {
                    engine.dispatch_timer(value.id);
                }
            },
            *event);
    }

    if (engine.exit_requested()) {
        exit_code = engine.exit_code();
    } else if (timed_out) {
        exit_code = 2;
    }

    // ---- Shutdown ----
    stdin_stop = true;
    asio::post(network_io, [&network] { network->disconnect(); });
    if (stdin_thread.joinable()) {
        // The stdin thread is blocked in std::getline with no portable way
        // to interrupt it; detach rather than join so process exit isn't
        // held up waiting for a line of input that may never come. This
        // only matters for interactive mode (never in --test mode, which
        // has no stdin thread at all).
        stdin_thread.detach();
    }
    network_work_guard.reset();
    network_thread.join();

    return exit_code;
}
