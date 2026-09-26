#include "mudclient/script_engine.hpp"

#include <cstdio>
#include <ctime>
#include <future>
#include <optional>

#include <lua.hpp>

namespace mudclient {

namespace {

// LUA_MASKCOUNT hook: fires every `instruction_budget_` VM instructions
// since the hook was (re)installed. Raising a Lua error here unwinds back
// to the nearest protected call boundary (our sol::protected_function
// invocation), which is exactly what "abort this callback" means -- it
// cannot escape past that boundary, so a runaway script can't hang or
// crash the client.
void instruction_hook(lua_State* L, lua_Debug*) {
    luaL_error(L, "script exceeded its instruction budget (possible infinite loop)");
}

class InstructionBudgetGuard {
public:
    InstructionBudgetGuard(lua_State* L, int budget) : L_(L) { lua_sethook(L_, instruction_hook, LUA_MASKCOUNT, budget); }
    ~InstructionBudgetGuard() { lua_sethook(L_, nullptr, 0, 0); }
    InstructionBudgetGuard(const InstructionBudgetGuard&) = delete;
    InstructionBudgetGuard& operator=(const InstructionBudgetGuard&) = delete;

private:
    lua_State* L_;
};

// Lua-side implementation of the coroutine-based test API
// (client.run_test/wait_for/sleep). Written in Lua rather than bound
// directly in C++ because coroutine.yield() must run on the coroutine's
// own Lua call stack; the low-level primitives it calls
// (register_trigger/remove_trigger/register_timer/kill_timer) are plain
// C++ bindings that don't need to know anything about coroutines --
// resuming a suspended coroutine from a fired trigger/timer callback is
// an ordinary Lua-level operation (coroutine.resume), not something that
// itself requires yielding.
// Deferred-resume queue: see the long comment on ScriptEngine::pump_resumes()
// in this file for why wait_for/sleep queue their resume instead of calling
// coroutine.resume directly from inside a trigger/timer callback.
constexpr const char* kBootstrapLua = R"lua(
client._pending_resumes = {}

function client._queue_resume(co, value)
    table.insert(client._pending_resumes, { co, value })
end

function client._pump_resumes()
    while #client._pending_resumes > 0 do
        local item = table.remove(client._pending_resumes, 1)
        local ok, err = coroutine.resume(item[1], item[2])
        if not ok then
            client.echo("error resuming test coroutine: " .. tostring(err))
        end
    end
end

function client.run_test(fn)
    local co = coroutine.create(fn)
    local ok, err = coroutine.resume(co)
    if not ok then
        client.echo("test error: " .. tostring(err))
    end
end

function client.wait_for(pattern, timeout)
    local co = coroutine.running()
    local trigger_id, timer_id
    local function cleanup()
        if trigger_id then client.remove_trigger(trigger_id) end
        if timer_id then client.kill_timer(timer_id) end
    end
    trigger_id = client.register_trigger(pattern, function(captures)
        cleanup()
        client._queue_resume(co, captures)
    end, { once = true, match_prompts = true })
    timer_id = client.register_timer(timeout, function()
        cleanup()
        client._queue_resume(co, nil)
    end, false)
    return coroutine.yield()
end

function client.sleep(seconds)
    local co = coroutine.running()
    client.register_timer(seconds, function()
        client._queue_resume(co, nil)
    end, false)
    coroutine.yield()
end
)lua";

} // namespace

ScriptEngine::ScriptEngine(NetworkClient& network, asio::io_context& network_io, TimerManager& timers,
                            std::function<void(std::string)> echo_fn)
    : network_(network), network_io_(network_io), timers_(timers), echo_(std::move(echo_fn)) {
    install_sandbox();
    install_client_api();
    lua_.script(kBootstrapLua);
}

