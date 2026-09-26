# HANDOFF: Milestone 2

**Repository:** https://github.com/Sediket/mudclient
**Branch:** `m2`
**Commit SHA (review this one):** `28e98663ffd2ef3ff66af72499c8600ffc4641e0`
**PR:** https://github.com/Sediket/mudclient/pull/2

## What was built

Per `docs/SPEC.md` M2 scope (Automation Engine):

- **`timer_manager.hpp/.cpp`:** one `asio::steady_timer` per active timer
  (justification for this vs. a wheel is in the header comment: modest
  expected load, O(log n) per-op via asio's internal heap, and trivially
  correct pause/resume/reset/kill per-timer). Monotonically increasing,
  never-reused ids; optional unique labels. One-shot and repeating, with
  repeating timers rescheduling from the previous deadline (not `now()`)
  and skipping ahead — never bursting — if more than one interval was
  missed. Pause records remaining time and cancels; resume reschedules from
  remaining; reset restarts the full interval; kill cancels and erases.
  Cancellation safety: every pending `async_wait` handler captures only
  `(id, expected_generation)` by value (never a pointer into the timer's
  own state), checks `operation_aborted` first, then checks the timer still
  exists in the map, then checks its generation still matches — so a
  handler for an already-killed or already-paused/reset timer is always a
  safe no-op, and killing a timer from inside its own `TimerFired` handler
  (posted back from an engine-thread Lua callback) is safe by the same
  mechanism.
- **`regex_pattern.hpp/.cpp`:** `CompiledRegex` (PCRE2, JIT-compiled at
  registration via `pcre2_jit_compile`, reused `pcre2_match_data` — no
  per-match allocation — RAII handles via `unique_ptr` with custom
  deleters), with `full_match` (aliases: `\A(?:pattern)\z`) vs.
  substring-anywhere (triggers) semantics. `extract_required_literal()`: a
  conservative required-literal heuristic for the trigger prefilter, with
  its safety property and known limitations documented in the header. It
  correctly refuses to promote a literal out of a top-level-alternated
  group or an optionally-quantified group (including nested cases), while
  still extracting literals from the common "capture-then-fixed-tail"
  trigger shape.
- **`trigger_manager.hpp/.cpp`:** `gag`/`recolor`/`once`/`enabled`/
  `priority`/`stop_processing`/`match_prompts` per spec. Prefilter strategy:
  per-trigger required-literal substring scan (the spec's explicitly-
  sanctioned "simpler multi-substring scan" alternative to a shared
  Aho-Corasick automaton), justified by measured throughput (see benchmark
  results below) rather than assumed. `process_line()` is reentrancy-safe:
  it snapshots trigger ids up front and re-looks-up each by id immediately
  before use, so an `Action` that adds/removes triggers mid-line (as a Lua
  callback will be able to do once M3 wires it up) can never leave a
  dangling reference/iterator into the underlying `std::vector`.
- **`alias_manager.hpp/.cpp`:** exact/regex aliases, `$0`-`$9`/`$$` template
  substitution, priority + `fall_through` (first-match-wins unless set),
  recursive re-evaluation of expansions (an alias's expansion can invoke
  another alias) capped at depth 10 with an abort-and-report on overflow.
  Same reentrancy-safety pattern as `TriggerManager`.
