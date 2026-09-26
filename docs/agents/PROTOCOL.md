# Orchestration Protocol

## Roles
- **Builder:** writes code, tests, workflows. Shell, git, `gh`.
- **Critic:** reviews and verifies in a separate clone. May only add files under `tests/critic/` (committed via its own PR or pushed to the milestone branch in a separate commit clearly labeled as Critic work). Starts every review with fresh context: receives the spec, the diff, and HANDOFF.md only.

## Loop (per milestone)
1. Builder works on branch `m<N>` until exit criteria pass, writes HANDOFF.md, spawns the Critic.
2. Critic emits a verdict (schema in CRITIC.md).
3. On REQUEST_CHANGES, Builder answers every finding in HANDOFF.md as `fixed` (commit SHA) or `disputed` (evidence: a command with its output, or a spec citation), then spawns a fresh Critic.
4. Critic re-reviews changed areas plus its prior findings and adjudicates disputes. Disputes are upheld only with reproducible evidence.
5. On APPROVE and green CI: Builder opens a PR to `main` and merges it once required checks pass.
6. After M3 merges: tag `v0.1.0`; confirm the release published.

## Limits and escalation
- Max 4 review rounds per milestone; max 6 hours wall time per milestone.
- Live server: max 5 connections per milestone, at least 60 seconds apart.
- Escalate (stop, write ESCALATION.md with the blocker, what was tried, and options) when: a limit is hit; the same finding is disputed twice; a requirement appears infeasible or contradictory; or an action needs credentials, permissions, or network access not already granted.
- Never escalate for things you can determine yourself (docs, experiments, CI logs).

## Environment
- Token scoped to this repository only. Branch protection on `main` (PR required, CI required, no force push) is set by the human and must not be changed.
