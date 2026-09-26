# MUD Client Specification

A high-performance, modular MUD client core in C++20 with embedded Lua scripting, built and released for Linux and Windows via GitHub Actions.

## Tech Stack (fixed; do not substitute)
- **Language:** C++20 (use concepts, `std::span`, `std::string_view`, designated initializers where they help; no coroutines unless justified).
- **Networking:** Standalone Asio (non-Boost, `ASIO_STANDALONE`).
- **Scripting:** Lua 5.4 via `sol2` v3 (single `sol::state`).
- **Regex:** PCRE2 (8-bit) with JIT enabled. Do not use `std::regex`.
- **JSON:** `nlohmann/json`.
- **Build:** CMake ≥ 3.25 using `FetchContent` for all dependencies (Asio, sol2, nlohmann/json, PCRE2, Lua 5.4 built from source, and the test framework: Catch2 or doctest). Do not use `find_package` for Lua. Declare dependencies with `SYSTEM`. Link `Threads::Threads`, and on Windows `ws2_32` and `mswsock`. Options: `ENABLE_ASAN` (non-MSVC only), `ENABLE_TESTS`. Include `install()` rules and CPack producing `.tar.gz` on Linux and `.zip` on Windows. Provide `CMakePresets.json` with `release`, `debug`, and `asan` presets.

## Threading Model (mandatory)
- **Network thread:** runs one `asio::io_context`. Owns the socket, telnet parser, and all `asio::steady_timer` instances. Never touches Lua.
- **Engine thread:** owns the `sol::state`, trigger/alias managers, GMCP cache, and display output. All Lua runs here only.
- **Stdin thread:** blocking `std::getline` on stdin, pushing `UserInput` events. Plain console; no TUI.
- Network → engine: thread-safe MPSC queue of `Event` values (`std::variant`), drained by the engine loop, which waits on a condition variable with timeout (no busy-spinning).
- Engine → network: `asio::post(io_context, ...)` for sends and all timer operations. Never touch sockets or timers directly from the engine thread.
- Timer expirations fire on the network thread and post `TimerFired{id}`; Lua callbacks run on the engine thread.
- Every cross-thread handoff carries a comment stating which thread owns the data.

## 1. Network & Telnet

### Async TCP client (`network_client.hpp/.cpp`)
- Async resolve + connect with a configurable connect timeout.
- One outstanding `async_read_some` into a fixed 16 KB buffer; bytes go to the parser.
- Outbound write queue (`std::deque<std::string>`), at most one `async_write` in flight.
- Escape outbound `0xFF` as `IAC IAC`; terminate lines with `\r\n`.
- Emit `Connected` and `Disconnected{reason}`. Shutdown cancels pending operations without use-after-free; document the lifetime strategy.

### Telnet parser (`telnet_parser.hpp/.cpp`)
Streaming byte-at-a-time FSM, resumable across arbitrary chunk boundaries (including between `IAC` and the next byte, inside `SB ... SE`, and inside ANSI sequences).
- States at minimum: `Data, Iac, Will, Wont, Do, Dont, Sb, SbData, SbDataIac, Esc, Csi`.
- `IAC IAC` → literal `0xFF`, in data and in subnegotiation data.
- Option negotiation with RFC 1143 Q-method state tracking. Support:
  - GMCP (201): accept `WILL`, respond `DO`, send `Core.Hello` and `Core.Supports.Set`.
  - NAWS (31), TTYPE (24): respond with fixed width/height and a terminal type.
  - EOR (25) and GA: `IAC GA` / `IAC EOR` mark a prompt boundary.
  - Refuse everything else, including MCCP.
- Cap subnegotiation buffers at 64 KB; discard and log on overflow.
- GMCP: split `"Package.Name {json}"` at the first space; parse with `nlohmann::json::parse(..., nullptr, false)`; emit `GmcpReceived{package, json}`.
- Text is UTF-8; never split a codepoint across spans.