void ScriptEngine::install_sandbox() {
    lua_.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table, sol::lib::math, sol::lib::utf8,
                         sol::lib::coroutine);

    lua_["dofile"] = sol::lua_nil;
    lua_["loadfile"] = sol::lua_nil;
    lua_["require"] = sol::lua_nil;
    lua_["string"]["dump"] = sol::lua_nil;

    // `load`: text-only (mode "t" rejects precompiled bytecode chunks,
    // closing off the classic sandbox-escape vector of handing a crafted
    // binary chunk to load()); runs with our existing sandboxed globals
    // table as its environment, since we never construct or expose a
    // different one.
    lua_["load"] = [](sol::this_state ts, std::string chunk,
                       sol::optional<std::string> chunkname) -> std::tuple<sol::object, sol::object> {
        // Mirrors real Lua's load(): returns (chunk_function, nil) on
        // success or (nil, error_message) on failure, so scripts using
        // the standard `local fn, err = load(...)` idiom keep working.
        // Deliberately never calls luaL_error/lua_error here: that would
        // longjmp past this lambda's C++ locals (chunk, name, msg) without
        // running their destructors -- undefined behavior when mixed with
        // sol2's own exception-based error handling, and a real crash was
        // observed from exactly this before it was changed to a plain
        // return.
        lua_State* L = ts;
        std::string name = chunkname.value_or(std::string("=(load)"));
        int status = luaL_loadbufferx(L, chunk.data(), chunk.size(), name.c_str(), "t");
        if (status != LUA_OK) {
            std::string msg = lua_tostring(L, -1);
            lua_pop(L, 1);
            return {sol::lua_nil, sol::make_object(L, msg)};
        }
        sol::object fn(L, -1);
        lua_pop(L, 1);
        return {fn, sol::lua_nil};
    };

    // Restrict collectgarbage to the read-only "count" query; anything
    // else (stop/restart/collect/...) is a no-op, so a script can't
    // disable or force full collections.
    sol::protected_function original_gc = lua_["collectgarbage"];
    lua_["collectgarbage"] = [original_gc](sol::variadic_args args) -> double {
        if (args.size() >= 1 && args[0].is<std::string>() && args[0].as<std::string>() == "count") {
            sol::protected_function_result result = original_gc("count");
            if (result.valid()) {
                return result.get<double>();
            }
        }
        return 0.0;
    };

    // os isn't opened at all (io/os/debug/package are excluded per spec),
    // so we provide a minimal table with only the three read-only
    // functions the spec allows: os.time, os.clock, os.date.
    sol::table os_table = lua_.create_table();
    os_table.set_function("time", [] { return static_cast<int64_t>(std::time(nullptr)); });
    os_table.set_function("clock", [] { return static_cast<double>(std::clock()) / CLOCKS_PER_SEC; });
    os_table.set_function("date", [](sol::optional<std::string> format) -> std::string {
        std::time_t t = std::time(nullptr);
        std::tm tm_buf{};
#if defined(_WIN32)
        localtime_s(&tm_buf, &t);
#else
        localtime_r(&t, &tm_buf);
#endif
        char buf[256];
        std::string fmt = format.value_or("%c");
        if (!fmt.empty() && fmt[0] == '!') {
            fmt = fmt.substr(1); // we don't distinguish UTC vs. local; treat '!' prefix as a no-op
        }
        std::strftime(buf, sizeof(buf), fmt.c_str(), &tm_buf);
        return buf;
    });
    lua_["os"] = os_table;
}

