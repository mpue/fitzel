-- A ball fired by shooter.lua. Physics carries it; it removes itself after a
-- few seconds so bullets don't pile up in the world.

local born = nil

function update(e, dt, t)
    if not born then born = t end
    if t - born > 4.0 then
        game.destroy(e.id)
    end
end
