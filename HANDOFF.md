# HANDOFF: Milestone 1

**Repository:** https://github.com/Sediket/mudclient
**Branch:** `m1`
**Commit SHA:** `13d39a69eaad55a1a5c06b10ea9eefcbf9c5dd07`
**PR:** https://github.com/Sediket/mudclient/pull/1

## What was built

Per `docs/SPEC.md` M1 scope:

- **Repo layout, CMake, presets:** top-level `CMakeLists.txt` using
  `FetchContent` (declared `SYSTEM`) for standalone Asio, `nlohmann/json`,
  and Catch2 v3; `CMakePresets.json` with `release`/`debug`/`asan` configure,
  build, and test presets; `-Wall -Wextra -Wpedantic -Werror` (GCC/Clang) and
  equivalent MSVC flags applied only to our own targets via an INTERFACE
  library; static MSVC runtime set globally; CPack `.tar.gz`/`.zip` install
  rules (verified locally — see below).
- **`telnet_parser.hpp/.cpp`:** streaming byte-at-a-time FSM
  (`Data/Iac/Will/Wont/Do/Dont/Sb/SbData/SbDataIac/Esc/Csi`), fully resumable
  across chunk boundaries. RFC 1143 Q-method option negotiation for
  GMCP (accept WILL → DO, send `Core.Hello`/`Core.Supports.Set`),
  NAWS/TTYPE (accept DO → WILL, answer NAWS size and TTYPE SEND), EOR
  (accept WILL → DO), and refusal of everything else including MCCP.
  64KB subnegotiation cap with discard+counter on overflow. ANSI SGR
  (0–9, 22–29, 30–37, 39, 40–47, 49, 90–97, 100–107, `38;5;n`, `48;5;n`,
  `38;2;r;g;b`, `48;2;r;g;b`) applied to `StyledLine`/`StyledSpan` with style
  persisting across lines. 250ms idle-flush path via `flush_idle()` (driven
  externally by `NetworkClient`'s `asio::steady_timer`, per the spec's
  threading model — the parser itself does no I/O or timing).
- **`network_client.hpp/.cpp`:** async resolve+connect with configurable
  timeout, single outstanding `async_read_some` into a 16KB buffer feeding
  the parser, outbound write queue with at most one `async_write` in flight,
  outbound `0xFF` escaping and `\r\n` termination, `Connected`/`Disconnected`
  events. Lifetime strategy documented in the header/source: every handler
  captures `shared_from_this()`, and `disconnect()`/`fail()` set a `stopped_`
  flag and cancel/close every I/O object so in-flight handlers observe
  `operation_aborted` (or `stopped_`) and return without touching state.
- **`events.hpp`/`event_queue.hpp`:** the `Event` variant and thread-safe MPSC
  `EventQueue` (condition-variable wait with timeout, no busy-spin) per the
  spec's threading model. Each cross-thread handoff is commented with which
  thread owns the data before/after the push.