void ScriptEngine::install_client_api() {
    sol::table client = lua_.create_named_table("client");

    client.set_function("send", [this](std::string text) { send_command(text); });
    client.set_function("echo", [this](std::string text) { echo_(std::move(text)); });
    client.set_function("exit", [this](sol::optional<int> code) {
        exit_requested_ = true;
        exit_code_ = code.value_or(0);
    });
    client.set_function("sent_log", [this] { return sol::as_table(sent_log_); });

    client.set_function("register_alias", [this](std::string pattern, sol::object action_obj,
                                                    sol::optional<sol::table> opts) -> uint64_t {
        AliasManager::Kind kind = AliasManager::Kind::Regex;
        // Exact-match aliases are plain strings passed as the pattern with
        // no metacharacters intended; since we can't tell from the string
        // alone, opts.exact = true selects Exact matching explicitly.
        AliasManager::Options options;
        if (opts) {
            options.priority = opts->get_or("priority", 0);
            options.fall_through = opts->get_or("fall_through", false);
            if (opts->get_or("exact", false)) {
                kind = AliasManager::Kind::Exact;
            }
        }
        AliasManager::Action action;
        if (action_obj.is<std::string>()) {
            action = action_obj.as<std::string>();
        } else {
            sol::protected_function fn = action_obj.as<sol::protected_function>();
            action = AliasManager::FunctionAction([this, fn](const std::vector<std::string>& captures) -> std::string {
                sol::table caps = lua_.create_table();
                for (size_t i = 0; i < captures.size(); ++i) {
                    caps[i] = captures[i];
                }
                InstructionBudgetGuard guard(lua_.lua_state(), instruction_budget_);
                sol::protected_function_result result = fn(caps);
                if (!result.valid()) {
                    sol::error err = result;
                    echo_(std::string("Lua error (alias): ") + err.what());
                    return std::string();
                }
                return result.get<sol::optional<std::string>>().value_or(std::string());
            });
        }
        return aliases_.add_alias(kind, std::move(pattern), std::move(action), options);
    });
    client.set_function("remove_alias", [this](uint64_t id) { aliases_.remove_alias(id); });

    client.set_function("register_trigger", [this](std::string pattern, sol::object action_obj,
                                                      sol::optional<sol::table> opts) -> uint64_t {
        TriggerManager::Options options;
        if (opts) {
            options.gag = opts->get_or("gag", false);
            options.once = opts->get_or("once", false);
            options.enabled = opts->get_or("enabled", true);
            options.priority = opts->get_or("priority", 0);
            options.stop_processing = opts->get_or("stop_processing", false);
            options.match_prompts = opts->get_or("match_prompts", false);
            sol::optional<std::string> recolor = opts->get<sol::optional<std::string>>("recolor");
            if (recolor) {
                options.recolor = true;
                if (*recolor == "red") options.recolor_style.fg = encode_standard_color(1);
                else if (*recolor == "green") options.recolor_style.fg = encode_standard_color(2);
                else if (*recolor == "yellow") options.recolor_style.fg = encode_standard_color(3);
                else if (*recolor == "blue") options.recolor_style.fg = encode_standard_color(4);
                else if (*recolor == "magenta") options.recolor_style.fg = encode_standard_color(5);
                else if (*recolor == "cyan") options.recolor_style.fg = encode_standard_color(6);
                else if (*recolor == "white") options.recolor_style.fg = encode_standard_color(7);
            }
        }
        sol::protected_function fn = action_obj.as<sol::protected_function>();
        TriggerManager::Action action = [this, fn](const std::vector<std::string>& captures) {
            sol::table caps = lua_.create_table();
            for (size_t i = 0; i < captures.size(); ++i) {
                caps[i] = captures[i];
            }
            InstructionBudgetGuard guard(lua_.lua_state(), instruction_budget_);
            sol::protected_function_result result = fn(caps);
            if (!result.valid()) {
                sol::error err = result;
                echo_(std::string("Lua error (trigger): ") + err.what());
            }
        };
        return triggers_.add_trigger(std::move(pattern), std::move(action), options);
    });
    client.set_function("remove_trigger", [this](uint64_t id) { triggers_.remove_trigger(id); });

    client.set_function("register_timer", [this](double seconds, sol::protected_function fn,
                                                    sol::optional<bool> repeating,
                                                    sol::optional<std::string> label) -> uint64_t {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::duration<double>(seconds));
        bool is_repeating = repeating.value_or(false);
        std::optional<std::string> label_opt = label ? std::optional<std::string>(*label) : std::nullopt;
        std::promise<uint64_t> promise;
        auto future = promise.get_future();
        asio::post(network_io_, [this, ms, is_repeating, label_opt, &promise] {
            promise.set_value(timers_.add_timer(ms, is_repeating, label_opt));
        });
        uint64_t id = future.get();
        timer_callbacks_[id] = {std::move(fn), is_repeating};
        return id;
    });
    client.set_function("kill_timer", [this](sol::object id_or_label) { with_timer_id(id_or_label, [this](uint64_t id) {
        asio::post(network_io_, [this, id] { timers_.kill(id); });
        timer_callbacks_.erase(id);
    }); });
    client.set_function("pause_timer", [this](sol::object id_or_label) {
        with_timer_id(id_or_label, [this](uint64_t id) { asio::post(network_io_, [this, id] { timers_.pause(id); }); });
    });
    client.set_function("resume_timer", [this](sol::object id_or_label) {
        with_timer_id(id_or_label, [this](uint64_t id) { asio::post(network_io_, [this, id] { timers_.resume(id); }); });
    });
    client.set_function("reset_timer", [this](sol::object id_or_label) {
        with_timer_id(id_or_label, [this](uint64_t id) { asio::post(network_io_, [this, id] { timers_.reset(id); }); });
    });

    client.set_function("get_gmcp", [this](std::string path) -> sol::object {
        auto value = gmcp_.get(path);
        if (!value) {
            return sol::lua_nil;
        }
        return json_to_lua(*value);
    });

    client.set_function("on", [this](std::string event_name, sol::protected_function fn) {
        event_handlers_[event_name].push_back(fn);
    });
}

void ScriptEngine::with_timer_id(const sol::object& id_or_label, const std::function<void(uint64_t)>& fn) {
    if (id_or_label.is<uint64_t>()) {
        fn(id_or_label.as<uint64_t>());
        return;
    }
    if (id_or_label.is<std::string>()) {
        std::string label = id_or_label.as<std::string>();
        std::promise<std::optional<uint64_t>> promise;
        auto future = promise.get_future();
        asio::post(network_io_, [this, label, &promise] { promise.set_value(timers_.id_for_label(label)); });
        auto id = future.get();
        if (id) {
            fn(*id);
        }
    }
}

