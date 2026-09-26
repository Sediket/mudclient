# mudclient

A high-performance, modular MUD client core in C++20 with embedded Lua
scripting. See [`docs/SPEC.md`](docs/SPEC.md) for the full specification and
[`NOTES.md`](NOTES.md) for the running log of decisions and deviations.

## Building (Linux/Windows, GCC 13 / Clang 17 / MSVC 2022)

Requires CMake ≥ 3.25 and Ninja. All dependencies (Asio, nlohmann/json,
PCRE2, Lua 5.4, sol2, Catch2) are fetched automatically via `FetchContent`.

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release --output-on-failure
```

`ctest` includes the deterministic replay test (`tests/fake_mud` replaying
`tests/replay/zombiemud_session.jsonl` against the client running
`tests/live/zombiemud.lua`), so a clean `ctest` run exercises the full
scripting/networking/automation stack with no real network access needed.

Debug and sanitizer builds:

```sh
cmake --preset asan
cmake --build --preset asan
ctest --preset asan --output-on-failure
```

## Usage

Interactive mode connects to a MUD and gives you a normal terminal session
with automation running underneath:

```sh
./build/release/mudclient <host> <port>
```

On startup, `scripts/init.lua` is loaded automatically if present in the
working directory — see that file for a small example (a repeating timer
and a combat-line trigger) to build your own scripts from. Built-in
commands (prefixed `#`) are available directly at the prompt, e.g.:

```
#alias add e4 east;east;east;east
#trigger add "^(\w+) hits you" flee
#timer add 5 repeat look
#lua client.echo("hello from Lua")
```

(`#alias add`/`#trigger add` take exactly a pattern and a single
whitespace-free expansion token; `#timer add <seconds> [repeat] <command>`
sends `<command>` on every tick. For anything richer — multi-word
expansions, capture-aware trigger actions, per-trigger options like `gag`
— write it directly in Lua with `client.register_alias`/
`client.register_trigger`/`client.register_timer`, either in
`scripts/init.lua` or via `#lua`.)

Anything not recognized as a built-in command (starting with `#`, or a
registered alias) is sent to the server as-is.

### Headless / scripted mode

```sh
./build/release/mudclient <host> <port> --test my_script.lua [options]
```

- `--test <script>`: runs `<script>` instead of `scripts/init.lua`, with no
  stdin/terminal interaction. The script drives the session and calls
  `client.exit(code)` when done.
- `--timeout <seconds>` (default 120): watchdog; if the script never calls
  `client.exit`, the process prints a diagnostic and exits with code 2.
- `--record <file>`: records every inbound chunk (base64) and outbound line
  as JSON Lines, with millisecond offsets from connect — see
  [Recording a session](#recording-a-session-and-building-a-replay-fixture).
- `--log-transcript <file>`: writes the rendered, ANSI-stripped session text.
- In `--test` mode only, the host/port can be overridden by the `MUD_HOST` /
  `MUD_PORT` environment variables (used by the replay test to point the
  client at an ephemeral `fake_mud` port).

Test scripts get an additional API on top of the normal scripting surface:
`client.run_test(fn)` (runs `fn` as a coroutine), `client.wait_for(pattern,
timeout)` (yields until a line matches or the timeout fires), `client.sleep
(seconds)`, and `client.sent_log()` (everything the client has sent so far).
See `tests/live/zombiemud.lua` for a full example.

## Scripting

Scripts run in a sandboxed Lua 5.4 (`sol2`), with `dofile`/`loadfile`/
`require`/`string.dump` removed, a per-callback instruction budget, and only
`base`/`string`/`table`/`math`/`utf8`/`coroutine` opened. The client API:

- `client.send(line)` — runs `line` through alias expansion, then sends it.
- `client.echo(text)` — writes `text` straight to the terminal (embed a raw
  ANSI escape in the string to style it yourself).
- `client.register_alias(pattern, replacement)` / `client.remove_alias(id)`
- `client.register_trigger(pattern, fn, opts)` / `client.remove_trigger(id)`
  — `opts` may set `gag`, `recolor`, `once`, `enabled`, `priority`,
  `stop_processing`, `match_prompts`.
- `client.register_timer(seconds, fn, repeating, label)` /
  `client.pause_timer/resume_timer/kill_timer(id_or_label)`
- `client.get_gmcp(dotted.path)` — reads the merged GMCP cache.
- `client.on(event, fn)` — `event` is one of `connect`, `disconnect`,
  `line`, `gmcp`, `timer`, `input`. A `line` handler receives a table with
  `plain`, `is_prompt`, and `spans` (each with `fg`, `bg`, `bold`,
  `underline`, `start`, `length`).
- `client.exit(code)` (test mode only, but harmless elsewhere).

## Recording a session and building a replay fixture

The replay test (`tests/fake_mud` + `tests/run_replay_test.py`, wired into
`ctest` as `replay_zombiemud`) lets CI exercise a full scripted session
deterministically, with no live server required. To build or refresh a
fixture from a real session:

1. Record it: `./build/release/mudclient <host> <port> --test
   your_script.lua --record tests/replay/your_session.jsonl --timeout 180`.
2. Point `tests/run_replay_test.py` (or a new `ctest` case) at that
   recording and the script that produced it — `tests/fake_mud` replays the
   recorded inbound bytes at (scaled) original timing and the real
   `mudclient` binary runs against it exactly as it would against the live
   server.
3. Every server-specific pattern in the script should be `local` and quoted
   back to a specific line in the recorded transcript, documented in
   `NOTES.md`, so the fixture's provenance is auditable.

**`tests/live/zombiemud.lua` and `tests/replay/zombiemud_session.jsonl` are
currently a synthetic placeholder**, not a real recording from
`zombiemud.org:3000` — outbound access to that host has been unavailable in
the environment these were built in. See `NOTES.md` ("Live test /
zombiemud") for the full account; once network access is available, redo
steps 1–3 above against the real server and replace both files.

## Running the live test locally

```sh
./build/release/mudclient zombiemud.org 3000 --test tests/live/zombiemud.lua \
  --record live.jsonl --log-transcript transcript.txt --timeout 180
```

Exits 0 if every check passed, 1 if any failed (see stdout for a `PASS`/
`FAIL` summary), or 0 with a `SKIP: server unreachable` message if the
connection never got as far as the login screen. `live-test.yml` runs this
on a weekly schedule, on manual dispatch, and as part of `release.yml`
(where a failure doesn't block the release, but is noted in its notes).

## Cutting a release

```sh
git tag v0.1.0
git push --tags
```

`release.yml` then builds and tests on Linux x86_64, Linux aarch64, and
Windows x64, packages each with CPack as
`mudclient-<version>-<os>-<arch>.<ext>` (binary, `scripts/init.lua`,
`LICENSE`, `README.md`), computes a `SHA256SUMS` over all of them, attests
build provenance, runs the live smoke test, and publishes a GitHub Release
with all artifacts plus `SHA256SUMS` and auto-generated notes (including
whether the live smoke test passed).

### Verifying a downloaded release

```sh
sha256sum -c SHA256SUMS --ignore-missing
gh attestation verify mudclient-<version>-<os>-<arch>.<ext> --repo <owner>/mudclient
```

## Status

All three milestones (parser/network/events, the automation engine, and
scripting/testing/release) are implemented; see `docs/SPEC.md` for the full
scope and `NOTES.md` for the milestone-by-milestone history, deviations, and
Critic review outcomes.
