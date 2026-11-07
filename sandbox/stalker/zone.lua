-- stalker/zone.lua -- the controller of a level converted by stalkerimport. One
-- object of the scene ("Zone") runs it; it reads what the game's data says about
-- the level (stalker/levels/<scene>.lua) and plays it through three modules:
--   stalker/player.lua  health, radiation, death, the HUD
--   stalker/world.lua   anomalies, radiation, mines, level changers, sound, music
--   stalker/life.lua    the people and creatures, the pistol
--
-- Controls on top of fitzel's walking: left mouse button shoots, R reloads,
-- E takes a level changer.
--
-- Written into the project by the converter; a new import overwrites it.

local P = require("stalker.player")
local W = require("stalker.world")
local Life = require("stalker.life")

local D

function start(e)
    -- The scene's level; a copy saved as "l01_escape_night" still finds it.
    local scene = game.currentScene() or ""
    local name, ok, data = scene, false, nil
    while name ~= "" do
        ok, data = pcall(require, "stalker.levels." .. name)
        if ok and type(data) == "table" then break end
        ok = false
        name = name:match("^(.*)_[^_]*$") or ""
    end
    if not ok then
        game.log("stalker: no level data for scene '" .. scene .. "' in scripts/stalker/levels/")
        return
    end
    D = data
    shared.stalker = {data = D, player = P, world = W, life = Life}
    P.start(D)
    W.start(D, P)
    Life.start(D, P)
end

function update(e, dt, t)
    if not D then return end
    P.update(dt)
    W.update(dt, t)
    Life.update(dt, t)
    P.hud(Life.ammo())
end
