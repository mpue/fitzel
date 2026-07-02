-- Spins the object and bobs it gently while playing.
-- Assign via Inspector > Script: "spin.lua", then press Play.

local baseY = nil

function start(e)
    baseY = e.y
end

function update(e, dt, t)
    e.ry = e.ry + 45.0 * dt                    -- degrees per second
    if baseY then
        e.y = baseY + math.sin(t * 2.0) * 0.4  -- soft bobbing
    end
end
