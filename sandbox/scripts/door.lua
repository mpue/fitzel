-- A door that opens when you walk up to it -- driving an Animation Graph from Lua.
--
-- WHAT TO SET UP FIRST. This script only presses the buttons; the movement and
-- the shape of the behaviour live in the scene, not here.
--
--   1. Timeline panel: make two clips, say "Door opens" and "Door closes".
--      Key the door's Rotation (or Position) at 0 s and again a second later.
--   2. Animation graph panel: a graph with four states --
--         Closed  (no clip, or a one-frame idle)
--         Opening (clip "Door opens",  Loop off)
--         Open    (no clip)
--         Closing (clip "Door closes", Loop off)
--      and five arrows:
--         Closed  -> Opening   on trigger `open`
--         Opening -> Open      Wait for the clip (100%)
--         Open    -> Closing   on trigger `close`
--         Closing -> Closed    Wait for the clip (100%)
--      Parameters: `open` and `close`, both Trigger.
--   3. On the door object: Add Component -> Animation Graph, pick the graph.
--      Then Add Component -> Script and choose this file.
--
-- Then press Play and walk into it.
--
-- WHY THE STATE MACHINE AND NOT JUST THIS SCRIPT. The script could set the
-- rotation itself in four lines. What it could not do is be read back: "opens,
-- waits, closes, and can be caught half way" is a shape, and the graph is where
-- that shape is visible and editable by somebody who does not write Lua.

-- How close the player has to be, in metres, and how far they have to walk away
-- again. The gap between the two is deliberate: with one distance, standing
-- exactly on the line fires open/close on alternate frames forever.
local NEAR = 4.0
local FAR  = 5.5

local wasNear = false

function start(self, dt, t)
    wasNear = false
end

function update(self, dt, t)
    -- The player's eye. There is no player entity to ask, so the camera IS the
    -- player's position while the game runs.
    local px, py, pz = game.cameraPos()
    -- self.x/z are the door's LOCAL position, which is its world position while
    -- the door is a root object. Parent it to something and ask for the world
    -- one instead: local wx, wy, wz = game.getPos(self.id)
    local dx, dz = px - self.x, pz - self.z
    local dist = math.sqrt(dx * dx + dz * dz)

    if not wasNear and dist < NEAR then
        wasNear = true
        -- Fires once. The trigger stays set until an arrow that tests it is
        -- taken, so it does not matter that the machine steps on another frame.
        game.animTrigger(self.id, "open")
    elseif wasNear and dist > FAR then
        wasNear = false
        game.animTrigger(self.id, "close")
    end

    -- What the machine is actually doing, for the corner of the screen. A graph
    -- that is not moving looks exactly like a graph that is not wired up, and
    -- this is the cheapest way to tell the two apart.
    local state = game.animState(self.id)
    game.setHud(string.format("door: %s   (%.1f m)", state or "no graph", dist))
end
