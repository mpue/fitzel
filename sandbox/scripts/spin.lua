-- Spins the object and bobs it gently while playing.
-- Assign via Inspector > Script: "spin.lua", then press Play.
--
-- The three globals below are exposed as editable fields in the Inspector (see
-- docs/lua-scripting.md §5.1). Tweak them per object; the values persist with the
-- scene and override these defaults at Play start. `local baseY` stays private.

spinSpeed = 45.0   -- degrees per second (Inspector: a drag field)
bobHeight = 0.4    -- vertical bob amplitude in metres
bob       = true   -- enable the bobbing (Inspector: a checkbox)

local baseY = nil

function start(e)
    baseY = e.y
end

function update(e, dt, t)
    e.ry = e.ry + spinSpeed * dt
    if bob and baseY then
        e.y = baseY + math.sin(t * 2.0) * bobHeight
    end
end