sol::object ScriptEngine::json_to_lua(const nlohmann::json& value) {
    switch (value.type()) {
    case nlohmann::json::value_t::null:
        return sol::lua_nil;
    case nlohmann::json::value_t::boolean:
        return sol::make_object(lua_, value.get<bool>());
    case nlohmann::json::value_t::number_integer:
    case nlohmann::json::value_t::number_unsigned:
        return sol::make_object(lua_, value.get<int64_t>());
    case nlohmann::json::value_t::number_float:
        return sol::make_object(lua_, value.get<double>());
    case nlohmann::json::value_t::string:
        return sol::make_object(lua_, value.get<std::string>());
    case nlohmann::json::value_t::array: {
        sol::table t = lua_.create_table();
        int idx = 1;
        for (const auto& element : value) {
            t[idx++] = json_to_lua(element);
        }
        return t;
    }
    case nlohmann::json::value_t::object: {
        sol::table t = lua_.create_table();
        for (const auto& [key, element] : value.items()) {
            t[key] = json_to_lua(element);
        }
        return t;
    }
    default:
        return sol::lua_nil;
    }
}

void ScriptEngine::send_command(const std::string& input) {
    AliasManager::ExpansionResult expansion = aliases_.expand(input);
    if (expansion.aborted) {
        echo_("alias error: " + expansion.error);
        return;
    }
    for (auto& cmd : expansion.commands) {
        sent_log_.push_back(cmd);
        asio::post(network_io_, [this, cmd] { network_.send_line(cmd); });
    }
}

void ScriptEngine::call_event_handlers(const std::string& event_name, sol::object arg1, sol::object arg2) {
    auto it = event_handlers_.find(event_name);
    if (it == event_handlers_.end()) {
        return;
    }
    // Copy the handler list before invoking: a handler may register or
    // remove handlers for this same event from inside itself, and we must
    // not hold a reference/iterator into event_handlers_ across that call
    // (same reentrancy hazard as TriggerManager::process_line).
    std::vector<sol::protected_function> handlers = it->second;
    for (auto& fn : handlers) {
        InstructionBudgetGuard guard(lua_.lua_state(), instruction_budget_);
        sol::protected_function_result result = fn(arg1, arg2);
        if (!result.valid()) {
            sol::error err = result;
            echo_("Lua error (" + event_name + "): " + err.what());
        }
    }
}

sol::table ScriptEngine::make_line_table(const StyledLine& line) {
    sol::table t = lua_.create_table();
    t["plain"] = line.plain;
    t["is_prompt"] = line.is_prompt;
    sol::table spans = lua_.create_table();
    int idx = 1;
    for (const auto& span : line.spans) {
        sol::table span_table = lua_.create_table();
        span_table["fg"] = span.style.fg;
        span_table["bg"] = span.style.bg;
        span_table["bold"] = (span.style.flags & style_bold) != 0;
        span_table["underline"] = (span.style.flags & style_underline) != 0;
        span_table["start"] = span.start;
        span_table["length"] = span.length;
        spans[idx++] = span_table;
    }
    t["spans"] = spans;
    return t;
}

namespace {
void append_color_sgr(std::string& out, uint32_t color, bool background) {
    uint32_t kind = color >> 24;
    uint32_t base_fg = background ? 40 : 30;
    uint32_t bright_fg = background ? 100 : 90;
    if (kind == color_kind_standard) {
        uint32_t index = color & 0xFF;
        out += "\x1b[" + std::to_string(index < 8 ? base_fg + index : bright_fg + (index - 8)) + "m";
    } else if (kind == color_kind_256) {
        out += "\x1b[" + std::string(background ? "48" : "38") + ";5;" + std::to_string(color & 0xFF) + "m";
    } else if (kind == color_kind_truecolor) {
        out += "\x1b[" + std::string(background ? "48" : "38") + ";2;" + std::to_string((color >> 16) & 0xFF) + ";" +
               std::to_string((color >> 8) & 0xFF) + ";" + std::to_string(color & 0xFF) + "m";
    }
}
} // namespace

void ScriptEngine::render_line(const StyledLine& line) {
    std::string out;
    for (const auto& span : line.spans) {
        out += "\x1b[0m";
        const TextStyle& style = span.style;
        if (style.flags & style_bold) out += "\x1b[1m";
        if (style.flags & style_italic) out += "\x1b[3m";
        if (style.flags & style_underline) out += "\x1b[4m";
        if (style.flags & style_blink) out += "\x1b[5m";
        if (style.flags & style_inverse) out += "\x1b[7m";
        if (style.fg != default_fg) append_color_sgr(out, style.fg, /*background=*/false);
        if (style.bg != default_bg) append_color_sgr(out, style.bg, /*background=*/true);
        out += line.plain.substr(span.start, span.length);
    }
    if (line.spans.empty()) {
        out += line.plain;
    }
    out += "\x1b[0m\n";
    std::fputs(out.c_str(), stdout);
    std::fflush(stdout);
}

