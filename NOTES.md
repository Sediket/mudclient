# NOTES

Persistent log of decisions, assumptions, and spec deviations. Every deviation includes a reason.

## Bootstrap

- Repository: `Sediket/mudclient`, currently empty (no commits). Working branch for agent session: `claude/relaxed-johnson-dbsb3w`. Per the spec's own protocol, `docs/SPEC.md`, `docs/agents/PROTOCOL.md`, `docs/agents/CRITIC.md`, and this file are committed to `main` via a PR titled "Bootstrap spec and agent docs", then milestone work proceeds on branch `m1`.
- GitHub access confirmed via `mcp__github__get_me` (user: Sediket). No `gh` CLI available in this environment; all GitHub operations go through the `mcp__github__*` MCP tools per the session's operating instructions, which supersedes the literal "Shell, git, `gh`" wording in PROTOCOL.md — this is a tooling substitution, not a scope change.

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
