# ESCALATION: M3 live test requires network access not available in this environment

**Blocker:** M3's Critic round 1 review (`reviews/m3-round1.json`, finding
`M3-F1`, blocking) confirms that `tests/live/zombiemud.lua` and
`tests/replay/zombiemud_session.jsonl` cannot be brought into compliance
with `docs/SPEC.md` — nor can M3's own exit criterion ("plus one
successful live run") be satisfied — without outbound TCP access from this
build environment to `zombiemud.org:3000`, which is not available.

## What was tried

- Repeatedly attempted a raw TCP connection to `zombiemud.org:3000` at
  multiple points across the milestone (bash `/dev/tcp`, Python
  `socket.connect`), most recently just before this escalation: all time
  out.
- Confirmed via this session's own `read_documentation("environment.network")`
  that outbound network access is governed by a policy the user sets for
  this environment, not something changeable from inside the session.
- Escalated this mid-milestone via `AskUserQuestion`; the user chose
  **"Grant network access, then continue"** — they would edit the
  environment's network settings and tell me to proceed, and I would
  continue all non-network-dependent work in the meantime. All of that
  other work (script_engine, main.cpp wiring, fake_mud, the replay ctest,
  live-test.yml/release.yml, README, and now this round's Critic-requested
  fixes) is complete, tested, and CI-green.
- The Critic independently reproduced the same block from its own,
  separately-provisioned environment (a Python `socket.create_connection`
  attempt to `zombiemud.org:3000` also timed out), which rules out this
  being specific to my container rather than a property of the host
  itself being unreachable from this session's network policy.
- Built and validated a synthetic placeholder (`tests/live/zombiemud.lua`
  against a local mock MUD server) so the live-test *mechanism*
  (`fake_mud`, the replay ctest, `live-test.yml`'s structure, the
  unreachable-server skip logic) is real, tested, and does not need
  rework — only the fixture's provenance and an actual successful live
  run remain outstanding, exactly as the Critic's verdict describes.

## What's needed to unblock

The user (or whoever administers this environment) adds `zombiemud.org`
(or broader outbound access) to this environment's allowed network
settings — per this session's own guidance, that's the cloud environment
menu → Edit, in the session's own UI — then tells me to proceed. Once
unblocked, per `docs/agents/PROTOCOL.md`'s "max 5 connections per
milestone, at least 60 seconds apart" limit, I will:

1. Connect once, manually observe the login flow and confirm the exits/
   colors/gag-line/failure-message shapes the current script assumes
   still hold (adapting the route if the visitor option doesn't work as
   currently described, per `docs/SPEC.md` section 5's own allowance for
   this).
2. Record a real session with `--record`, respecting the connection
   budget.
3. Rebuild `tests/live/zombiemud.lua`'s patterns from that real transcript
   (quoting the specific lines each pattern is based on in `NOTES.md`, as
   the spec requires) and replace `tests/replay/zombiemud_session.jsonl`
   with the real recording.
4. Confirm the replay test still passes against the real fixture, and that
   the live run itself succeeds end to end.
5. Update `HANDOFF.md` and spawn a fresh Critic round for this specific
   change.

## In the meantime

All of M3-F2, M3-F3, and M3-F4 (the round 1 verdict's non-blocking-on-
network findings) have been fixed on `m3` in the meantime — see the
commit that accompanies this file and the updated `HANDOFF.md`. I have not
stopped work; this file exists so the one genuinely-blocked item has a
clear, durable record independent of chat history, per
`docs/agents/PROTOCOL.md`'s escalation requirement.
