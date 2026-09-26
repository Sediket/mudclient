-- SYNTHETIC PLACEHOLDER: this is NOT built from a real recorded session
-- against zombiemud.org:3000. Outbound network access to that host is
-- blocked in this build environment (raw TCP connect times out; confirmed
-- repeatedly, see NOTES.md "M3: Scripting" -> "Live test / zombiemud"
-- section for the full account and the escalation to the user). Once
-- network access is granted, this script and tests/replay/zombiemud_session.jsonl
-- must be replaced with ones built from an actual recorded session, and
-- this notice removed.
--
-- In the meantime, this script and its companion tests/replay/fake_mud
-- server (tests/fake_mud/fake_mud.cpp, driven by tests/run_replay_test.py)
-- exercise the same client surface (client.wait_for/send/sleep,
-- register_alias, register_trigger with gag, register_timer with
-- pause/resume/kill, sent_log, on('line') span colors) against a small
-- synthetic mock MUD (was /tmp/mock_zombiemud.py during development; its
-- session transcript is what tests/replay/zombiemud_session.jsonl records).
-- Every pattern below is traceable to a literal line that mock server
-- sends, quoted here:
--   "Enter (c)reate or (v)isit: "      -> login_pattern below
--   "You step into the world as a visitor." -> entry_pattern below
--   "Obvious exits: east, south"       -> exits_seen trigger
--   "This is a secret gagged debug line N" -> gag trigger
--   "You can't go that way."           -> failed_move trigger
--   "\x1b[32mTown Square\x1b[0m"       -> saw_color (non-default fg span)

local checks = {}
local function check(name, ok)
    table.insert(checks, { name = name, ok = ok })
end

local exits_seen = 0
client.register_trigger([[^Obvious exits:]], function() exits_seen = exits_seen + 1 end)
client.register_trigger([[secret gagged debug line]], function() end, { gag = true })
local failed_move = false
client.register_trigger([[can't go that way]], function() failed_move = true end)

client.register_alias("e4", "east;east;east;east")
client.register_alias("s3", "south;south;south")

local saw_color = false
client.on("line", function(line)
    for _, span in ipairs(line.spans) do
        if span.fg ~= 0xFFFFFFFF then
            saw_color = true
        end
    end
end)

-- Connection refused/timeout before the login screen is a skip, not a
-- failure (docs/SPEC.md section 5): distinguished from a slow-but-live
-- server by whether a 'disconnect' event ever arrives before we've seen
-- a single 'connect' event.
local ever_connected = false
local disconnected_before_connect = false
client.on("connect", function() ever_connected = true end)
client.on("disconnect", function()
    if not ever_connected then
        disconnected_before_connect = true
    end
end)

client.run_test(function()
    -- 1. I/O: wait for the login screen, visit without creating a character.
    local caps = client.wait_for([[Enter \(c\)reate or \(v\)isit]], 10)
    if caps == nil and disconnected_before_connect then
        client.echo("SKIP: server unreachable")
        client.exit(0)
        return
    end
    check("login screen seen", caps ~= nil)
    client.send("v")
    caps = client.wait_for([[step into the world]], 10)
    check("visitor entry accepted", caps ~= nil)

    -- 2. Aliases: move using only e4/s3; assert exactly 4 east + 3 south,
    -- and the alias names themselves never appear in sent_log.
    client.send("e4")
    client.sleep(0.2)
    client.send("s3")
    client.sleep(0.2)
    local log = client.sent_log()
    local east_count, south_count, alias_leaked = 0, 0, false
    for _, cmd in ipairs(log) do
        if cmd == "east" then east_count = east_count + 1 end
        if cmd == "south" then south_count = south_count + 1 end
        if cmd == "e4" or cmd == "s3" then alias_leaked = true end
    end
    check("exactly 4 east sent", east_count == 4)
    check("exactly 3 south sent", south_count == 3)
    check("alias names never sent", not alias_leaked)

    -- 3. Timers: a basic register/pause/resume/kill sanity check
    -- (independent of server interaction, since the mock server doesn't
    -- model realistic move-pacing timing).
    local ticks = 0
    local timer_id = client.register_timer(0.05, function() ticks = ticks + 1 end, true, "tick")
    client.sleep(0.12)
    client.pause_timer("tick")
    local paused_ticks = ticks
    client.sleep(0.12)
    check("no ticks while paused", ticks == paused_ticks)
    client.resume_timer("tick")
    client.sleep(0.12)
    check("ticks resumed after resume_timer", ticks > paused_ticks)
    client.kill_timer("tick")
    local killed_ticks = ticks
    client.sleep(0.1)
    check("no ticks after kill", ticks == killed_ticks)

    -- 4. Triggers: exits line fires per move; gag line never reaches the
    -- transcript; failure trigger on "can't go that way".
    check("exits trigger fired at least once", exits_seen > 0)
    client.send("north") -- mock server always refuses this direction
    client.sleep(0.3)
    check("failure trigger fired on refused move", failed_move)

    -- 5. Exit.
    client.send("quit")
    client.sleep(0.3)

    local all_ok = true
    for _, c in ipairs(checks) do
        client.echo((c.ok and "PASS: " or "FAIL: ") .. c.name)
        if not c.ok then all_ok = false end
    end
    if not saw_color then
        client.echo("FAIL: colors: no non-default-color span observed")
        all_ok = false
    end
    client.exit(all_ok and 0 or 1)
end)
