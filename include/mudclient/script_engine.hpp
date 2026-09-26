#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <asio.hpp>
#include <sol/sol.hpp>

#include "mudclient/alias_manager.hpp"
#include "mudclient/events.hpp"
#include "mudclient/gmcp_cache.hpp"
#include "mudclient/network_client.hpp"
#include "mudclient/timer_manager.hpp"
#include "mudclient/trigger_manager.hpp"

// Owns the sandboxed sol::state and every Lua-facing binding. Engine
// thread only: all Lua execution happens here, never on the network or
// stdin threads. Sends/timer operations reach the network thread only via
// asio::post on the network io_context, per the spec's threading model.
namespace mudclient {

class ScriptEngine {
public:
    // `network`/`network_io` are used only to post outbound sends and
    // timer operations to the network thread; `timers` is likewise a
    // network-thread-owned object reached only via post(). `echo_fn` is
    // called (on the engine thread) to display a line of client-generated
    // or error text.
    ScriptEngine(NetworkClient& network, asio::io_context& network_io, TimerManager& timers,
                 std::function<void(std::string)> echo_fn);

    // Loads and runs a Lua file. Parse/runtime errors are echoed, never
    // thrown further.
    void load_file(const std::string& path);
    // Runs a chunk of Lua source (e.g. from "#lua <code>"). Same error
    // handling as load_file.
    void run_string(const std::string& code);

    // Event dispatch, called by the engine loop as Events are drained
    // from the EventQueue.
    void dispatch_connect();
    void dispatch_disconnect(const std::string& reason);
    void dispatch_line(const StyledLine& line);
    void dispatch_gmcp(const std::string& package, const nlohmann::json& json);
    void dispatch_input(const std::string& text);
    void dispatch_timer(uint64_t id);

    bool exit_requested() const { return exit_requested_; }
    int exit_code() const { return exit_code_; }

    const std::vector<std::string>& sent_log() const { return sent_log_; }

    TriggerManager& triggers() { return triggers_; }
    AliasManager& aliases() { return aliases_; }
    GmcpCache& gmcp() { return gmcp_; }

private:
    void install_sandbox();
    void install_client_api();
    void send_command(const std::string& input);
    void call_event_handlers(const std::string& event_name, sol::object arg1, sol::object arg2 = sol::lua_nil);
    sol::table make_line_table(const StyledLine& line);
    void render_line(const StyledLine& line);
    sol::object json_to_lua(const nlohmann::json& value);
    // Runs any coroutine resumes that a trigger/timer callback queued via
    // client._queue_resume during the dispatch that just completed. See
    // the long comment at this method's definition for why this
    // indirection exists.
    void pump_resumes();
    // Resolves a Lua-side id-or-label argument (an integer id or a string
    // label) to a numeric timer id and invokes `fn` with it. Reads
    // TimerManager state only via a posted call to the network thread,
    // since TimerManager is network-thread-owned.
    void with_timer_id(const sol::object& id_or_label, const std::function<void(uint64_t)>& fn);

    sol::state lua_;
    NetworkClient& network_;
    asio::io_context& network_io_;
    TimerManager& timers_;
    std::function<void(std::string)> echo_;

    TriggerManager triggers_;
    AliasManager aliases_;
    GmcpCache gmcp_;

    std::unordered_map<std::string, std::vector<sol::protected_function>> event_handlers_;
    // (callback, repeating) per registered timer id; erased when a
    // one-shot timer fires or the timer is killed.
    std::unordered_map<uint64_t, std::pair<sol::protected_function, bool>> timer_callbacks_;
    std::vector<std::string> sent_log_;

    bool exit_requested_ = false;
    int exit_code_ = 0;
    int instruction_budget_ = 10'000'000;
};

} // namespace mudclient
