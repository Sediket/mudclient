# NOTES

Persistent log of decisions, assumptions, and spec deviations. Every deviation includes a reason.

## Bootstrap

- Repository: `Sediket/mudclient`, currently empty (no commits). Working branch for agent session: `claude/relaxed-johnson-dbsb3w`. Per the spec's own protocol, `docs/SPEC.md`, `docs/agents/PROTOCOL.md`, `docs/agents/CRITIC.md`, and this file are committed to `main` via a PR titled "Bootstrap spec and agent docs", then milestone work proceeds on branch `m1`.
- GitHub access confirmed via `mcp__github__get_me` (user: Sediket). No `gh` CLI available in this environment; all GitHub operations go through the `mcp__github__*` MCP tools per the session's operating instructions, which supersedes the literal "Shell, git, `gh`" wording in PROTOCOL.md — this is a tooling substitution, not a scope change.

## M2: Automation engine

- **PCRE2 added via FetchContent in M2** (target `pcre2-8-static`, tag `pcre2-10.45`), completing the dependency set alongside Asio/nlohmann-json/Catch2 already fetched in M1. Per the M1 NOTES entry, dependencies are fetched incrementally as each milestone actually needs them rather than all fetched unused from M1.
- **`TriggerOptions`/`AliasOptions` are free namespace-scope structs, not nested inside `TriggerManager`/`AliasManager`.** Both GCC 13.3 and Clang 18.1 reject a nested aggregate struct with default member initializers used as a `= {}` default argument on a *sibling* member function of the same enclosing class (confirmed via a minimal reproduction: `class M { struct O { bool b=false; }; void f(O o={}); };` fails to compile on both compilers with "default member initializer ... required/needed ... before the end of its enclosing class"). Pulling the options structs out to namespace scope (with a `using Options = ...;` alias inside the manager class for a clean call-site name) resolves it with no semantic change. Documented in both headers.
- **Trigger prefilter: per-trigger required-literal substring scan instead of a shared Aho-Corasick automaton.** The spec explicitly allows "a justified simpler multi-substring scan" as an alternative. At the target scale (200 triggers), the simpler approach — each trigger independently checks `line.plain.find(required_literal)` — measured at ~50,000+ lines/sec in this environment (25x the 2,000 lines/sec target) with a corpus mixing matching and non-matching lines, so building and lazily rebuilding a combined automaton wasn't justified by the actual performance need. `extract_required_literal()`'s heuristic and its safety property (never claim a substring is required when the pattern could match without it) are documented in `regex_pattern.hpp` and covered by an extensive test suite in `tests/test_regex_pattern.cpp`, including the specific hazard of a literal run inside an optionally-quantified group being wrongly promoted to "required".
- **Alias regex matching uses full-match semantics; trigger regex matching uses substring/anywhere semantics.** The spec doesn't explicitly state this distinction for aliases. Following the common MUD-client convention (an alias replaces what you typed, so the *whole* typed command should match, not just a substring of it), `AliasManager` compiles regex-kind patterns wrapped as `\A(?:pattern)\z`. Triggers match `StyledLine::plain` per spec text with no such wrapping, matching anywhere in the line, which is the universal convention for trigger patterns (e.g. `you (win|lose)` should fire regardless of what precedes it on the line).
- **Reentrancy safety in `TriggerManager::process_line` and `AliasManager::expand_recursive`.** Neither M2 component has Lua wired in yet, but both `Action`/`FunctionAction` callback types are designed so that in M3 a Lua callback can freely call back into the same manager (e.g. `client.remove_trigger(id)` from inside a trigger's own callback) without holding a stale reference/iterator into a `std::vector` that a nested `add_trigger`/`remove_trigger` call might reallocate or shrink. Both methods snapshot the ids to process before running any callback and re-look-up each entry by id immediately before use; a regression test for this (`tests/test_trigger_manager.cpp` / `tests/test_alias_manager.cpp`, "...mid-line/mid-expansion does not corrupt iteration") exercises exactly this pattern and passes clean under ASan/UBSan.
- **ASan caught a real stack-use-after-return bug in `bench/trigger_bench.cpp` itself** (not library code): a local `int fired` counter was captured by reference into a lambda stored inside `TriggerManager` past the end of the function that declared it. Fixed by removing the (unnecessary, since `TriggerManager::MatchOutcome::fired_count` already reports this) per-trigger counter. Left as a NOTES entry since it's good evidence the ASan job is actually exercising the code, not just building it.
- **`command_parser` validates only shape (arity/subcommand-name), not semantics.** `#connect <host> <port>` is accepted with any two tokens; whether the host resolves, the port is numeric, etc. is the dispatcher's job. Full dispatch into `NetworkClient`/`TimerManager`/`TriggerManager`/`AliasManager`/Lua is engine wiring, which per spec scope belongs to M3 (`main.cpp`, headless mode) alongside `script_engine`. `command_parser` is deliberately usable and fully tested standalone ahead of that wiring.

## Deviations tracker (append as they occur)

- **M1 Critic round 1: APPROVE with 2 non-blocking findings (`reviews/m1-round1.json`).**
  M1-F1 (HANDOFF.md's embedded "review this commit" SHA lagged one commit
  behind its own tip; the Critic verified the actual tip's CI independently)
  is left as-is since it's a historical record of the review conversation,
  not something worth another edit cycle. M1-F2 (a truncated extended-color
  SGR sequence like `ESC[38;5m` fell through and reinterpreted its leftover
  sub-parameter as an unrelated top-level SGR code) was fixed before merge:
  `apply_sgr` now `break`s out of the SGR code loop on a truncated 38/48
  sequence instead of continuing, with a regression test
  (`tests/test_telnet_parser.cpp`, "truncated extended-color sequence").

- **Bootstrap commit pushed directly to `main` instead of via PR.** The remote repository had zero commits and no `main` ref, so a pull request (which requires an existing base branch) was not possible for the very first commit. Pushed the bootstrap commit directly to establish `main`, then created `m1` from it. All subsequent milestone work goes through PRs as specified.