- **Parser tests** (`tests/test_telnet_parser.cpp`): a generic
  `check_every_split()` harness that, for every test input, compares a
  single-call `feed()` against (a) every possible two-way split point,
  (b) a full one-byte-at-a-time feed, and (c) 50 seeded random multi-way
  splits — then asserts byte-for-byte identical output (events + negotiation
  bytes) in all cases. Used across plain lines, `IAC IAC` in data, `IAC
  GA`/`IAC EOR` prompts, GMCP negotiation + SB parsing (including a literal
  `0xFF` inside a subnegotiation payload), NAWS/TTYPE negotiation (including
  a NAWS size byte of 255 needing IAC-escaping), Q-method idempotency
  (repeated WILL/DO don't loop), all SGR color modes, style persistence
  across lines, non-SGR CSI/bare-ESC handling, UTF-8 multibyte codepoints
  under arbitrary splits, subnegotiation overflow (over and exactly at the
  64KB cap), and a malformed-SB resynchronization case. Plus
  `tests/test_event_queue.cpp` (timeout, FIFO, concurrent producers) and
  `tests/test_network_client.cpp` (real loopback TCP integration: connect/
  send/receive/disconnect, connect-timeout, and a repeated
  connect+immediate-disconnect race regression test, the last of which is
  the closest thing to a lifetime/UAF stress test available without a
  purpose-built fuzzer).
- **`ci.yml`:** matrix `ubuntu-22.04` × {GCC 13 via `ubuntu-toolchain-r/test`
  PPA, Clang 17 via apt.llvm.org}, `windows-2022` × MSVC (via
  `ilammy/msvc-dev-cmd`), plus a separate `ubuntu-22.04` ASan/UBSan job.
  `FETCHCONTENT_BASE_DIR` is pinned to `build/_deps` in `CMakePresets.json`
  and cached via `actions/cache` keyed on
  `hashFiles('CMakeLists.txt', 'CMakePresets.json')`. Workflow-level
  `permissions: contents: read`; `concurrency` group cancels superseded runs
  per ref. Every third-party action is pinned to a full commit SHA (obtained
  via `git ls-remote --tags` against each action's repo) with the version
  tag in a trailing comment.

## Exit commands — results

All three run clean on a fresh checkout of `13d39a69eaad55a1a5c06b10ea9eefcbf9c5dd07`
(verified locally with GCC 13.3.0 and, additionally, Clang 18.1.3 — this
container did not have Clang 17 specifically, but CI's `linux-clang17` job
below covers that exact version):

```
$ cmake --preset release && cmake --build --preset release
... (clean build, -Wall -Wextra -Wpedantic -Werror, no warnings)
$ ctest --preset release --output-on-failure
100% tests passed, 0 tests failed out of 28
$ cmake --preset asan && cmake --build --preset asan && ctest --preset asan
100% tests passed, 0 tests failed out of 28
```

`cpack` (TGZ generator) verified locally to produce
`mudclient-0.1.0-linux-x86_64.tar.gz` containing the binary, `LICENSE`, and
`README.md` (`scripts/init.lua` is `OPTIONAL` and doesn't exist yet — it's
M3 scope per the spec).

## CI: green on the exact commit under review

PR: https://github.com/Sediket/mudclient/pull/1 (branch `m1` → `main`)
Workflow run: https://github.com/Sediket/mudclient/actions/runs/36206869041
(triggered by `13d39a69eaad55a1a5c06b10ea9eefcbf9c5dd07`, the current tip of `m1`)

| Job | Conclusion | Job log URL |
|---|---|---|
| `linux-gcc13` | success (28/28 tests) | https://github.com/Sediket/mudclient/actions/runs/36206869041/job/108305141626 |
| `linux-clang17` | success (28/28 tests) | https://github.com/Sediket/mudclient/actions/runs/36206869041/job/108305141594 |
| `windows-msvc` | success (28/28 tests) | https://github.com/Sediket/mudclient/actions/runs/36206869041/job/108305141476 |
| `linux-asan` | success (28/28 tests, ASan+UBSan) | https://github.com/Sediket/mudclient/actions/runs/36206869041/job/108305141624 |

I fetched and read the tail of each job's raw log (not just the green
checkmark) to confirm each one actually compiled from source and ran all 28
tests to completion — none short-circuited or skipped.

## Known limitations / things I'd flag for review myself

- **RFC 1143 "queued opposite" branches are reactive-only.** This
  implementation never spontaneously initiates a WantYes/WantNo on our own
  (we only ever answer WILL/WONT/DO/DONT with the immediate Yes/No
  transition — see `on_him_became_yes`/`on_us_became_yes`, which are the only
  places we send unsolicited bytes, and those are subnegotiations, not
  option-negotiation commands). The `queued_opposite` branches in
  `handle_will/wont/do/dont` exist for protocol correctness against a peer
  that re-negotiates rapidly, and are covered by the "Q-method: repeated
  WILL/DO" test for the receive side, but there is no test that drives our
  *own* WantYes/WantNo queuing path (we'd need to initiate a negotiation
  ourselves to exercise it, which we don't do in M1). Documented in the
  header comment above `handle_will`.
- **GMCP JSON validation happens in the parser, not later.** I read
  spec §1 as putting `nlohmann::json::parse(..., nullptr, false)` in the
  telnet-parser layer (it's listed under "Telnet parser" in the spec), so
  `GmcpMessage`/`GmcpReceived` carry a `nlohmann::json` (parsed, possibly
  `.is_discarded()`) rather than a raw string. Noted in NOTES.md.
- **CMake fetches only the M1-relevant dependencies** (Asio, nlohmann/json,
  Catch2). sol2, PCRE2, and Lua 5.4 are deferred to M2/M3 when they're
  actually used, rather than fetched unused from M1. Noted in NOTES.md as a
  deliberate incremental-scope deviation from the spec's dependency list,
  which describes the project's final state.
- **`network_client` tests are integration-style** (real loopback sockets, a
  background io_context thread), not spec-mandated, but I added them because
  the threading/lifetime rules in the spec are exactly the kind of thing
  that looks right on inspection and fails under real concurrency. I'd
  specifically want the Critic's mutation-testing pass to target
  `disconnect()`/`fail()`'s `stopped_` guard and the `shared_from_this()`
  capture in every handler.
- **No `main.cpp` engine wiring yet.** M1 scope per the spec is parser +
  network + events + CI; `main.cpp` is currently a placeholder that only
  handles `--version`. Full engine wiring (stdin thread, dispatcher, Lua)
  is M2/M3 scope.
- Bootstrap commit deviation (pushed directly to `main` instead of via PR,
  because the repo had no `main` ref yet) is recorded in `NOTES.md`.

## Response to prior Critic findings

None yet — this is round 1.