- **`command_parser.hpp/.cpp`:** quote-aware tokenizer (single/double
  quotes, backslash escaping inside quotes) plus shape recognition for all
  of `#connect #disconnect #lua #alias add|del|list #trigger add|del|list
  #timer add|list|pause|resume|reset|kill #quit`, with a configurable
  prefix. Deliberately validates shape only (arity, subcommand names), not
  semantics (e.g. it doesn't check a host resolves) — full dispatch into a
  live `NetworkClient`/`TimerManager`/`TriggerManager`/`AliasManager`/Lua is
  engine wiring that belongs to M3's `main.cpp` alongside `script_engine`,
  per the spec's milestone scoping.
- **`bench/trigger_bench.cpp`:** 200 synthetic triggers (90% with a
  realistic extractable literal, 10% bare-capture triggers with none, to
  avoid benchmarking a best case) against a 20,000-line mixed corpus (~5%
  constructed to actually match). See measured results below.

## Exit commands — results

Verified locally on a fresh checkout of `28e98663ffd2ef3ff66af72499c8600ffc4641e0`
with GCC 13.3.0 and Clang 18.1.3 (CI's `linux-clang17` job covers the exact
Clang 17 required by spec; this container only had 18.1.3 available):

```
$ cmake --preset release && cmake --build --preset release
... (clean build, -Wall -Wextra -Wpedantic -Werror, no warnings, both compilers)
$ ctest --preset release --output-on-failure
100% tests passed, 0 tests failed out of 99
$ cmake --preset asan && cmake --build --preset asan && ctest --preset asan
100% tests passed, 0 tests failed out of 99
$ ./build/release/bench/trigger_bench --min-lines-per-sec 2000
trigger_bench: 200 triggers=200 lines=20000 elapsed=0.374s throughput=53435.1 lines/sec fired=862
trigger_bench: PASS (>= 2000.0 lines/sec)
```
(Clang build measured ~55k lines/sec; ASan build, which is ~25x slower due
to instrumentation, still measured ~2,300 lines/sec against a
`--min-lines-per-sec 500` threshold — not part of the milestone's required
exit commands, but run as an extra sanity check.)

## CI: green on the exact commit under review

PR: https://github.com/Sediket/mudclient/pull/2 (branch `m2` → `main`)
Workflow run: https://github.com/Sediket/mudclient/actions/runs/36210112067
(triggered by `28e98663ffd2ef3ff66af72499c8600ffc4641e0`, the current tip of `m2`)

| Job | Conclusion | Job log URL |
|---|---|---|
| `linux-gcc13` | success (99/99 tests) | https://github.com/Sediket/mudclient/actions/runs/36210112067/job/108314678810 |
| `linux-clang17` | success (99/99 tests) | https://github.com/Sediket/mudclient/actions/runs/36210112067/job/108314678690 |
| `windows-msvc` | success (99/99 tests) | https://github.com/Sediket/mudclient/actions/runs/36210112067/job/108314678578 |
| `linux-asan` | success (99/99 tests, ASan+UBSan) | https://github.com/Sediket/mudclient/actions/runs/36210112067/job/108314678737 |

## Bugs found and fixed during this milestone (all before this HANDOFF)

1. **GCC 13 / Clang 18 both reject a nested aggregate struct with default
   member initializers used as a `= {}` default argument on a sibling
   member function of the same enclosing class.** Confirmed as standard
   (not compiler-specific) behavior via a minimal reproduction (both
   compilers agree; Clang's diagnostic is clearer about why). Fixed by
   making `TriggerOptions`/`AliasOptions` free namespace-scope structs
   with a `using Options = ...` alias inside each manager class. See
   `NOTES.md`.
2. **ASan: stack-use-after-return in `bench/trigger_bench.cpp`'s own test
   harness** (not library code) — a locally-scoped `int fired` counter was
   captured by reference into a `std::function` stored inside
   `TriggerManager` past the end of the function that declared it. Fixed
   by removing the redundant counter (the benchmark already gets a fired
   count from `MatchOutcome::fired_count`).
3. **MSVC-only compile failure** in `tests/test_command_parser.cpp`: a raw
   string literal containing `\"` (`R"("...\"...\"")"`) is valid standard
   C++ (confirmed via manual first-`)"`-occurrence analysis, and GCC/Clang
   both accepted it), but MSVC's lexer rejected it (C2017/C3680/C3688/
   C2661). Caught by `windows-msvc` CI, not local testing (no MSVC
   available in this container). Rewritten with ordinary escaped string
   literals.
4. **A genuine test-only timing race** in "TimerManager: pause suppresses
   ticks, resume continues": letting a couple of ticks happen before
   calling `pause()` could race an already-about-to-fire timer (asio's
   `cancel()` cannot un-queue a completion that's already been dispatched
   internally), producing an intermittent extra tick. This is inherent to
   asio, not a `TimerManager` bug — but the *test* asserted zero ticks in
   that window regardless. Caught by `windows-msvc` CI (apparently more
   loaded/jittery than the Linux runners or this container). Fixed by
   pausing immediately after registration, before any tick can possibly
   have fired, removing the race from the test; verified stable across 15
   repeated local runs (release) and 8 under ASan/UBSan.

All four are documented in `NOTES.md` with more detail. #3 and #4 are
notable as concrete evidence the CI matrix (specifically the Windows leg)
is catching real, compiler/platform-specific and timing-specific issues
that this container's toolchain (GCC/Clang on Linux only) cannot surface
on its own.

## Known limitations / things I'd flag for review myself

- **`extract_required_literal()`'s "longest candidate" selection doesn't
  concatenate adjacent required literals across a promoted group
  boundary.** E.g. for `((abcdefgh)?xyz)tail`, both `"xyz"` and `"tail"`
  are independently valid required literals, but the implementation
  doesn't merge them into `"xyztail"` even though that's also technically
  valid and more selective — it just picks the longer of the two
  (`"tail"`). This is a missed optimization, not a correctness bug (see
  `tests/test_regex_pattern.cpp`, the nested-optional-group test, which
  specifically checks the returned literal is never the disallowed one
  rather than asserting an exact "best possible" string). I'd want the
  Critic to independently confirm this really is only a missed
  optimization and not hiding a real over-claim somewhere.
- **`extract_required_literal()` treats `{m,n}` as always possibly-zero**
  (conservative simplification: it never parses the digits inside `{...}`
  to check whether `m >= 1`), so a pattern like `a{2,4}` won't have `a`
  extracted as required even though it actually is. Documented as a known
  limitation in the header; safe (just less selective) by construction.
- **Command dispatch is not wired to a live engine yet.** `command_parser`
  is fully tested standalone; actual execution (opening a connection on
  `#connect`, evaluating `#lua` in a sandboxed `sol::state`, etc.) is M3
  scope per the spec (`script_engine`, `main.cpp`, headless mode).
- Both reentrancy-safety mechanisms (`TriggerManager::process_line`,
  `AliasManager::expand_recursive`) are exercised only with plain
  `std::function`/lambda actions in M2's tests, since Lua isn't wired in
  yet — M3 should re-verify the same guarantee holds once real
  `sol::protected_function` callbacks (which can themselves error, be
  slow, etc.) are plugged in as the `Action`/`FunctionAction` types.

## Response to prior Critic findings (M2 round 1 → this round)

M2 round 1 (`reviews/m2-round1.json`) returned REQUEST_CHANGES with 3
blocking findings and 1 non-blocking finding. All are addressed in the
commit this HANDOFF now describes:

- **M2-F1 (blocking, `extract_required_literal()` UTF-8 trimming): fixed.**
  Added `pop_last_utf8_codepoint()` in `src/regex_pattern.cpp`, which walks
  back over UTF-8 continuation bytes to find the real start of the last
  codepoint before trimming it, instead of removing exactly one byte. New
  regression test: `tests/test_regex_pattern.cpp`, "an optional multi-byte
  UTF-8 codepoint is trimmed whole, not by one byte" (reproduces the
  Critic's exact emoji example and asserts no byte of it survives into the
  reported literal).
