-- stalker/player.lua -- the player of a level converted by stalkerimport: health,
-- radiation, being hurt, dying (and getting up again where the level was
-- entered), and the HUD. Written into the project by the converter; a new
-- import overwrites it.

local P = {}

local D
P.health = 1.0          -- 0..1
P.rad = 0.0             -- radiation dose, 0..1; it eats the health while it lasts
P.dead = false
P.spawn = nil           -- where the player gets up after dying

local flash = 0.0       -- the red of a fresh wound
local msg, msgT = nil, 0.0
local deadT = 0.0

local function pick(t)
    if t and #t > 0 then return t[math.random(#t)] end
end

-- The player's feet and eye.
function P.feet()
    local x, y, z = game.cameraPos()
    return x, y - 1.6, z
end

function P.say(text, seconds)
    msg, msgT = text, seconds or 0.1
end

function P.hurt(amount)
    if P.dead or amount <= 0 then return end
    P.health = P.health - amount
    flash = math.min(1.0, flash + 0.25 + amount * 2.0)
    local s = pick(D.fx.hurt)
    if s then game.sound(s, 0.8, 0.95 + math.random() * 0.1, "SFX") end
    if P.health <= 0 then
        P.health = 0
        P.dead = true
        deadT = 0
        local d = pick(D.fx.die)
        if d then game.sound(d, 1.0, 1.0, "SFX") end
    end
end

function P.irradiate(dose)
    P.rad = math.min(1.0, P.rad + dose)
end

function P.start(data)
    D = data
    P.health, P.rad, P.dead = 1.0, 0.0, false
    game.setCrosshair(true)
    -- Come in through a level changer: stand where it leads.
    local rec = game.loadData("stalker_travel")
    if type(rec) == "table" and rec.to == D.level then
        game.teleport(rec.gx, rec.gy + D.lift + 0.05, -rec.gz, rec.yaw)
        P.health = math.max(0.3, rec.health or 1.0)
        P.rad = rec.rad or 0.0
        game.saveData("stalker_travel", false)
    end
end

function P.update(dt)
    if not P.spawn then
        local x, y, z = P.feet()
        P.spawn = {x, y, z}
    end
    flash = math.max(0.0, flash - dt * 1.5)
    if msgT > 0 then msgT = msgT - dt else msg = nil end
    if P.dead then
        deadT = deadT + dt
        if deadT > 5.0 then
            game.teleport(P.spawn[1], P.spawn[2], P.spawn[3])
            P.health, P.rad, P.dead = 1.0, 0.0, false
        end
        return
    end
    -- Radiation wears off slowly and hurts while it does.
    if P.rad > 0 then
        P.health = P.health - P.rad * 0.03 * dt
        P.rad = math.max(0.0, P.rad - 0.006 * dt)
        if P.health <= 0 then P.hurt(0.01) end
    end
    -- A little health comes back on its own.
    if P.rad < 0.05 then P.health = math.min(1.0, P.health + 0.004 * dt) end
end

function P.hud(ammo)
    local w, h = game.hudSize()
    game.hudRect(30, h - 74, 264, 18, 0.0, 0.0, 0.0, 0.55, 4)
    game.hudRect(32, h - 72, 260 * math.max(0, P.health), 14, 0.72, 0.1, 0.08, 0.92, 3)
    if P.rad > 0.004 then
        game.hudRect(30, h - 50, 264, 12, 0.0, 0.0, 0.0, 0.55, 3)
        game.hudRect(32, h - 48, 260 * P.rad, 8, 0.85, 0.78, 0.1, 0.92, 2)
        game.hudText(300, h - 54, "RAD", 18, 0.95, 0.85, 0.2, 1.0, 0.0, true)
    end
    if ammo then game.hudText(w - 34, h - 80, ammo, 30, 0.92, 0.9, 0.82, 0.95, 1.0, true) end
    if flash > 0.01 then game.hudRect(0, 0, w, h, 0.55, 0.0, 0.0, flash * 0.35) end
    if msg then game.hudText(w * 0.5, h * 0.7, msg, 30, 1.0, 0.97, 0.88, 1.0, 0.5, true) end
    if P.dead then
        game.hudRect(0, 0, w, h, 0.0, 0.0, 0.0, math.min(0.8, deadT * 0.4))
        game.hudText(w * 0.5, h * 0.42, "Du bist tot", 64, 0.85, 0.12, 0.08, math.min(1.0, deadT), 0.5, true)
    end
end

return P
