# NOTES

Persistent log of decisions, assumptions, and spec deviations. Every deviation includes a reason.

## Bootstrap

- Repository: `Sediket/mudclient`, currently empty (no commits). Working branch for agent session: `claude/relaxed-johnson-dbsb3w`. Per the spec's own protocol, `docs/SPEC.md`, `docs/agents/PROTOCOL.md`, `docs/agents/CRITIC.md`, and this file are committed to `main` via a PR titled "Bootstrap spec and agent docs", then milestone work proceeds on branch `m1`.
- GitHub access confirmed via `mcp__github__get_me` (user: Sediket). No `gh` CLI available in this environment; all GitHub operations go through the `mcp__github__*` MCP tools per the session's operating instructions, which supersedes the literal "Shell, git, `gh`" wording in PROTOCOL.md — this is a tooling substitution, not a scope change.

## Deviations tracker (append as they occur)

- **Bootstrap commit pushed directly to `main` instead of via PR.** The remote repository had zero commits and no `main` ref, so a pull request (which requires an existing base branch) was not possible for the very first commit. Pushed the bootstrap commit directly to establish `main`, then created `m1` from it. All subsequent milestone work goes through PRs as specified.
