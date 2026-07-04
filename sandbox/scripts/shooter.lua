-- Dosen schiessen -- the game controller.
-- Put this script on any small object (e.g. a Box), then press Play:
--   * Left mouse  -> shoot a ball in the look direction
--   * R           -> reset the row of cans
-- Cans are spawned in front of the player; knocking one over scores a point.

local function spawnCans()
    local px, py, pz = game.cameraPos()
    local dx, dy, dz = game.cameraDir()
    -- Forward flattened onto the ground, plus a perpendicular "right".
    local fl = math.sqrt(dx * dx + dz * dz)
    if fl < 0.001 then fl = 1.0 end
    local fx, fz = dx / fl, dz / fl
    local rx, rz = -fz, fx
    -- The camera sits eyeHeight (1.8 m) above the player's feet == the ground.
    -- Spawn cans just resting on it (half height 0.6), a hair high so they
    -- settle down instead of intersecting the terrain.
    local ground = py - 1.8
    for i = 0, 5 do
        local off = (i - 2.5) * 1.2
        game.spawn{
            type = game.CYLINDER,
            x = px + fx * 10.0 + rx * off,
            y = ground + 0.62,
            z = pz + fz * 10.0 + rz * off,
            sx = 0.35, sy = 0.6, sz = 0.35,
            r = 0.85, g = 0.22, b = 0.15,
            mass = 0.6,
            script = "can.lua",
            name = "can",
        }
    end
end

function start(e)
    spawnCans()
    game.setHud("Left click: shoot   R: reset cans")
end

function update(e, dt, t)
    if game.mousePressed(game.MOUSE_LEFT) then
        local px, py, pz = game.cameraPos()
        local dx, dy, dz = game.cameraDir()
        local speed = 34.0
        game.spawn{
            type = game.SPHERE,
            x = px + dx * 1.2, y = py + dy * 1.2, z = pz + dz * 1.2,
            sx = 0.14, sy = 0.14, sz = 0.14,
            r = 0.95, g = 0.9, b = 0.3,
            mass = 0.5,
            vx = dx * speed, vy = dy * speed, vz = dz * speed,
            script = "bullet.lua",
        }
        game.playSound("shot.wav")   -- drop a shot.wav in content/sounds (optional)
    end

    if game.keyPressed(game.KEY_R) then
        spawnCans()
    end
end