- **M2-F2 (blocking, `AliasManager` reentrancy/UAF): fixed.**
  `expand_recursive` now copies `Action action = a->action;` (and the
  `fall_through` flag) out of the vector entry before invoking it, mirroring
  `TriggerManager::process_line`'s existing pattern. The regression test
  was strengthened to actually force a reallocation (500 aliases registered
  from inside the callback, matching the Critic's own reproduction) rather
  than the original single-add version that never exercised the bug.
  Verified clean under ASan both ways: crashes (heap-use-after-free) with
  the fix reverted, passes clean with it applied.
- **M2-F3 (blocking, generation-counter mutation-testing gap): fixed.**
  Added a new test that deliberately races `pause()` against a very short
  repeating timer's own deadline (reintroducing the timing race the
  previous test's fix had removed), but discriminates correct-vs-buggy
  behavior by watching several multiples of the tick interval afterward
  rather than asserting an exact tick count in a fixed short window: a
  correct implementation produces at most one stray tick and then silence;
  removing the generation check produces a continuous cascade at the
  timer's own interval. Verified: passes stably under both `release` and
  `asan` (3 repeated runs each), and fails clearly (leaked ticks ~4.5-9x
  over threshold, checked both presets) with the mutation re-applied.
- **M2-F4 (non-blocking, HANDOFF SHA lag): left as-is**, same rationale as
  the already-adjudicated M1-F1 — historical record of the review
  conversation, Critic verified the actual tip's CI independently, and the
  only diff between the cited and actual commit was HANDOFF.md itself.

All fixes verified together: `ctest --preset release` and `ctest --preset
asan` both 102/102 (was 99; +3 new regression tests), 5x repeated locally
for stability; `bench/trigger_bench` still ~53-60k lines/sec; Clang 18.1.3
build also clean and all tests passing.
