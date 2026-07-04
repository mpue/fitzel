-- A can. It scores one point the moment it is knocked over (tipped past ~35
-- degrees or shoved off its spot), then clears itself away shortly after so a
-- fresh row can be spawned.
--
-- Important: a freshly spawned can first drops and settles onto the terrain.
-- We must NOT treat that initial settling as a hit, so hit detection only arms
-- once the can has come to rest, and the resting height/pose is the baseline.

local born    = nil
local settled = false
local baseY   = nil
local prevY   = nil
local hit     = false
local hitT    = 0.0

function update(e, dt, t)
    if not born then born = t; prevY = e.y end

    -- Phase 1: wait until the can stops moving vertically (rested on ground).
    if not settled then
        local still = math.abs(e.y - prevY) < 0.01
        prevY = e.y
        if still and (t - born) > 0.4 then
            settled = true
            baseY   = e.y          -- baseline captured at rest
        end
        return
    end

    -- Phase 2: armed. Score once when knocked over or shoved off the spot.
    if not hit then
        local tipped = math.abs(e.rx) > 35.0 or math.abs(e.rz) > 35.0
        local fell   = e.y < baseY - 0.4
        if tipped or fell then
            hit  = true
            hitT = t
            game.addScore(1)
            game.playSound("shot.wav")   -- content/sounds/shot.wav
        end
    elseif t - hitT > 1.5 then
        game.destroy(e.id)
    end
end