### ANSI → structured lines (use exactly these types)
```cpp
struct TextStyle {
    uint32_t fg = default_fg;   // encodes 16-color, 256-color, and 24-bit truecolor
    uint32_t bg = default_bg;
    uint8_t  flags = 0;         // bold, underline, italic, inverse, blink
};
struct StyledSpan {
    TextStyle style;
    uint32_t  start;            // byte offset into StyledLine::plain
    uint32_t  length;
};
struct StyledLine {
    std::string             plain;   // ANSI-stripped; used for trigger matching
    std::vector<StyledSpan> spans;   // styling over `plain`; used for display
    bool                    is_prompt = false;
};
```
- SGR: 0–9, 22–29, 30–37, 39, 40–47, 49, 90–97, 100–107, `38;5;n`, `48;5;n`, `38;2;r;g;b`, `48;2;r;g;b`. Silently consume other CSI sequences.
- Style persists across lines.
- A line completes on `\n` (strip trailing `\r`), on `IAC GA`/`IAC EOR` (`is_prompt = true`), or after a partial line sits unterminated for 250 ms (`is_prompt = true`).

## 2. Engine Core & Events
- `Event` = `std::variant<Connected, Disconnected, LineReceived{StyledLine}, GmcpReceived, UserInput{std::string}, TimerFired{id}>`.
- `EventDispatcher` on the engine thread with Lua-visible hooks: `on_connect`, `on_disconnect`, `on_line`, `on_gmcp`, `on_input`, `on_timer`.
- Line output path: triggers → gag/recolor → render to stdout with ANSI re-encoded from spans.

## 3. Automation Engine

### Aliases (outbound)
- Evaluated on user input before sending, in registration order with optional priority; first match wins unless `fall_through`.
- Exact-match and PCRE2 regex kinds. Templates support `$0`–`$9` and `$$`.
- Expand to commands (split on configurable separator, default `;`) or call a Lua function with captures as a table.
- Re-evaluate expansions with a recursion depth limit of 10; exceeding it echoes an error and aborts.

