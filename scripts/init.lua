-- Example init.lua, loaded automatically on startup in interactive mode
-- (main.cpp looks for this file relative to the working directory). Shows
-- the two most common automation building blocks: a repeating timer and a
-- line trigger.

-- A 5 second repeating timer that echoes a tick counter.
local ticks = 0
client.register_timer(5, function()
    ticks = ticks + 1
    client.echo("[tick " .. ticks .. "]")
end, true, "tick_counter")

-- A trigger on a combat-style line ("Orc hits you", "Goblin misses you",
-- ...), echoing a styled (ANSI-colored) message. client.echo writes its
-- argument straight to the terminal, so embedding a raw SGR escape
-- sequence in the string is how a script applies its own styling.
client.register_trigger([[^(\w+) (hits|misses) you]], function(captures)
    local attacker, verb = captures[1], captures[2]
    if verb == "hits" then
        client.echo("\27[31m>> " .. attacker .. " hits you!\27[0m")
    else
        client.echo("\27[33m>> " .. attacker .. " misses you.\27[0m")
    end
end)