void ScriptEngine::load_file(const std::string& path) {
    InstructionBudgetGuard guard(lua_.lua_state(), instruction_budget_ * 100);
    sol::protected_function_result result = lua_.safe_script_file(path, sol::script_pass_on_error);
    if (!result.valid()) {
        sol::error err = result;
        echo_("Lua error loading " + path + ": " + std::string(err.what()));
    }
}

void ScriptEngine::run_string(const std::string& code) {
    InstructionBudgetGuard guard(lua_.lua_state(), instruction_budget_);
    sol::protected_function_result result = lua_.safe_script(code, sol::script_pass_on_error);
    if (!result.valid()) {
        sol::error err = result;
        echo_(std::string("Lua error: ") + err.what());
    }
}

// Resuming a Lua coroutine via coroutine.resume() called *directly* from
// inside a callback that C++ is invoking through a stored
// sol::protected_function (i.e. from inside a trigger/timer/event-handler
// callback) reliably corrupted the target coroutine's internal call-info
// chain in testing here (a segfault inside Lua's own resume(), reading
// through a dangling/garbage CallInfo pointer), specifically when that
// coroutine had yielded from a *nested* Lua function call (e.g.
// client.wait_for's own frame) rather than directly from its top-level
// function. The exact same coroutine.resume() call, made instead from a
// **fresh top-level call** (i.e. another sol::state::safe_script /
// safe_script_file invocation, not from inside an already-running
// protected_function call), works correctly every time.
//
// Rather than resume directly, wait_for/sleep's trigger/timer callbacks
// (see kBootstrapLua) push the coroutine and its resume value onto a Lua
// table via client._queue_resume(), and every dispatch_* method below
// calls pump_resumes() once it's done, which runs client._pump_resumes()
// as its own fresh top-level script -- exactly the call shape that works.
void ScriptEngine::pump_resumes() {
    sol::protected_function_result result = lua_.safe_script("client._pump_resumes()", sol::script_pass_on_error);
    if (!result.valid()) {
        sol::error err = result;
        echo_(std::string("Lua error (resume pump): ") + err.what());
    }
}

void ScriptEngine::dispatch_connect() {
    call_event_handlers("connect", sol::lua_nil);
    pump_resumes();
}

void ScriptEngine::dispatch_disconnect(const std::string& reason) {
    call_event_handlers("disconnect", sol::make_object(lua_, reason));
    pump_resumes();
}

void ScriptEngine::dispatch_line(const StyledLine& line) {
    TriggerManager::MatchOutcome outcome = triggers_.process_line(line);
    StyledLine display = line;
    if (outcome.recolor) {
        for (auto& span : display.spans) {
            span.style = outcome.recolor_style;
        }
    }
    if (!outcome.gag) {
        render_line(display);
        if (transcript_) {
            transcript_(display.plain);
        }
    }
    call_event_handlers("line", make_line_table(display));
    pump_resumes();
}

void ScriptEngine::dispatch_gmcp(const std::string& package, const nlohmann::json& json) {
    gmcp_.update(package, json);
    call_event_handlers("gmcp", sol::make_object(lua_, package), json_to_lua(json));
    pump_resumes();
}

void ScriptEngine::dispatch_input(const std::string& text) {
    call_event_handlers("input", sol::make_object(lua_, text));
    send_command(text);
    pump_resumes();
}

void ScriptEngine::dispatch_timer(uint64_t id) {
    auto it = timer_callbacks_.find(id);
    if (it == timer_callbacks_.end()) {
        return; // dropped: id no longer exists (spec: engine drops TimerFired for unknown ids)
    }
    sol::protected_function fn = it->second.first;
    bool repeating = it->second.second;
    if (!repeating) {
        timer_callbacks_.erase(it); // erase first: safe if the callback registers a new timer of its own
    }
    {
        InstructionBudgetGuard guard(lua_.lua_state(), instruction_budget_);
        sol::protected_function_result result = fn();
        if (!result.valid()) {
            sol::error err = result;
            echo_(std::string("Lua error (timer): ") + err.what());
        }
    }
    call_event_handlers("timer", sol::make_object(lua_, id));
    pump_resumes();
}

} // namespace mudclient