### Triggers (inbound)
- Match against `StyledLine::plain`. Options: `gag`, `recolor`, `once`, `enabled`, `priority`, `stop_processing`, `match_prompts`.
- Target: 200 active triggers at 2,000 lines/second on one core, with bounded engine-queue growth.
- Required strategy:
  1. Compile with PCRE2 JIT at registration; reuse `pcre2_match_data` (no per-match allocation).
  2. Prefilter: extract a required literal from each pattern where possible (document the heuristic's limits); build an Aho-Corasick automaton (or justified simpler multi-substring scan). Per line, run only regexes whose literal occurs, plus the no-literal bucket. The prefilter must never skip a regex that would have matched.
  3. Rebuild the prefilter lazily on trigger changes.
- `bench/trigger_bench.cpp` measures lines/second with 200 synthetic triggers and accepts `--min-lines-per-sec N`, exiting nonzero below it.

### Built-in commands
Configurable prefix (default `#`), quote-aware tokenizing:
`#connect <host> <port>`, `#disconnect`, `#lua <code>`, `#alias add|del|list`, `#trigger add|del|list`, `#timer add <seconds> [repeat] <command>`, `#timer list`, `#timer pause|resume|reset|kill <id|label>`, `#quit`.

### Timers (`timer_manager.hpp/.cpp`)
- One `asio::steady_timer` per active timer (justify vs. a wheel in a comment).
- IDs: monotonically increasing `uint64_t`, never reused; optional unique label.
- One-shot and repeating. Repeating timers schedule from the previous deadline, not `now()`; if more than one interval was missed, skip ahead rather than bursting.
- Pause (record remaining, cancel), resume (reschedule remaining), reset (full interval), kill.
- Cancellation semantics (document precisely):
  - Handlers receiving `asio::error::operation_aborted` do nothing.
  - Every handler checks a per-timer generation counter incremented on cancel/pause/reset/kill; stale generations are ignored.
  - The engine drops `TimerFired` events for IDs that no longer exist.
  - Killing a timer from inside its own callback is safe.

## 4. Lua Scripting (`script_engine.hpp/.cpp`)

### Sandbox
- Open only `base, string, table, math, utf8, coroutine`. Not `io, os, debug, package`.
- Remove `dofile`, `loadfile`, `string.dump`; replace `load` with a text-only variant bound to the sandbox environment; restrict `collectgarbage` to `"count"`.
- Provide `os.time`, `os.clock`, `os.date` only.
- Instruction-count hook (`lua_sethook`, `LUA_MASKCOUNT`) aborts any single callback exceeding a configurable budget (default ~10M instructions).

### Error handling
All callbacks are stored and invoked as `sol::protected_function`, engine thread only. Errors echo to display with a traceback and never crash the client.

### `client` API
- `client.send(str)`, `client.echo(str)`
- `client.register_alias(pattern, fn_or_string, opts?) -> id`, `client.remove_alias(id)`
- `client.register_trigger(pattern, fn_or_string, opts?) -> id`, `client.remove_trigger(id)`
- `client.register_timer(seconds, fn_or_string, repeating?, label?) -> id`
- `client.kill_timer(id_or_label)`, `client.pause_timer(...)`, `client.resume_timer(...)`
- `client.get_gmcp(path)`: deep copy as a Lua table, or `nil`; dotted paths (`"Char.Vitals.hp"`).
- `client.on(event_name, fn)`

### GMCP cache
Keyed by package; incoming messages merge with `merge_patch` semantics (arrays replace). `on_gmcp` fires after the cache updates.

## 5. Integration Testing

### Headless mode (CLI flags)
- `--test <script.lua>`: no stdin thread; exits with the code passed to `client.exit(code)`.
- `--timeout <seconds>`: watchdog (default 120); on expiry print the pending step and exit 2.
- `--record <file>`: JSON Lines of every raw inbound chunk (base64, with ms offset from connect, boundaries preserved) and every outbound line.
- `--log-transcript <file>`: rendered, ANSI-stripped session text.
- Host and port come from `argv`, overridable by env vars `MUD_HOST` / `MUD_PORT` in test mode.

### Test API
- `client.exit(code)`
- `client.run_test(fn)`: runs `fn` as a coroutine.
- `client.wait_for(pattern, timeout) -> captures | nil`: coroutine-only; registers a `once` trigger and a one-shot timer, yields, resumes on whichever fires first, cancels the other.
- `client.sleep(seconds)`: coroutine-only, one-shot timer.
- `client.sent_log()`: list of transmitted lines.
- `on_line` handlers receive the line as a table with `plain`, `is_prompt`, and `spans` (each with `fg`, `bg`, `bold`, `underline`).

### Live test: `tests/live/zombiemud.lua` (zombiemud.org, port 3000)
Collect all check results and report a summary table at the end rather than stopping at the first failure.
1. **I/O:** wait for the login screen, send `v` to visit without creating a character.
2. **Aliases:** register `e4` → `east;east;east;east` and `s3` → `south;south;south`; move using only these. Assert `client.sent_log()` shows exactly 4 `east` and 3 `south`, and never the alias names.
3. **Timers:** pace moves with a repeating 1.5 s timer (one command per tick). Assert tick spacing within ±150 ms. Pause for 3 s partway through, resume, assert no ticks while paused. Kill at the end and assert no further ticks.
4. **Triggers:** a trigger on the room/exits line must fire once per successful move. A gag trigger on a known line must not appear in the transcript. A failure trigger on the "can't go that way" message fails the test.
5. **Colors:** assert at least one line has a non-default-color span. If none, report whether ESC bytes were seen at all ("server sent no ANSI") or seen but not parsed ("parser dropped ANSI").
6. **Exit:** send `quit`, wait for disconnect, exit 0 if all checks passed, else 1.
- All server-specific patterns are `local` variables at the top of the script. Determine them from a real recorded session, and quote the transcript lines each is based on in NOTES.md. If the visitor option or route doesn't work as described, document what was observed and adapt to a valid route of similar length.
- Connection refused or timeout before the login screen reports "server unreachable" as a skip, not a failure.

### Replay test (deterministic; gates CI)
- `tests/fake_mud/`: test-only Asio C++ server that replays a `--record` file with original chunk boundaries and relative timing (optional 10× compression), advancing when it receives the corresponding outbound line.
- Fixture: `tests/replay/zombiemud_session.jsonl`.
- A ctest case runs the fake server on an ephemeral port and the client with `--test tests/live/zombiemud.lua` against it; requires exit 0.
- The parser unit tests also replay the fixture at every byte split boundary.

## 6. Portability & Code Quality
- RAII throughout; no raw `new`/`delete`. PCRE2 handles in `unique_ptr` with custom deleters.
- No per-byte allocation in the parser hot path beyond amortized growth of reused buffers.
- Warning-clean: GCC 13 and Clang 17 (`-Wall -Wextra -Wpedantic -Werror`); MSVC 2022 (`/W4 /WX /permissive-`). Flags apply to our targets only.
- Platform-specific code isolated in `src/platform/`:
  - Windows: `_WIN32_WINNT=0x0A00`, `WIN32_LEAN_AND_MEAN`; enable `ENABLE_VIRTUAL_TERMINAL_PROCESSING`; `SetConsoleOutputCP(CP_UTF8)`; static MSVC runtime (`CMAKE_MSVC_RUNTIME_LIBRARY = MultiThreaded$<$<CONFIG:Debug>:Debug>`).
  - Linux release: statically link libstdc++ and libgcc.
- Comments explain why, document state transitions and thread ownership.
- Version comes from `project(... VERSION ...)`, overridable from CI; `--version` prints it.

## 7. CI/CD (GitHub Actions)

### `ci.yml` (push to `main`, all PRs)
- Matrix: `ubuntu-22.04` × {GCC 13, Clang 17}; `windows-2022` × MSVC. Ninja everywhere (MSVC env via `ilammy/msvc-dev-cmd` or equivalent).
- Build, `ctest --output-on-failure`, including the replay integration test (required check).
- Extra Linux job with `ENABLE_ASAN=ON` running tests under ASan/UBSan.
- Cache `FETCHCONTENT_BASE_DIR` keyed on CMake file hashes.
- `concurrency` group cancelling superseded runs per branch.

### `release.yml` (tags `v*.*.*`)
- Release builds on `ubuntu-22.04` (x86_64), `ubuntu-24.04-arm` (aarch64), `windows-2022` (x64). Tests must pass before packaging.
- CPack artifacts `mudclient-<version>-<os>-<arch>.<ext>` containing the binary, `scripts/init.lua`, `LICENSE`, `README.md`.
- `SHA256SUMS` over all artifacts; provenance via `actions/attest-build-provenance`.
- Final job creates the GitHub Release with auto-generated notes and uploads artifacts plus `SHA256SUMS`. Notes include whether the live smoke test passed.
- Calls `live-test.yml` via `workflow_call` with `continue-on-error: true`.

### `live-test.yml`
- Triggers: weekly `schedule`, `workflow_dispatch`, `workflow_call`.
- `ubuntu-22.04` and `windows-2022`, `max-parallel: 1`; repo-wide `concurrency` group.
- Runs `--test tests/live/zombiemud.lua --record live.jsonl --log-transcript transcript.txt --timeout 180`; always uploads both files as artifacts.

### Workflow security
- Pin every third-party action to a full commit SHA with the version tag in a comment.
- Workflow-level `permissions: contents: read`; grant `contents: write`, `id-token: write`, `attestations: write` only to jobs that need them.
- No secrets beyond `GITHUB_TOKEN`.
- Pass untrusted input (branch names, PR titles) via `env:`, never interpolated into `run:`.

## 8. Deliverable Files
`main.cpp` loads `scripts/init.lua` if present. Ship an example `init.lua` registering a 5 s repeating timer that echoes a tick counter, and a trigger on a combat-style line (e.g. `^(\w+) (hits|misses) you`) that echoes a styled message. README covers building, usage, cutting a release (`git tag v0.1.0 && git push --tags`), verifying downloads (checksums and `gh attestation verify`), recording a session, promoting it to the replay fixture, and running the live test locally.

## Milestones
A milestone is complete only when every exit command exits 0 on a clean checkout, CI is green on all matrix entries for the final commit, and the Critic has issued APPROVE.

**M1: Parser, network, events, CI**
Scope: layout, CMake + presets, telnet_parser, network_client, event types and queue, parser tests (every-byte-split, covering IAC, SB/SE, `IAC IAC`, split ANSI, split UTF-8), ci.yml.
Exit:
- `cmake --preset release && cmake --build --preset release`
- `ctest --preset release --output-on-failure`
- `cmake --preset asan && cmake --build --preset asan && ctest --preset asan`

**M2: Automation engine**
Scope: timer_manager, trigger_manager with prefilter, aliases, built-in commands, trigger benchmark.
Exit: M1 commands, plus `./build/release/bench/trigger_bench --min-lines-per-sec 2000`.

**M3: Scripting, testing, release**
Scope: script_engine, GMCP cache, main.cpp, headless mode, test API, fake_mud, live script (patterns resolved from a real recording), replay fixture, live-test.yml, release.yml, README.
Exit: M2 commands, plus replay test green on Linux and Windows CI, plus one successful live run. After merge, tag `v0.1.0` and confirm release.yml publishes all artifacts.
