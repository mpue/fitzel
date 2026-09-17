-- SKYSTRIKE -- a vertical shoot'em up, entirely in Lua.
--
-- Put this script on ONE object (an Empty is best) and press Play. The sea, the
-- islands, the clouds, your ship, the squadrons, the fleet and the bosses are
-- all spawned around it and removed again when Play stops.
--
--   Arrows / WASD         fly
--   Shift (hold)          focus: fly slowly, guns tight, your hitbox shows
--   Space / J (hold)      fire          (or set autoFire and never hold it)
--   B / X / K             bomb: clears the bullets, hurts everything on screen
--   P                     pause
--   Enter                 start / play again
--
-- Four stages -- CORAL SEA, IRON FLEET, STORM FRONT and IRON COAST -- each a run
-- of squadrons and swarms, gunships, laser drones, mines, gunboats, tanks and
-- batteries, a mid-boss halfway and a boss at the end. The ground scrolls from
-- open sea into a storm and on over the coast into farmland. Then the loop comes
-- round again, faster and angrier.
--
-- Two guns: A fires a wide VULCAN spread, B a LASER that pierces what it hits
-- (picked in the hangar). Out of ships, ten seconds to CONTINUE.
--
-- The title menu has the CAMPAIGN, the MISSIONS board (every mission once
-- cleared can be flown on its own) and the HANGAR, where the credits a run
-- pays buy upgrades in three tiers -- firepower, fire rate, missiles, shield,
-- thrusters, bombs, reserve ships, salvage. The pilot's profile is kept with
-- game.saveData, between sessions and across both scenes of the run.
--
-- Orange orbs power up the guns (twin, spread, options, missiles, max), green
-- ones are a bomb, the pink one after a boss is a ship. A lost ship costs one
-- power level. Kills in quick succession build a CHAIN that multiplies their
-- score, bullets that brush past you (GRAZE) score too, and every cancelled
-- bullet turns into a gold gem that flies to you.
--
-- The textures (sea, islands, clouds, fire, smoke) come from
-- tools/gen_shmup_textures.py. The sounds are recorded explosions and foley cut
-- from a sample library by tools/import_shmup_sounds.py (skystrike_*.wav), with
-- the synthesised set from tools/gen_shmup_sounds.py (shmup_*.wav) behind them.
-- All of it lands in content/; without it the game still runs, on plain
-- materials and the engine's stock sounds.
--
-- Everything is pooled: some 1700 objects are spawned once at the start and
-- switched on and off, because game.spawn only delivers at the end of the frame
-- -- a bullet that appears a frame late is a bullet that hit you from nowhere.
-- Collisions are circles on the flight plane, done here. The HUD is drawn with
-- game.hud* on a canvas 1080 units high.
--
-- In a scene with a Terrain it flies over that instead: HIGH VALLEY, over the
-- scene's own hills, river and forests (see SYS.LAND). The carrier is where the
-- route starts, an Empty named "Shmup Arena" where the fortress waits. With
-- nextScene set, a run goes on in that scene after this one's last stage -- the
-- sea into the valley and back.
--
-- The camera is driven every frame. Make sure no Camera entity in the scene is
-- "active on start" -- that one would take the view back.

-- --- Inspector parameters (globals = editable fields on the Script component) --
ships       = 3       -- ships per game
bombs       = 3       -- bombs at the start (and at least 2 after a lost ship)
autoFire    = false   -- fire all the time, no button to hold
difficulty  = 1.0     -- scales enemy bullet speed and how often they fire
scrollSpeed = 6.0     -- how fast the sea slides by
highClouds  = true    -- thin wisps that drift ABOVE the ships now and then
screenShake = true
sound       = true
landscape   = true    -- in a scene with a Terrain: fly over it instead of the painted sea
nextScene   = ""      -- a scene of the project the run goes on in after this one's last stage

-- --- Layout (flight-plane units, 1 unit = 1 world metre) -------------------------
-- u runs across the screen, v up it, y is the height above the sea. The sea
-- lies 2 m above the carrier, the ships FLY_Y above the sea, the low clouds in
-- between, the high wisps above the ships.
local FLY_Y, LOW_CLOUD_Y, HIGH_CLOUD_Y = 6.0, 2.7, 13.5
local CAM_H, CAM_BACK, CAM_FOV = 33.0, 3.0, 45.0
local FIELD_U = 15.0                 -- half width of the playfield (the HUD frames it)
local LIMIT_U, LIMIT_V0, LIMIT_V1 = 14.0, -11.5, 10.5
local SPAWN_V, GONE_V = 16.5, 19.0
local PLAYER_R, GRAZE_R = 0.26, 1.15 -- the hitbox is the cockpit, not the wings
local SHOT_SPEED = 38.0
local MAX_BOMBS, MAX_POWER = 6, 5
local EXTENDS, EXTEND_EVERY = { 50000, 150000 }, 300000
local SEA_TILE = 20.0

local sin, cos, atan, sqrt = math.sin, math.cos, math.atan, math.sqrt
local floor, abs, min, max = math.floor, math.abs, math.min, math.max
local pi, deg = math.pi, math.deg
local TAU = pi * 2

-- Kept darker than they look right: the sea is dark, the auto exposure opens up
-- for it, and anything pale then blooms.
local MAT_DEFS = {
    hull      = { color = {0.60, 0.64, 0.70}, roughness = 0.3, reflectivity = 0.5 },
    hullDark  = { color = {0.10, 0.12, 0.17}, roughness = 0.35, reflectivity = 0.4 },
    wing      = { color = {0.12, 0.28, 0.62}, roughness = 0.35, reflectivity = 0.3 },
    accent    = { color = {0.95, 0.42, 0.06}, roughness = 0.4 },
    glass     = { color = {0.10, 0.55, 0.80}, emission = {0.1, 0.6, 0.9}, emissionStrength = 1.5,
                  roughness = 0.05, reflectivity = 0.8 },
    flame     = { color = {1.0, 0.6, 0.2}, emission = {1.0, 0.55, 0.15}, emissionStrength = 8.0 },
    navRed    = { color = {1.0, 0.1, 0.1}, emission = {1.0, 0.08, 0.05}, emissionStrength = 7.0 },
    navGreen  = { color = {0.1, 1.0, 0.3}, emission = {0.05, 1.0, 0.2}, emissionStrength = 7.0 },
    hitbox    = { color = {1.0, 1.0, 1.0}, emission = {1.0, 1.0, 1.0}, emissionStrength = 7.0 },
    shield    = { color = {0.3, 0.8, 1.0}, emission = {0.2, 0.6, 1.0}, emissionStrength = 1.2,
                  opacity = 0.2, alphaMode = 2 },
    option    = { color = {1.0, 0.85, 0.5}, emission = {1.0, 0.7, 0.25}, emissionStrength = 1.8 },
    -- Projectiles are glow, not matter: drawn see-through, so they throw no
    -- more than a faint dither of shadow on the sea (a low sun turned a stream
    -- of shots into a ladder of dark boxes beside it). The emission is raised
    -- to make up for the blend.
    shot      = { color = {0.6, 1.0, 1.0}, emission = {0.3, 0.9, 1.0}, emissionStrength = 22.0,
                  opacity = 0.22, alphaMode = 2 },
    optShot   = { color = {1.0, 0.9, 0.5}, emission = {1.0, 0.75, 0.25}, emissionStrength = 20.0,
                  opacity = 0.22, alphaMode = 2 },
    missile   = { color = {0.55, 0.57, 0.6}, roughness = 0.3, reflectivity = 0.4 },
    orb       = { color = {1.0, 0.3, 0.8}, emission = {1.0, 0.15, 0.6}, emissionStrength = 30.0,
                  opacity = 0.3, alphaMode = 2 },
    big       = { color = {1.0, 0.6, 0.2}, emission = {1.0, 0.4, 0.08}, emissionStrength = 24.0,
                  opacity = 0.3, alphaMode = 2 },
    needle    = { color = {0.5, 0.9, 1.0}, emission = {0.3, 0.8, 1.0}, emissionStrength = 30.0,
                  opacity = 0.3, alphaMode = 2 },
    beam      = { color = {1.0, 0.6, 0.7}, emission = {1.0, 0.3, 0.45}, emissionStrength = 26.0,
                  opacity = 0.4, alphaMode = 2 },
    beamWarn  = { color = {1.0, 0.3, 0.3}, emission = {1.0, 0.2, 0.2}, emissionStrength = 3.0,
                  opacity = 0.5, alphaMode = 2 },
    spark     = { color = {1.0, 0.7, 0.3}, emission = {1.0, 0.55, 0.15}, emissionStrength = 9.0 },
    flash     = { color = {0.95, 0.75, 0.6}, emission = {1.0, 0.55, 0.3}, emissionStrength = 0.9 },
    debris    = { color = {0.10, 0.10, 0.11}, roughness = 0.6 },
    gem       = { color = {1.0, 0.8, 0.2}, emission = {1.0, 0.7, 0.1}, emissionStrength = 5.0 },
    laser     = { color = {0.8, 0.6, 1.0}, emission = {0.7, 0.4, 1.0}, emissionStrength = 24.0,
                  opacity = 0.25, alphaMode = 2 },
    red       = { color = {0.72, 0.10, 0.08}, roughness = 0.35, reflectivity = 0.3 },
    white     = { color = {0.55, 0.56, 0.58}, roughness = 0.35, reflectivity = 0.3 },
    darkMetal = { color = {0.14, 0.15, 0.17}, roughness = 0.4, reflectivity = 0.5 },
    grey      = { color = {0.34, 0.36, 0.40}, roughness = 0.4, reflectivity = 0.4 },
    purple    = { color = {0.42, 0.22, 0.62}, roughness = 0.25, reflectivity = 0.6 },
    yellow    = { color = {0.80, 0.60, 0.05}, roughness = 0.35, reflectivity = 0.3 },
    black     = { color = {0.0, 0.0, 0.0}, roughness = 0.3, reflectivity = 1.0 },
    olive     = { color = {0.26, 0.32, 0.18}, roughness = 0.6 },
    oliveDark = { color = {0.16, 0.20, 0.11}, roughness = 0.6 },
    navy      = { color = {0.22, 0.26, 0.32}, roughness = 0.5, reflectivity = 0.3 },
    deck      = { color = {0.24, 0.21, 0.17}, roughness = 0.9 },
    boss      = { color = {0.17, 0.18, 0.21}, roughness = 0.35, reflectivity = 0.5 },
    bossPlate = { color = {0.34, 0.36, 0.39}, roughness = 0.3, reflectivity = 0.5 },
    hazard    = { color = {0.75, 0.55, 0.05}, roughness = 0.5 },
    eye       = { color = {1.0, 0.9, 0.2}, emission = {1.0, 0.85, 0.1}, emissionStrength = 6.0 },
    eyeHot    = { color = {1.0, 0.2, 0.1}, emission = {1.0, 0.15, 0.05}, emissionStrength = 14.0 },
    redGlow   = { color = {1.0, 0.2, 0.1}, emission = {1.0, 0.15, 0.05}, emissionStrength = 6.0 },
    blueGlow  = { color = {0.3, 0.6, 1.0}, emission = {0.2, 0.5, 1.0}, emissionStrength = 6.0 },
    padLight  = { color = {0.3, 0.6, 1.0}, emission = {0.25, 0.55, 1.0}, emissionStrength = 1.6 },
    core      = { color = {1.0, 0.1, 0.1}, emission = {1.0, 0.1, 0.05}, emissionStrength = 5.0 },
    pwP       = { color = {1.0, 0.6, 0.1}, emission = {1.0, 0.5, 0.05}, emissionStrength = 6.0 },
    pwB       = { color = {0.3, 1.0, 0.3}, emission = {0.2, 1.0, 0.2}, emissionStrength = 6.0 },
    pw1       = { color = {1.0, 0.4, 0.7}, emission = {1.0, 0.3, 0.6}, emissionStrength = 6.0 },
    ring      = { color = {0.9, 0.9, 0.9}, roughness = 0.2, reflectivity = 0.9 },
    trunk     = { color = {0.25, 0.18, 0.10}, roughness = 0.9 },
    palm      = { color = {0.13, 0.32, 0.08}, roughness = 0.85 },
    sand      = { color = {0.46, 0.39, 0.26}, roughness = 0.9 },
    grass     = { color = {0.12, 0.33, 0.10}, roughness = 0.85 },
    cloud     = { color = {0.52, 0.54, 0.57}, roughness = 1.0 },
    concrete  = { color = {0.40, 0.40, 0.38}, roughness = 0.8 },
    rain      = { color = {0.70, 0.75, 0.85}, emission = {0.4, 0.45, 0.55}, emissionStrength = 1.0,
                  opacity = 0.35, alphaMode = 2 },
}
-- Materials that need a generated texture, each with the plain look it falls
-- back on when the file is missing.
local TEX_DEFS = {
    sea     = { texture = "shmup_sea.png", normalMap = "shmup_sea_n.png", roughness = 0.55,
                reflectivity = 0.06, plain = { color = {0.0, 0.08, 0.26}, roughness = 0.45 } },
    seaStorm = { texture = "shmup_sea_storm.png", normalMap = "shmup_sea_n.png", roughness = 0.55,
                 reflectivity = 0.06, plain = { color = {0.03, 0.06, 0.1}, roughness = 0.5 } },
    seaFront = { texture = "shmup_sea_front.png", normalMap = "shmup_sea_n.png", roughness = 0.55,
                 reflectivity = 0.06, plain = { color = {0.02, 0.07, 0.18}, roughness = 0.5 } },
    seaFrontOut = { texture = "shmup_sea_front_out.png", normalMap = "shmup_sea_n.png", roughness = 0.55,
                    reflectivity = 0.06, plain = { color = {0.02, 0.07, 0.18}, roughness = 0.5 } },
    land     = { texture = "shmup_land.png", roughness = 0.9,
                 plain = { color = {0.18, 0.30, 0.12}, roughness = 0.9 } },
    landRoad = { texture = "shmup_land_road.png", roughness = 0.9,
                 plain = { color = {0.18, 0.30, 0.12}, roughness = 0.9 } },
    coast    = { texture = "shmup_coast.png", roughness = 0.8,
                 plain = { color = {0.30, 0.28, 0.18}, roughness = 0.9 } },
    coastOut = { texture = "shmup_coast_out.png", roughness = 0.8,
                 plain = { color = {0.30, 0.28, 0.18}, roughness = 0.9 } },
    stormA  = { texture = "shmup_storm_a.png", alphaMode = 2, roughness = 1.0 },
    stormB  = { texture = "shmup_storm_b.png", alphaMode = 2, roughness = 1.0 },
    islandA = { texture = "shmup_island_a.png", alphaMode = 2, roughness = 0.85 },
    islandB = { texture = "shmup_island_b.png", alphaMode = 2, roughness = 0.85 },
    islandC = { texture = "shmup_island_c.png", alphaMode = 2, roughness = 0.85 },
    cloudA  = { texture = "shmup_cloud_a.png", alphaMode = 2, roughness = 1.0 },
    cloudB  = { texture = "shmup_cloud_b.png", alphaMode = 2, roughness = 1.0 },
    cloudC  = { texture = "shmup_cloud_c.png", alphaMode = 2, roughness = 1.0 },
    haze    = { texture = "shmup_cloud_b.png", alphaMode = 2, opacity = 0.3, roughness = 1.0 },
    fire1   = { texture = "shmup_fire.png", emissionMap = "shmup_fire.png", alphaMode = 2,
                emission = {1.0, 0.75, 0.45}, emissionStrength = 4.5 },
    fire2   = { texture = "shmup_fire.png", emissionMap = "shmup_fire.png", alphaMode = 2,
                emission = {1.0, 0.6, 0.3}, emissionStrength = 3.0, opacity = 0.6 },
    fire3   = { texture = "shmup_fire.png", emissionMap = "shmup_fire.png", alphaMode = 2,
                emission = {1.0, 0.45, 0.2}, emissionStrength = 2.0, opacity = 0.3 },
    ring1   = { texture = "shmup_ring.png", emissionMap = "shmup_ring.png", alphaMode = 2,
                emission = {0.7, 0.85, 1.0}, emissionStrength = 3.0, opacity = 0.9 },
    ring2   = { texture = "shmup_ring.png", emissionMap = "shmup_ring.png", alphaMode = 2,
                emission = {0.7, 0.85, 1.0}, emissionStrength = 1.5, opacity = 0.4 },
    muzzle  = { texture = "shmup_fire.png", emissionMap = "shmup_fire.png", alphaMode = 2,
                emission = {1.0, 0.8, 0.5}, emissionStrength = 6.0 },
    puff1   = { texture = "shmup_puff.png", alphaMode = 2, opacity = 0.45, roughness = 1.0 },
    puff2   = { texture = "shmup_puff.png", alphaMode = 2, opacity = 0.25, roughness = 1.0 },
    puff3   = { texture = "shmup_puff.png", alphaMode = 2, opacity = 0.1, roughness = 1.0 },
    splash1 = { texture = "shmup_splash.png", alphaMode = 2, opacity = 0.9, roughness = 1.0 },
    splash2 = { texture = "shmup_splash.png", alphaMode = 2, opacity = 0.5, roughness = 1.0 },
    splash3 = { texture = "shmup_splash.png", alphaMode = 2, opacity = 0.2, roughness = 1.0 },
    smoke1  = { texture = "shmup_smoke.png", alphaMode = 2, opacity = 0.8, roughness = 1.0 },
    smoke2  = { texture = "shmup_smoke.png", alphaMode = 2, opacity = 0.5, roughness = 1.0 },
    smoke3  = { texture = "shmup_smoke.png", alphaMode = 2, opacity = 0.25, roughness = 1.0 },
    embers  = { texture = "shmup_puff.png", emissionMap = "shmup_puff.png", alphaMode = 2,
                emission = {1.0, 0.32, 0.06}, emissionStrength = 3.0, opacity = 0.45 },
    -- A column's smoke: lit from the fire under it while it is low, then only
    -- grey. Kept thin -- a sprite's shadow is as solid as its opacity, and a
    -- column of solid puffs threw a string of black discs on the ground.
    colSmoke1 = { texture = "shmup_smoke.png", emissionMap = "shmup_smoke.png", alphaMode = 2,
                  color = {0.5, 0.45, 0.42}, emission = {1.0, 0.45, 0.16}, emissionStrength = 6.0,
                  opacity = 0.42, roughness = 1.0 },
    colSmoke2 = { texture = "shmup_smoke.png", emissionMap = "shmup_smoke.png", alphaMode = 2,
                  color = {0.45, 0.43, 0.42}, emission = {0.5, 0.42, 0.38}, emissionStrength = 1.8,
                  opacity = 0.3, roughness = 1.0 },
    colSmoke3 = { texture = "shmup_smoke.png", emissionMap = "shmup_smoke.png", alphaMode = 2,
                  color = {0.4, 0.4, 0.42}, emission = {0.4, 0.4, 0.42}, emissionStrength = 1.1,
                  opacity = 0.16, roughness = 1.0 },
}
-- Glows keep their look when a hit flashes the rest of the craft white.
local NO_FLASH = { glass = true, flame = true, eye = true, eyeHot = true, redGlow = true,
                   blueGlow = true, core = true, navRed = true, navGreen = true }
local mats, hasTex = {}, {}

-- Each cue is a list of groups, best first: the recorded samples, then the
-- synthesised ones, then the engine's stock sound. The first group with a file
-- present wins, and a group of several is a set of variants picked at random.
local CUES = {
    shot    = { { "shmup_shot.wav" }, { "shot.wav" } },
    missile = { { "skystrike_missile.wav" }, { "shmup_missile.wav" } },
    hit     = { { "shmup_hit.wav" }, { "missile_deny.wav" } },
    clank   = { { "skystrike_clank1.wav", "skystrike_clank2.wav", "skystrike_clank3.wav" },
                { "shmup_hit.wav" }, { "missile_deny.wav" } },
    item    = { { "shmup_item.wav" } },
    boomS   = { { "skystrike_boom_s1.wav", "skystrike_boom_s2.wav", "skystrike_boom_s3.wav",
                  "skystrike_boom_s4.wav" }, { "shmup_boom_s.wav" }, { "missile_hit.wav" } },
    boomM   = { { "skystrike_boom_m1.wav", "skystrike_boom_m2.wav", "skystrike_boom_m3.wav",
                  "skystrike_boom_m4.wav" }, { "shmup_boom_m.wav" }, { "missile_hit.wav" } },
    boomL   = { { "skystrike_boom_l1.wav", "skystrike_boom_l2.wav", "skystrike_boom_l3.wav" },
                { "shmup_boom_l.wav" }, { "impact.wav" } },
    bomb    = { { "skystrike_bomb.wav" }, { "shmup_bomb.wav" }, { "missile_launch.wav" } },
    die     = { { "skystrike_die.wav" }, { "shmup_die.wav" }, { "impact.wav" } },
    mineArm = { { "skystrike_mine_arm.wav" } },
    mineBoom = { { "skystrike_mine_boom.wav" }, { "shmup_boom_s.wav" }, { "missile_hit.wav" } },
    launch  = { { "skystrike_launch.wav" }, { "shmup_missile.wav" } },
    cannon  = { { "skystrike_cannon1.wav", "skystrike_cannon2.wav" } },
    debris  = { { "skystrike_debris.wav" } },
    power   = { { "shmup_power.wav" }, { "plopp.wav" } },
    extend  = { { "shmup_extend.wav" }, { "plopp.wav" } },
    warning = { { "shmup_warning.wav" }, { "missile_lock.wav" } },
    charge  = { { "shmup_charge.wav" } },
    laser   = { { "shmup_laser.wav" } },
    start   = { { "shmup_start.wav" } },
    splash  = { { "splash.wav" } },
    thunder = { { "skystrike_thunder1.wav", "skystrike_thunder2.wav", "skystrike_thunder3.wav",
                  "skystrike_thunder4.wav" }, { "thunder.wav" } },
    rain    = { { "skystrike_rain.wav" }, { "rain.wav" } },
    clear   = { { "shmup_clear.wav" } },
}
local cueFile, lastCue = {}, {}

-- --- State ------------------------------------------------------------------------
-- OX/OZ/BY: where the flight plane's origin is in the world. RS: world metres
-- per flight-plane unit -- 1 over the painted sea, more over a real landscape,
-- whose trees and rivers are life-size (see SYS.LAND).
local OX, OZ, BY, RS = 0.0, 0.0, 0.0, 1.0
local now, frameNo, dtReal = 0.0, 0, 0.0
local objects = {}          -- every pooled thing: { root, active, shown }
local world = { sea = {}, islands = {}, clouds = {}, haze = {} }
local pools = {}            -- shots, optShots, pMissiles, orb, big, needle, sparks, ...
local BULLET_KINDS = { "orb", "big", "needle" }
local enemyPools, enemies = {}, {}
local player = nil
local beams = {}

local mode = "title"        -- title / play / over
local paused = false
local G = {}                -- the running game: score, lives, stage, chain ... (see resetGame)
local hiScore = 0
local queue = {}            -- delayed calls: { at, fn }
local popups = {}           -- floating score texts: { u, v, text, t, col, size }
local banner = nil          -- the big centre text: { kind, t0, dur, ... }
local shake, flash, flashCol, timeScale, slowUntil = 0.0, 0.0, {1, 1, 1}, 1.0, 0.0
local keysPrev = {}
local view = { w = 1920.0, h = 1080.0, t = 0.4142 }
local noFire = false

-- Forward declarations: the systems below call into each other.
local FX, HUD, WAVE, KINDS = {}, {}, {}, {}
-- Wrecks, trails, weather, the ground, the laser, continue: kept in one table,
-- because the main chunk is close to Lua's limit of 200 locals.
local SYS = { wrecks = {}, weapon = 1, recentMuzzles = {}, muzzleIdx = 0 }
local STAGES
local launch, killEnemy, loseShip, addScore, cancelBullets
-- The game around the stages: profile, hangar, mission board (see the section
-- before Lifecycle).
local META = { sel = 1, missionSel = 1 }

-- Sizes are in flight-plane units, like everything else here.
function SYS.setScale(id, a, b, c)
    if b then game.setScale(id, a * RS, b * RS, c * RS) else game.setScale(id, a * RS) end
end

-- --- Helpers ------------------------------------------------------------------------

local function rand(a, b) return a + (b - a) * math.random() end
local function clamp(x, a, b) if x < a then return a elseif x > b then return b end return x end
local function lerp(a, b, t) return a + (b - a) * t end
local function wrapAngle(a) return (a + pi) % TAU - pi end

local function fmt(n)
    local out = tostring(floor(n)):reverse():gsub("(%d%d%d)", "%1."):reverse()
    if out:sub(1, 1) == "." then out = out:sub(2) end
    return out
end

local function cue(name, gap)
    if not sound then return end
    local list = cueFile[name]
    if not list then return end
    if lastCue[name] and now - lastCue[name] < (gap or 0.05) then return end
    lastCue[name] = now
    game.playSound(list[math.random(1, #list)])
end

-- Flight-plane point -> world point.
local function W(u, v, y) return OX + u * RS, BY + (y or FLY_Y) * RS, OZ - v * RS end

-- Where a point at height y appears on the flight plane: ground targets are
-- hit, and fire, where you SEE them, not where they are.
local function toPlane(u, v, y)
    local ch = FLY_Y + CAM_H
    local k = (ch - FLY_Y) / (ch - y)
    return u * k, -CAM_BACK + (v + CAM_BACK) * k
end

-- Flight-plane point -> HUD canvas, through the camera driveCamera set up.
local function project(u, v, y)
    local px, py, pz = W(u, v, y)
    local dx, dy, dz = px - view.ex, py - view.ey, pz - view.ez
    local z = dx * view.fx + dy * view.fy + dz * view.fz
    if z < 0.1 then z = 0.1 end
    local x = dx * view.rx + dy * view.ry + dz * view.rz
    local yy = dx * view.ux + dy * view.uy + dz * view.uz
    local s = view.h * 0.5 / (z * view.t)
    return view.w * 0.5 + x * s, view.h * 0.5 - yy * s
end

-- A math angle (0 = across, pi/2 = up the screen) as the model yaw that points a
-- craft built nose-up along it.
local function yawOf(ang) return ang - pi * 0.5 end

-- Heading (yaw) plus a bank about the craft's OWN nose, as the Euler angles the
-- scene stores (Rz*Ry*Rx, degrees). A plain rz would roll about the world axis.
local function attitude(yaw, roll)
    local ca, sa, cb, sb = cos(yaw), sin(yaw), cos(roll), sin(roll)
    return deg(atan(sa * sb, ca)), deg(math.asin(clamp(sa * cb, -1, 1))), deg(atan(sb, ca * cb))
end

-- A point on the craft (across, forward) -> flight plane, for a craft at u, v
-- turned by yaw.
local function local2plane(u, v, yaw, across, fwd)
    local c, s = cos(yaw), sin(yaw)
    return u + across * c - fwd * s, v + across * s + fwd * c
end

-- --- Spawning & pools -------------------------------------------------------------

local BOX, SPH, CYL, PLN, EMP = 0, 3, 2, 8, 7   -- set again from game.* in start

local function parkedAt() return W(0.0, 0.0, -40.0) end

-- x, y, z: a world point (from W) for a root, an offset in flight-plane units
-- for a part, which the render scale still has to stretch.
local function spawn(kind, x, y, z, hx, hy, hz, mat, parent, name, rot)
    if parent then x, y, z = x * RS, y * RS, z * RS end
    return game.spawn{
        type = kind, x = x, y = y, z = z, sx = hx * RS, sy = hy * RS, sz = hz * RS,
        rx = rot and rot[1] or 0.0, ry = rot and rot[2] or 0.0, rz = rot and rot[3] or 0.0,
        material = mat and mats[mat] or nil, parent = parent or -1,
        physics = game.PHYSICS_NONE, name = name or "shmup",
    }
end

-- Parts: { kind, across, up, forward, hx, hy, hz, material, rot }. Forward is up
-- the screen (world -z). A part { "mount", key, across, up, forward, parts } is
-- a sub-assembly of its own -- a turret that turns, a pod that can be shot off.
local function addParts(o, root, list, skin, name)
    for _, p in ipairs(list) do
        if p[1] == "mount" then
            local m = { key = p[2], across = p[3], up = p[4], fwd = p[5], skin = {},
                        alive = true, shown = true }
            m.root = spawn(EMP, p[3], p[4], -p[5], 0.2, 0.2, 0.2, nil, root, name .. " mount")
            addParts(o, m.root, p[6], m.skin, name)
            o.mounts[#o.mounts + 1] = m
            o.mount[p[2]] = m
        else
            local id = spawn(p[1], p[2], p[3], -p[4], p[5], p[6], p[7], p[8], root,
                             name .. " part", p[9])
            o.parts[#o.parts + 1] = id
            if p[8] and not NO_FLASH[p[8]] then skin[#skin + 1] = { id, p[8] } end
        end
    end
end

local function assembly(name, parts, x, y, z)
    if not x then x, y, z = parkedAt() end
    local o = { parts = {}, skin = {}, mounts = {}, mount = {}, active = false, shown = true }
    o.root = spawn(EMP, x, y, z, 0.3, 0.3, 0.3, nil, nil, name)
    addParts(o, o.root, parts, o.skin, name)
    objects[#objects + 1] = o
    return o
end

local function single(name, kind, hx, hy, hz, mat, rot)
    local x, y, z = parkedAt()
    local o = { root = spawn(kind, x, y, z, hx, hy, hz, mat, nil, name, rot),
                active = false, shown = true, mounts = {} }
    objects[#objects + 1] = o
    return o
end

local function newPool(n, make)
    local p = { items = {}, cursor = 1 }
    for i = 1, n do p.items[i] = make(i) end
    return p
end

local function take(pool)
    local n = #pool.items
    for k = 0, n - 1 do
        local i = (pool.cursor - 1 + k) % n + 1
        local it = pool.items[i]
        if not it.active and not it.busy then
            pool.cursor = i % n + 1
            it.active = true
            return it
        end
    end
    return nil
end

-- Pooled objects are switched on and off only when their state changes, and
-- only from the second frame: before that they do not exist in the scene yet.
local function syncVisibility()
    if frameNo < 2 then return end
    for _, o in ipairs(objects) do
        local want = (o.active or o.busy) and not o.blinkOff
        if want ~= o.shown then
            game.setActive(o.root, want)
            o.shown = want
        end
        for _, m in ipairs(o.mounts) do
            if m.alive ~= m.shown then
                game.setActive(m.root, m.alive)
                m.shown = m.alive
            end
        end
    end
end

local function place(o, u, v, y) game.setPos(o.root, W(u, v, y)) end

-- A craft's paint: its own colours (nil), the hit flash, or charred once it is
-- a wreck. Changed only when it changes -- every part is a material swap.
function SYS.skin(holder, state)
    if holder.skinState == state then return end
    holder.skinState = state
    local m = state and mats[state == "flash" and "flash" or "debris"]
    for _, s in ipairs(holder.skin) do game.setMaterial(s[1], m or mats[s[2]]) end
end

-- A hit turns the craft (or the one part that was hit) warm for a moment.
local function setFlash(holder, on) SYS.skin(holder, on and "flash" or nil) end

-- --- Building: the world -------------------------------------------------------------

local ISLAND_MATS = { "islandA", "islandB", "islandC" }
local CLOUD_MATS = { "cloudA", "cloudB", "cloudC" }

local function buildWorld()
    world.rows = {}
    if SYS.land then
        -- The scene's own ground is the ground: no sea, no painted islands.
    elseif hasTex.sea then
        for row = 0, 3 do
            local r = { v = -30.0 + row * SEA_TILE, tiles = {}, mat = "sea" }
            for col = -2, 2 do
                local x, y, z = W(col * SEA_TILE, r.v, 0.0)
                r.tiles[#r.tiles + 1] = { col = col, u = col * SEA_TILE,
                    id = spawn(PLN, x, y, z, SEA_TILE * 0.5, 0.02, SEA_TILE * 0.5, "sea", nil, "ground") }
            end
            world.rows[#world.rows + 1] = r
        end
    else
        local x, y, z = W(0.0, 0.0, -0.3)
        spawn(BOX, x, y, z, 90.0, 0.3, 90.0, "sea", nil, "sea")
    end

    for i = 1, SYS.land and 0 or 4 do
        local isl = { u = rand(-22, 22), v = -16.0 + i * 18.0 }
        local parts
        if hasTex.islandA then
            parts = { { PLN, 0.0, 0.03, 0.0, 11.0, 0.02, 11.0, ISLAND_MATS[i % 3 + 1] } }
        else
            parts = { { CYL, 0.0, 0.1, 0.0, 4.5, 0.12, 4.5, "sand" },
                      { CYL, 0.2, 0.28, 0.1, 3.6, 0.18, 3.4, "grass" } }
        end
        -- A few palms stand up out of the painted canopy: they throw shadows
        -- and slide against the ground as you pass, which is what makes the
        -- island read as ground far below rather than a picture on the sea.
        for _ = 1, 3 do
            local a, d = rand(0, TAU), rand(0.4, 1.8)
            local x, z = cos(a) * d, sin(a) * d
            parts[#parts + 1] = { CYL, x, 0.6, z, 0.06, 0.6, 0.06, "trunk" }
            local turn = rand(0, TAU)
            for k = 0, 4 do
                local la = turn + k * TAU / 5
                parts[#parts + 1] = { BOX, x + cos(la) * 0.42, 1.12, z + sin(la) * 0.42,
                                      0.44, 0.02, 0.11, "palm", { attitude(la, -0.4) } }
            end
        end
        local x, y, z = W(isl.u, isl.v, 0.0)
        isl.obj = assembly("island", parts, x, y, z)
        isl.obj.active = true
        isl.plane = isl.obj.parts[1]
        world.islands[i] = isl
    end

    -- (Not over a landscape: its trees stand taller than these hang, and its
    -- sky throws cloud shadows of its own.)
    for i = 1, SYS.land and 0 or 7 do
        local c = { u = rand(-26, 26), v = -30.0 + i * 10.0, y = LOW_CLOUD_Y + i * 0.07 }
        local x, y, z = W(c.u, c.v, c.y)
        if hasTex.cloudA then
            local s = rand(6.5, 10.0)
            c.id = spawn(PLN, x, y, z, s, 0.02, s * rand(0.8, 1.0), CLOUD_MATS[i % 3 + 1], nil,
                         "cloud", { 0.0, rand(-18, 18), 0.0 })
        else
            local s = rand(1.0, 1.8)
            c.id = assembly("cloud", {
                { SPH, 0.0, 0.0, 0.0, 2.2 * s, 0.5, 1.5 * s, "cloud" },
                { SPH, 1.6 * s, 0.1, 0.4, 1.5 * s, 0.45, 1.1 * s, "cloud" },
                { SPH, -1.5 * s, -0.1, -0.3, 1.4 * s, 0.4, 1.0 * s, "cloud" },
            }, x, y, z).root
        end
        world.clouds[i] = c
    end

    if hasTex.haze and highClouds then
        for i = 1, 2 do
            local c = { u = rand(-12, 12), v = 20.0 + i * 60.0, y = HIGH_CLOUD_Y + i * 0.1 }
            local x, y, z = W(c.u, c.v, c.y)
            c.id = spawn(PLN, x, y, z, rand(5.0, 6.5), 0.02, rand(4.5, 5.5), "haze", nil, "haze",
                         { 0.0, rand(-15, 15), 0.0 })
            world.haze[i] = c
        end
    end
end

-- --- Building: the player ----------------------------------------------------------

local function buildPlayer()
    local o = assembly("player", {
        { BOX, 0.0, 0.0, 0.0, 0.2, 0.17, 0.95, "hull" },
        { SPH, 0.0, 0.0, 0.95, 0.2, 0.15, 0.55, "hull" },
        { SPH, 0.0, 0.15, 0.4, 0.13, 0.11, 0.34, "glass" },
        { BOX, -0.72, -0.02, -0.25, 0.62, 0.045, 0.34, "wing", { 0, 22, 0 } },
        { BOX, 0.72, -0.02, -0.25, 0.62, 0.045, 0.34, "wing", { 0, -22, 0 } },
        { BOX, -0.74, 0.03, -0.28, 0.46, 0.01, 0.06, "accent", { 0, 22, 0 } },
        { BOX, 0.74, 0.03, -0.28, 0.46, 0.01, 0.06, "accent", { 0, -22, 0 } },
        { BOX, 0.0, 0.0, 0.55, 0.48, 0.03, 0.1, "hullDark" },
        { BOX, -0.28, 0.24, -0.78, 0.03, 0.2, 0.2, "wing", { 0, 0, -12 } },
        { BOX, 0.28, 0.24, -0.78, 0.03, 0.2, 0.2, "wing", { 0, 0, 12 } },
        { CYL, -0.12, 0.0, -1.0, 0.1, 0.12, 0.1, "hullDark", { 90, 0, 0 } },
        { CYL, 0.12, 0.0, -1.0, 0.1, 0.12, 0.1, "hullDark", { 90, 0, 0 } },
        { SPH, -1.28, 0.0, -0.5, 0.05, 0.05, 0.05, "navRed" },
        { SPH, 1.28, 0.0, -0.5, 0.05, 0.05, 0.05, "navGreen" },
    })
    o.flames = {
        spawn(SPH, -0.12, 0.0, 1.25, 0.08, 0.08, 0.25, "flame", o.root, "flame"),
        spawn(SPH, 0.12, 0.0, 1.25, 0.08, 0.08, 0.25, "flame", o.root, "flame"),
    }
    o.muzzles = {}
    if hasTex.muzzle then
        for i, x in ipairs({ -0.22, 0.22 }) do
            o.muzzles[i] = spawn(PLN, x, 0.05, -1.6, 0.001, 0.02, 0.001, "muzzle", o.root, "muzzle")
        end
    end
    player = { obj = o, u = 0.0, v = -8.0, roll = 0.0, alive = false, invuln = 0.0,
               respawn = 0.0, fireCd = 0.0, missileCd = 0.0, focus = false, firing = false }
    player.hitbox = single("hitbox", SPH, 0.11, 0.11, 0.11, "hitbox")
    player.shield = single("shield", SPH, 1.35, 0.6, 1.35, "shield")
    player.options = {}
    for i = 1, 2 do
        local op = assembly("option", {
            { SPH, 0.0, 0.0, 0.0, 0.22, 0.22, 0.22, "option" },
            { CYL, 0.0, 0.0, 0.0, 0.42, 0.03, 0.42, "ring" },
        })
        op.u, op.v, op.side = 0.0, -8.0, (i == 1) and -1 or 1
        player.options[i] = op
    end
end

-- --- Building: enemies ------------------------------------------------------------------
-- Every craft is built nose-up (forward = up the screen) and turned to face where
-- it flies; the big set pieces are built the way they sit on screen.

local function turret(key, across, up, fwd, scale, barrels, mat)
    local s = scale or 1.0
    local parts = {
        { CYL, 0.0, 0.0, 0.0, 0.34 * s, 0.12 * s, 0.34 * s, mat or "darkMetal" },
        { SPH, 0.0, 0.12 * s, 0.0, 0.24 * s, 0.14 * s, 0.24 * s, mat or "darkMetal" },
    }
    local n = barrels or 2
    for i = 1, n do
        local x = (n == 1) and 0.0 or ((i - 1) / (n - 1) - 0.5) * 0.24 * s
        parts[#parts + 1] = { BOX, x, 0.12 * s, 0.42 * s, 0.05 * s, 0.05 * s, 0.34 * s, "darkMetal" }
    end
    return { "mount", key, across, up, fwd, parts }
end

KINDS.dart = {
    pool = 18, hp = 1, r = 0.75, score = 100, boom = 1,
    build = function()
        return assembly("dart", {
            { SPH, 0.0, 0.0, 0.0, 0.2, 0.16, 0.8, "red" },
            { SPH, 0.0, 0.0, 0.72, 0.12, 0.1, 0.32, "white" },
            { SPH, 0.0, 0.12, 0.22, 0.09, 0.07, 0.22, "glass" },
            { BOX, -0.46, 0.0, -0.15, 0.48, 0.04, 0.2, "red", { 0, 30, 0 } },
            { BOX, 0.46, 0.0, -0.15, 0.48, 0.04, 0.2, "red", { 0, -30, 0 } },
            { BOX, 0.0, 0.16, -0.6, 0.03, 0.16, 0.14, "darkMetal" },
            { BOX, 0.0, 0.0, -0.8, 0.07, 0.06, 0.05, "redGlow" },
        })
    end,
}
KINDS.saucer = {
    pool = 12, hp = 3, r = 0.9, score = 200, boom = 1,
    build = function()
        return assembly("saucer", {
            { CYL, 0.0, 0.0, 0.0, 0.95, 0.1, 0.95, "purple" },
            { CYL, 0.0, -0.08, 0.0, 0.62, 0.12, 0.62, "darkMetal" },
            { SPH, 0.0, 0.14, 0.0, 0.4, 0.26, 0.4, "glass" },
            { SPH, 0.78, 0.04, 0.0, 0.1, 0.08, 0.1, "eye" },
            { SPH, -0.78, 0.04, 0.0, 0.1, 0.08, 0.1, "eye" },
            { SPH, 0.0, 0.04, 0.78, 0.1, 0.08, 0.1, "eye" },
            { SPH, 0.0, 0.04, -0.78, 0.1, 0.08, 0.1, "eye" },
        })
    end,
}
KINDS.hornet = {
    pool = 8, hp = 4, r = 0.75, score = 300, boom = 1,
    build = function()
        local o = assembly("hornet", {
            { SPH, 0.0, 0.0, 0.0, 0.3, 0.22, 0.55, "yellow" },
            { SPH, 0.0, 0.02, 0.55, 0.22, 0.18, 0.26, "black" },
            { BOX, 0.0, 0.17, -0.08, 0.26, 0.06, 0.07, "black" },
            { BOX, 0.0, 0.14, -0.3, 0.2, 0.06, 0.06, "black" },
            { BOX, -0.46, 0.2, 0.05, 0.42, 0.02, 0.16, "glass", { 0, -18, 0 } },
            { BOX, 0.46, 0.2, 0.05, 0.42, 0.02, 0.16, "glass", { 0, 18, 0 } },
            { SPH, 0.0, 0.0, -0.66, 0.06, 0.06, 0.2, "darkMetal" },
        })
        o.eye = spawn(SPH, 0.0, 0.05, -0.78, 0.13, 0.09, 0.09, "eye", o.root, "hornet eye")
        return o
    end,
}
KINDS.gunship = {
    pool = 3, hp = 32, r = 1.5, score = 1500, boom = 2,
    build = function()
        return assembly("gunship", {
            { BOX, 0.0, 0.0, 0.0, 1.0, 0.35, 1.5, "olive" },
            { BOX, 0.0, 0.38, -0.1, 0.7, 0.1, 1.1, "oliveDark" },
            { BOX, 0.0, -0.05, 0.1, 2.0, 0.1, 0.45, "darkMetal" },
            { BOX, -1.75, 0.0, -0.2, 0.3, 0.28, 0.6, "oliveDark" },
            { BOX, 1.75, 0.0, -0.2, 0.3, 0.28, 0.6, "oliveDark" },
            { BOX, -1.75, 0.0, -0.84, 0.22, 0.14, 0.06, "blueGlow" },
            { BOX, 1.75, 0.0, -0.84, 0.22, 0.14, 0.06, "blueGlow" },
            { SPH, 0.0, 0.25, 1.35, 0.35, 0.2, 0.4, "glass" },
            turret("tl", -0.52, 0.5, 0.35, 1.0, 2, "darkMetal"),
            turret("tr", 0.52, 0.5, 0.35, 1.0, 2, "darkMetal"),
        })
    end,
}
KINDS.sentinel = {
    pool = 3, hp = 22, r = 1.1, score = 1200, boom = 2,
    build = function()
        return assembly("sentinel", {
            { CYL, 0.0, 0.0, 0.0, 0.75, 0.22, 0.75, "grey" },
            { CYL, 0.0, 0.2, 0.0, 0.45, 0.12, 0.45, "darkMetal" },
            { BOX, 0.0, 0.0, 0.0, 1.5, 0.08, 0.16, "darkMetal" },
            { SPH, -1.5, 0.0, 0.0, 0.2, 0.2, 0.2, "blueGlow" },
            { SPH, 1.5, 0.0, 0.0, 0.2, 0.2, 0.2, "blueGlow" },
            { SPH, 0.0, 0.0, 0.62, 0.24, 0.22, 0.24, "redGlow" },
        })
    end,
}
KINDS.mine = {
    pool = 8, hp = 6, r = 0.7, score = 150, boom = 1,
    build = function()
        local o = assembly("mine", {
            { SPH, 0.0, 0.0, 0.0, 0.5, 0.5, 0.5, "darkMetal" },
            { BOX, 0.0, 0.0, 0.0, 0.8, 0.07, 0.07, "grey" },
            { BOX, 0.0, 0.0, 0.0, 0.07, 0.07, 0.8, "grey" },
            { BOX, 0.0, 0.0, 0.0, 0.57, 0.07, 0.07, "grey", { 0, 45, 0 } },
            { BOX, 0.0, 0.0, 0.0, 0.57, 0.07, 0.07, "grey", { 0, -45, 0 } },
        })
        o.lamp = spawn(SPH, 0.0, 0.48, 0.0, 0.14, 0.1, 0.14, "redGlow", o.root, "mine lamp")
        return o
    end,
}
KINDS.missile = {
    pool = 12, hp = 2, r = 0.45, score = 50, boom = 1,
    build = function()
        local o = assembly("missile", {
            { BOX, 0.0, 0.0, 0.0, 0.1, 0.1, 0.42, "grey" },
            { SPH, 0.0, 0.0, 0.42, 0.1, 0.1, 0.16, "red" },
            { BOX, 0.0, 0.0, -0.32, 0.28, 0.02, 0.09, "darkMetal" },
        })
        o.flame = spawn(SPH, 0.0, 0.0, 0.62, 0.08, 0.08, 0.22, "flame", o.root, "missile flame")
        return o
    end,
}
KINDS.boat = {
    pool = 8, hp = 8, r = 1.3, score = 400, boom = 2, ground = true,
    build = function()
        return assembly("boat", {
            { BOX, 0.0, 0.0, 0.0, 0.72, 0.3, 1.8, "navy" },
            { BOX, 0.0, 0.0, 1.85, 0.51, 0.3, 0.51, "navy", { 0, 45, 0 } },
            { BOX, 0.0, 0.31, 0.0, 0.6, 0.02, 1.7, "deck" },
            { BOX, 0.0, 0.55, -0.5, 0.45, 0.25, 0.55, "white" },
            { BOX, 0.0, 0.62, -0.02, 0.4, 0.04, 0.03, "black" },
            { CYL, 0.0, 0.9, -0.75, 0.12, 0.18, 0.12, "darkMetal" },
            turret("gun", 0.0, 0.4, 0.85, 1.1, 1),
        })
    end,
}
KINDS.aa = {
    pool = 6, hp = 10, r = 0.9, score = 500, boom = 2, ground = true,
    build = function()
        return assembly("aa site", {
            { CYL, 0.0, 0.0, 0.0, 0.8, 0.25, 0.8, "grey" },
            { CYL, 0.0, 0.25, 0.0, 0.6, 0.06, 0.6, "darkMetal" },
            turret("gun", 0.0, 0.35, 0.0, 1.0, 2, "olive"),
        })
    end,
}
KINDS.cruiser = {
    pool = 1, hp = 70, r = 1.4, score = 6000, boom = 3, ground = true,
    build = function()
        return assembly("cruiser", {
            { BOX, 0.0, 0.0, 0.0, 1.1, 0.35, 4.2, "navy" },
            { BOX, 0.0, 0.0, 4.3, 0.78, 0.35, 0.78, "navy", { 0, 45, 0 } },
            { BOX, 0.0, 0.36, 0.0, 0.95, 0.02, 4.1, "deck" },
            { BOX, 0.0, 0.6, -0.3, 0.6, 0.3, 1.0, "white" },
            { BOX, 0.0, 1.0, -0.1, 0.4, 0.2, 0.5, "grey" },
            { CYL, 0.0, 1.0, -1.3, 0.2, 0.4, 0.2, "darkMetal" },
            turret("t1", 0.0, 0.45, 2.6, 1.2, 2),
            turret("t2", 0.0, 0.45, 1.3, 1.1, 2),
            turret("t3", 0.0, 0.45, -2.8, 1.2, 2),
        })
    end,
    parts = { t1 = 20, t2 = 16, t3 = 20 },
}
KINDS.bomber = {
    pool = 1, hp = 170, r = 2.4, score = 8000, boom = 4, midboss = true, name = "HEAVY BOMBER",
    build = function()
        return assembly("bomber", {
            { BOX, 0.0, 0.0, 0.0, 0.75, 0.55, 3.2, "boss" },
            { SPH, 0.0, 0.0, 3.1, 0.72, 0.52, 1.0, "boss" },
            { SPH, 0.0, 0.45, 3.0, 0.4, 0.22, 0.5, "glass" },
            { BOX, 0.0, 0.0, 0.4, 5.8, 0.14, 1.05, "bossPlate" },
            { BOX, 0.0, 0.05, -3.0, 2.1, 0.1, 0.5, "bossPlate" },
            { BOX, 0.0, 0.7, -2.9, 0.08, 0.7, 0.55, "boss" },
            { BOX, -2.2, -0.12, 0.9, 0.3, 0.3, 0.85, "darkMetal" },
            { BOX, 2.2, -0.12, 0.9, 0.3, 0.3, 0.85, "darkMetal" },
            { BOX, -4.1, -0.12, 0.7, 0.28, 0.28, 0.75, "darkMetal" },
            { BOX, 4.1, -0.12, 0.7, 0.28, 0.28, 0.75, "darkMetal" },
            { BOX, -2.2, -0.12, 0.02, 0.22, 0.18, 0.06, "blueGlow" },
            { BOX, 2.2, -0.12, 0.02, 0.22, 0.18, 0.06, "blueGlow" },
            { BOX, -4.1, -0.12, -0.08, 0.2, 0.16, 0.06, "blueGlow" },
            { BOX, 4.1, -0.12, -0.08, 0.2, 0.16, 0.06, "blueGlow" },
            turret("w1", -2.2, 0.28, 1.2, 1.0, 2),
            turret("w2", 2.2, 0.28, 1.2, 1.0, 2),
            turret("w3", -4.1, 0.26, 1.0, 0.9, 2),
            turret("w4", 4.1, 0.26, 1.0, 0.9, 2),
        })
    end,
    parts = { w1 = 22, w2 = 22, w3 = 18, w4 = 18 },
}
KINDS.leviathan = {
    pool = 1, hp = 600, r = 2.8, score = 30000, boom = 5, boss = true, name = "LEVIATHAN",
    -- A flying fortress seen from above, nose down the screen toward you:
    -- an armoured spine, swept wings with a cannon pod at each tip, four
    -- engines at the tail, and the core under two shutters amidships.
    build = function()
        local pod = function(key, x)
            local lamp = (x < 0) and "navRed" or "navGreen"
            return { "mount", key, x, 0.0, 0.3, {
                { BOX, 0.0, 0.0, 0.0, 0.62, 0.48, 1.7, "boss" },
                { BOX, 0.0, 0.5, 0.2, 0.44, 0.05, 1.2, "bossPlate" },
                { BOX, 0.0, 0.52, -0.9, 0.4, 0.03, 0.12, "hazard" },
                { CYL, -0.24, 0.0, -1.95, 0.12, 0.45, 0.12, "grey", { 90, 0, 0 } },
                { CYL, 0.24, 0.0, -1.95, 0.12, 0.45, 0.12, "grey", { 90, 0, 0 } },
                { BOX, 0.0, 0.0, 1.72, 0.42, 0.3, 0.06, "blueGlow" },
                { SPH, (x < 0) and -0.62 or 0.62, 0.1, 0.6, 0.08, 0.08, 0.08, lamp },
            } }
        end
        local o = assembly("leviathan", {
            -- spine and nose
            { BOX, 0.0, 0.0, 0.2, 1.5, 0.75, 3.6, "boss" },
            { BOX, -0.75, 0.0, -3.6, 1.05, 0.62, 0.55, "boss", { 0, -38, 0 } },
            { BOX, 0.75, 0.0, -3.6, 1.05, 0.62, 0.55, "boss", { 0, 38, 0 } },
            { BOX, 0.0, 0.3, -4.05, 0.55, 0.12, 0.14, "eyeHot" },
            { BOX, 0.0, 0.78, -2.3, 1.1, 0.06, 1.0, "bossPlate" },
            { BOX, 0.0, 0.8, 2.4, 1.2, 0.06, 1.4, "bossPlate" },
            { BOX, -1.0, 0.8, 0.2, 0.08, 0.04, 3.2, "red" },
            { BOX, 1.0, 0.8, 0.2, 0.08, 0.04, 3.2, "red" },
            -- wings, swept back, with armour plates on top
            { BOX, -3.1, -0.1, 0.9, 2.7, 0.22, 1.35, "boss", { 0, 16, 0 } },
            { BOX, 3.1, -0.1, 0.9, 2.7, 0.22, 1.35, "boss", { 0, -16, 0 } },
            { BOX, -3.0, 0.14, 1.0, 2.2, 0.05, 0.7, "bossPlate", { 0, 16, 0 } },
            { BOX, 3.0, 0.14, 1.0, 2.2, 0.05, 0.7, "bossPlate", { 0, -16, 0 } },
            { BOX, -2.6, 0.2, 0.5, 0.5, 0.04, 0.1, "hazard", { 0, 16, 0 } },
            { BOX, 2.6, 0.2, 0.5, 0.5, 0.04, 0.1, "hazard", { 0, -16, 0 } },
            -- engines
            { CYL, -1.05, 0.0, 3.85, 0.38, 0.35, 0.38, "grey", { 90, 0, 0 } },
            { CYL, 1.05, 0.0, 3.85, 0.38, 0.35, 0.38, "grey", { 90, 0, 0 } },
            { CYL, -2.5, -0.1, 2.35, 0.3, 0.3, 0.3, "grey", { 90, 0, 0 } },
            { CYL, 2.5, -0.1, 2.35, 0.3, 0.3, 0.3, "grey", { 90, 0, 0 } },
            { SPH, -1.05, 0.0, 4.3, 0.3, 0.3, 0.3, "blueGlow" },
            { SPH, 1.05, 0.0, 4.3, 0.3, 0.3, 0.3, "blueGlow" },
            { SPH, -2.5, -0.1, 2.75, 0.24, 0.24, 0.24, "blueGlow" },
            { SPH, 2.5, -0.1, 2.75, 0.24, 0.24, 0.24, "blueGlow" },
            -- the core and its shutters
            { SPH, 0.0, 0.6, -0.5, 0.8, 0.55, 0.8, "core" },
            { "mount", "shutL", -0.42, 0.95, -0.5, { { BOX, 0.0, 0.0, 0.0, 0.44, 0.12, 0.95, "bossPlate" } } },
            { "mount", "shutR", 0.42, 0.95, -0.5, { { BOX, 0.0, 0.0, 0.0, 0.44, 0.12, 0.95, "bossPlate" } } },
            turret("tl", -2.3, 0.35, 0.3, 1.15, 2, "grey"),
            turret("tr", 2.3, 0.35, 0.3, 1.15, 2, "grey"),
            pod("podL", -5.3), pod("podR", 5.3),
        })
        o.exhaust = {}
        for i, x in ipairs({ -1.05, 1.05 }) do
            o.exhaust[i] = spawn(SPH, x, 0.0, -4.9, 0.22, 0.22, 0.7, "flame", o.root, "boss flame")
        end
        return o
    end,
    parts = { podL = 140, podR = 140 }, partR = 1.5,
}
KINDS.dreadnought = {
    pool = 1, hp = 480, r = 1.9, score = 30000, boom = 5, boss = true, ground = true,
    name = "DREADNOUGHT",
    -- A battleship on the sea, bow toward you: grey hull round a dark deck,
    -- two gun turrets fore and two aft, missile launchers beside a stepped
    -- bridge, and funnels that smoke.
    build = function()
        local launcher = function(key, x, fwd)
            return { "mount", key, x, 0.75, fwd, {
                { BOX, 0.0, 0.0, 0.0, 0.5, 0.25, 0.7, "grey" },
                { BOX, -0.2, 0.28, 0.0, 0.13, 0.08, 0.5, "red" },
                { BOX, 0.2, 0.28, 0.0, 0.13, 0.08, 0.5, "red" },
                { BOX, 0.0, 0.28, -0.55, 0.45, 0.04, 0.08, "hazard" },
            } }
        end
        return assembly("dreadnought", {
            { BOX, 0.0, 0.0, 0.0, 2.4, 0.6, 7.0, "navy" },
            { BOX, 0.0, 0.0, -7.0, 1.7, 0.6, 1.7, "navy", { 0, 45, 0 } },
            { BOX, 0.0, 0.0, -8.3, 0.9, 0.6, 0.9, "navy", { 0, 45, 0 } },
            { CYL, 0.0, 0.0, 7.0, 2.4, 0.6, 2.4, "navy" },
            { BOX, 0.0, 0.62, 0.0, 1.95, 0.02, 6.9, "deck" },
            { BOX, 0.0, 0.62, -7.0, 1.2, 0.02, 1.2, "deck", { 0, 45, 0 } },
            { BOX, 0.0, 1.0, 0.6, 1.3, 0.4, 2.0, "white" },
            { BOX, 0.0, 1.55, 0.4, 0.95, 0.25, 1.3, "grey" },
            { BOX, 0.0, 1.85, 0.1, 0.7, 0.12, 0.55, "white" },
            { BOX, 0.0, 1.88, -0.47, 0.6, 0.06, 0.04, "black" },
            { BOX, 0.0, 2.6, 0.4, 0.06, 0.7, 0.06, "darkMetal" },
            { BOX, 0.0, 3.1, 0.4, 0.5, 0.04, 0.08, "darkMetal" },
            { CYL, 0.0, 1.6, 2.9, 0.42, 0.8, 0.42, "darkMetal" },
            { CYL, 0.0, 1.5, 3.9, 0.36, 0.7, 0.36, "darkMetal" },
            { SPH, 0.0, 2.0, 1.2, 0.5, 0.3, 0.5, "core" },
            turret("t1", 0.0, 0.8, -5.2, 1.7, 3, "grey"),
            turret("t2", 0.0, 0.8, -3.0, 1.6, 3, "grey"),
            turret("t3", 0.0, 0.8, 5.0, 1.6, 3, "grey"),
            turret("t4", 0.0, 0.8, 6.9, 1.6, 3, "grey"),
            launcher("l1", -1.55, -1.1), launcher("l2", 1.55, -1.1),
        })
    end,
    parts = { t1 = 60, t2 = 55, t3 = 55, t4 = 60, l1 = 45, l2 = 45 }, partR = 1.1,
}

KINDS.drone = {
    pool = 16, hp = 1, r = 0.6, score = 80, boom = 1,
    build = function()
        return assembly("drone", {
            { SPH, 0.0, 0.0, 0.0, 0.3, 0.22, 0.3, "grey" },
            { CYL, 0.0, 0.0, 0.0, 0.58, 0.03, 0.58, "darkMetal" },
            { SPH, 0.0, 0.05, 0.3, 0.12, 0.08, 0.12, "redGlow" },
        })
    end,
}
KINDS.tank = {
    pool = 8, hp = 10, r = 1.1, score = 500, boom = 2, ground = true, onLand = true,
    build = function()
        return assembly("tank", {
            { BOX, 0.0, 0.0, 0.0, 0.62, 0.25, 0.95, "olive" },
            { BOX, -0.74, -0.04, 0.0, 0.17, 0.22, 1.05, "darkMetal" },
            { BOX, 0.74, -0.04, 0.0, 0.17, 0.22, 1.05, "darkMetal" },
            { BOX, 0.0, 0.26, -0.6, 0.5, 0.04, 0.28, "oliveDark" },
            { "mount", "gun", 0.0, 0.3, 0.1, {
                { CYL, 0.0, 0.0, 0.0, 0.44, 0.12, 0.44, "oliveDark" },
                { BOX, 0.0, 0.06, 0.8, 0.06, 0.06, 0.6, "darkMetal" },
                { SPH, 0.22, 0.14, -0.12, 0.08, 0.06, 0.08, "darkMetal" },
            } },
        })
    end,
}
KINDS.tempest = {
    pool = 1, hp = 620, r = 2.6, score = 30000, boom = 5, boss = true, name = "TEMPEST",
    -- A flying wing in the storm, nose down the screen: two tesla pylons on the
    -- wings throw aimed lightning; with them gone the eye opens and turns two
    -- beams round like a lighthouse.
    build = function()
        local pylon = function(key, x)
            return { "mount", key, x, 0.3, 1.5, {
                { CYL, 0.0, 0.0, 0.0, 0.6, 0.35, 0.6, "boss" },
                { CYL, 0.0, 0.7, 0.0, 0.26, 0.7, 0.26, "grey" },
                { CYL, 0.0, 1.0, 0.0, 0.5, 0.05, 0.5, "grey" },
                { CYL, 0.0, 1.3, 0.0, 0.42, 0.05, 0.42, "grey" },
                { SPH, 0.0, 1.6, 0.0, 0.36, 0.36, 0.36, "blueGlow" },
            } }
        end
        local o = assembly("tempest", {
            { BOX, 0.0, 0.0, 0.5, 1.5, 0.6, 2.6, "boss" },
            { BOX, 0.0, 0.0, -2.3, 1.05, 0.5, 1.05, "boss", { 0, 45, 0 } },
            { BOX, -3.7, -0.05, 1.3, 3.3, 0.25, 1.5, "boss", { 0, -22, 0 } },
            { BOX, 3.7, -0.05, 1.3, 3.3, 0.25, 1.5, "boss", { 0, 22, 0 } },
            { BOX, -3.5, 0.22, 1.35, 2.6, 0.04, 0.8, "bossPlate", { 0, -22, 0 } },
            { BOX, 3.5, 0.22, 1.35, 2.6, 0.04, 0.8, "bossPlate", { 0, 22, 0 } },
            { BOX, -1.0, 0.62, 0.8, 0.08, 0.04, 2.0, "blueGlow" },
            { BOX, 1.0, 0.62, 0.8, 0.08, 0.04, 2.0, "blueGlow" },
            -- leading edges lit, engine pods under the wings, a sawtooth trailing edge
            { BOX, -3.95, 0.1, 0.02, 3.3, 0.05, 0.06, "blueGlow", { 0, -22, 0 } },
            { BOX, 3.95, 0.1, 0.02, 3.3, 0.05, 0.06, "blueGlow", { 0, 22, 0 } },
            { CYL, -2.6, -0.3, 1.2, 0.42, 0.9, 0.42, "darkMetal", { 90, 0, 0 } },
            { CYL, 2.6, -0.3, 1.2, 0.42, 0.9, 0.42, "darkMetal", { 90, 0, 0 } },
            { SPH, -2.6, -0.3, 2.15, 0.3, 0.3, 0.2, "blueGlow" },
            { SPH, 2.6, -0.3, 2.15, 0.3, 0.3, 0.2, "blueGlow" },
            { BOX, -2.0, 0.0, 2.75, 0.7, 0.2, 0.7, "boss", { 0, 45, 0 } },
            { BOX, 2.0, 0.0, 2.75, 0.7, 0.2, 0.7, "boss", { 0, 45, 0 } },
            { BOX, -4.4, 0.0, 3.3, 0.6, 0.18, 0.6, "boss", { 0, 45, 0 } },
            { BOX, 4.4, 0.0, 3.3, 0.6, 0.18, 0.6, "boss", { 0, 45, 0 } },
            { BOX, -2.9, 0.24, 1.5, 0.9, 0.03, 0.05, "hazard", { 0, -22, 0 } },
            { BOX, 2.9, 0.24, 1.5, 0.9, 0.03, 0.05, "hazard", { 0, 22, 0 } },
            { BOX, -0.8, 0.0, 3.15, 0.35, 0.3, 0.1, "blueGlow" },
            { BOX, 0.8, 0.0, 3.15, 0.35, 0.3, 0.1, "blueGlow" },
            { SPH, 0.0, 0.5, -0.6, 0.85, 0.45, 0.85, "core" },
            { "mount", "shutL", -0.45, 0.9, -0.6, { { BOX, 0.0, 0.0, 0.0, 0.46, 0.12, 0.95, "bossPlate" } } },
            { "mount", "shutR", 0.45, 0.9, -0.6, { { BOX, 0.0, 0.0, 0.0, 0.46, 0.12, 0.95, "bossPlate" } } },
            turret("tl", -2.2, 0.35, -0.4, 1.1, 2, "grey"),
            turret("tr", 2.2, 0.35, -0.4, 1.1, 2, "grey"),
            pylon("pyL", -5.4), pylon("pyR", 5.4),
        })
        return o
    end,
    parts = { pyL = 130, pyR = 130 }, partR = 1.4,
}
KINDS.bastion = {
    pool = 1, hp = 520, r = 2.2, score = 30000, boom = 5, boss = true, ground = true, onLand = true,
    name = "BASTION",
    -- A fortress on the mainland. The ground stops for it: four gun towers at
    -- the corners, a great cannon on the dome, and under the dome the core.
    build = function()
        local tower = function(key, x, f)
            return { "mount", key, x, 0.5, f, {
                { CYL, 0.0, 0.0, 0.0, 1.15, 1.0, 1.15, "concrete" },
                { CYL, 0.0, 1.05, 0.0, 0.75, 0.15, 0.75, "darkMetal" },
                { BOX, -0.2, 1.2, 0.8, 0.1, 0.1, 0.7, "darkMetal" },
                { BOX, 0.2, 1.2, 0.8, 0.1, 0.1, 0.7, "darkMetal" },
                { BOX, 0.0, 1.02, -0.9, 0.5, 0.04, 0.12, "hazard" },
            } }
        end
        return assembly("bastion", {
            { BOX, 0.0, 0.0, 0.0, 4.6, 0.5, 4.6, "concrete" },
            { BOX, 0.0, 0.55, 4.5, 4.8, 0.45, 0.35, "grey" },
            { BOX, 0.0, 0.55, -4.5, 4.8, 0.45, 0.35, "grey" },
            { BOX, 4.5, 0.55, 0.0, 0.35, 0.45, 4.8, "grey" },
            { BOX, -4.5, 0.55, 0.0, 0.35, 0.45, 4.8, "grey" },
            { BOX, -2.4, 0.52, -3.2, 1.2, 0.02, 0.2, "hazard" },
            { BOX, 2.4, 0.52, 3.2, 1.2, 0.02, 0.2, "hazard" },
            { SPH, 0.0, 0.6, 0.0, 2.3, 1.4, 2.3, "concrete" },
            { SPH, 0.0, 1.9, 0.0, 1.0, 0.6, 1.0, "core" },
            { "mount", "cannon", 0.0, 2.2, 0.0, {
                { BOX, 0.0, 0.0, 0.0, 0.8, 0.45, 0.9, "darkMetal" },
                { BOX, 0.0, 0.05, 2.1, 0.22, 0.22, 1.6, "grey" },
                { CYL, 0.0, 0.05, 3.7, 0.3, 0.12, 0.3, "darkMetal", { 90, 0, 0 } },
            } },
            tower("t1", -4.0, 4.0), tower("t2", 4.0, 4.0), tower("t3", -4.0, -4.0), tower("t4", 4.0, -4.0),
        })
    end,
    parts = { t1 = 70, t2 = 70, t3 = 70, t4 = 70 }, partR = 1.5,
}

-- Exhaust trails ({ across, forward, puff size }) and, on ships, the wake.
KINDS.dart.trail = { { 0.0, -0.9, 0.14 } }
KINDS.hornet.trail = { { 0.0, -0.8, 0.2 } }
KINDS.hornet.trailWhen = function(e) return e.state == "dash" end
KINDS.missile.trail = { { 0.0, -0.8, 0.16 } }
KINDS.missile.trailRate = 0.04
KINDS.gunship.trail = { { -1.75, -0.95, 0.3 }, { 1.75, -0.95, 0.3 } }
KINDS.gunship.trailRate = 0.06
KINDS.bomber.trail = { { -2.2, -0.1, 0.35 }, { 2.2, -0.1, 0.35 }, { -4.1, -0.2, 0.3 }, { 4.1, -0.2, 0.3 } }
KINDS.bomber.trailRate = 0.07
KINDS.leviathan.trail = { { -1.05, 4.7, 0.7 }, { 1.05, 4.7, 0.7 }, { -2.5, 3.1, 0.5 }, { 2.5, 3.1, 0.5 } }
KINDS.leviathan.trailRate = 0.06
KINDS.boat.trail = { { 0.0, -2.0, 0.45 }, { -0.5, 1.8, 0.3 }, { 0.5, 1.8, 0.3 } }
KINDS.boat.trailRate = 0.12
KINDS.cruiser.trail = { { 0.0, -4.4, 0.8 }, { -0.9, 4.2, 0.5 }, { 0.9, 4.2, 0.5 } }
KINDS.cruiser.trailRate = 0.12
KINDS.dreadnought.trail = { { 0.0, 9.6, 1.4 }, { -1.8, -7.8, 0.8 }, { 1.8, -7.8, 0.8 } }
KINDS.dreadnought.trailRate = 0.1
KINDS.drone.trail = { { 0.0, -0.4, 0.1 } }
KINDS.drone.trailRate = 0.09
KINDS.tank.trail = { { -0.74, -1.2, 0.3 }, { 0.74, -1.2, 0.3 } }
KINDS.tank.trailRate = 0.18
KINDS.tempest.trail = { { -0.8, 3.4, 0.7 }, { 0.8, 3.4, 0.7 }, { -2.6, 2.5, 0.5 }, { 2.6, 2.5, 0.5 } }
KINDS.tempest.trailRate = 0.06

-- --- Building: pools ------------------------------------------------------------------

local function buildPools()
    pools.shots = newPool(110, function() return single("shot", BOX, 0.06, 0.06, 0.36, "shot") end)
    pools.optShots = newPool(40, function() return single("opt shot", BOX, 0.06, 0.06, 0.34, "optShot") end)
    pools.pMissiles = newPool(16, function()
        local o = assembly("player missile", {
            { BOX, 0.0, 0.0, 0.0, 0.07, 0.07, 0.3, "missile" },
            { SPH, 0.0, 0.0, -0.36, 0.06, 0.06, 0.14, "flame" },
        })
        return o
    end)
    pools.orb = newPool(220, function() return single("bullet", SPH, 0.2, 0.2, 0.2, "orb") end)
    pools.big = newPool(70, function() return single("big bullet", SPH, 0.36, 0.36, 0.36, "big") end)
    pools.needle = newPool(110, function()
        local o = single("needle", BOX, 0.07, 0.07, 0.36, "needle")
        o.orient = true
        return o
    end)
    pools.sparks = newPool(120, function() return single("spark", BOX, 0.2, 0.2, 0.2, "spark") end)
    pools.debris = newPool(36, function() return single("debris", BOX, 0.2, 0.12, 0.16, "debris") end)
    if hasTex.ring1 then
        pools.rings = newPool(6, function() return single("ring", PLN, 1.0, 0.02, 1.0, "ring1") end)
    end
    pools.gems = newPool(90, function()
        return single("gem", BOX, 0.16, 0.16, 0.16, "gem", { 45, 45, 0 })
    end)
    if hasTex.fire1 then
        pools.fires = newPool(40, function() return single("fire", PLN, 1.0, 0.02, 1.0, "fire1") end)
        pools.smokes = newPool(SYS.land and 170 or 40,
                               function() return single("smoke", PLN, 1.0, 0.02, 1.0, "smoke1") end)
        if SYS.land then SYS.buildBlazes() end
    end
    pools.lasers = newPool(60, function() return single("laser", BOX, 0.06, 0.06, 0.9, "laser") end)
    if hasTex.puff1 then
        pools.puffs = newPool(160, function() return single("puff", PLN, 1.0, 0.02, 1.0, "puff1") end)
        pools.splashes = newPool(10, function() return single("splash", PLN, 1.0, 0.02, 1.0, "splash1") end)
    end
    if hasTex.muzzle then
        pools.muzzles = newPool(24, function() return single("muzzle", PLN, 1.0, 0.02, 1.0, "muzzle") end)
    end
    pools.orbs = newPool(6, function()
        local o = assembly("orb", { { CYL, 0.0, 0.0, 0.0, 0.62, 0.05, 0.62, "ring" } })
        o.coreId = spawn(SPH, 0.0, 0.0, 0.0, 0.38, 0.38, 0.38, "pwP", o.root, "orb core")
        return o
    end)
    for i = 1, 5 do
        beams[i] = { beam = single("beam", BOX, 0.3, 0.1, 10.0, "beam"),
                     warn = single("beam warning", BOX, 0.05, 0.05, 10.0, "beamWarn") }
    end
    for kind, def in pairs(KINDS) do
        enemyPools[kind] = newPool(def.pool, function()
            local o = def.build()
            o.kind, o.def = kind, def
            return o
        end)
    end
end

-- --- Effects -------------------------------------------------------------------------------

-- Sparks: little glowing chips thrown out from a hit.
function FX.sparks(u, v, n, size, speed, y)
    for _ = 1, n do
        local s = take(pools.sparks)
        if not s then return end
        local a = rand(0, TAU)
        local sp = rand(0.3, 1.0) * (speed or 7.0)
        s.u, s.v, s.y = u, v, y or FLY_Y
        s.vu, s.vv, s.vy = cos(a) * sp, sin(a) * sp, rand(-1.0, 3.0)
        s.life = rand(0.25, 0.6)
        s.max = s.life
        s.size = rand(0.1, 0.22) * (size or 1.0)
    end
end

function FX.fire(u, v, size, delay, y)
    local p = pools.fires
    if not p then
        FX.sparks(u, v, 5, size, 4.0, y)
        return
    end
    local f = take(p)
    if not f then return end
    f.u, f.v, f.y = u + rand(-0.3, 0.3) * size, v + rand(-0.3, 0.3) * size, (y or FLY_Y) + rand(0.1, 0.6)
    f.size, f.age, f.delay = size * rand(0.8, 1.2), 0.0, delay or 0.0
    f.life = rand(0.45, 0.7) + size * 0.08
    f.stage, f.rot = 0, rand(0, 360)
    f.hidden = true
end

-- rise (optional): a plume -- it climbs that fast, lives longer, keeps to the
-- ground it rose from and is blown off along the valley's wind. From straight
-- above a column would only be a blob over its fire; a plume trailing away
-- downwind is what a fire looks like from the air.
function FX.smoke(u, v, size, y, rise)
    local p = pools.smokes
    if not p then return end
    local s = take(p)
    if not s then return end
    s.u, s.v, s.y = u + rand(-0.5, 0.5) * size, v + rand(-0.5, 0.5) * size, (y or FLY_Y) - 0.3
    s.size, s.age, s.life = size * rand(0.9, 1.3), 0.0, rand(1.2, 1.8)
    s.drift = rand(0.5, 1.2)
    s.rise = rise
    if rise then s.life, s.drift = rand(4.5, 6.0), rand(0.8, 1.2) end
    s.stage, s.rot = 0, rand(0, 360)
    game.setMaterial(s.root, rise and mats.colSmoke1 or mats.smoke1)
end

function FX.debris(u, v, n, y)
    for _ = 1, n do
        local d = take(pools.debris)
        if not d then return end
        local a = rand(0, TAU)
        local sp = rand(2.0, 7.0)
        d.u, d.v, d.y = u, v, y or FLY_Y
        d.vu, d.vv, d.vy = cos(a) * sp, sin(a) * sp, rand(1.0, 5.0)
        d.spin, d.rx, d.ry = rand(200, 600), rand(0, 360), rand(0, 360)
        d.life = 2.5
    end
end

function FX.ring(u, v, size, y)
    local r = pools.rings and take(pools.rings)
    if not r then return end
    r.u, r.v, r.y, r.t, r.size, r.faint = u, v, (y or FLY_Y) - 0.3, 0.0, size, false
    game.setMaterial(r.root, mats.ring1)
end

-- A soft white puff: contrails in the air, wake and spray on the water. It
-- drifts down the screen with the world -- the ship flies on, the trail stays.
-- len/yaw stretch it along a line: puffs laid end to end make a streak.
function FX.puff(u, v, y, s0, s1, life, vu, vy, len, yaw)
    local p = pools.puffs
    if not p then return end
    local f = take(p)
    if not f then return end
    f.u, f.v, f.y, f.s0, f.s1, f.life = u, v, y, s0, s1, life
    f.vu, f.vy, f.age, f.stage, f.rot = vu or 0.0, vy or 0.0, 0.0, 0, rand(0, 360)
    f.len, f.yaw = len or 0.0, yaw
    game.setMaterial(f.root, mats.puff1)
end

-- Something hitting the water: a foam ring from above and spray thrown up.
function FX.splash(u, v, size)
    local p = pools.splashes
    if p then
        local f = take(p)
        if f then
            f.u, f.v, f.size, f.age, f.stage, f.rot = u, v, size, 0.0, 0, rand(0, 360)
            game.setMaterial(f.root, mats.splash1)
        end
    end
    for _ = 1, 3 + floor(size * 2) do
        FX.puff(u + rand(-0.5, 0.5) * size, v + rand(-0.5, 0.5) * size, 0.2, 0.3 * size, 1.2 * size,
                rand(0.5, 0.9), rand(-1.5, 1.5), rand(2.0, 5.0))
    end
    cue("splash", 0.08)
end

-- A flash at a gun: where a shot came from, the instant it came. One per spot:
-- a ring of twenty bullets is one flash.
function FX.muzzle(u, v, size)
    local p = pools.muzzles
    if not p then return end
    for _, m in ipairs(SYS.recentMuzzles) do
        if now - m[3] < 0.05 and abs(m[1] - u) < 0.7 and abs(m[2] - v) < 0.7 then return end
    end
    SYS.muzzleIdx = SYS.muzzleIdx % 8 + 1
    local r = SYS.recentMuzzles[SYS.muzzleIdx] or {}
    r[1], r[2], r[3] = u, v, now
    SYS.recentMuzzles[SYS.muzzleIdx] = r
    local f = take(p)
    if not f then return end
    f.u, f.v, f.age, f.size, f.rot = u, v, 0.0, (size or 1.0) * rand(0.55, 0.75), rand(0, 360)
end

-- A beat of near-stillness when something big goes: the hit lands harder.
function FX.hitstop(secs)
    if timeScale >= 1.0 then FX.slowmo(secs, 0.08) end
end

function FX.shake(amount)
    if screenShake then shake = min(1.6, shake + amount) end
end

function FX.flash(amount, r, g, b)
    flash = max(flash, amount)
    flashCol[1], flashCol[2], flashCol[3] = r or 1, g or 1, b or 1
end

function FX.slowmo(secs, scale)
    slowUntil, timeScale = now + secs, scale
end

-- size: 1 a fighter, 2 a gunship, 3 a cruiser, 4+ a boss.
function FX.explode(u, v, size, y, silent)
    for i = 1, 1 + floor(size * 1.5) do
        FX.fire(u, v, 0.7 + size * 0.45, (i - 1) * 0.04, y)
    end
    FX.sparks(u, v, 5 + floor(size * 4), size * 0.8, 5.0 + 2.0 * size, y)
    for _ = 1, floor(size) do FX.smoke(u, v, 1.0 + size * 0.4, y) end
    FX.debris(u, v, 2 + floor(size * 1.5), y)
    if size >= 2 then FX.ring(u, v, size * 0.9, y) end
    FX.shake(0.05 * size * size)
    if not silent then cue(size >= 4 and "boomL" or (size >= 2 and "boomM" or "boomS"), 0.05) end
end

function FX.popup(u, v, text, col, size)
    if #popups > 40 then table.remove(popups, 1) end
    popups[#popups + 1] = { u = u, v = v, text = text, t = 0.0, col = col or { 1, 1, 1 },
                            size = size or 20 }
end

local function stepEffects(dt)
    for _, s in ipairs(pools.sparks.items) do
        if s.active then
            s.life = s.life - dt
            if s.life <= 0.0 then
                s.active = false
            else
                s.u, s.v, s.y = s.u + s.vu * dt, s.v + s.vv * dt, s.y + s.vy * dt
                s.vy = s.vy - 9.0 * dt
                game.setPos(s.root, W(s.u, s.v, s.y))
                SYS.setScale(s.root, s.size * (s.life / s.max) + 0.02)
            end
        end
    end
    if pools.fires then
        for _, f in ipairs(pools.fires.items) do
            if f.active then
                if f.delay > 0.0 then
                    f.delay = f.delay - dt
                else
                    f.age = f.age + dt
                    local k = f.age / f.life
                    if k >= 1.0 then
                        f.active = false
                    else
                        -- Swell fast, then burn out through two fainter materials:
                        -- one material per object is shared, so a sprite fades by
                        -- stepping to a fainter one.
                        local stage = (k > 0.72) and 2 or ((k > 0.42) and 1 or 0)
                        if stage ~= f.stage then
                            f.stage = stage
                            game.setMaterial(f.root, stage == 2 and mats.fire3 or mats.fire2)
                        end
                        if f.hidden then
                            f.hidden = false
                            game.setMaterial(f.root, mats.fire1)
                        end
                        local s = f.size * (0.45 + 0.55 * (1.0 - (1.0 - k) ^ 3))
                        f.v = f.v - scrollSpeed * 0.15 * dt
                        game.setPos(f.root, W(f.u, f.v, f.y))
                        SYS.setScale(f.root, s, 0.02, s)
                        game.setRot(f.root, 0, f.rot + k * 40.0, 0)
                    end
                end
                f.blinkOff = f.delay > 0.0
            end
        end
        for _, s in ipairs(pools.smokes.items) do
            if s.active then
                s.age = s.age + dt
                local k = s.age / s.life
                if k >= 1.0 then
                    s.active = false
                else
                    local stage = (k > 0.7) and 2 or ((k > 0.35) and 1 or 0)
                    if stage ~= s.stage then
                        s.stage = stage
                        game.setMaterial(s.root, s.rise and (stage == 2 and mats.colSmoke3 or mats.colSmoke2)
                                                 or (stage == 2 and mats.smoke3 or mats.smoke2))
                    end
                    local sz = s.size * (0.6 + 0.8 * k)
                    if s.rise then
                        -- A plume: with the ground, and off down the wind.
                        local w = SYS.WIND
                        sz = s.size * (0.5 + 2.4 * k)
                        s.v = s.v - scrollSpeed * SYS.groundK() * dt + w[2] * s.drift * dt
                        s.u = s.u + w[1] * s.drift * dt
                        s.y = s.y + s.rise * (1.0 - 0.6 * k) * dt
                    else
                        s.v = s.v - scrollSpeed * 0.55 * s.drift * dt
                        s.y = s.y + 0.4 * dt
                    end
                    game.setPos(s.root, W(s.u, s.v, s.y))
                    SYS.setScale(s.root, sz, 0.02, sz)
                    game.setRot(s.root, 0, s.rot + k * 25.0, 0)
                end
            end
        end
    end
    for _, d in ipairs(pools.debris.items) do
        if d.active then
            d.life = d.life - dt
            d.u, d.v, d.y = d.u + d.vu * dt, d.v + d.vv * dt - scrollSpeed * 0.3 * dt, d.y + d.vy * dt
            d.vy = d.vy - 12.0 * dt
            d.vu, d.vv = d.vu * (1.0 - dt), d.vv * (1.0 - dt)
            if d.y < 0.0 or d.life <= 0.0 then
                d.active = false
            else
                d.rx, d.ry = d.rx + d.spin * dt, d.ry + d.spin * 0.7 * dt
                game.setPos(d.root, W(d.u, d.v, d.y))
                game.setRot(d.root, d.rx, d.ry, 0)
            end
        end
    end
    for _, r in ipairs(pools.rings and pools.rings.items or {}) do
        if r.active then
            r.t = r.t + dt
            local dur = 0.35 + 0.08 * r.size
            if r.t > dur then
                r.active = false
            else
                local k = r.t / dur
                if k > 0.55 and not r.faint then
                    r.faint = true
                    game.setMaterial(r.root, mats.ring2)
                end
                local s = 0.5 + r.size * 3.2 * (1.0 - (1.0 - k) ^ 2)
                game.setPos(r.root, W(r.u, r.v, r.y))
                SYS.setScale(r.root, s, 0.02, s)
            end
        end
    end
    local keep = {}
    for _, p in ipairs(popups) do
        p.t = p.t + dt
        if p.t < 1.0 then keep[#keep + 1] = p end
    end
    popups = keep
end

-- --- Enemy fire --------------------------------------------------------------------------

-- Later stages and loops fire faster bullets, and more often.
local function bspd(base) return base * G.speedK end
local function rate() return G.rateK end

local function aimAt(u, v)
    local tu, tv = player.u, player.v
    if not player.alive then tu, tv = 0.0, -9.0 end
    return atan(tv - v, tu - u)
end

local function fire(kind, u, v, ang, speed)
    if noFire then return nil end
    local b = take(pools[kind])
    if not b then return nil end
    FX.muzzle(u, v, kind == "big" and 1.3 or 0.9)
    b.u, b.v, b.ang, b.speed, b.t = u, v, ang, speed, 0.0
    b.turn, b.accel, b.minSpeed, b.aimAfter, b.grazed = nil, nil, nil, nil, false
    b.vu, b.vv = cos(ang) * speed, sin(ang) * speed
    if b.orient then game.setRot(b.root, 0, deg(yawOf(ang)), 0) end
    return b
end

local function fireSpread(kind, u, v, n, spreadDeg, speed, ang)
    ang = ang or aimAt(u, v)
    local step = (n > 1) and math.rad(spreadDeg) / (n - 1) or 0.0
    local a0 = ang - step * (n - 1) * 0.5
    for i = 0, n - 1 do fire(kind, u, v, a0 + step * i, speed) end
end

local function fireRing(kind, u, v, n, speed, offset)
    for i = 0, n - 1 do fire(kind, u, v, (offset or 0.0) + i * TAU / n, speed) end
end

-- Every bullet in the air becomes a gem (or just goes, if gems = false).
cancelBullets = function(gems)
    for _, kind in ipairs(BULLET_KINDS) do
        for _, b in ipairs(pools[kind].items) do
            if b.active then
                b.active = false
                if gems then
                    local g = take(pools.gems)
                    if g then
                        g.u, g.v, g.t = b.u, b.v, 0.0
                        g.vu, g.vv = rand(-2, 2), rand(-1, 3)
                    end
                else
                    FX.sparks(b.u, b.v, 1, 0.6, 2.0)
                end
            end
        end
    end
end

-- --- Enemies: paths -----------------------------------------------------------------------
-- A squadron flies a path: points on the flight plane, passed through by a
-- Catmull-Rom spline in `dur` seconds. Members follow one another along it.
local PATHS = {
    dive  = { dur = 5.5, { -3, 17 }, { -2, 9 }, { 1, 2 }, { -1, -5 }, { 2, -12 }, { 0, -21 } },
    hook  = { dur = 5.0, { -7, 17 }, { -7, 8 }, { -5, 3 }, { 0, 1.5 }, { 8, 3 }, { 16, 7 }, { 25, 9 } },
    loop  = { dur = 7.0, { -9, 17 }, { -9, 7 }, { -6, 2 }, { -2, 3 }, { -3, 7.5 }, { -7, 7 },
              { -9, 2 }, { -9, -6 }, { -10, -21 } },
    uturn = { dur = 6.0, { -5, 17 }, { -5, 6 }, { -4, 1 }, { -1, -1 }, { 2, 1 }, { 3, 6 },
              { 3, 17 }, { 3, 22 } },
    sweep = { dur = 6.0, { -25, 11 }, { -12, 7 }, { 0, 5 }, { 12, 7 }, { 25, 11 } },
    arc   = { dur = 6.0, { -25, 4 }, { -14, 7 }, { -4, 9 }, { 6, 7 }, { 14, 2 }, { 18, -6 },
              { 22, -17 } },
    zig   = { dur = 7.0, { 9, 17 }, { 8, 9 }, { -7, 5 }, { 6, 0 }, { -6, -5 }, { 0, -12 }, { 0, -21 } },
    cross = { dur = 5.0, { -21, 14 }, { -8, 6 }, { 4, -2 }, { 14, -10 }, { 25, -18 } },
    orbit = { dur = 7.5, { 0, 17 }, { 0, 11 }, { 4, 8 }, { 5, 4 }, { 2, 1 }, { -3, 1.5 }, { -5, 5 },
              { -3, 9 }, { 1, 10 }, { 5, 7 }, { 9, 1 }, { 13, -6 }, { 19, -14 } },
    fan   = { dur = 5.5, { 0, 7 }, { 3, 5 }, { 6, 1 }, { 5, -4 }, { 2, -10 }, { 0, -20 } },
}

local function catmull(pts, s)
    local n = #pts
    local f = clamp(s, 0.0, 1.0) * (n - 1)
    local i = floor(f)
    if i >= n - 1 then i = n - 2 end
    local t = f - i
    local p0, p1 = pts[max(1, i)], pts[i + 1]
    local p2, p3 = pts[i + 2], pts[min(n, i + 3)]
    local t2, t3 = t * t, t * t * t
    local function c(a, b, cc, d)
        return 0.5 * ((2 * b) + (-a + cc) * t + (2 * a - 5 * b + 4 * cc - d) * t2 +
                      (-a + 3 * b - 3 * cc + d) * t3)
    end
    return c(p0[1], p1[1], p2[1], p3[1]), c(p0[2], p1[2], p2[2], p3[2])
end

-- Move along the path; turn to face the way it goes and bank into the turn.
local function followPath(e, dt)
    local s = e.t / e.dur
    if s >= 1.0 then e.gone = true return end
    local u, v = catmull(e.path, s)
    if e.mirror then u = -u end
    u = u + (e.du or 0.0)
    local du, dv = u - e.u, v - e.v
    e.u, e.v = u, v
    if du * du + dv * dv > 1e-6 then
        local want = atan(-du, dv)
        local turn = wrapAngle(want - e.yaw)
        e.yaw = e.yaw + turn * min(1.0, dt * 12.0)
        e.bank = lerp(e.bank, clamp(turn / max(dt, 1e-3) * 0.22, -1.1, 1.1), min(1.0, dt * 6.0))
    end
end

local function onScreen(e)
    return e.v < 13.0 and e.v > -13.0 and abs(e.u) < 17.0
end

-- Where a part (mount) of the craft is on the flight plane.
local function mountPos(e, m)
    local mu, mv = local2plane(e.u, e.v, e.yaw, m.across, m.fwd)
    if e.def.ground then return toPlane(mu, mv, e.y + m.up) end
    return mu, mv
end

-- Turn a turret (mount) toward the player, as a world yaw.
local function aimMount(e, m)
    local mu, mv = mountPos(e, m)
    local a = aimAt(mu, mv)
    game.setRot(m.root, 0, deg(yawOf(a)), 0)
    return mu, mv, a
end

-- --- Enemies: behaviour -----------------------------------------------------------------

function KINDS.dart.update(e, dt)
    followPath(e, dt)
    if not e.fired and e.t / e.dur > e.fireAt and onScreen(e) then
        e.fired = true
        if math.random() < e.fireChance then fire("orb", e.u, e.v, aimAt(e.u, e.v), bspd(9.0)) end
    end
end

function KINDS.saucer.update(e, dt)
    followPath(e, dt)
    e.yaw, e.bank = e.t * 5.0, 0.0
    if not e.fired and e.t / e.dur > e.fireAt and onScreen(e) then
        e.fired = true
        if G.rank >= 1 or math.random() < 0.7 then
            fireSpread("orb", e.u, e.v, 3, 30.0, bspd(7.5))
        end
    end
end

function KINDS.hornet.update(e, dt)
    if e.state == "enter" then
        local k = min(1.0, e.t / 1.2)
        local ease = 1.0 - (1.0 - k) ^ 3
        e.u, e.v = lerp(e.u0, e.hu, ease), lerp(SPAWN_V, e.hv, ease)
        e.yaw = pi
        if k >= 1.0 then e.state, e.st = "lock", 0.0 end
    elseif e.state == "lock" then
        e.st = e.st + dt
        local a = aimAt(e.u, e.v)
        e.yaw = e.yaw + wrapAngle(yawOf(a) - e.yaw) * min(1.0, dt * 8.0)
        e.u = e.u + sin(e.st * 20.0) * 0.02
        local blink = floor(e.st * 10.0) % 2 == 0
        game.setMaterial(e.eye, blink and mats.eyeHot or mats.eye)
        if e.st > 0.9 then
            e.state, e.dir = "dash", a
            game.setMaterial(e.eye, mats.eyeHot)
        end
    else
        local sp = min(22.0, 8.0 + e.t * 6.0) * G.speedK
        e.u, e.v = e.u + cos(e.dir) * sp * dt, e.v + sin(e.dir) * sp * dt
        e.yaw = yawOf(e.dir)
        if e.v < -GONE_V or abs(e.u) > 24.0 or e.v > GONE_V + 2 then e.gone = true end
    end
    e.bank = sin(e.t * 30.0) * 0.12
end

function KINDS.gunship.update(e, dt)
    e.yaw = pi
    if e.t < 1.8 then
        local k = e.t / 1.8
        e.v = SPAWN_V + (7.5 - SPAWN_V) * (1.0 - (1.0 - k) ^ 2)
        e.u = e.u0
    elseif e.t < 11.0 then
        e.u = e.u0 + sin((e.t - 1.8) * 0.8) * 3.0
        e.bank = cos((e.t - 1.8) * 0.8) * 0.2
        e.fireCd = e.fireCd - dt
        local fireNow = e.fireCd <= 0.0
        if fireNow then e.fireCd = 1.5 / rate() end
        for i, m in ipairs(e.mounts) do
            local mu, mv, a = aimMount(e, m)
            if fireNow and onScreen(e) then
                if (e.volley + i) % 2 == 0 then
                    fireSpread("orb", mu + cos(a) * 0.8, mv + sin(a) * 0.8, G.rank >= 2 and 5 or 3,
                               36.0, bspd(8.0), a)
                else
                    for k = 0, 2 do
                        local b = fire("needle", mu, mv, a, bspd(9.0 + k * 1.6))
                        if b then b.u, b.v = mu + cos(a) * 0.8, mv + sin(a) * 0.8 end
                    end
                end
            end
        end
        if fireNow then e.volley = e.volley + 1 end
    else
        e.v = e.v + 6.0 * dt
        if e.v > GONE_V then e.gone = true end
    end
end

function KINDS.sentinel.update(e, dt)
    e.yaw = pi
    local bm = e.beamObj
    if e.state == "enter" then
        local k = min(1.0, e.t / 1.6)
        e.v = SPAWN_V + (e.hv - SPAWN_V) * (1.0 - (1.0 - k) ^ 3)
        if k >= 1.0 then e.state, e.st, e.shots = "aim", 0.0, 0 end
    elseif e.state == "aim" then
        e.st = e.st + dt
        e.u = e.u + clamp(player.u - e.u, -1.0, 1.0) * 2.5 * dt
        if e.st > 0.8 then
            e.state, e.st = "charge", 0.0
            cue("charge", 0.3)
        end
    elseif e.state == "charge" then
        e.st = e.st + dt
        bm.warn.active = true
        if e.st > 1.0 then
            e.state, e.st = "fire", 0.0
            bm.warn.active, bm.beam.active = false, true
            cue("laser", 0.3)
            FX.shake(0.15)
        end
    elseif e.state == "fire" then
        e.st = e.st + dt
        e.u = e.u + clamp(player.u - e.u, -1.0, 1.0) * 1.2 * dt
        if e.st > 1.6 then
            bm.beam.active = false
            e.shots = e.shots + 1
            e.state, e.st = (e.shots >= 2) and "leave" or "aim", 0.0
        end
    else
        e.v = e.v + 5.0 * dt
        if e.v > GONE_V then e.gone = true end
    end
    -- The beam runs from the emitter to the bottom of the screen.
    local top = e.v - 0.7
    local len = (top + 16.0) * 0.5
    if bm.warn.active then
        place(bm.warn, e.u, top - len, FLY_Y - 0.05)
        SYS.setScale(bm.warn.root, 0.05 + 0.03 * sin(now * 40.0), 0.05, len)
    end
    if bm.beam.active then
        place(bm.beam, e.u, top - len, FLY_Y)
        local w = 0.3 + 0.06 * sin(now * 50.0)
        SYS.setScale(bm.beam.root, w, 0.1, len)
        e.beamU, e.beamTop = e.u, top
        if math.random() < 0.3 then FX.sparks(e.u, top, 1, 0.8, 3.0) end
    else
        e.beamU = nil
    end
end

function KINDS.mine.update(e, dt)
    e.v = e.v - (2.2 + (e.drift or 0.0)) * dt
    e.u = e.u + sin(e.t * 1.3 + e.phase) * 0.6 * dt
    e.yaw = e.t * 0.8
    if not e.armed then
        local du, dv = player.u - e.u, player.v - e.v
        if e.t > 7.0 or (player.alive and du * du + dv * dv < 12.0 and e.t > 1.0) then
            e.armed, e.st = true, 0.0
            cue("mineArm", 0.15)
        end
        if floor(e.t * 2.0) % 2 == 0 then SYS.setScale(e.lamp, 0.14, 0.1, 0.14)
        else SYS.setScale(e.lamp, 0.08, 0.06, 0.08) end
    else
        e.st = e.st + dt
        -- Ten beeps an eighth of a second apart, then it goes: the lamp blinks
        -- with the recording (skystrike_mine_arm.wav).
        local on = (e.st % 0.125) < 0.06
        SYS.setScale(e.lamp, on and 0.2 or 0.08, 0.12, on and 0.2 or 0.08)
        if e.st > 1.28 then
            fireRing("orb", e.u, e.v, 12 + 2 * G.rank, bspd(6.0), rand(0, 1))
            FX.explode(e.u, e.v, 1, nil, true)
            cue("mineBoom", 0.1)
            e.gone = true
        end
    end
    if e.v < -GONE_V then e.gone = true end
end

function KINDS.missile.update(e, dt)
    if e.t < 2.6 and player.alive then
        local want = aimAt(e.u, e.v)
        e.dir = e.dir + clamp(wrapAngle(want - e.dir), -2.2 * dt, 2.2 * dt)
    end
    local sp = min(10.0, 5.0 + e.t * 2.0) * G.speedK
    e.u, e.v = e.u + cos(e.dir) * sp * dt, e.v + sin(e.dir) * sp * dt
    e.yaw = yawOf(e.dir)
    SYS.setScale(e.flame, 0.08, 0.08, 0.18 + 0.08 * sin(now * 70.0))
    if e.t > 7.0 or e.v < -GONE_V or abs(e.u) > 24.0 or e.v > GONE_V + 3 then e.gone = true end
end

-- Things on the sea drift down the screen with it.
local function sail(e, dt)
    e.v = e.v - scrollSpeed * SYS.groundK() * dt + (e.sv or 0.0) * dt
    e.u = e.u + (e.su or 0.0) * dt
    if e.v < -GONE_V - 4 then e.gone = true end
end

function KINDS.boat.update(e, dt)
    sail(e, dt)
    e.yaw = atan(-(e.su or 0.0), (e.sv or 0.0) + 1e-3)
    e.bank = sin(e.t * 1.7 + e.u) * 0.05
    local m = e.mounts[1]
    local mu, mv, a = aimMount(e, m)
    e.fireCd = e.fireCd - dt
    if e.fireCd <= 0.0 and onScreen(e) and e.v > player.v + 2.0 then
        e.fireCd = 1.8 / rate()
        fireSpread("orb", mu, mv, G.rank >= 1 and 2 or 1, 10.0, bspd(7.0), a)
    end
end

function KINDS.aa.update(e, dt)
    local isl = e.island
    if isl then
        e.u, e.v = isl.u + e.ou, isl.v + e.ov
        if e.v < -GONE_V - 4 then e.gone = true end
    else
        sail(e, dt)                -- a bunker on the mainland
    end
    local m = e.mounts[1]
    local mu, mv, a = aimMount(e, m)
    e.fireCd = e.fireCd - dt
    if e.fireCd <= 0.0 and onScreen(e) and e.v > player.v + 1.5 then
        e.fireCd = 2.2 / rate()
        e.burst = 3
    end
    if (e.burst or 0) > 0 then
        e.burstCd = (e.burstCd or 0.0) - dt
        if e.burstCd <= 0.0 then
            e.burstCd, e.burst = 0.12, e.burst - 1
            fire("needle", mu, mv, a, bspd(10.0))
        end
    end
end

function KINDS.cruiser.update(e, dt)
    -- Steams up against the scroll, so it lingers on screen.
    e.v = e.v - scrollSpeed * 0.45 * dt
    e.yaw = 0.0
    if e.v < -GONE_V - 6 then e.gone = true end
    e.fireCd = e.fireCd - dt
    local fireNow = e.fireCd <= 0.0 and onScreen(e)
    if fireNow then e.fireCd, e.volley = 1.7 / rate(), e.volley + 1 end
    for i, m in ipairs(e.mounts) do
        if m.alive then
            local mu, mv, a = aimMount(e, m)
            if fireNow and (e.volley + i) % 2 == 0 and mv > player.v + 1.0 then
                fireSpread("big", mu, mv, 3, 24.0, bspd(6.5), a)
                cue("cannon", 0.35)
            end
        end
    end
end

function KINDS.bomber.update(e, dt)
    e.yaw = pi
    if e.t < 4.0 then
        local k = e.t / 4.0
        e.v = 20.0 + (7.0 - 20.0) * (1.0 - (1.0 - k) ^ 2)
        e.u = 0.0
    elseif e.t < 32.0 then
        local bt = e.t - 4.0
        e.u = sin(bt * 0.32) * 6.0
        e.v = 7.0 + sin(bt * 0.55) * 1.0
        e.bank = cos(bt * 0.32) * 0.15
        e.fireCd = e.fireCd - dt
        e.ringCd = (e.ringCd or 2.0) - dt
        local alive = 0
        for _, m in ipairs(e.mounts) do if m.alive then alive = alive + 1 end end
        local fireNow = e.fireCd <= 0.0
        if fireNow then e.fireCd, e.volley = 1.0 / rate(), e.volley + 1 end
        for i, m in ipairs(e.mounts) do
            if m.alive then
                local mu, mv, a = aimMount(e, m)
                if fireNow and (e.volley + i) % 2 == 0 then
                    fireSpread("orb", mu, mv, 3, 20.0, bspd(8.5), a)
                end
            end
        end
        if e.ringCd <= 0.0 then
            e.ringCd = (alive == 0 and 1.4 or 3.2) / rate()
            local nu, nv = local2plane(e.u, e.v, e.yaw, 0.0, 3.6)
            fireRing("big", nu, nv, 14 + 2 * G.rank, bspd(5.5), rand(0, 1))
            cue("cannon", 0.4)
            if alive == 0 then fireRing("orb", nu, nv, 18, bspd(7.5), rand(0, 1)) end
        end
    else
        e.v = e.v + 4.0 * dt
        if e.v > GONE_V + 4 then e.gone = true end
    end
end

-- The flying fortress. Its core sits under two armour shutters; the cannon pods
-- on the wings have to go first (or it tires of waiting), then the shutters
-- slide open and the core fights for itself.
function KINDS.leviathan.update(e, dt)
    e.yaw = 0.0
    if e.t < 3.0 then
        local k = e.t / 3.0
        e.v = 20.0 + (7.2 - 20.0) * (1.0 - (1.0 - k) ^ 3)
        e.u = 0.0
        return
    end
    local bt = e.t - 3.0
    local rage = (not e.armored) and e.hp < e.maxHp * 0.35
    e.u = sin(bt * (rage and 0.6 or 0.4)) * 6.0
    e.v = 7.2 + sin(bt * 0.7) * 0.8
    local podL, podR = e.mount.podL, e.mount.podR
    if e.armored and ((not podL.alive and not podR.alive) or bt > 45.0) then
        e.armored, e.openT = false, 0.0
        FX.popup(e.u, e.v + 1.5, "CORE EXPOSED", { 1.0, 0.4, 0.3 }, 30)
        cue("warning", 1.0)
    end
    if not e.armored then e.openT = min(1.0, e.openT + dt) end
    local r = rate() * (rage and 1.5 or 1.0)
    e.fireCd = e.fireCd - dt
    e.fire2 = (e.fire2 or 0.0) - dt
    for _, key in ipairs({ "tl", "tr" }) do aimMount(e, e.mount[key]) end
    if e.armored then
        if e.fireCd <= 0.0 then
            e.fireCd, e.volley = 1.2 / r, e.volley + 1
            for _, pod in ipairs({ podL, podR }) do
                if pod.alive then
                    local pu, pv = e.u + pod.across, e.v - 2.1
                    fireSpread("needle", pu, pv, G.rank >= 1 and 7 or 5, 40.0, bspd(9.5))
                    cue("cannon", 0.4)
                end
            end
        end
        if e.fire2 <= 0.0 then
            e.fire2 = 0.75 / r
            for _, key in ipairs({ "tl", "tr" }) do
                local mu, mv, a = aimMount(e, e.mount[key])
                fire("orb", mu, mv, a, bspd(8.0))
                fire("orb", mu, mv, a + 0.12, bspd(8.0))
                fire("orb", mu, mv, a - 0.12, bspd(8.0))
            end
        end
    else
        local block = floor(bt / 6.0) % 3
        if rage then block = 3 end
        local cu, cv = e.u, e.v - 0.5
        if block == 0 then
            if e.fireCd <= 0.0 then
                e.fireCd = 0.07 / r
                e.spin = (e.spin or 0.0) + 0.23
                fire("orb", cu, cv, e.spin, bspd(7.0))
                fire("orb", cu, cv, e.spin + pi, bspd(7.0))
            end
            if e.fire2 <= 0.0 then
                e.fire2 = 2.0 / r
                fireRing("big", cu, cv, 16, bspd(5.0), rand(0, 1))
            end
        elseif block == 1 then
            if e.fireCd <= 0.0 then
                e.fireCd = 0.9 / r
                e.volley = e.volley + 1
                fireRing("big", cu, cv, 20 + 2 * G.rank, bspd(5.5), (e.volley % 2) * pi / 20)
                cue("cannon", 0.4)
            end
            if e.fire2 <= 0.0 then
                e.fire2 = 0.5 / r
                for _, key in ipairs({ "tl", "tr" }) do
                    local mu, mv, a = aimMount(e, e.mount[key])
                    fire("needle", mu, mv, a, bspd(11.0))
                end
            end
        elseif block == 2 then
            if e.fireCd <= 0.0 then
                e.fireCd = 0.12 / r
                local sweep = -pi * 0.5 + sin(bt * 1.4) * 0.9
                fireSpread("needle", cu, cv, 3, 16.0, bspd(9.0), sweep)
            end
        else
            if e.fireCd <= 0.0 then
                e.fireCd = 0.06 / r
                e.spin = (e.spin or 0.0) + 0.29
                for k = 0, 2 do fire("orb", cu, cv, e.spin + k * TAU / 3, bspd(7.5)) end
            end
            if e.fire2 <= 0.0 then
                e.fire2 = 0.7 / r
                fireSpread("needle", cu, cv, 5, 30.0, bspd(11.0))
            end
        end
    end
    e.bank = sin(bt * 0.4) * -0.06
end

function KINDS.drone.update(e, dt)
    followPath(e, dt)
    e.bank = 0.0
    if not e.fired and e.t / e.dur > e.fireAt and onScreen(e) then
        e.fired = true
        if math.random() < e.fireChance * 0.6 then fire("orb", e.u, e.v, aimAt(e.u, e.v), bspd(8.0)) end
    end
end

function KINDS.tank.update(e, dt)
    sail(e, dt)
    e.yaw = atan(-(e.su or 0.0), (e.sv or 0.0) + 1e-3)
    local mu, mv, a = aimMount(e, e.mounts[1])
    e.fireCd = e.fireCd - dt
    if e.fireCd <= 0.0 and onScreen(e) and e.v > player.v + 2.0 then
        e.fireCd = 2.4 / rate()
        fire("big", mu + cos(a) * 0.9, mv + sin(a) * 0.9, a, bspd(5.5))
        cue("cannon", 0.4)
    end
end

-- A beam (or its warning line) from (ou, ov) along ang, len long, w wide.
function SYS.placeBeam(obj, ou, ov, ang, len, w)
    place(obj, ou + cos(ang) * len * 0.5, ov + sin(ang) * len * 0.5, FLY_Y)
    game.setRot(obj.root, 0, deg(yawOf(ang)), 0)
    SYS.setScale(obj.root, w, 0.1, len * 0.5)
end

-- Is the ship inside that beam?
function SYS.segHit(ou, ov, ang, len, w)
    local p = player
    if not p.alive or p.invuln > 0.0 then return false end
    local dx, dy = cos(ang), sin(ang)
    local px, py = p.u - ou, p.v - ov
    local t = clamp(px * dx + py * dy, 0.0, len)
    local qx, qy = px - dx * t, py - dy * t
    local rr = w + PLAYER_R
    return qx * qx + qy * qy < rr * rr
end

function KINDS.tempest.update(e, dt)
    e.yaw = 0.0
    local bp = e.beamPairs
    if e.t < 3.5 then
        local k = e.t / 3.5
        e.v = 21.0 + (7.5 - 21.0) * (1.0 - (1.0 - k) ^ 3)
        e.u = 0.0
        return
    end
    local bt = e.t - 3.5
    local L, R = e.mount.pyL, e.mount.pyR
    if e.armored and ((not L.alive and not R.alive) or bt > 50.0) then
        e.armored, e.openT, e.cycT, e.rot = false, 0.0, 0.0, 0.0
        for _, b in ipairs(bp) do b.beam.active, b.warn.active = false, false end
        FX.popup(e.u, e.v + 1.5, "EYE OPEN", { 1.0, 0.4, 0.3 }, 30)
        cue("warning", 1.0)
    end
    if not e.armored then e.openT = min(1.0, e.openT + dt) end
    local rage = (not e.armored) and e.hp < e.maxHp * 0.35
    local r = rate() * (rage and 1.4 or 1.0)
    e.fireCd = e.fireCd - dt
    e.fire2 = (e.fire2 or 4.0) - dt
    for _, key in ipairs({ "tl", "tr" }) do aimMount(e, e.mount[key]) end
    if e.armored then
        e.u = sin(bt * 0.35) * 5.0
        e.v = 7.5 + sin(bt * 0.6) * 0.7
        -- Lightning from the pylons, one after the other: aimed where you are,
        -- a moment's warning line, then the bolt along it.
        e.zapT = (e.zapT or 0.0) + dt
        local cycle = 2.6 / r
        local n = floor(e.zapT / cycle)
        local tIn = e.zapT - n * cycle
        local idx = n % 2 + 1
        if e.zapN ~= n then e.zapN, e.zapAng = n, nil end
        bp[3 - idx].beam.active, bp[3 - idx].warn.active = false, false
        local py, pair = (idx == 1) and L or R, bp[idx]
        if py.alive then
            local pu, pv = mountPos(e, py)
            if not e.zapAng then
                e.zapAng = aimAt(pu, pv)
                cue("charge", 0.5)
            end
            if tIn < 0.9 then
                pair.warn.active, pair.beam.active = true, false
                SYS.placeBeam(pair.warn, pu, pv, e.zapAng, 34.0, 0.05 + 0.03 * sin(now * 40.0))
            elseif tIn < 1.9 then
                if not pair.beam.active then
                    cue("laser", 0.3)
                    FX.shake(0.12)
                end
                pair.warn.active, pair.beam.active = false, true
                SYS.placeBeam(pair.beam, pu, pv, e.zapAng, 34.0, 0.32 + 0.06 * sin(now * 50.0))
                if SYS.segHit(pu, pv, e.zapAng, 34.0, 0.32) then loseShip() end
            else
                pair.warn.active, pair.beam.active = false, false
            end
        else
            pair.warn.active, pair.beam.active = false, false
        end
        if e.fireCd <= 0.0 then
            e.fireCd = 0.9 / r
            for _, key in ipairs({ "tl", "tr" }) do
                local mu, mv, a = aimMount(e, e.mount[key])
                fireSpread("needle", mu, mv, 3, 16.0, bspd(10.0), a)
            end
        end
        if e.fire2 <= 0.0 then
            e.fire2 = 7.0 / r
            local path = PATHS.fan
            for k = 1, 4 do
                queue[#queue + 1] = { at = G.clock + k * 0.25, fn = function()
                    if e.active then
                        local mir = (k % 2 == 0)
                        launch("drone", { path = path, dur = path.dur, mirror = mir, du = e.u,
                                          u = e.u, v = e.v, yaw = pi, fireAt = 0.3, fireChance = 0.6 })
                    end
                end }
            end
        end
    else
        -- The eye: two beams turning round it, a warning sweep first, then the
        -- beams, then a breath of rings.
        e.u = e.u + (sin(bt * 0.2) * 3.0 - e.u) * min(1.0, dt)
        e.v = lerp(e.v, 7.0, min(1.0, dt))
        e.cycT = e.cycT + dt
        local warnD, fireD, restD = 1.4, rage and 6.0 or 5.0, 2.5
        local c = e.cycT % (warnD + fireD + restD)
        if c < warnD + fireD then e.rot = e.rot + (rage and 0.75 or 0.5) * dt end
        local cu, cv = e.u, e.v - 0.6
        for i = 1, 2 do
            local pair = bp[i]
            local ang = e.rot + (i - 1) * pi
            if c < warnD then
                pair.warn.active, pair.beam.active = true, false
                SYS.placeBeam(pair.warn, cu, cv, ang, 30.0, 0.06)
            elseif c < warnD + fireD then
                if not pair.beam.active then cue("laser", 0.5) end
                pair.warn.active, pair.beam.active = false, true
                SYS.placeBeam(pair.beam, cu, cv, ang, 30.0, 0.34 + 0.06 * sin(now * 50.0))
                if SYS.segHit(cu, cv, ang, 30.0, 0.34) then loseShip() end
            else
                pair.warn.active, pair.beam.active = false, false
            end
        end
        if c >= warnD + fireD then
            if e.fireCd <= 0.0 then
                e.fireCd, e.volley = 0.8 / r, e.volley + 1
                fireRing("big", cu, cv, 16 + 2 * G.rank, bspd(5.5), (e.volley % 2) * pi / 16)
                cue("cannon", 0.4)
            end
        elseif e.fireCd <= 0.0 then
            e.fireCd = 1.1 / r
            for _, key in ipairs({ "tl", "tr" }) do
                local mu, mv, a = aimMount(e, e.mount[key])
                fire("orb", mu, mv, a, bspd(8.0))
            end
        end
    end
    e.bank = sin(bt * 0.35) * -0.05
end

-- The eye's lids, placed after the hull has moved (as the leviathan's shutters).
function KINDS.tempest.post(e)
    local off = 0.45 + 1.1 * (e.armored and 0.0 or (e.openT or 0.0))
    game.setPos(e.mount.shutL.root, W(e.u - off, e.v - 0.6, FLY_Y + 0.9))
    game.setPos(e.mount.shutR.root, W(e.u + off, e.v - 0.6, FLY_Y + 0.9))
end

function KINDS.bastion.update(e, dt)
    e.yaw = 0.0
    if not e.arrived then
        e.v = e.v - scrollSpeed * SYS.groundK() * dt
        if e.v < 10.0 then G.scrollTarget = 0.0 end
        if SYS.groundK() < 0.01 then e.arrived = true end
    end
    local left = 0
    for _, m in ipairs(e.mounts) do if m.hp and m.alive then left = left + 1 end end
    if e.armored and (left == 0 or e.t > 75.0) then
        e.armored = false
        FX.popup(e.cu, e.cv + 2.0, "CORE EXPOSED", { 1.0, 0.4, 0.3 }, 30)
        cue("warning", 1.0)
    end
    if e.v > 13.0 then return end
    local rage = (not e.armored) and e.hp < e.maxHp * 0.35
    local r = rate() * (rage and 1.4 or 1.0)
    e.fireCd = e.fireCd - dt
    local towersFire = e.fireCd <= 0.0
    if towersFire then e.fireCd, e.volley = 1.7 / r, e.volley + 1 end
    for i, key in ipairs({ "t1", "t2", "t3", "t4" }) do
        local m = e.mount[key]
        if m.alive then
            local mu, mv, a = aimMount(e, m)
            if towersFire and (e.volley + i) % 2 == 0 then
                fireSpread("big", mu, mv, 3, 20.0, bspd(6.5), a)
                cue("cannon", 0.35)
            end
        end
    end
    -- The great cannon: it swings onto you, the warning line locks, then seven
    -- shells in a tight fan.
    local cm, warn = e.mount.cannon, e.beamPairs[1].warn
    local cu, cv = mountPos(e, cm)
    e.canT = (e.canT or 0.0) + dt
    local cycle = 4.5 / r
    if e.canT > cycle then e.canT, e.canAng = 0.0, nil end
    if e.canT < cycle - 1.0 then
        warn.active = false
        local _, _, a = aimMount(e, cm)
        e.canLast = a
    else
        if not e.canAng then
            e.canAng = e.canLast or aimAt(cu, cv)
            cue("charge", 0.5)
        end
        game.setRot(cm.root, 0, deg(yawOf(e.canAng)), 0)
        local mu, mv = cu + cos(e.canAng) * 2.8, cv + sin(e.canAng) * 2.8
        if e.canT < cycle - 0.15 then
            warn.active = true
            SYS.placeBeam(warn, mu, mv, e.canAng, 30.0, 0.05 + 0.03 * sin(now * 40.0))
        elseif warn.active then
            warn.active = false
            fireSpread("big", mu, mv, 7, 14.0, bspd(11.0), e.canAng)
            cue("cannon", 0.1)
            FX.shake(0.25)
            FX.muzzle(mu, mv, 2.2)
        end
    end
    if not e.armored then
        e.fire3 = (e.fire3 or 0.0) - dt
        if e.fire3 <= 0.0 then
            local bu, bv = toPlane(e.u, e.v, e.y + 2.0)
            if rage then
                e.fire3 = 0.08 / r
                e.spin = (e.spin or 0.0) + 0.31
                fire("orb", bu, bv, e.spin, bspd(7.0))
                fire("orb", bu, bv, e.spin + pi, bspd(7.0))
            else
                e.fire3, e.volley = 1.2 / r, e.volley + 1
                fireRing("orb", bu, bv, 20 + 2 * G.rank, bspd(6.0), (e.volley % 2) * pi / 20)
            end
        end
    end
end

-- The shutters are placed after the hull has moved this frame (a child's
-- position is written in world space against its parent's current place):
-- closed while armoured, sliding apart over a second once the core is out.
function KINDS.leviathan.post(e)
    for i, id in ipairs(e.exhaust) do
        SYS.setScale(id, 0.22, 0.22, 0.6 + 0.2 * sin(now * 40.0 + i))
    end
    local off = 0.42 + 1.1 * (e.armored and 0.0 or (e.openT or 0.0))
    game.setPos(e.mount.shutL.root, W(e.u - off, e.v - 0.5, FLY_Y + 0.95))
    game.setPos(e.mount.shutR.root, W(e.u + off, e.v - 0.5, FLY_Y + 0.95))
end

-- The battleship. Four gun turrets and two missile launchers stand between you
-- and the bridge; with them gone the bridge opens up on its own.
function KINDS.dreadnought.update(e, dt)
    e.yaw = 0.0
    if e.t < 6.0 then
        local k = e.t / 6.0
        e.v = 34.0 + (5.0 - 34.0) * (1.0 - (1.0 - k) ^ 2)
        e.u = 0.0
        return
    end
    local bt = e.t - 6.0
    e.smokeCd = (e.smokeCd or 0.0) - dt
    if e.smokeCd <= 0.0 then
        e.smokeCd = 0.25
        for _, f in ipairs({ 2.9, 3.9 }) do
            local su, sv = toPlane(e.u, e.v + f, e.y + 2.4)
            FX.smoke(su, sv, 1.2, e.y + 2.4)
        end
    end
    e.u = sin(bt * 0.25) * 3.5
    e.v = 5.0 + sin(bt * 0.4) * 1.2
    local left = 0
    for _, m in ipairs(e.mounts) do if m.alive then left = left + 1 end end
    if e.armored and (left == 0 or bt > 60.0) then
        e.armored = false
        FX.popup(e.u, e.v, "BRIDGE EXPOSED", { 1.0, 0.4, 0.3 }, 30)
        cue("warning", 1.0)
    end
    local rage = (not e.armored) and e.hp < e.maxHp * 0.35
    local r = rate() * (rage and 1.5 or 1.0)
    e.fireCd = e.fireCd - dt
    e.fire2 = (e.fire2 or 1.0) - dt
    local fireNow = e.fireCd <= 0.0
    if fireNow then e.fireCd, e.volley = 1.4 / r, e.volley + 1 end
    for i, m in ipairs(e.mounts) do
        if m.alive then
            if m.key:sub(1, 1) == "t" then
                local mu, mv, a = aimMount(e, m)
                if fireNow and (e.volley + i) % 2 == 0 then
                    fireSpread("big", mu, mv, 3, 22.0, bspd(6.5), a)
                    fire("needle", mu, mv, a, bspd(10.0))
                    cue("cannon", 0.35)
                end
            elseif e.fire2 <= 0.0 then
                local mu, mv = mountPos(e, m)
                launch("missile", { u = mu, v = mv, dir = (m.across < 0) and pi * 0.75 or pi * 0.25 })
                cue("launch", 0.15)
            end
        end
    end
    if e.fire2 <= 0.0 then e.fire2 = 3.2 / r end
    if not e.armored then
        e.fire3 = (e.fire3 or 0.0) - dt
        local bu, bv = toPlane(e.u, e.v + 1.2, e.y + 2.0)
        if e.fire3 <= 0.0 then
            if rage then
                e.fire3 = 0.08 / r
                e.spin = (e.spin or 0.0) + 0.27
                fire("orb", bu, bv, e.spin, bspd(7.0))
                fire("orb", bu, bv, -e.spin, bspd(7.0))
            else
                e.fire3 = 1.1 / r
                e.volley = e.volley + 1
                fireRing("orb", bu, bv, 22 + 2 * G.rank, bspd(6.0), (e.volley % 2) * pi / 22)
            end
        end
    end
end

-- --- Enemies: lifecycle -----------------------------------------------------------------

local function mountHp(def, key)
    return def.parts and def.parts[key] or nil
end

launch = function(kind, init)
    local e = take(enemyPools[kind])
    if not e then return nil end
    local def = KINDS[kind]
    local hpK = 1.0 + 0.2 * G.rank
    e.hp = floor(def.hp * hpK + 0.5)
    e.maxHp = e.hp
    e.t, e.fired, e.fireCd, e.flash, e.volley = 0.0, false, 0.0, 0.0, 0
    e.r, e.gone, e.yaw, e.bank = def.r, false, pi, 0.0
    e.y = def.ground and 0.4 or FLY_Y
    e.armored = def.boss or false
    e.state, e.st, e.armed, e.beamU = "enter", 0.0, false, nil
    e.u, e.v = 0.0, SPAWN_V
    e.cu, e.cv = 0.0, SPAWN_V
    for _, m in ipairs(e.mounts) do
        local hp = mountHp(def, m.key)
        m.hp = hp and floor(hp * hpK + 0.5) or nil
        m.maxHp = m.hp
        m.alive, m.flash = true, 0.0
        SYS.skin(m, nil)
    end
    SYS.skin(e, nil)
    e.trailCd, e.dmgSmoke, e.pu, e.pv = 0.0, 0.0, nil, nil
    -- Per-kind counters a pooled boss must not carry into its next life.
    e.arrived, e.canT, e.canAng, e.zapT, e.zapN, e.zapAng = false, 0.0, nil, 0.0, nil, nil
    e.fire2, e.fire3, e.spin, e.openT, e.cycT, e.rot, e.smokeCd = nil, nil, nil, 0.0, 0.0, 0.0, nil
    for k, v in pairs(init) do e[k] = v end
    enemies[#enemies + 1] = e
    if not def.boss and not def.midboss and kind ~= "missile" then G.spawned = G.spawned + 1 end
    return e
end

local function dropOrb(u, v, kind)
    local o = take(pools.orbs)
    if not o then return end
    o.u, o.v, o.t, o.kind = u, v, 0.0, kind
    game.setMaterial(o.coreId, mats["pw" .. kind])
end

-- A destroyed part: its own explosion and score, the part hidden.
local function destroyMount(e, m)
    m.alive = false
    m.hp = 0
    SYS.skin(m, nil)
    local mu, mv = mountPos(e, m)
    FX.explode(mu, mv, 2, e.y)
    FX.hitstop(0.05)
    addScore(800, mu, mv)
end

killEnemy = function(e, scored)
    e.active = false
    e.dead = true
    if e.beamObj then e.beamObj.beam.active, e.beamObj.warn.active = false, false end
    for _, b in ipairs(e.beamPairs or {}) do b.beam.active, b.warn.active = false, false end
    if not scored then return end
    local def = e.def
    local u, v = e.cu, e.cv
    G.killed = G.killed + ((def.boss or def.midboss or e.kind == "missile") and 0 or 1)
    addScore(def.score, u, v, true)
    if SYS.land and def.ground then
        for k = 1, def.boss and 3 or 1 do
            SYS.blazeAt(e.u + (k - 1) * rand(-3, 3), e.v + (k - 1) * rand(-2, 2),
                        def.boss and rand(1.8, 2.4) or rand(0.8, 1.1))
        end
    end
    if def.boss then
        for i = 1, 12 do
            queue[#queue + 1] = { at = G.clock + i * 0.13, fn = function()
                FX.explode(u + rand(-4, 4), v + rand(-2, 2), 3, e.y)
            end }
        end
        FX.explode(u, v, 5, e.y)
        FX.flash(0.45)
        queue[#queue + 1] = { at = G.clock + 1.1, fn = function() cue("debris") end }
        FX.slowmo(0.8, 0.3)
        FX.shake(1.2)
        cancelBullets(true)
        dropOrb(u, v, "1")
        G.bossAlive = false
        G.clearAt = now + 4.5
        G.bossBonus = 20000 * G.stageNo
        G.scrollTarget = 1.0
    elseif def.midboss then
        FX.explode(u, v, 4, e.y)
        queue[#queue + 1] = { at = G.clock + 0.7, fn = function() cue("debris") end }
        FX.flash(0.5)
        cancelBullets(true)
        dropOrb(u - 1.5, v, "P")
        dropOrb(u + 1.5, v, "B")
    else
        FX.explode(u, v, def.boom, e.y)
        if e.kind == "gunship" or e.kind == "cruiser" or e.kind == "sentinel" then
            dropOrb(u, v, G.power < MAX_POWER and "P" or "B")
        elseif e.kind == "mine" then
            fireRing("orb", u, v, 8, bspd(4.0), rand(0, 1))
        elseif e.kind ~= "missile" and math.random() < 0.06 then
            dropOrb(u, v, math.random() < 0.75 and "P" or "B")
        end
    end
    -- What was shot down falls: into the sea with a splash, onto the land in
    -- flames, or it sinks. Bosses in the air come down slowly, burning.
    if e.kind ~= "missile" and e.kind ~= "mine" then SYS.startWreck(e, def.boss and not def.ground) end
    if def.midboss then FX.hitstop(0.12)
    elseif not def.boss and def.boom >= 2 then FX.hitstop(0.05) end
end

-- A hit flickers the craft (or the part) warm for a moment -- and not again
-- for a little while, or a boss under a stream of fire glows solid.
local function flashHit(holder)
    if (holder.flashCd or 0.0) <= 0.0 then holder.flash, holder.flashCd = 0.045, 0.13 end
end

-- A hit on a zone: a part (mount) or the hull. Armoured hulls only spark.
local function hurt(e, m, dmg)
    if m then
        m.hp = m.hp - dmg
        flashHit(m)
        cue("clank", 0.11)
        if m.hp <= 0 then destroyMount(e, m) end
    elseif e.armored then
        cue("clank", 0.13)
    else
        e.hp = e.hp - dmg
        flashHit(e)
        cue("hit", 0.07)
        if e.hp <= 0 then killEnemy(e, true) end
    end
end

-- The zone a shot at (u, v) hits, if any: parts first, then the hull.
local function hitZone(e, u, v, extra)
    for _, m in ipairs(e.mounts) do
        if m.alive and m.hp then
            local mu, mv = mountPos(e, m)
            local du, dv = u - mu, v - mv
            local rr = (e.def.partR or 0.9) + extra
            if du * du + dv * dv < rr * rr then return true, m end
        end
    end
    -- An armoured ship down on the sea is not in the way: shots pass over it to
    -- the turrets behind. An armoured hull in the air takes them (and rings).
    if e.armored and e.def.ground then return false, nil end
    local du, dv = u - e.cu, v - e.cv
    local rr = e.r + extra
    if du * du + dv * dv < rr * rr then return true, nil end
    return false, nil
end

local function stepEnemies(dt)
    local keep = {}
    for _, e in ipairs(enemies) do
        if e.active then
            e.t = e.t + dt
            e.pu, e.pv, e.pdt = e.u, e.v, dt
            if SYS.land and e.def.ground then e.y = SYS.gy(e.u, e.v) + 0.4 end
            e.def.update(e, dt)
            if e.gone then
                killEnemy(e, false)
            else
                -- Ground things are seen (and shot) where they appear.
                if e.def.ground then e.cu, e.cv = toPlane(e.u, e.v, e.y + 0.5)
                else e.cu, e.cv = e.u, e.v end
                e.flash, e.flashCd = max(0.0, e.flash - dt), (e.flashCd or 0.0) - dt
                setFlash(e, e.flash > 0.0)
                for _, m in ipairs(e.mounts) do
                    if m.hp and m.alive then
                        m.flash, m.flashCd = max(0.0, m.flash - dt), (m.flashCd or 0.0) - dt
                        setFlash(m, m.flash > 0.0)
                    end
                end
                place(e, e.u, e.v, e.y)
                game.setRot(e.root, attitude(e.yaw, e.bank))
                if e.def.post then e.def.post(e) end
                SYS.enemyFx(e, dt)
                -- Ramming: costs the ship, and the enemy a good chunk.
                if not e.def.ground and player.alive and player.invuln <= 0.0 then
                    local du, dv = e.u - player.u, e.v - player.v
                    local rr = e.r * 0.75 + PLAYER_R
                    if du * du + dv * dv < rr * rr then
                        loseShip()
                        hurt(e, nil, 8)
                    end
                end
                -- The beam: a strip down the screen below the drone.
                if e.beamU and player.alive and player.invuln <= 0.0 and player.v < e.beamTop and
                   abs(player.u - e.beamU) < 0.32 + PLAYER_R then
                    loseShip()
                end
                if e.def.boss then
                    local pulse = e.armored and 2.0 or (5.0 + 3.0 * sin(now * 7.0))
                    game.setMaterialProps(mats.core, { emissionStrength = pulse })
                end
            end
            if e.active then keep[#keep + 1] = e end
        end
    end
    enemies = keep
end

-- --- Scoring ------------------------------------------------------------------------------

local C_TEXT, C_DIM = { 0.92, 0.95, 1.0 }, { 0.55, 0.62, 0.72 }
local C_CYAN, C_GOLD = { 0.35, 0.85, 1.0 }, { 1.0, 0.8, 0.3 }
local C_RED, C_MAG, C_GREEN = { 1.0, 0.3, 0.24 }, { 1.0, 0.42, 0.78 }, { 0.45, 1.0, 0.55 }
local CHAIN_WINDOW = 1.6

-- Kills in quick succession build the chain; it multiplies what they score
-- (up to x3) and breaks when nothing dies for CHAIN_WINDOW seconds.
addScore = function(base, u, v, kill)
    if mode ~= "play" then return end
    local mult = 1.0
    if kill then
        G.chain = (G.clock - G.chainAt < CHAIN_WINDOW) and G.chain + 1 or 1
        G.chainAt = G.clock
        G.maxChain = max(G.maxChain, G.chain)
        mult = 1.0 + min(G.chain, 60) / 30.0
    end
    local pts = floor(base * G.loopK * mult / 10.0 + 0.5) * 10
    G.score = G.score + pts
    if u and pts >= 100 then
        local big = pts >= 5000
        FX.popup(u, v, fmt(pts), big and C_GOLD or C_TEXT, big and 30 or 19)
    end
end

-- --- Game flow -------------------------------------------------------------------------------

local function after(delay, fn) queue[#queue + 1] = { at = G.clock + delay, fn = fn } end

local function setRank()
    G.rank = min(8, (G.stageNo - 1) + (G.loop - 1))
    local d = max(0.3, difficulty)
    G.speedK = (1.0 + 0.07 * G.rank) * d
    G.rateK = (1.0 + 0.12 * G.rank) * d
    G.loopK = 1.0 + 0.5 * (G.loop - 1)
end

local function resetGame()
    G = { score = 0, lives = max(1, floor(ships)), bombs = max(0, floor(bombs)), power = 1,
          stageIdx = 1, stageNo = 1, loop = 1, stageT = 0.0, waveIdx = 1, bossAlive = false,
          clearAt = -1.0, nextAt = -1.0, hold = nil, boss = nil, chain = 0, chainAt = -10.0,
          maxChain = 0, graze = 0, extendNo = 1, nextExtend = EXTENDS[1], missed = false,
          spawned = 0, killed = 0, bombT = 0.0, bombTick = 0.0, bombU = 0.0, bombV = 0.0,
          overAt = 0.0, newHi = false, bossBonus = 0, clock = G.clock or 0.0, tally = nil }
    setRank()
end

-- The high score survives the session in a one-line file.
local HI_FILE = (os.getenv("APPDATA") or os.getenv("HOME") or ".") .. "/fitzel_skystrike.txt"
local function loadHi()
    if not io or not io.open then return end
    local ok, f = pcall(io.open, HI_FILE, "r")
    if ok and f then
        hiScore = tonumber(f:read("l") or "") or 0
        f:close()
    end
end
local function saveHi()
    if not io or not io.open then return end
    local ok, f = pcall(io.open, HI_FILE, "w")
    if ok and f then
        f:write(tostring(floor(hiScore)), "\n")
        f:close()
    end
end

local function respawnPlayer()
    local p = player
    p.alive, p.u, p.v, p.roll = true, 0.0, -14.5, 0.0
    p.respawn, p.invuln = 0.9, 3.2
    p.obj.active = true
    for _, op in ipairs(p.options) do op.u, op.v = p.u, p.v end
end

local function gameOver()
    META.bank()
    mode = "over"
    G.overAt = now
    noFire = true
    if G.score > hiScore then
        hiScore, G.newHi = G.score, true
        saveHi()
    end
    cancelBullets(false)
end

loseShip = function()
    if META.shieldHit() then return end
    local p = player
    FX.explode(p.u, p.v, 3)
    FX.flash(0.45, 1.0, 0.35, 0.25)
    FX.slowmo(0.5, 0.35)
    FX.shake(0.7)
    cue("die", 0.3)
    p.alive = false
    p.obj.active = false
    G.lives = G.lives - 1
    G.power = max(META.basePower(), G.power - 1)
    G.bombs = max(G.bombs, 2)
    G.missed, G.chain = true, 0
    if G.lives <= 0 then
        SYS.enterContinue()
        return
    end
    after(1.4, function() if mode == "play" then respawnPlayer() end end)
end

local function clearField()
    for _, e in ipairs(enemies) do killEnemy(e, false) end
    enemies = {}
    for _, key in ipairs({ "shots", "optShots", "pMissiles", "orb", "big", "needle", "orbs", "gems",
                           "lasers" }) do
        for _, it in ipairs(pools[key].items) do it.active = false end
    end
    for _, e in ipairs(SYS.wrecks) do e.busy = false end
    SYS.wrecks = {}
    for _, r in ipairs(pools.rain.items) do r.active = false end
    for _, b in ipairs(beams) do b.beam.active, b.warn.active = false, false end
    queue = {}
end

local function startGame()
    clearField()
    resetGame()
    G.weapon = SYS.weapon
    META.loadout()
    SYS.planGround(STAGES[1])
    mode, paused, noFire = "play", false, false
    respawnPlayer()
    banner = { kind = "stage", t0 = now, dur = 3.2 }
    cue("start")
end

-- --- A run across two scenes -------------------------------------------------------------
-- With nextScene set, the last stage of this scene hands the run on to that
-- one -- the sea to the high valley and back, a lap. What carries over goes
-- into a small file beside the high score, and the other scene's start() picks
-- it up (if it is fresh: an old one is a run that ended in between). The valley
-- closes a lap, so the sea after it is the next loop.
SYS.RUN_FILE = HI_FILE:gsub("%.txt$", "") .. "_run.txt"
SYS.CARRY = { "score", "lives", "bombs", "power", "loop", "stageNo", "extendNo", "nextExtend",
              "maxChain", "graze", "weapon" }

function SYS.handOff(extra)
    if not (io and io.open and game.loadScene) then return false end
    G.weapon = SYS.weapon
    if G.score > hiScore then hiScore = G.score end
    saveHi()
    local ok, f = pcall(io.open, SYS.RUN_FILE, "w")
    if not (ok and f) then return false end
    f:write("time=", tostring(os.time()), "\n")
    if not extra then
        for _, k in ipairs(SYS.CARRY) do
            local v = G[k]
            if k == "loop" and SYS.land then v = v + 1 end
            f:write(k, "=", tostring(v), "\n")
        end
    end
    -- fresh: a new run starts there; single + stage: one mission; back: return
    -- here when it is over; menu: open that menu (1 title, 2 missions).
    for k, v in pairs(extra or {}) do f:write(k, "=", tostring(v), "\n") end
    f:close()
    game.loadScene(nextScene)
    G.nextAt = 1e9        -- nothing more happens here: the scene is on its way out
    return true
end

-- In start(): a run handed on from the other scene carries on here -- or a
-- mission picked over there starts here, or its board opens again.
function SYS.takeOver()
    if not (io and io.open) then return end
    local ok, f = pcall(io.open, SYS.RUN_FILE, "r")
    if not (ok and f) then return end
    local got = {}
    for line in f:lines() do
        local k, v = line:match("^(%w+)=(.*)$")
        if k then got[k] = tonumber(v) end
    end
    f:close()
    os.remove(SYS.RUN_FILE)
    if not got.time or os.time() - got.time > 60 then return end
    SYS.weapon = got.weapon or SYS.weapon
    if got.menu then
        META.toMenu(got.menu == 2 and "missions" or "title")
        return
    end
    startGame()
    if got.fresh then
        if got.single == 1 then META.single(got.stage or 1, got.back == 1) end
        return
    end
    for _, k in ipairs(SYS.CARRY) do
        if got[k] and k ~= "stageNo" then G[k] = got[k] end
    end
    G.banked, G.stageStartScore = G.score, G.score
    G.stageNo = (got.stageNo or 0) + 1
    setRank()
end

-- --- Waves & stages ------------------------------------------------------------------------

-- Formations: { across offset, delay } per member. Without one the squadron
-- flies nose to tail along the path.
WAVE.FORMS = {
    v    = { { 0, 0 }, { -1.6, 0.16 }, { 1.6, 0.16 }, { -3.2, 0.32 }, { 3.2, 0.32 },
             { -4.8, 0.48 }, { 4.8, 0.48 } },
    line = { { 0, 0 }, { -3, 0 }, { 3, 0 }, { -6, 0 }, { 6, 0 }, { -9, 0 }, { 9, 0 } },
    pair = { { -1.2, 0 }, { 1.2, 0 }, { -1.2, 0.7 }, { 1.2, 0.7 }, { -1.2, 1.4 }, { 1.2, 1.4 },
             { -1.2, 2.1 }, { 1.2, 2.1 } },
}

function WAVE.squad(w)
    local path = PATHS[w.path]
    local form = w.form and WAVE.FORMS[w.form]
    for i = 0, (w.n or 5) - 1 do
        local off, delay = 0.0, i * (w.gap or 0.34)
        if form then
            local f = form[i + 1] or form[#form]
            off, delay = f[1], f[2]
        end
        after(delay, function()
            local du = (w.du or 0.0) + off
            local u, v = catmull(path, 0.0)
            local u2, v2 = catmull(path, 0.02)
            if w.mirror then u, u2 = -u, -u2 end
            u, u2 = u + du, u2 + du
            launch(w.kind, { path = path, dur = path.dur, mirror = w.mirror, du = du, u = u, v = v,
                             yaw = atan(-(u2 - u), v2 - v), fireAt = rand(0.15, 0.5),
                             fireChance = min(0.95, 0.35 + 0.15 * G.rank) })
        end)
    end
end

function WAVE.hornets(w)
    for i = 1, w.n do
        after((i - 1) * 0.45, function()
            local hu = rand(-11, 11)
            launch("hornet", { u0 = hu + rand(-3, 3), hu = hu, hv = rand(5.0, 9.5), u = hu })
        end)
    end
end

function WAVE.gunship(w) launch("gunship", { u0 = w.u, u = w.u }) end
function WAVE.sentinel(w) launch("sentinel", { u = w.u, hv = w.v or 9.0 }) end
function WAVE.cruiser(w) launch("cruiser", { u = w.u, v = 32.0 }) end

function WAVE.mines(w)
    for i = 1, w.n do
        after((i - 1) * 0.55, function()
            launch("mine", { u = rand(-12, 12), phase = rand(0, TAU), drift = rand(0, 1.2) })
        end)
    end
end

-- Gunboats steam up the screen against the scroll, so they stay a while.
function WAVE.boats(w)
    for i = 1, w.n do
        after((i - 1) * 0.8, function()
            launch("boat", { u = w.u + rand(-4, 4), v = 25.0 + rand(0, 3),
                             su = rand(-0.5, 0.5), sv = rand(1.5, 3.0) })
        end)
    end
end

-- The island furthest from view comes round at the top, with guns on it.
function WAVE.island(w)
    local pick = nil
    for _, it in ipairs(world.islands) do
        if (it.v > 30.0 or it.v < -28.0) and (not pick or it.v > pick.v) then pick = it end
    end
    if not pick then return end
    pick.u, pick.v = w.u, 33.0
    for k = 1, w.turrets do
        local a = (k / w.turrets) * TAU + 0.6
        local ou, ov = cos(a) * 2.0, sin(a) * 1.6
        launch("aa", { island = pick, ou = ou, ov = ov, u = pick.u + ou, v = pick.v + ov, yaw = 0.0,
                       onLand = true })
    end
end

-- Tanks: a convoy up the road, or a platoon across the fields.
function WAVE.tanks(w)
    for i = 1, w.n do
        after((i - 1) * (w.road and 1.1 or 0.7), function()
            local u = w.road and 0.0 or (w.u + rand(-4, 4))
            launch("tank", { u = u, v = 25.0, su = w.road and 0.0 or rand(-0.4, 0.4),
                             sv = w.road and 2.2 or rand(1.0, 2.0) })
        end)
    end
end

-- Bunkers dug into the fields, clear of the road.
function WAVE.bunkers(w)
    for i = 1, w.n do
        local side = (i % 2 == 0) and 1 or -1
        launch("aa", { u = side * rand(3.5, 12.0), v = 25.0 + i * 2.5, island = false, onLand = true,
                       yaw = 0.0 })
    end
end

function WAVE.bomber()
    G.hold = launch("bomber", { u = 0.0, v = 20.0 })
    banner = { kind = "alert", t0 = now, dur = 2.2, text = "HEAVY BOMBER APPROACHING" }
    cue("warning", 1.0)
end

function WAVE.boss()
    G.bossAlive = true
    banner = { kind = "warning", t0 = now, dur = 3.4 }
    cue("warning")
    after(2.8, function()
        local u, v = 0.0, 20.0
        -- Over a landscape the fortress stands where the scene says: on the
        -- Empty named "Shmup Arena", a clearing, so no forest grows through it.
        -- Still ahead of the ships it waits there, further up the screen.
        local arena = SYS.land and game.find and game.find("Shmup Arena")
        if arena then
            local x, _, z = game.getPos(arena)
            local av = (OZ - z) / RS
            if av > 12.0 then u, v = clamp((x - OX) / RS, -8.0, 8.0), av end
        end
        G.boss = launch(STAGES[G.stageIdx].boss, { u = u, v = v })
    end)
end

STAGES = {
    { name = "CORAL SEA", sub = "The outer islands", boss = "leviathan",
      ground = { { "sea", 999 } }, events = {
        { 1.5, "squad", kind = "dart", path = "hook", n = 5, form = "v" },
        { 4.5, "squad", kind = "dart", path = "hook", n = 5, mirror = true },
        { 8.0, "squad", kind = "saucer", path = "arc", n = 5 },
        { 10.5, "squad", kind = "saucer", path = "arc", n = 5, mirror = true },
        { 13.0, "gunship", u = -6 },
        { 14.0, "squad", kind = "dart", path = "dive", n = 5, form = "line", du = 1 },
        { 17.0, "boats", n = 3, u = 7 },
        { 19.0, "hornets", n = 3 },
        { 21.5, "island", u = -8, turrets = 2 },
        { 23.0, "squad", kind = "dart", path = "loop", n = 6 },
        { 25.5, "squad", kind = "dart", path = "loop", n = 6, mirror = true },
        { 28.0, "gunship", u = 7 },
        { 29.5, "gunship", u = -7 },
        { 32.0, "mines", n = 5 },
        { 34.5, "squad", kind = "saucer", path = "sweep", n = 7 },
        { 37.0, "sentinel", u = -7 },
        { 40.0, "squad", kind = "dart", path = "zig", n = 6 },
        { 41.0, "hornets", n = 3 },
        { 44.0, "island", u = 8, turrets = 3 },
        { 47.0, "bomber" },
        { 49.0, "squad", kind = "dart", path = "cross", n = 6 },
        { 50.5, "squad", kind = "dart", path = "cross", n = 6, mirror = true },
        { 53.0, "gunship", u = 0 },
        { 55.0, "boats", n = 4, u = -7 },
        { 57.0, "mines", n = 6 },
        { 58.5, "squad", kind = "saucer", path = "uturn", n = 6 },
        { 61.0, "hornets", n = 5 },
        { 64.0, "sentinel", u = 7 },
        { 64.5, "sentinel", u = -7 },
        { 68.0, "squad", kind = "dart", path = "dive", n = 8, du = -4 },
        { 69.0, "squad", kind = "dart", path = "dive", n = 8, du = 4, mirror = true },
        { 74.0, "boss" },
    } },
    { name = "IRON FLEET", sub = "The northern convoy", boss = "dreadnought",
      ground = { { "sea", 999 } }, events = {
        { 1.5, "boats", n = 4, u = -6 },
        { 3.0, "squad", kind = "dart", path = "hook", n = 6 },
        { 6.0, "boats", n = 4, u = 7 },
        { 7.0, "squad", kind = "dart", path = "hook", n = 6, mirror = true },
        { 10.0, "cruiser", u = -5 },
        { 12.0, "squad", kind = "saucer", path = "sweep", n = 7 },
        { 15.0, "hornets", n = 4 },
        { 18.0, "sentinel", u = 0 },
        { 20.0, "mines", n = 6 },
        { 22.0, "squad", kind = "dart", path = "zig", n = 6, form = "pair" },
        { 23.5, "squad", kind = "drone", path = "orbit", n = 8, gap = 0.22 },
        { 26.0, "cruiser", u = 6 },
        { 28.0, "gunship", u = -7 },
        { 29.0, "gunship", u = 7 },
        { 32.0, "squad", kind = "saucer", path = "uturn", n = 6, mirror = true },
        { 34.0, "boats", n = 5, u = 0 },
        { 36.0, "island", u = -9, turrets = 3 },
        { 38.0, "hornets", n = 5 },
        { 41.0, "bomber" },
        { 43.0, "squad", kind = "dart", path = "loop", n = 7 },
        { 45.0, "squad", kind = "dart", path = "loop", n = 7, mirror = true },
        { 48.0, "sentinel", u = -8 },
        { 48.5, "sentinel", u = 8 },
        { 52.0, "cruiser", u = 0 },
        { 54.0, "mines", n = 7 },
        { 57.0, "squad", kind = "saucer", path = "arc", n = 7 },
        { 58.5, "squad", kind = "saucer", path = "arc", n = 7, mirror = true },
        { 62.0, "gunship", u = -6 },
        { 63.0, "gunship", u = 6 },
        { 65.0, "hornets", n = 6 },
        { 69.0, "squad", kind = "dart", path = "cross", n = 8 },
        { 70.0, "squad", kind = "dart", path = "cross", n = 8, mirror = true },
        { 75.0, "boss" },
    } },
    { name = "STORM FRONT", sub = "Into the weather", boss = "tempest", ground = { { "storm", 999 } },
      islands = false, clouds = "storm", weather = "storm", events = {
        { 1.5, "squad", kind = "dart", path = "hook", n = 5, form = "v" },
        { 4.0, "squad", kind = "drone", path = "orbit", n = 8, gap = 0.22 },
        { 7.0, "boats", n = 3, u = -6 },
        { 8.5, "sentinel", u = 6 },
        { 11.0, "squad", kind = "dart", path = "dive", n = 7, form = "line" },
        { 13.0, "hornets", n = 4 },
        { 16.0, "squad", kind = "drone", path = "orbit", n = 8, gap = 0.22, mirror = true },
        { 18.0, "gunship", u = -6 },
        { 19.0, "gunship", u = 6 },
        { 22.0, "mines", n = 6 },
        { 24.0, "squad", kind = "saucer", path = "sweep", n = 7 },
        { 26.0, "sentinel", u = -8 },
        { 26.5, "sentinel", u = 8 },
        { 30.0, "squad", kind = "dart", path = "loop", n = 6, form = "pair" },
        { 33.0, "cruiser", u = 5 },
        { 36.0, "hornets", n = 5 },
        { 39.0, "bomber" },
        { 41.0, "squad", kind = "drone", path = "orbit", n = 8, gap = 0.22 },
        { 42.0, "squad", kind = "drone", path = "orbit", n = 8, gap = 0.22, mirror = true },
        { 45.0, "squad", kind = "dart", path = "zig", n = 6, form = "pair" },
        { 48.0, "sentinel", u = 0 },
        { 50.0, "gunship", u = -7 },
        { 51.0, "gunship", u = 7 },
        { 54.0, "mines", n = 7 },
        { 56.0, "squad", kind = "saucer", path = "arc", n = 7 },
        { 57.5, "squad", kind = "saucer", path = "arc", n = 7, mirror = true },
        { 60.0, "hornets", n = 6 },
        { 63.0, "squad", kind = "dart", path = "cross", n = 8, form = "pair" },
        { 64.0, "squad", kind = "dart", path = "cross", n = 8, form = "pair", mirror = true },
        { 69.0, "boss" },
    } },
    { name = "IRON COAST", sub = "The mainland", boss = "bastion", ground = { { "sea", 3 }, { "land", 999 } },
      islands = false, events = {
        { 1.5, "boats", n = 4, u = 6 },
        { 3.0, "squad", kind = "dart", path = "hook", n = 5, form = "v" },
        { 6.0, "boats", n = 4, u = -6 },
        { 8.0, "squad", kind = "saucer", path = "arc", n = 6 },
        { 11.0, "gunship", u = 0 },
        { 15.0, "squad", kind = "drone", path = "orbit", n = 8, gap = 0.22 },
        { 18.0, "squad", kind = "dart", path = "dive", n = 7, form = "line" },
        { 22.0, "tanks", n = 4, u = 0, road = true },
        { 24.0, "bunkers", n = 3 },
        { 27.0, "hornets", n = 4 },
        { 29.0, "tanks", n = 3, u = -8 },
        { 31.0, "sentinel", u = 7 },
        { 33.0, "bunkers", n = 4 },
        { 35.0, "squad", kind = "dart", path = "loop", n = 6, form = "pair" },
        { 38.0, "tanks", n = 4, u = 0, road = true },
        { 41.0, "bomber" },
        { 43.0, "squad", kind = "drone", path = "orbit", n = 8, gap = 0.22 },
        { 44.0, "squad", kind = "drone", path = "orbit", n = 8, gap = 0.22, mirror = true },
        { 47.0, "gunship", u = -6 },
        { 48.0, "gunship", u = 6 },
        { 51.0, "tanks", n = 5, u = 0, road = true },
        { 52.0, "bunkers", n = 4 },
        { 55.0, "mines", n = 6 },
        { 57.0, "squad", kind = "saucer", path = "uturn", n = 6 },
        { 60.0, "hornets", n = 6 },
        { 63.0, "squad", kind = "dart", path = "cross", n = 8, form = "pair" },
        { 64.0, "squad", kind = "dart", path = "cross", n = 8, form = "pair", mirror = true },
        { 70.0, "boss" },
    } },
}

-- Over a landscape (a scene with its own Terrain) this is the whole run: the
-- valley, flown from the carrier up the screen. No sea, so no boats, cruisers
-- or island batteries -- tanks in the meadows and flak dug in along the way.
SYS.LAND_STAGES = {
    { name = "HIGH VALLEY", sub = "Up the mountain river", boss = "bastion", islands = false,
      events = {
        { 1.5, "squad", kind = "dart", path = "hook", n = 5, form = "v" },
        { 4.0, "tanks", n = 3, u = 6 },
        { 5.5, "squad", kind = "saucer", path = "arc", n = 6 },
        { 8.0, "bunkers", n = 3 },
        { 10.5, "gunship", u = 0 },
        { 13.0, "tanks", n = 4, u = -7 },
        { 15.0, "squad", kind = "drone", path = "orbit", n = 8, gap = 0.22 },
        { 18.0, "squad", kind = "dart", path = "dive", n = 7, form = "line" },
        { 20.0, "bunkers", n = 4 },
        { 22.5, "hornets", n = 4 },
        { 25.0, "tanks", n = 4, u = 5 },
        { 27.0, "sentinel", u = -7 },
        { 30.0, "squad", kind = "dart", path = "loop", n = 6, form = "pair" },
        { 32.0, "bunkers", n = 3 },
        { 35.0, "bomber" },
        { 38.0, "tanks", n = 5, u = -4 },
        { 39.0, "squad", kind = "drone", path = "orbit", n = 8, gap = 0.22, mirror = true },
        { 42.0, "gunship", u = -6 },
        { 43.0, "gunship", u = 6 },
        { 46.0, "bunkers", n = 4 },
        { 48.0, "mines", n = 6 },
        { 50.0, "squad", kind = "saucer", path = "uturn", n = 6 },
        { 53.0, "hornets", n = 6 },
        { 55.0, "tanks", n = 4, u = 7 },
        { 57.0, "sentinel", u = 7 },
        { 57.5, "sentinel", u = -7 },
        { 61.0, "squad", kind = "dart", path = "cross", n = 8, form = "pair" },
        { 62.0, "squad", kind = "dart", path = "cross", n = 8, form = "pair", mirror = true },
        { 67.0, "boss" },
    } },
}

local function stageClear()
    local ratio = (G.spawned > 0) and G.killed / G.spawned or 1.0
    local t = { boss = G.bossBonus, kills = floor(ratio * 100 + 0.5),
                killBonus = floor(ratio * 200) * 100 * G.stageNo,
                noMiss = G.missed and 0 or 30000 * G.stageNo }
    t.total = t.boss + t.killBonus + t.noMiss
    G.score = G.score + t.total
    G.tally = t
    META.onClear()
    banner = { kind = "clear", t0 = now, dur = 5.2 }
    cue("clear")
end

local function nextStage()
    if G.single then META.missionDone() return end
    if nextScene ~= "" and G.stageIdx == #STAGES and SYS.handOff() then return end
    G.stageIdx = G.stageIdx % #STAGES + 1
    if G.stageIdx == 1 then G.loop = G.loop + 1 end
    G.stageNo = G.stageNo + 1
    setRank()
    G.stageT, G.waveIdx, G.missed, G.spawned, G.killed = 0.0, 1, false, 0, 0
    G.boss, G.hold = nil, nil
    SYS.planGround(STAGES[G.stageIdx])
    banner = { kind = "stage", t0 = now, dur = 3.2 }
end

local function stepStage(dt)
    local st = STAGES[G.stageIdx]
    if G.hold and not G.hold.active then G.hold = nil end
    if not G.hold and not G.bossAlive and G.clearAt < 0 and G.nextAt < 0 then
        G.stageT = G.stageT + dt
        while st.events[G.waveIdx] and G.stageT >= st.events[G.waveIdx][1] do
            local w = st.events[G.waveIdx]
            WAVE[w[2]](w)
            G.waveIdx = G.waveIdx + 1
        end
    end
    if G.clearAt > 0.0 and now >= G.clearAt then
        G.clearAt = -1.0
        stageClear()
        G.nextAt = now + 5.4
    end
    if G.nextAt > 0.0 and now >= G.nextAt then
        G.nextAt = -1.0
        nextStage()
    end
end

-- --- Input -----------------------------------------------------------------------------------

local KEYS = {}
local function anyDown(list)
    for _, k in ipairs(list) do if game.keyDown(k) then return true end end
    return false
end
local function pressed(name, down)
    local was = keysPrev[name]
    keysPrev[name] = down
    return down and not was
end

-- --- Player ---------------------------------------------------------------------------------

-- Gun layouts per power level: { across, angle in degrees }.
local GUNS = {
    { { -0.22, 0 }, { 0.22, 0 } },
    { { -0.22, 0 }, { 0.22, 0 }, { -0.5, -6 }, { 0.5, 6 } },
    { { -0.22, 0 }, { 0.22, 0 }, { -0.5, -7 }, { 0.5, 7 } },
    { { -0.22, 0 }, { 0.22, 0 }, { -0.5, -7 }, { 0.5, 7 } },
    { { -0.22, 0 }, { 0.22, 0 }, { -0.5, -5 }, { 0.5, 5 }, { -0.7, -12 }, { 0.7, 12 } },
}
local WEAPON_NAMES = { "TWIN VULCAN", "SPREAD", "OPTIONS", "MISSILES", "MAXIMUM" }

local function shootShot(pool, u, v, angDeg, dmg)
    local s = take(pool)
    if not s then return end
    local a = math.rad(angDeg)
    s.u, s.v, s.dmg = u, v, dmg
    s.vu, s.vv = sin(a) * SHOT_SPEED, cos(a) * SHOT_SPEED
    game.setRot(s.root, 0, -angDeg, 0)
end

local function shoot()
    local p = player
    if G.weapon == 2 then SYS.shootLaser() return end
    for _, g in ipairs(GUNS[G.power]) do
        local ang = p.focus and g[2] * 0.2 or g[2]
        local off = p.focus and g[1] * 0.6 or g[1]
        shootShot(pools.shots, p.u + off, p.v + 1.1, ang, 1.0)
    end
    if G.power >= 3 then
        for _, op in ipairs(p.options) do
            shootShot(pools.optShots, op.u, op.v + 0.4, p.focus and 0 or op.side * 5, 0.7)
        end
    end
    cue("shot", 0.09)
end

local function nearestTarget(u, v)
    local best, bd = nil, 1e9
    for _, e in ipairs(enemies) do
        if e.active and e.cv < 13.5 and e.cv > -12.0 and abs(e.cu) < 16.0 then
            local du, dv = e.cu - u, e.cv - v
            local d = du * du + dv * dv
            if dv > -1.0 then d = d * 0.5 end      -- prefer what is ahead
            if d < bd then best, bd = e, d end
        end
    end
    return best
end

local function launchMissiles()
    for side = -1, 1, 2 do
        local m = take(pools.pMissiles)
        if m then
            m.u, m.v = player.u + side * 0.7, player.v
            m.ang, m.speed, m.t, m.trail = pi * 0.5 - side * 0.55, 8.0, 0.0, 0.0
            m.target = nearestTarget(m.u, m.v)
        end
    end
    cue("missile", 0.25)
end

local function stepPlayer(dt)
    local p = player
    if not p.alive then
        p.hitbox.active, p.shield.active = false, false
        return
    end
    local l = anyDown(KEYS.left) and 1 or 0
    local r = anyDown(KEYS.right) and 1 or 0
    local u = anyDown(KEYS.up) and 1 or 0
    local d = anyDown(KEYS.down) and 1 or 0
    local mu, mv = r - l, u - d
    if mu ~= 0 and mv ~= 0 then mu, mv = mu * 0.7071, mv * 0.7071 end
    p.focus = anyDown(KEYS.focus)
    local speed = (p.focus and 5.5 or 12.5) * META.speedK()
    if p.respawn > 0.0 then
        p.respawn = p.respawn - dt
        p.v = p.v + 7.0 * dt
    else
        p.u = clamp(p.u + mu * speed * dt, -LIMIT_U, LIMIT_U)
        p.v = clamp(p.v + mv * speed * dt, LIMIT_V0, LIMIT_V1)
    end
    p.roll = p.roll + (-mu * 0.5 - p.roll) * min(1.0, dt * 10.0)
    p.climb = mv

    p.fireCd, p.missileCd = p.fireCd - dt, p.missileCd - dt
    p.firing = autoFire or anyDown(KEYS.fire)
    if p.firing and p.respawn <= 0.0 then
        if p.fireCd <= 0.0 then
            p.fireCd = ((G.power >= 5) and 0.065 or 0.08) * META.rateK()
            shoot()
        end
        local mcd = META.missileCd()
        if (G.power >= 4 or mcd) and p.missileCd <= 0.0 then
            p.missileCd = min((G.power >= 5) and 0.4 or ((G.power >= 4) and 0.6 or 9.0), mcd or 9.0)
            launchMissiles()
        end
    end
    p.invuln = max(0.0, p.invuln - dt)
    META.stepShield(dt)
    p.obj.blinkOff = p.invuln > 0.0 and p.invuln < 1.0 and (floor(now * 16.0) % 2 == 0)
    p.hitbox.active = p.focus
    p.shield.active = p.invuln > 1.0
end

-- The title screen flies the ship by itself: it drifts, dodges nothing and
-- shoots whatever comes by.
local function stepDemo(dt)
    local p = player
    p.obj.active, p.obj.blinkOff = true, false
    p.hitbox.active, p.shield.active = false, false
    local tgt = nearestTarget(p.u, p.v)
    local want = tgt and clamp(tgt.cu, -10, 10) or sin(now * 0.4) * 6.0
    local mu = clamp((want - p.u) * 0.5, -1.0, 1.0)
    p.u = p.u + mu * 7.0 * dt
    p.v = -8.0 + sin(now * 0.6) * 1.5
    p.roll = p.roll + (-mu * 0.5 - p.roll) * min(1.0, dt * 6.0)
    p.fireCd = p.fireCd - dt
    if tgt and p.fireCd <= 0.0 then
        p.fireCd = 0.1
        for _, g in ipairs(GUNS[3]) do shootShot(pools.shots, p.u + g[1], p.v + 1.1, g[2], 1.0) end
        for _, op in ipairs(p.options) do shootShot(pools.optShots, op.u, op.v + 0.4, op.side * 5, 0.7) end
    end
end

local function stepOptions(dt)
    local on = player.obj.active and (G.power >= 3 or mode == "title" or mode == "missions" or
                                      false)
    for _, op in ipairs(player.options) do
        op.active = on
        if on then
            local tu = player.u + op.side * (player.focus and 0.9 or 1.8)
            local tv = player.v - (player.focus and 0.2 or 0.7)
            local k = min(1.0, dt * (player.focus and 14.0 or 8.0))
            op.u, op.v = lerp(op.u, tu, k), lerp(op.v, tv, k)
            place(op, op.u, op.v)
            game.setRot(op.root, 0, now * 400.0 * op.side, 20.0)
        end
    end
end

local function useBomb()
    local p = player
    if G.bombs <= 0 or not p.alive or p.respawn > 0.0 then return end
    G.bombs = G.bombs - 1
    G.bombT, G.bombTick, G.bombU, G.bombV = 1.3, 0.0, p.u, p.v
    cancelBullets(true)
    FX.ring(p.u, p.v, 6.0)
    FX.ring(p.u, p.v, 3.5)
    FX.flash(0.3, 0.85, 0.92, 1.0)
    FX.shake(0.6)
    p.invuln = max(p.invuln, 2.6)
    cue("bomb")
end

-- While the bomb burns: bullets vanish, fire rolls outward, everything on
-- screen takes a beating in four hits.
local function stepBomb(dt)
    if G.bombT <= 0.0 then return end
    G.bombT = G.bombT - dt
    G.bombTick = G.bombTick - dt
    if G.bombTick <= 0.0 then
        G.bombTick = 0.3
        local r = 3.0 + (1.3 - G.bombT) * 12.0
        for k = 1, 5 do
            local a = rand(0, TAU)
            FX.fire(G.bombU + cos(a) * r * rand(0.6, 1.0), G.bombV + sin(a) * r * rand(0.6, 1.0), 1.7,
                    k * 0.03)
        end
        for _, e in ipairs(enemies) do
            if e.active and onScreen(e) then
                local zone = nil
                if e.armored then
                    for _, m in ipairs(e.mounts) do if m.alive and m.hp then zone = m break end end
                end
                if zone or not e.armored then
                    hurt(e, zone, (e.def.boss or e.def.midboss) and 9 or 12)
                end
            end
        end
    end
    for _, kind in ipairs(BULLET_KINDS) do
        for _, b in ipairs(pools[kind].items) do if b.active then b.active = false end end
    end
end

local function checkExtend()
    if G.score >= G.nextExtend then
        G.extendNo = G.extendNo + 1
        G.nextExtend = EXTENDS[G.extendNo] or (G.nextExtend + EXTEND_EVERY)
        G.lives = G.lives + 1
        FX.popup(player.u, player.v + 2.0, "EXTEND!", C_MAG, 34)
        cue("extend")
    end
end

-- --- Shots, bullets, items ----------------------------------------------------------------------

local BULLET_R = { orb = 0.17, big = 0.3, needle = 0.12 }

local function shotHits(u, v, extra, dmg)
    for _, e in ipairs(enemies) do
        if e.active and e.cv < SPAWN_V then
            local hit, zone = hitZone(e, u, v, extra)
            if hit then
                hurt(e, zone, dmg)
                return true
            end
        end
    end
    return false
end

local function stepShots(dt)
    for _, pool in ipairs({ pools.shots, pools.optShots }) do
        for _, s in ipairs(pool.items) do
            if s.active then
                s.u, s.v = s.u + s.vu * dt, s.v + s.vv * dt
                if s.v > GONE_V or abs(s.u) > 24.0 then
                    s.active = false
                elseif shotHits(s.u, s.v, 0.2, s.dmg) then
                    s.active = false
                    FX.sparks(s.u, s.v, 1, 0.5, 3.0)
                else
                    place(s, s.u, s.v)
                end
            end
        end
    end
    for _, m in ipairs(pools.pMissiles.items) do
        if m.active then
            m.t = m.t + dt
            m.speed = min(30.0, m.speed + 45.0 * dt)
            local tgt = m.target
            if not tgt or not tgt.active then
                tgt = (m.t > 0.1) and nearestTarget(m.u, m.v) or nil
                m.target = tgt
            end
            if tgt and m.t > 0.08 then
                local want = atan(tgt.cv - m.v, tgt.cu - m.u)
                m.ang = m.ang + clamp(wrapAngle(want - m.ang), -7.0 * dt, 7.0 * dt)
            end
            m.u, m.v = m.u + cos(m.ang) * m.speed * dt, m.v + sin(m.ang) * m.speed * dt
            m.trail = m.trail - dt
            if m.trail <= 0.0 then
                m.trail = 0.035
                FX.puff(m.u - cos(m.ang) * 0.4, m.v - sin(m.ang) * 0.4, FLY_Y, 0.14, 0.45, 0.4)
            end
            if m.v > GONE_V or m.v < -16.0 or abs(m.u) > 24.0 or m.t > 3.0 then
                m.active = false
            elseif shotHits(m.u, m.v, 0.3, 3.0) then
                m.active = false
                FX.fire(m.u, m.v, 0.9)
                FX.sparks(m.u, m.v, 3, 0.7, 4.0)
            else
                place(m, m.u, m.v)
                game.setRot(m.root, 0, deg(yawOf(m.ang)), 0)
            end
        end
    end
end

local function stepBullets(dt)
    local p = player
    local vuln = p.alive and p.invuln <= 0.0 and mode == "play"
    for _, kind in ipairs(BULLET_KINDS) do
        local br = BULLET_R[kind] + PLAYER_R
        local gr = br + GRAZE_R
        for _, b in ipairs(pools[kind].items) do
            if b.active then
                b.t = b.t + dt
                if b.turn or b.accel then
                    if b.turn then b.ang = b.ang + b.turn * dt end
                    if b.accel then b.speed = max(b.minSpeed or 1.0, b.speed + b.accel * dt) end
                    b.vu, b.vv = cos(b.ang) * b.speed, sin(b.ang) * b.speed
                    if b.orient then game.setRot(b.root, 0, deg(yawOf(b.ang)), 0) end
                end
                b.u, b.v = b.u + b.vu * dt, b.v + b.vv * dt
                if b.v < -15.0 or b.v > GONE_V or abs(b.u) > 24.0 then
                    b.active = false
                elseif p.alive then
                    local du, dv = b.u - p.u, b.v - p.v
                    local d2 = du * du + dv * dv
                    if d2 < br * br and vuln then
                        b.active = false
                        loseShip()
                        vuln = false
                    elseif not b.grazed and d2 < gr * gr and vuln then
                        b.grazed = true
                        G.graze = G.graze + 1
                        G.score = G.score + 10 * G.stageNo
                        FX.sparks(p.u + du * 0.4, p.v + dv * 0.4, 1, 0.35, 2.0)
                    end
                end
                if b.active then place(b, b.u, b.v) end
            end
        end
    end
end

local function collect(o)
    cue("power", 0.1)
    local p = player
    if o.kind == "P" then
        if G.power < MAX_POWER then
            G.power = G.power + 1
            FX.popup(p.u, p.v + 1.5, SYS.weaponName(), C_GOLD, 26)
        else
            addScore(5000, p.u, p.v + 1.5)
        end
    elseif o.kind == "B" then
        if G.bombs < META.maxBombs() then
            G.bombs = G.bombs + 1
            FX.popup(p.u, p.v + 1.5, "BOMB +1", C_GREEN, 26)
        else
            addScore(5000, p.u, p.v + 1.5)
        end
    else
        G.lives = G.lives + 1
        FX.popup(p.u, p.v + 1.5, "1UP", C_MAG, 30)
        cue("extend")
    end
end

local function stepItems(dt)
    local p = player
    for _, o in ipairs(pools.orbs.items) do
        if o.active then
            o.t = o.t + dt
            o.v = o.v - 2.0 * dt
            o.u = clamp(o.u + sin(o.t * 2.0) * 1.5 * dt, -LIMIT_U, LIMIT_U)
            place(o, o.u, o.v)
            game.setRot(o.root, 0, o.t * 180.0, 20.0)
            if o.v < -14.0 then o.active = false end
            if p.alive and mode == "play" then
                local du, dv = o.u - p.u, o.v - p.v
                if du * du + dv * dv < 1.5 * 1.5 then
                    o.active = false
                    collect(o)
                end
            end
        end
    end
    for _, g in ipairs(pools.gems.items) do
        if g.active then
            g.t = g.t + dt
            if p.alive and g.t > 0.35 then
                local du, dv = p.u - g.u, p.v - g.v
                local d = sqrt(du * du + dv * dv) + 1e-4
                local sp = min(45.0, 10.0 + g.t * 30.0)
                g.u, g.v = g.u + du / d * sp * dt, g.v + dv / d * sp * dt
                if d < 1.0 then
                    g.active = false
                    if mode == "play" then G.score = G.score + 50 * G.stageNo end
                    cue("item", 0.035)
                end
            else
                g.u, g.v = g.u + g.vu * dt, g.v + g.vv * dt - 1.5 * dt
                g.vu, g.vv = g.vu * (1.0 - 2.0 * dt), g.vv * (1.0 - 2.0 * dt)
            end
            if g.t > 6.0 or g.v < -15.0 then g.active = false end
            if g.active then
                place(g, g.u, g.v)
                game.setRot(g.root, 45, now * 300.0, 0)
            end
        end
    end
end

-- --- Wrecks, trails, laser, continue ------------------------------------------------------

-- How fast the ground slides by right now (a fortress boss holds it still).
function SYS.groundK() return (G.scrollK or 1.0) * (SYS.land and SYS.LAND.pace or 1.0) end

-- Is the ground under (u, v) land? (Set up with the ground rows, below.)
function SYS.overLand(u, v) return SYS.landAt and SYS.landAt(u, v) or false end

function SYS.startWreck(e, big)
    e.busy = true
    local dt = max(e.pdt or 0.016, 1e-3)
    e.wu = clamp((e.u - (e.pu or e.u)) / dt, -10.0, 10.0)
    e.wv = clamp((e.v - (e.pv or e.v)) / dt, -12.0, 12.0)
    e.wvy = big and 0.0 or 1.5
    e.wspin = rand(-1, 1) * (big and 0.3 or 3.0)
    e.wroll = rand(-1, 1) * (big and 0.25 or 6.0)
    e.wsmoke, e.wt, e.wbig = 0.0, 0.0, big
    e.wmode = e.def.ground and ((e.def.onLand or e.onLand) and "burn" or "sink") or "fall"
    SYS.skin(e, "char")
    for _, m in ipairs(e.mounts) do SYS.skin(m, "char") end
    SYS.wrecks[#SYS.wrecks + 1] = e
end

function SYS.stepWrecks(dt)
    if dt <= 0.0 then return end
    local keep = {}
    local gk = scrollSpeed * SYS.groundK()
    for _, e in ipairs(SYS.wrecks) do
        e.wt = e.wt + dt
        local done = false
        e.wsmoke = e.wsmoke - dt
        if e.wmode == "fall" then
            e.wvy = e.wvy - (e.wbig and 3.0 or 14.0) * dt
            e.y = e.y + e.wvy * dt
            e.u = e.u + e.wu * dt
            e.wv = lerp(e.wv, -gk, min(1.0, dt * 1.5))
            e.v = e.v + e.wv * dt
            e.yaw, e.bank = e.yaw + e.wspin * dt, e.bank + e.wroll * dt
            if e.wsmoke <= 0.0 then
                e.wsmoke = e.wbig and 0.05 or 0.07
                FX.smoke(e.u, e.v, e.wbig and 2.2 or 0.8, e.y + 0.4)
                if e.wbig or math.random() < 0.35 then
                    FX.fire(e.u + rand(-1, 1) * (e.wbig and 3.0 or 0.3), e.v, e.wbig and 2.0 or 0.6, 0.0, e.y)
                end
            end
            if e.wbig and math.random() < dt * 4.0 then
                FX.explode(e.u + rand(-4, 4), e.v + rand(-2, 2), 2, e.y, true)
                cue("boomM", 0.25)
            end
            local gy = SYS.gy(e.u, e.v)
            if e.y <= gy + 0.3 then
                if SYS.overLand(e.u, e.v) then
                    FX.explode(e.u, e.v, e.wbig and 4 or 1, gy + 0.3)
                else
                    FX.splash(e.u, e.v, e.wbig and 4.0 or 1.1)
                end
                done = true
            end
        elseif e.wmode == "sink" then
            e.v = e.v - gk * dt
            e.y = e.y - (e.wbig and 0.25 or 0.45) * dt
            e.bank = e.bank + e.wroll * 0.04 * dt
            if e.wsmoke <= 0.0 then
                e.wsmoke = e.wbig and 0.08 or 0.14
                FX.smoke(e.u + rand(-1, 1), e.v + rand(-1, 1), e.wbig and 2.0 or 0.9, 1.0)
                FX.puff(e.u + rand(-1.5, 1.5), e.v + rand(-1.5, 1.5), 0.06, 0.4, 1.8, 1.2)
            end
            done = e.y < (e.wbig and -2.5 or -1.2)
        else
            e.v = e.v - gk * dt
            local gy = SYS.gy(e.u, e.v)
            if SYS.land then e.y = gy + 0.4 end
            if e.wsmoke <= 0.0 then
                e.wsmoke = SYS.land and 0.2 or 0.12
                FX.smoke(e.u + rand(-0.4, 0.4), e.v, e.wbig and 2.0 or 0.9, gy + 1.0,
                         SYS.land and 1.8 or nil)
                if math.random() < 0.3 then FX.fire(e.u, e.v, e.wbig and 1.6 or 0.5, 0.0, gy + 0.6) end
            end
            done = e.wt > (e.wbig and 6.0 or 2.5) * (SYS.land and 2.5 or 1.0)
        end
        if e.v < -GONE_V - 6 or abs(e.u) > 32.0 then done = true end
        if done then
            e.busy = false
        else
            place(e, e.u, e.v, e.y)
            game.setRot(e.root, attitude(e.yaw, e.bank))
            keep[#keep + 1] = e
        end
    end
    SYS.wrecks = keep
end

-- Per living enemy, each frame: exhaust trails or a wake, and smoke once it is
-- badly hurt (and from every part already shot off).
function SYS.enemyFx(e, dt)
    local def = e.def
    local tr = def.trail
    if tr and (not def.trailWhen or def.trailWhen(e)) then
        e.trailCd = e.trailCd - dt
        if e.trailCd <= 0.0 then
            local rate_ = def.trailRate or 0.07
            e.trailCd = rate_
            -- In the air the puffs join into a streak: each is stretched along
            -- the way the craft moves against the air, as far as the next one.
            local len, yaw = 0.0, nil
            if not def.ground and e.pu then
                local d = max(e.pdt or dt, 1e-3)
                local ru = (e.u - e.pu) / d
                local rv = (e.v - e.pv) / d + scrollSpeed * SYS.groundK()
                len = sqrt(ru * ru + rv * rv) * rate_ * 0.55
                yaw = deg(yawOf(atan(rv, ru)))
            end
            for _, t in ipairs(tr) do
                local tu, tv = local2plane(e.u, e.v, e.yaw, t[1], t[2])
                if def.ground then
                    FX.puff(tu, tv, 0.06, t[3], t[3] * 3.5, 1.6, rand(-0.6, 0.6), 0.0)
                else
                    FX.puff(tu, tv, e.y, t[3], t[3] * 2.2, 0.45, 0.0, 0.0, len, yaw)
                end
            end
        end
    end
    if e.maxHp >= 20 and not e.armored then
        e.dmgSmoke = e.dmgSmoke - dt
        if e.dmgSmoke <= 0.0 then
            e.dmgSmoke = 0.16
            if e.hp < e.maxHp * 0.5 then
                FX.smoke(e.cu + rand(-0.6, 0.6), e.cv + rand(-0.6, 0.6), 0.7, e.y + 0.6)
            end
            for _, m in ipairs(e.mounts) do
                if m.hp == 0 and math.random() < 0.5 then
                    local mu, mv = mountPos(e, m)
                    FX.smoke(mu, mv, 0.7, e.y + 0.6)
                end
            end
        end
    end
end

-- Contrails off the ship's two nozzles.
function SYS.playerTrail(dt)
    local p = player
    if dt <= 0.0 or not p.obj.active then return end
    SYS.trailCd = (SYS.trailCd or 0.0) - dt
    if SYS.trailCd > 0.0 then return end
    SYS.trailCd = 0.03
    local len = (scrollSpeed * SYS.groundK() + max(0.0, p.climb or 0) * 12.0) * 0.03 * 0.6
    for _, x in ipairs({ -0.12, 0.12 }) do
        FX.puff(p.u + x, p.v - 1.35, FLY_Y, 0.1, 0.4, 0.3, 0.0, 0.0, len, 0.0)
    end
end

function SYS.stepFx(dt)
    local gk = scrollSpeed * SYS.groundK()
    for _, f in ipairs(pools.puffs and pools.puffs.items or {}) do
        if f.active then
            f.age = f.age + dt
            local k = f.age / f.life
            if k >= 1.0 then
                f.active = false
            else
                local st = (k > 0.7) and 2 or ((k > 0.4) and 1 or 0)
                if st ~= f.stage then
                    f.stage = st
                    game.setMaterial(f.root, st == 2 and mats.puff3 or mats.puff2)
                end
                f.u = f.u + f.vu * dt
                f.v = f.v - gk * dt
                f.y = f.y + f.vy * dt
                f.vy = f.vy * (1.0 - 2.0 * dt)
                local sz = f.s0 + (f.s1 - f.s0) * k ^ 0.6
                game.setPos(f.root, W(f.u, f.v, f.y))
                SYS.setScale(f.root, sz, 0.02, sz + f.len)
                game.setRot(f.root, 0, f.yaw or (f.rot + k * 30.0), 0)
            end
        end
    end
    for _, f in ipairs(pools.splashes and pools.splashes.items or {}) do
        if f.active then
            f.age = f.age + dt
            local k = f.age / 0.9
            if k >= 1.0 then
                f.active = false
            else
                local st = (k > 0.65) and 2 or ((k > 0.3) and 1 or 0)
                if st ~= f.stage then
                    f.stage = st
                    game.setMaterial(f.root, st == 2 and mats.splash3 or mats.splash2)
                end
                f.v = f.v - gk * dt
                local sz = f.size * (0.6 + 1.8 * (1.0 - (1.0 - k) ^ 2))
                game.setPos(f.root, W(f.u, f.v, 0.07))
                SYS.setScale(f.root, sz, 0.02, sz)
                game.setRot(f.root, 0, f.rot, 0)
            end
        end
    end
    for _, f in ipairs(pools.muzzles and pools.muzzles.items or {}) do
        if f.active then
            f.age = f.age + dt
            if f.age > 0.07 then
                f.active = false
            else
                local sz = f.size * (0.6 + 0.4 * f.age / 0.07)
                game.setPos(f.root, W(f.u, f.v, FLY_Y + 0.15))
                SYS.setScale(f.root, sz, 0.02, sz)
                game.setRot(f.root, 0, f.rot, 0)
            end
        end
    end
end

-- The laser ship: lances that go through what they hit.
local LASER_NAMES = { "LANCE", "TWIN LANCE", "OPTIONS", "MISSILES", "MAXIMUM" }
SYS.LASER_OFFS = { { 0 }, { -0.25, 0.25 }, { -0.25, 0.25 }, { -0.25, 0.25 }, { -0.42, 0, 0.42 } }

function SYS.weaponName()
    return ((G.weapon == 2) and LASER_NAMES or WEAPON_NAMES)[G.power]
end

function SYS.lance(u, v, dmg)
    local s = take(pools.lasers)
    if not s then return end
    s.u, s.v, s.dmg, s.hitE, s.hitM = u, v, dmg, nil, nil
end

function SYS.shootLaser()
    local p = player
    for _, off in ipairs(SYS.LASER_OFFS[G.power]) do
        SYS.lance(p.u + off * (p.focus and 0.4 or 1.0), p.v + 1.7, 1.6)
    end
    if G.power >= 3 then
        for _, op in ipairs(p.options) do SYS.lance(op.u, op.v + 0.8, 1.0) end
    end
    cue("shot", 0.09)
end

function SYS.stepLasers(dt)
    for _, s in ipairs(pools.lasers.items) do
        if s.active then
            s.v = s.v + 60.0 * dt
            if s.v > GONE_V then
                s.active = false
            else
                for _, e in ipairs(enemies) do
                    if e.active and e.cv < SPAWN_V then
                        local hit, zone = hitZone(e, s.u, s.v, 0.15)
                        if hit and not (s.hitE == e and s.hitM == zone) then
                            s.hitE, s.hitM = e, zone
                            hurt(e, zone, s.dmg)
                            FX.sparks(s.u, s.v, 1, 0.5, 3.0)
                        end
                    end
                end
                place(s, s.u, s.v)
            end
        end
    end
end

-- Out of ships: ten seconds to put another coin in. The score starts again at
-- zero (the high score keeps the run that was); the stage carries on.
function SYS.enterContinue()
    META.bank()
    if G.score > hiScore then
        hiScore, G.newHi = G.score, true
        saveHi()
    end
    mode, G.contT, noFire = "continue", 10.0, true
    cancelBullets(false)
end

function SYS.continueGame()
    G.continues = (G.continues or 0) + 1
    G.score, G.chain, G.extendNo, G.nextExtend = 0, 0, 1, EXTENDS[1]
    G.banked, G.stageStartScore = 0, 0
    G.lives = max(1, floor(ships)) + META.tier("reserve")
    G.bombs = max(G.bombs, floor(bombs))
    mode, noFire = "play", false
    respawnPlayer()
    cue("start")
end

-- --- The ground ------------------------------------------------------------------------------
-- Four rows of five tiles wrap round under the camera; each row that comes
-- round at the top gets the kind of ground the stage is at. Between two kinds
-- a transition row goes in first -- a coastline, a weather front -- so the
-- ground never changes at a hard seam.
SYS.GROUND_MAT = { sea = "sea", storm = "seaStorm", land = "land" }
SYS.GROUND_TRANS = { sea = { storm = "seaFront", land = "coast" },
                     storm = { sea = "seaFrontOut" }, land = { sea = "coastOut" } }

function SYS.planGround(stage)
    world.plan, world.planIdx = {}, 1
    for i, step in ipairs(stage.ground or { { "sea", 999 } }) do world.plan[i] = { step[1], step[2] } end
    world.last = world.last or "sea"
    -- Over a landscape every stage flies the route from its start again: a
    -- cut, hidden in a flash.
    if SYS.land and abs(OZ - SYS.startZ) > 1.0 then
        OZ = SYS.startZ
        BY = SYS.groundTop()
        SYS.clearBlazes()
        FX.flash(1.0, 1.0, 1.0, 1.0)
    end
end

function SYS.nextGround()
    local step = world.plan and world.plan[world.planIdx]
    if not step then return SYS.GROUND_MAT[world.last or "sea"] end
    local want = step[1]
    if want ~= world.last then
        local via = SYS.GROUND_TRANS[world.last] and SYS.GROUND_TRANS[world.last][want]
        if via then
            world.last = want
            return via
        end
        -- No front between these two: by way of the open sea.
        via = SYS.GROUND_TRANS[world.last] and SYS.GROUND_TRANS[world.last].sea
        world.last = "sea"
        if via then return via end
        return SYS.nextGround()
    end
    step[2] = step[2] - 1
    if step[2] <= 0 and world.plan[world.planIdx + 1] then world.planIdx = world.planIdx + 1 end
    return SYS.GROUND_MAT[want]
end

function SYS.setRow(r, kind)
    r.mat = kind
    for _, t in ipairs(r.tiles) do
        local m = (kind == "land" and t.col == 0) and "landRoad" or kind
        if mats[m] then game.setMaterial(t.id, mats[m]) end
    end
end

-- The row under v: land or water? (A coast row is land on its upper half.)
function SYS.landAt(u, v)
    if SYS.land then return true end
    for _, r in ipairs(world.rows or {}) do
        if abs(r.v - v) <= SEA_TILE * 0.5 then
            if r.mat == "land" then return true end
            if r.mat == "coast" then return v > r.v end
            if r.mat == "coastOut" then return v < r.v end
            return false
        end
    end
    return false
end

-- The ground eases to a stop (and back) rather than jumping.
function SYS.stepScroll(dt)
    local k, want = G.scrollK or 1.0, G.scrollTarget or 1.0
    G.scrollK = k + clamp(want - k, -0.8 * dt, 0.8 * dt)
end

-- --- A real landscape --------------------------------------------------------------------------
-- In a scene with a Terrain the painted sea stays away and the flight goes on
-- over the scene's own ground: its hills, its river, its forests. At RS metres
-- a unit the ships are fighter-sized and a tank stands a tank's size beside a
-- tree. The ground holds still in the world and the plane's origin travels over
-- it; everything on the ground already slides down the screen at the scroll
-- speed, which is exactly what keeps it where it stands. The plane rides the
-- terrain: BY follows the highest ground in view (rising quickly, sinking
-- slowly), so a hill falls away below you and never comes up into the ships.
--   scale  world metres per unit (the ships fly FLY_Y * scale above the ground)
--   clear  metres the plane's floor keeps above the highest ground in view
--   pace   how fast the ground scrolls here, against the sea (a slower drift
--          reads as the same speed from this height, and the valley lasts)
SYS.LAND = { scale = 5.0, clear = 4.0, rise = 16.0, sink = 4.0, pace = 0.32, attract = 500.0 }

-- The world height under a flight-plane point.
function SYS.groundAt(u, v) return game.terrainHeight(OX + u * RS, OZ - v * RS) end

-- The ground under (u, v) as a height on the flight plane (0 over the sea).
function SYS.gy(u, v)
    if not SYS.land then return 0.0 end
    return (SYS.groundAt(u, v) - BY) / RS
end

-- The highest ground in view, and a little ahead of it.
function SYS.groundTop()
    local top = -1e9
    for iu = -2, 2 do
        for iv = -2, 3 do top = max(top, SYS.groundAt(iu * 8.0, iv * 7.0)) end
    end
    return top + SYS.LAND.clear
end

function SYS.stepLand(sp, dt)
    OZ = OZ - sp * RS
    -- The title screen's flight goes round the start of the route again.
    if (mode == "title" or mode == "missions") and SYS.startZ - OZ > SYS.LAND.attract then
        OZ = SYS.startZ
        BY = SYS.groundTop()
        SYS.clearBlazes()
        FX.flash(1.0, 1.0, 1.0, 1.0)
    end
    local want = SYS.groundTop()
    local L = SYS.LAND
    BY = BY + clamp(want - BY, -L.sink * dt, L.rise * dt)
end

-- --- The valley burns ----------------------------------------------------------------------------
-- Fires on the ground, a glow of embers under each, and a column of smoke that
-- climbs from it past the ships -- rising towards the camera, so it opens out
-- as it comes, the one thing here that shows how far below the ground lies.
-- Some are already burning when they come into view; everything shot down on
-- the ground adds another.
SYS.BLAZE = { n = 8, gap = { 2.5, 5.5 } }
SYS.WIND = { 1.7, -0.9 }      -- where the smoke goes, units a second (across, up the screen)

function SYS.buildBlazes()
    SYS.blazes = newPool(SYS.BLAZE.n, function()
        local parts = { { PLN, 0.0, 0.03, 0.0, 2.6, 0.02, 2.6, "embers" } }
        for k = 1, 3 do
            local a = k * 2.1
            parts[#parts + 1] = { PLN, cos(a) * 0.55, 0.1 + k * 0.04, sin(a) * 0.55, 1.0, 0.02, 1.0,
                                  "fire1", { 0.0, k * 67.0, 0.0 } }
        end
        local o = assembly("blaze", parts)
        o.flames = { o.parts[2], o.parts[3], o.parts[4] }
        return o
    end)
end

function SYS.blazeAt(u, v, size)
    local b = SYS.blazes and take(SYS.blazes)
    if not b then return end
    b.u, b.v, b.size = u, v, size
    b.seed, b.smokeCd = rand(0, 100), 0.0
end

function SYS.clearBlazes()
    for _, b in ipairs(SYS.blazes and SYS.blazes.items or {}) do b.active = false end
end

function SYS.stepBlazes(dt)
    if not SYS.blazes then return end
    local B = SYS.BLAZE
    -- A dry storm somewhere over the ridge: the valley lights up, and the
    -- thunder comes a while after.
    B.skyCd = (B.skyCd or 9.0) - dt
    if B.skyCd <= 0.0 then
        B.skyCd = rand(12.0, 24.0)
        FX.flash(rand(0.1, 0.2), 0.72, 0.78, 1.0)
        after(rand(0.9, 2.4), function() cue("thunder", 1.0) end)
    end
    B.cd = (B.cd or 1.0) - dt
    if B.cd <= 0.0 then
        B.cd = rand(B.gap[1], B.gap[2])
        SYS.blazeAt(rand(-20, 20), 25.0 + rand(0, 4), rand(0.7, 1.4))
    end
    for _, b in ipairs(SYS.blazes.items) do
        if b.active then
            b.v = b.v - scrollSpeed * SYS.groundK() * dt
            if b.v < -24.0 then
                b.active = false
            else
                local gy = SYS.gy(b.u, b.v)
                place(b, b.u, b.v, gy + 0.05)
                for i, id in ipairs(b.flames) do
                    local f = b.size * (0.75 + 0.3 * sin(now * (7.0 + i * 3.1) + b.seed + i * 1.7))
                    SYS.setScale(id, f, 0.02, f * 1.15)
                end
                b.smokeCd = b.smokeCd - dt
                if b.smokeCd <= 0.0 then
                    b.smokeCd = rand(0.22, 0.32)
                    -- Out over the crowns: from lower down the forest hides it.
                    FX.smoke(b.u + rand(-0.4, 0.4) * b.size, b.v + rand(-0.4, 0.4) * b.size,
                             1.4 * b.size, gy + 4.4, rand(0.6, 1.0))
                end
            end
        end
    end
end

-- --- Weather -----------------------------------------------------------------------------------
-- The storm stage: rain streaking down past the ships, lightning now and then
-- (a flash, a bolt, the thunder after it), and the sound of rain laid over
-- itself every few seconds.
function SYS.stepWeather(dt)
    local storm = STAGES[G.stageIdx].weather == "storm" and (mode == "play" or mode == "continue")
    if storm and dt > 0.0 then
        SYS.rainAcc = (SYS.rainAcc or 0.0) + dt * 110.0
        while SYS.rainAcc >= 1.0 do
            SYS.rainAcc = SYS.rainAcc - 1.0
            local r = take(pools.rain)
            if not r then break end
            r.u, r.v, r.y = rand(-24, 24), rand(-16, 22), FLY_Y + rand(8.0, 16.0)
        end
        G.boltCd = (G.boltCd or 3.0) - dt
        if G.boltCd <= 0.0 then
            G.boltCd = rand(3.5, 8.0)
            SYS.bolt = { t0 = now, x = rand(0.15, 0.85), seed = math.random(1, 99999) }
            FX.flash(0.25, 0.75, 0.8, 1.0)
            after(rand(0.2, 1.3), function() cue("thunder", 1.0) end)
        end
        G.rainCd = (G.rainCd or 0.0) - dt
        if G.rainCd <= 0.0 then
            G.rainCd = 13.5
            cue("rain", 5.0)
        end
    end
    for _, r in ipairs(pools.rain.items) do
        if r.active then
            r.y = r.y - 42.0 * dt
            r.v = r.v - 9.0 * dt
            if r.y < 0.0 then r.active = false else game.setPos(r.root, W(r.u, r.v, r.y)) end
        end
    end
end

-- The bolt, drawn over the field for a flicker: jagged, forked, gone.
function SYS.drawBolt(xl, xr)
    local b = SYS.bolt
    if not b then return end
    local t = now - b.t0
    if t > 0.24 then SYS.bolt = nil return end
    if not (t < 0.07 or (t > 0.11 and t < 0.2)) then return end
    local seed = b.seed
    local function rnd()
        seed = (seed * 1103515245 + 12345) % 2147483648
        return seed / 2147483648
    end
    local x, y = xl + (xr - xl) * b.x, 0.0
    local bottom = 300 + rnd() * 380
    local fork = floor(3 + rnd() * 4)
    for i = 1, 12 do
        local nx, ny = x + (rnd() - 0.5) * 90, y + bottom / 12
        game.hudLine(x, y, nx, ny, 0.7, 0.8, 1.0, 0.25, 12)
        game.hudLine(x, y, nx, ny, 1.0, 1.0, 1.0, 0.95, 3)
        if i == fork then
            local fx, fy = nx, ny
            for _ = 1, 4 do
                local gx, gy = fx + (rnd() - 0.3) * 70, fy + bottom / 14
                game.hudLine(fx, fy, gx, gy, 1.0, 1.0, 1.0, 0.7, 2)
                fx, fy = gx, gy
            end
        end
        x, y = nx, ny
    end
end

-- --- World scroll -----------------------------------------------------------------------------

local function stepWorld(dt)
    local sp = scrollSpeed * SYS.groundK() * dt
    local st = STAGES[G.stageIdx]
    if SYS.land then SYS.stepLand(sp, dt) end
    for _, r in ipairs(world.rows) do
        r.v = r.v - sp
        if r.v < -40.0 then
            r.v = r.v + 4 * SEA_TILE
            SYS.setRow(r, SYS.nextGround())
        end
        for _, t in ipairs(r.tiles) do game.setPos(t.id, W(t.u, r.v, 0.0)) end
    end
    for _, isl in ipairs(world.islands) do
        isl.v = isl.v - sp
        if isl.v < -34.0 then
            isl.v, isl.u = isl.v + 72.0, rand(-24, 24)
            isl.obj.active = st.islands ~= false
            if hasTex.islandA then
                game.setMaterial(isl.plane, mats[ISLAND_MATS[math.random(1, 3)]])
            end
        end
        place(isl.obj, isl.u, isl.v, 0.0)
    end
    for _, c in ipairs(world.clouds) do
        c.v = c.v - sp * 1.25
        if c.v < -30.0 then
            c.v, c.u = c.v + 70.0, rand(-26, 26)
            local storm = st.clouds == "storm" and hasTex.stormA
            if hasTex.cloudA then
                game.setMaterial(c.id, mats[storm and (math.random() < 0.5 and "stormA" or "stormB")
                                          or CLOUD_MATS[math.random(1, 3)]])
                local sz = storm and rand(9.0, 13.0) or rand(6.5, 10.0)
                SYS.setScale(c.id, sz, 0.02, sz * rand(0.8, 1.0))
            end
        end
        game.setPos(c.id, W(c.u, c.v, c.y))
    end
    for _, c in ipairs(world.haze) do
        c.v = c.v - sp * 1.6
        if c.v < -25.0 then c.v, c.u = c.v + 120.0, rand(-12, 12) end
        game.setPos(c.id, W(c.u, c.v, c.y))
    end
end

-- --- Drawing the ship ---------------------------------------------------------------------------

local function drawPlayer()
    local p = player
    if not p.obj.active then return end
    place(p.obj, p.u, p.v)
    if mode == "hangar" then
        game.setRot(p.obj.root, 0, 200.0 + now * 14.0, 0)
    else
        game.setRot(p.obj.root, 0, 0, deg(p.roll))
    end
    local boost = (p.climb or 0) > 0 and 0.12 or ((p.climb or 0) < 0 and -0.06 or 0.0)
    local thr = (1.0 + 0.22 * META.tier("thrusters")) * (mode == "hangar" and 0.55 or 1.0)
    for i, id in ipairs(p.obj.flames) do
        SYS.setScale(id, 0.08 * thr, 0.08 * thr, (0.22 + boost + 0.07 * sin(now * 60.0 + i)) * thr)
    end
    local gun = p.alive and p.firing and p.respawn <= 0.0 and not paused
    for _, id in ipairs(p.obj.muzzles) do
        local ms = gun and rand(0.3, 0.5) or 0.001
        SYS.setScale(id, ms, 0.02, ms * 1.5)
    end
    if p.hitbox.active then place(p.hitbox, p.u, p.v, FLY_Y + 0.4) end
    if p.shield.active then
        place(p.shield, p.u, p.v)
        local s = 1.35 + 0.08 * sin(now * 9.0)
        SYS.setScale(p.shield.root, s, 0.6, s)
    end
end

-- --- Camera ------------------------------------------------------------------------------------

local function driveCamera(dt)
    shake = max(0.0, shake - dt * 1.8)
    local sx, sy = 0.0, 0.0
    if shake > 0.0 then
        local a = shake * shake * 0.9
        sx, sy = rand(-a, a), rand(-a, a)
    end
    local follow = (player.obj.active and player.u or 0.0) * 0.06
    local ex, ey, ez = W(follow + sx, -CAM_BACK + sy, FLY_Y + CAM_H)
    local tx, ty, tz = W(follow * 0.9 + sx * 0.5, sy * 0.5, FLY_Y)
    if META.inHangar() then ex, ey, ez, tx, ty, tz = META.hangarCam() end
    game.setCameraPos(ex, ey, ez)
    game.setCameraDir(tx - ex, ty - ey, tz - ez)
    game.setCameraFov(CAM_FOV)
    -- The same basis the engine builds from that direction, for project().
    local fx, fy, fz = tx - ex, ty - ey, tz - ez
    local fl = sqrt(fx * fx + fy * fy + fz * fz)
    fx, fy, fz = fx / fl, fy / fl, fz / fl
    local rx, rz = -fz, fx
    local rl = sqrt(rx * rx + rz * rz)
    rx, rz = rx / rl, rz / rl
    view.ex, view.ey, view.ez = ex, ey, ez
    view.fx, view.fy, view.fz = fx, fy, fz
    view.rx, view.ry, view.rz = rx, 0.0, rz
    view.ux, view.uy, view.uz = -rz * fy, rz * fx - rx * fz, rx * fy
    view.t = math.tan(math.rad(CAM_FOV) * 0.5)
    if game.hudSize then view.w, view.h = game.hudSize() end
end

-- --- HUD ---------------------------------------------------------------------------------------

local function txt(x, y, s, size, c, a, align, bold)
    game.hudText(x, y, s, size, c[1], c[2], c[3], a or 1.0, align or 0.0, bold or false)
end
local function rect(x, y, w, h, c, a, round)
    game.hudRect(x, y, w, h, c[1], c[2], c[3], a or 1.0, round or 0.0)
end
local function frameBox(x, y, w, h, c, a, th, round)
    game.hudFrame(x, y, w, h, c[1], c[2], c[3], a or 1.0, th or 2.0, round or 0.0)
end
local function line(x1, y1, x2, y2, c, a, th)
    game.hudLine(x1, y1, x2, y2, c[1], c[2], c[3], a or 1.0, th or 2.0)
end
local function tri(x1, y1, x2, y2, x3, y3, c, a)
    game.hudTri(x1, y1, x2, y2, x3, y3, c[1], c[2], c[3], a or 1.0)
end

local function shipIcon(x, y, s, c, a)
    tri(x, y - 13 * s, x - 3.5 * s, y + 9 * s, x + 3.5 * s, y + 9 * s, c, a)
    tri(x, y - 3 * s, x - 13 * s, y + 6 * s, x + 13 * s, y + 6 * s, c, a)
    tri(x, y + 4 * s, x - 6 * s, y + 11 * s, x + 6 * s, y + 11 * s, c, a)
end

local function bombIcon(x, y, s, c, a)
    game.hudCircle(x, y + 2 * s, 9 * s, c[1], c[2], c[3], a)
    rect(x - 3 * s, y - 10 * s, 6 * s, 5 * s, c, a)
    game.hudCircle(x + 4 * s, y - 13 * s, 2.5 * s, 1.0, 0.85, 0.3, a)
    game.hudCircle(x - 3 * s, y - 1 * s, 2.5 * s, 1.0, 1.0, 1.0, a * 0.5)
end

local function label(x, y, s, text)
    rect(x, y + 3 * s, 4 * s, 15 * s, C_CYAN, 0.9)
    txt(x + 11 * s, y, text, 17 * s, C_DIM, 1.0, 0.0, true)
end

-- Panels beside the playfield: the widescreen margins carry the instruments,
-- so nothing sits on top of the bullets.
function HUD.left(x, w, s)
    local y = 34 * s
    txt(x, y, "SKYSTRIKE", 40 * s, C_CYAN, 1.0, 0.0, true)
    txt(x + 2 * s, y + 46 * s, "V E R T I C A L   A S S A U L T", 12 * s, C_DIM, 0.9, 0.0, true)
    y = 140 * s
    label(x, y, s, "SCORE")
    txt(x, y + 24 * s, fmt(G.score), 46 * s, C_TEXT, 1.0, 0.0, true)
    y = 232 * s
    label(x, y, s, "HI-SCORE")
    txt(x, y + 24 * s, fmt(max(hiScore, G.score)), 28 * s, C_GOLD, 1.0, 0.0, true)
    y = 318 * s
    label(x, y, s, "STAGE")
    local st = STAGES[G.stageIdx]
    txt(x, y + 24 * s, tostring(G.stageNo) .. "  " .. st.name, 26 * s, C_TEXT, 1.0, 0.0, true)
    if G.loop > 1 then txt(x, y + 56 * s, "LOOP " .. G.loop, 18 * s, C_MAG, 1.0, 0.0, true) end
    if (G.continues or 0) > 0 then
        txt(x + w, y + 56 * s, "CONTINUE " .. G.continues, 16 * s, C_DIM, 0.9, 1.0, true)
    end
    y = 420 * s
    label(x, y, s, "CHAIN")
    local live = G.clock - G.chainAt < CHAIN_WINDOW and G.chain > 1
    local cc = (G.chain >= 40) and C_MAG or ((G.chain >= 15) and C_GOLD or C_TEXT)
    txt(x, y + 22 * s, live and tostring(G.chain) or "-", 48 * s, cc, live and 1.0 or 0.4, 0.0, true)
    if live then
        local mult = 1.0 + min(G.chain, 60) / 30.0
        txt(x + w, y + 36 * s, string.format("x%.1f", mult), 28 * s, cc, 1.0, 1.0, true)
        local k = 1.0 - (G.clock - G.chainAt) / CHAIN_WINDOW
        rect(x, y + 80 * s, w, 6 * s, C_DIM, 0.25, 3 * s)
        rect(x, y + 80 * s, w * k, 6 * s, cc, 0.95, 3 * s)
    end
    y = 530 * s
    label(x, y, s, "GRAZE")
    txt(x, y + 24 * s, tostring(G.graze), 28 * s, C_TEXT, 0.9, 0.0, true)
    local hy = view.h - 150 * s
    local hints = { "SHIFT   focus", "SPACE   fire", "B   bomb", "P   pause" }
    for i, h in ipairs(hints) do txt(x, hy + (i - 1) * 24 * s, h, 16 * s, C_DIM, 0.7, 0.0, false) end
end

function HUD.right(x, w, s)
    local y = 34 * s
    label(x, y, s, "SHIPS")
    local n = G.lives
    for i = 1, min(n, 6) do shipIcon(x + 16 * s + (i - 1) * 34 * s, y + 52 * s, 1.1 * s, C_TEXT, 1.0) end
    if n > 6 then txt(x + 6 * 34 * s + 8 * s, y + 38 * s, "+" .. (n - 6), 22 * s, C_TEXT, 1.0, 0.0, true) end
    y = 140 * s
    label(x, y, s, "BOMBS")
    for i = 1, G.bombs do bombIcon(x + 14 * s + (i - 1) * 32 * s, y + 52 * s, 1.1 * s, C_GREEN, 1.0) end
    if G.bombs == 0 then txt(x, y + 36 * s, "EMPTY", 20 * s, C_DIM, 0.8, 0.0, true) end
    y = 246 * s
    label(x, y, s, "POWER")
    local seg = (w - 4 * 6 * s) / MAX_POWER
    for i = 1, MAX_POWER do
        local on = i <= G.power
        local c = (G.power >= MAX_POWER) and C_GOLD or C_CYAN
        rect(x + (i - 1) * (seg + 6 * s), y + 30 * s, seg, 18 * s, on and c or C_DIM, on and 0.95 or 0.2, 3 * s)
    end
    txt(x, y + 58 * s, SYS.weaponName(), 22 * s, (G.power >= MAX_POWER) and C_GOLD or C_TEXT,
        1.0, 0.0, true)
    y = 360 * s
    label(x, y, s, "FOCUS")
    local fo = player.focus and player.alive
    rect(x, y + 28 * s, w, 30 * s, fo and C_CYAN or C_DIM, fo and 0.85 or 0.15, 4 * s)
    txt(x + w * 0.5, y + 31 * s, fo and "ON" or "HOLD SHIFT", 18 * s, fo and { 0.02, 0.1, 0.16 } or C_DIM,
        1.0, 0.5, true)
    y = 452 * s
    label(x, y, s, "RANK")
    local k = G.rank / 8.0
    rect(x, y + 30 * s, w, 8 * s, C_DIM, 0.2, 4 * s)
    rect(x, y + 30 * s, max(8 * s, w * k), 8 * s, C_RED, 0.9, 4 * s)
    local most = META.shieldMax()
    if most > 0 then
        y = 530 * s
        label(x, y, s, "SHIELD")
        local seg = (w - (most - 1) * 8 * s) / most
        for i = 1, most do
            local full = i <= (G.shield or 0)
            local fill = full and 1.0 or ((i == (G.shield or 0) + 1) and (G.shieldT or 0) / META.shieldTime() or 0.0)
            rect(x + (i - 1) * (seg + 8 * s), y + 30 * s, seg, 18 * s, C_DIM, 0.2, 3 * s)
            rect(x + (i - 1) * (seg + 8 * s), y + 30 * s, max(3 * s, seg * fill), 18 * s,
                 full and C_CYAN or C_DIM, full and 0.95 or 0.6, 3 * s)
        end
    end
    local nx = view.h - 60 * s
    txt(x, nx, "NEXT SHIP  " .. fmt(G.nextExtend), 16 * s, C_DIM, 0.7, 0.0, false)
end

-- The boss's health across the top of the playfield, one notch per part.
function HUD.bossBar(xl, xr)
    local e = (G.boss and G.boss.active and G.boss) or (G.hold and G.hold.active and G.hold) or nil
    if not e then return end
    local hp, mx = max(0, e.hp), e.maxHp
    for _, m in ipairs(e.mounts) do
        if m.maxHp then hp, mx = hp + max(0, m.hp or 0), mx + m.maxHp end
    end
    local x, w, y = xl + 40, (xr - xl) - 80, 16
    txt(x, y, e.def.name or "", 22, C_RED, 1.0, 0.0, true)
    if e.armored then txt(x + w, y + 3, "ARMOURED", 16, C_DIM, 0.9, 1.0, true) end
    rect(x, y + 30, w, 16, { 0.05, 0.02, 0.02 }, 0.75, 4)
    local k = hp / max(1, mx)
    rect(x + 2, y + 32, (w - 4) * k, 12, e.armored and { 0.7, 0.55, 0.5 } or C_RED, 0.95, 3)
    frameBox(x, y + 30, w, 16, C_RED, 0.8, 1.5, 4)
end

local ORB_LABEL = { P = { "P", C_GOLD }, B = { "B", C_GREEN }, ["1"] = { "1UP", C_MAG } }

function HUD.popups()
    for _, o in ipairs(pools.orbs.items) do
        if o.active then
            local x, y = project(o.u, o.v, FLY_Y + 0.4)
            local l = ORB_LABEL[o.kind]
            txt(x, y - 15, l[1], 24, l[2], 1.0, 0.5, true)
        end
    end
    for _, p in ipairs(popups) do
        local x, y = project(p.u, p.v + p.t * 1.5)
        local a = 1.0 - p.t * p.t
        txt(x, y - p.size * 0.6, p.text, p.size, p.col, a, 0.5, true)
    end
end

local function fadeOf(b, inT, outT)
    local t = now - b.t0
    return clamp(min(t / inT, (b.dur - t) / outT), 0.0, 1.0), t
end

-- Diagonal hazard stripes across a band.
local function hazard(x0, x1, y, h, a, shift)
    local step = 44
    local x = x0 - step * 2 + (shift % step)
    while x < x1 do
        local xa, xb = max(x0, x), min(x1, x + step * 0.5)
        if xb > xa then
            tri(xa, y + h, xb, y + h, min(x1, xb + h * 0.5), y, C_RED, a)
            tri(xa, y + h, min(x1, xb + h * 0.5), y, min(x1, xa + h * 0.5), y, C_RED, a)
        end
        x = x + step
    end
end

function HUD.banner(xl, xr)
    local b = banner
    if not b then return end
    if now - b.t0 > b.dur then banner = nil return end
    local cx, cy = (xl + xr) * 0.5, view.h * 0.42
    if b.kind == "stage" then
        local a, t = fadeOf(b, 0.25, 0.6)
        local st = STAGES[G.stageIdx]
        local wline = min(1.0, t * 2.5) * (xr - xl) * 0.35
        line(cx - wline, cy - 10, cx + wline, cy - 10, C_CYAN, a * 0.8, 2)
        line(cx - wline, cy + 150, cx + wline, cy + 150, C_CYAN, a * 0.8, 2)
        txt(cx, cy, "STAGE " .. G.stageNo, 90, C_TEXT, a, 0.5, true)
        txt(cx, cy + 96, st.name, 32, C_CYAN, a, 0.5, true)
        if G.loop > 1 and G.stageIdx == 1 then
            txt(cx, cy - 60, "LOOP " .. G.loop .. "  --  THE SKIES GROW DARKER", 22, C_MAG, a, 0.5, true)
        else
            txt(cx, cy - 50, st.sub, 20, C_DIM, a, 0.5, false)
        end
    elseif b.kind == "warning" or b.kind == "alert" then
        local a, t = fadeOf(b, 0.2, 0.4)
        local big = b.kind == "warning"
        local h = big and 170 or 90
        local y = cy - h * 0.5
        rect(xl, y, xr - xl, h, { 0.25, 0.0, 0.0 }, a * 0.45)
        hazard(xl, xr, y, 16, a * 0.8, t * 120)
        hazard(xl, xr, y + h - 16, 16, a * 0.8, -t * 120)
        local on = floor(t * 4.0) % 2 == 0
        if big then
            txt(cx, y + 28, "WARNING", 96, C_RED, on and a or a * 0.35, 0.5, true)
            txt(cx, y + 128, "A HUGE ENEMY IS APPROACHING FAST", 22, C_TEXT, a, 0.5, true)
        else
            txt(cx, y + 26, b.text, 34, C_RED, on and a or a * 0.5, 0.5, true)
        end
    elseif b.kind == "clear" and G.tally then
        local a, t = fadeOf(b, 0.3, 0.6)
        local tl = G.tally
        rect(cx - 330, cy - 70, 660, 330, { 0.0, 0.03, 0.08 }, a * 0.7, 10)
        frameBox(cx - 330, cy - 70, 660, 330, C_GOLD, a * 0.7, 2, 10)
        txt(cx, cy - 50, "STAGE CLEAR", 64, C_GOLD, a, 0.5, true)
        local rows = {
            { "BOSS DESTROYED", fmt(tl.boss) },
            { "KILL RATIO  " .. tl.kills .. " %", fmt(tl.killBonus) },
            { "NO MISS", tl.noMiss > 0 and fmt(tl.noMiss) or "--" },
        }
        for i, row in ipairs(rows) do
            local ra = clamp((t - 0.5 - i * 0.45) * 4.0, 0.0, 1.0) * a
            local ry = cy + 40 + (i - 1) * 44
            txt(cx - 290, ry, row[1], 24, C_TEXT, ra, 0.0, true)
            txt(cx + 290, ry, row[2], 24, C_TEXT, ra, 1.0, true)
        end
        local ta = clamp((t - 2.4) * 3.0, 0.0, 1.0) * a
        line(cx - 290, cy + 176, cx + 290, cy + 176, C_GOLD, ta * 0.8, 2)
        txt(cx - 290, cy + 190, "BONUS", 30, C_GOLD, ta, 0.0, true)
        txt(cx + 290, cy + 190, fmt(tl.total), 30, C_GOLD, ta, 1.0, true)
        if tl.credits then
            local ca = clamp((t - 3.0) * 3.0, 0.0, 1.0) * a
            txt(cx, cy + 236, (tl.first and "FIRST CLEAR!   " or "") .. "+" .. fmt(tl.credits) .. " CREDITS",
                24, C_CYAN, ca, 0.5, true)
        end
    end
end

function HUD.continue(xl, xr)
    local cx = (xl + xr) * 0.5
    game.hudGradient(xl, 0, xr - xl, view.h, 0.0, 0.0, 0.05, 0.3, 0.0, 0.0, 0.05, 0.7)
    txt(cx, 280, "CONTINUE?", 90, C_TEXT, 1.0, 0.5, true)
    local n = max(0, math.ceil(G.contT) - 1)
    local k = G.contT % 1.0
    txt(cx, 390, tostring(n), 170 + 40 * k, C_GOLD, 0.6 + 0.4 * k, 0.5, true)
    if floor(now * 2.5) % 2 == 0 then
        txt(cx, 640, "PRESS  ENTER", 36, C_TEXT, 1.0, 0.5, true)
    end
    txt(cx, 700, "the score starts again at 0 -- the stage goes on", 20, C_DIM, 0.9, 0.5, false)
end

function HUD.over(xl, xr)
    local cx = (xl + xr) * 0.5
    local t = now - G.overAt
    local a = clamp(t * 2.0, 0.0, 1.0)
    game.hudGradient(xl, 0, xr - xl, view.h, 0.05, 0.0, 0.0, 0.2 * a, 0.02, 0.0, 0.0, 0.7 * a)
    txt(cx, 300, "GAME OVER", 110, C_RED, a, 0.5, true)
    txt(cx, 440, "SCORE   " .. fmt(G.score), 38, C_TEXT, a, 0.5, true)
    if G.newHi and floor(now * 3.0) % 2 == 0 then
        txt(cx, 500, "NEW HIGH SCORE!", 34, C_GOLD, a, 0.5, true)
    end
    txt(cx, 570, string.format("STAGE %d     MAX CHAIN %d     GRAZE %d", G.stageNo, G.maxChain, G.graze),
        22, C_DIM, a, 0.5, true)
    META.drawEarned(cx, 612, a)
    if t > 1.5 then
        txt(cx, 690, "PRESS  ENTER", 30, C_GOLD, (floor(now * 1.8) % 2 == 0) and 1.0 or 0.4, 0.5, true)
    end
end

-- Without the engine's HUD calls (an older build) the game still shows the numbers.
local function textHud()
    if mode == "play" then
        game.setHud(string.format("SCORE %s   HI %s   STAGE %d\nSHIPS %d   BOMBS %d   POWER %d",
            fmt(G.score), fmt(max(hiScore, G.score)), G.stageNo, G.lives, G.bombs, G.power))
    else
        game.setHud(mode == "over" and ("GAME OVER   " .. fmt(G.score) .. "   --   ENTER: play again")
                    or "SKYSTRIKE   --   Press ENTER to start")
    end
end

function HUD.draw()
    if not game.hudRect then textHud() return end
    game.setHud("")      -- the engine's own notices would sit on top of ours
    local xl = project(-FIELD_U, 0.0, FLY_Y)
    local xr = project(FIELD_U, 0.0, FLY_Y)
    -- Panels need room; on a narrow window they shrink and sit over the edges.
    local pw = max(xl, 250.0)
    local s = clamp(pw / 360.0, 0.62, 1.0)
    local lw, rw = pw, pw
    if mode == "play" or mode == "over" or mode == "continue" then
        game.hudGradient(0, 0, lw, view.h, 0.02, 0.05, 0.1, 0.78, 0.01, 0.03, 0.07, 0.9)
        game.hudGradient(view.w - rw, 0, rw, view.h, 0.02, 0.05, 0.1, 0.78, 0.01, 0.03, 0.07, 0.9)
        line(lw, 0, lw, view.h, C_CYAN, 0.55, 2)
        line(lw - 7, 0, lw - 7, view.h, C_CYAN, 0.15, 1)
        line(view.w - rw, 0, view.w - rw, view.h, C_CYAN, 0.55, 2)
        line(view.w - rw + 7, 0, view.w - rw + 7, view.h, C_CYAN, 0.15, 1)
        local pad = 26 * s
        HUD.left(pad, lw - pad * 2 - 8, s)
        HUD.right(view.w - rw + pad + 8, rw - pad * 2 - 8, s)
    end
    local fl, fr = max(xl, lw), min(xr, view.w - rw)
    if STAGES[G.stageIdx].weather == "storm" and (mode == "play" or mode == "continue") then
        game.hudGradient(fl, 0, fr - fl, view.h, 0.02, 0.03, 0.06, 0.34, 0.03, 0.05, 0.08, 0.18)
    end
    HUD.popups()
    if mode == "play" then
        HUD.bossBar(fl, fr)
        HUD.banner(fl, fr)
        local p = player
        if p.alive and p.respawn > 0.0 then
            local x, y = project(p.u, p.v + 2.2)
            txt(x, y, "READY", 30, C_CYAN, 1.0, 0.5, true)
        end
        if paused then
            rect(fl, 0, fr - fl, view.h, { 0.0, 0.0, 0.0 }, 0.45)
            txt((fl + fr) * 0.5, view.h * 0.4, "PAUSED", 90, C_TEXT, 1.0, 0.5, true)
            txt((fl + fr) * 0.5, view.h * 0.4 + 110, "P  TO  CONTINUE", 26, C_DIM, 1.0, 0.5, true)
        end
    elseif mode == "title" then
        META.drawTitle(fl, fr)
    elseif mode == "missions" then
        META.drawMissions(fl, fr)
    elseif mode == "hangar" then
        META.drawHangar()
    elseif mode == "continue" then
        HUD.continue(fl, fr)
    else
        HUD.over(fl, fr)
    end
    SYS.drawBolt(fl, fr)
    if flash > 0.01 then
        rect(0, 0, view.w, view.h, flashCol, min(0.85, flash))
    end
end

-- --- The pilot, the hangar, the mission board ---------------------------------------------------
-- Around the stages sits a small game of its own. Every run pays CREDITS -- a
-- hundredth of the score it made, more with SALVAGE, and a bonus the first time
-- a mission is cleared. In the HANGAR they buy upgrades in three tiers, and
-- the ship wears what was bought. A mission once cleared can be flown again on
-- its own from the MISSIONS board. All of it is the pilot's profile, kept with
-- game.saveData (a save of this game in the user's folder, not in the project).

META.UPGRADES = {
    { key = "firepower", name = "FIREPOWER", cost = { 2000, 6000, 15000 },
      tiers = { "Start with the spread guns", "Start with options on the wings", "Start with missiles armed" } },
    { key = "rapid", name = "RAPID FIRE", cost = { 1500, 4500, 11000 },
      tiers = { "Guns fire 15 % faster", "Guns fire 30 % faster", "Guns fire 50 % faster" } },
    { key = "missiles", name = "HOMING MISSILES", cost = { 2500, 6000, 14000 },
      tiers = { "Missiles at every power level", "Missiles reload faster", "Missiles reload twice as fast" } },
    { key = "shield", name = "SHIELD", cost = { 3000, 8000, 18000 },
      tiers = { "Stops one hit, back after 30 s", "Stops one hit, back after 20 s", "Stops two hits, back after 14 s" } },
    { key = "thrusters", name = "THRUSTERS", cost = { 1200, 3500, 9000 },
      tiers = { "12 % more speed", "25 % more speed", "40 % more speed" } },
    { key = "bombs", name = "BOMB BAY", cost = { 1000, 3000, 8000 },
      tiers = { "One more bomb, one more carried", "Two more bombs, two more carried", "Three more bombs, three more carried" } },
    { key = "reserve", name = "RESERVE SHIP", cost = { 4000, 10000, 22000 },
      tiers = { "One more ship per run", "Two more ships per run", "Three more ships per run" } },
    { key = "salvage", name = "SALVAGE", cost = { 1500, 5000, 12000 },
      tiers = { "25 % more credits", "50 % more credits", "Twice the credits" } },
}

-- Missions across both scenes of the run: the sea's four and the valley's one.
META.MISSIONS = {
    { id = "coral",  name = "CORAL SEA",   sub = "The outer islands",    land = false, stage = 1 },
    { id = "fleet",  name = "IRON FLEET",  sub = "The northern convoy",  land = false, stage = 2 },
    { id = "storm",  name = "STORM FRONT", sub = "Into the weather",     land = false, stage = 3 },
    { id = "coast",  name = "IRON COAST",  sub = "The mainland",         land = false, stage = 4 },
    { id = "valley", name = "HIGH VALLEY", sub = "Up the mountain river", land = true, stage = 1 },
}
META.TITLE_ITEMS = { "CAMPAIGN", "MISSIONS", "HANGAR" }

-- --- The profile ------------------------------------------------------------------------------

function META.load()
    local p = game.loadData and game.loadData("profile")
    if type(p) ~= "table" then p = {} end
    p.credits = tonumber(p.credits) or 0
    p.up = type(p.up) == "table" and p.up or {}
    p.cleared = type(p.cleared) == "table" and p.cleared or {}
    p.best = type(p.best) == "table" and p.best or {}
    for _, u in ipairs(META.UPGRADES) do p.up[u.key] = clamp(floor(tonumber(p.up[u.key]) or 0), 0, 3) end
    if p.weapon == 1 or p.weapon == 2 then SYS.weapon = p.weapon end
    META.P = p
    META.looksDirty = true
end

function META.save()
    META.P.weapon = SYS.weapon
    if game.saveData then game.saveData("profile", META.P) end
end

function META.tier(key) return META.P and META.P.up[key] or 0 end

-- What the tiers do, read by the game wherever it needs them.
function META.basePower()  return 1 + META.tier("firepower") end
function META.maxBombs()   return MAX_BOMBS + META.tier("bombs") end
function META.rateK()      return ({ 1.0, 0.87, 0.77, 0.67 })[META.tier("rapid") + 1] end
function META.speedK()     return ({ 1.0, 1.12, 1.25, 1.4 })[META.tier("thrusters") + 1] end
function META.missileCd()  return ({ false, 0.9, 0.62, 0.45 })[META.tier("missiles") + 1] end
function META.shieldMax()  return ({ 0, 1, 1, 2 })[META.tier("shield") + 1] end
function META.shieldTime() return ({ 0, 30, 20, 14 })[META.tier("shield") + 1] end
function META.creditK()    return ({ 1.0, 1.25, 1.5, 2.0 })[META.tier("salvage") + 1] end

function META.missionOf(stageIdx)
    for i, m in ipairs(META.MISSIONS) do
        if m.land == (SYS.land == true) and m.stage == stageIdx then return m, i end
    end
end

-- A mission in the other scene is only there if this one is linked to it.
function META.reachable(m) return m.land == (SYS.land == true) or nextScene ~= "" end

-- --- Credits ------------------------------------------------------------------------------------

-- Pays out the score made since the last payout. Called at a stage clear,
-- on game over and before a continue (which starts the score again at 0).
function META.bank(bonus)
    if not META.P or mode == "title" then return 0 end
    local gain = max(0, G.score - (G.banked or 0))
    G.banked = G.score
    local cr = floor(gain / 100 * META.creditK()) + (bonus or 0)
    META.P.credits = META.P.credits + cr
    G.earned = (G.earned or 0) + cr
    META.save()
    return cr
end

function META.onClear()
    local m = META.missionOf(G.stageIdx)
    local bonus = 0
    if m then
        if not META.P.cleared[m.id] then bonus = 1000 * m.stage + (m.land and 3000 or 0) end
        META.P.cleared[m.id] = true
        META.P.best[m.id] = max(META.P.best[m.id] or 0, G.score - (G.stageStartScore or 0))
    end
    local cr = META.bank(bonus)
    if G.tally then G.tally.credits, G.tally.first = cr, bonus > 0 end
    G.stageStartScore = G.score
end

-- --- Starting a run --------------------------------------------------------------------------

-- What the hangar put on the ship, at the start of every run.
function META.loadout()
    G.power = META.basePower()
    G.bombs = max(0, floor(bombs)) + META.tier("bombs")
    G.lives = max(1, floor(ships)) + META.tier("reserve")
    G.shield, G.shieldT = META.shieldMax(), 0.0
    G.banked, G.earned, G.stageStartScore = 0, 0, 0
end

-- One mission on its own: that stage, then back to the board.
function META.single(stageIdx, back)
    G.single, G.back = true, back or false
    G.stageIdx, G.stageNo = stageIdx, stageIdx
    setRank()
    SYS.planGround(STAGES[stageIdx])
end

-- Fly a mission (or the campaign, m = nil) -- here, or in the other scene.
function META.launch(m)
    local land = m and m.land or false
    -- Unlinked, a scene's campaign is its own stages.
    if land ~= (SYS.land == true) and nextScene ~= "" then
        SYS.handOff({ fresh = 1, single = m and 1 or 0, stage = m and m.stage or 1, back = 1 })
        return
    end
    META.leaveHangar()
    startGame()
    if m then META.single(m.stage, false) end
end

-- A single mission is over: its board, here or back where it was picked.
function META.missionDone()
    if G.back and nextScene ~= "" then
        SYS.handOff({ menu = 2 })
        return
    end
    clearField()
    META.toMenu("missions")
end

-- --- The shield ---------------------------------------------------------------------------------

function META.stepShield(dt)
    local most = META.shieldMax()
    if most <= 0 or (G.shield or 0) >= most then return end
    G.shieldT = (G.shieldT or 0.0) + dt
    if G.shieldT >= META.shieldTime() then
        G.shield, G.shieldT = G.shield + 1, 0.0
        FX.popup(player.u, player.v + 1.6, "SHIELD UP", C_CYAN, 24)
        cue("power", 0.2)
    end
end

-- A hit the shield takes instead of the ship. True when it did.
function META.shieldHit()
    if mode ~= "play" or (G.shield or 0) < 1 then return false end
    local p = player
    G.shield, G.shieldT = G.shield - 1, 0.0
    p.invuln = max(p.invuln, 1.8)
    FX.ring(p.u, p.v, 3.2)
    FX.sparks(p.u, p.v, 14, 1.2, 9.0)
    FX.flash(0.22, 0.35, 0.75, 1.0)
    FX.shake(0.35)
    FX.popup(p.u, p.v + 1.6, "SHIELD", C_CYAN, 26)
    cue("clank", 0.1)
    return true
end

-- --- What the ship wears ------------------------------------------------------------------------
-- Extra parts on the player's craft, shown by tier: gun barrels, missile pods,
-- shield emitters, bigger nozzles, a belly bomb, a salvage antenna.
function META.buildLooks(o)
    local looks = {}
    local function add(key, tier, kind, x, y, fwd, hx, hy, hz, mat, rot)
        looks[#looks + 1] = { id = spawn(kind, x, y, -fwd, hx, hy, hz, mat, o.root, "upgrade", rot),
                              key = key, tier = tier }
    end
    for _, sx in ipairs({ -1, 1 }) do
        add("firepower", 1, CYL, sx * 0.42, 0.03, 0.95, 0.035, 0.3, 0.035, "darkMetal", { 90, 0, 0 })
        add("firepower", 2, CYL, sx * 0.66, -0.01, 0.62, 0.035, 0.26, 0.035, "darkMetal", { 90, 0, 0 })
        add("missiles", 1, CYL, sx * 0.86, -0.13, -0.05, 0.075, 0.34, 0.075, "missile", { 90, 0, 0 })
        add("missiles", 1, SPH, sx * 0.86, -0.13, 0.3, 0.075, 0.075, 0.12, "red")
        add("missiles", 3, CYL, sx * 1.08, -0.11, -0.2, 0.06, 0.28, 0.06, "missile", { 90, 0, 0 })
        add("shield", 1, SPH, sx * 1.22, 0.07, -0.5, 0.08, 0.08, 0.08, "padLight")
        add("thrusters", 1, CYL, sx * 0.12, 0.0, -1.12, 0.13, 0.09, 0.13, "darkMetal", { 90, 0, 0 })
    end
    add("firepower", 3, CYL, 0.0, -0.1, 1.45, 0.06, 0.34, 0.06, "darkMetal", { 90, 0, 0 })
    add("shield", 2, CYL, 0.0, 0.27, -0.25, 0.3, 0.02, 0.3, "padLight")
    add("bombs", 1, SPH, 0.0, -0.21, 0.05, 0.13, 0.1, 0.36, "oliveDark")
    add("salvage", 1, BOX, 0.0, 0.36, -0.62, 0.012, 0.2, 0.012, "grey")
    o.looks = looks
end

function META.applyLooks()
    if not META.looksDirty or frameNo < 3 or not player.obj.looks then return end
    META.looksDirty = false
    for _, l in ipairs(player.obj.looks) do game.setActive(l.id, META.tier(l.key) >= l.tier) end
end

-- --- The hangar: a bay with the ship on its pad ------------------------------------------------------
-- Built once, parked, and put under the ship when the hangar opens. The camera
-- swings down from the top-down view to look at the craft across the deck.
META.HANGAR_V = -2.0

function META.buildHangar()
    local parts = {
        { BOX, 0.0, -0.45, 1.5, 6.4, 0.12, 5.4, "darkMetal" },
        { CYL, 0.0, -0.31, 0.0, 2.1, 0.012, 2.1, "hazard" },
        { CYL, 0.0, -0.3, 0.0, 1.9, 0.02, 1.9, "grey" },
        { BOX, 0.0, -0.325, -3.6, 6.4, 0.012, 0.09, "hazard" },
        { BOX, 0.0, 2.3, 6.7, 6.8, 2.9, 0.25, "boss" },
        { BOX, 0.0, 4.9, 6.45, 6.2, 0.05, 0.04, "padLight" },
        { BOX, 0.0, 0.25, 6.45, 6.2, 0.04, 0.04, "hazard" },
        { BOX, -6.6, 1.6, 3.2, 0.25, 2.2, 3.6, "boss" },
        { BOX, 6.6, 1.6, 3.2, 0.25, 2.2, 3.6, "boss" },
        { BOX, -4.5, 0.12, 4.3, 0.6, 0.55, 0.6, "olive" },
        { BOX, -3.4, 0.0, 4.9, 0.42, 0.42, 0.42, "oliveDark" },
        { BOX, -4.3, 0.95, 4.4, 0.4, 0.28, 0.4, "oliveDark" },
        { BOX, 4.6, 0.05, 4.0, 0.55, 0.48, 0.9, "navy" },
        { CYL, 5.3, 0.1, 1.6, 0.3, 0.52, 0.3, "red" },
        { CYL, 4.6, 0.1, 1.9, 0.3, 0.52, 0.3, "red" },
        { CYL, -5.4, -3.4, -3.5, 0.4, 3.0, 0.4, "grey" },
        { CYL, 5.4, -3.4, -3.5, 0.4, 3.0, 0.4, "grey" },
        { CYL, -5.4, -3.4, 6.0, 0.4, 3.0, 0.4, "grey" },
        { CYL, 5.4, -3.4, 6.0, 0.4, 3.0, 0.4, "grey" },
    }
    for k = 1, 5 do
        local x = -5.0 + (k - 1) * 2.5
        parts[#parts + 1] = { BOX, x, 2.3, 6.4, 0.14, 2.9, 0.14, "bossPlate" }
    end
    for k = 0, 7 do
        local a = k / 8 * TAU
        parts[#parts + 1] = { SPH, cos(a) * 2.3, -0.27, sin(a) * 2.3, 0.045, 0.045, 0.045, "padLight" }
    end
    META.hangar = assembly("hangar", parts)
end
-- (padLight: a glow for close up -- the game's own glows are made to read
-- from 40 m away and flood the picture from 6.)

function META.inHangar() return mode == "hangar" end

function META.enterHangar()
    mode, META.sel, noFire = "hangar", 2, true
    META.fadeAt = now
    META.hangar.active = true
    G.scrollTarget = 0.0
    clearField()
    cue("item")
end

function META.leaveHangar()
    if META.hangar then META.hangar.active = false end
    G.scrollTarget = 1.0
    if SYS.land and game.setFocus then game.setFocus(5000.0, 10000.0)
    elseif game.setFocus then game.setFocus() end
end

function META.toMenu(which)
    META.leaveHangar()
    mode, noFire, paused = which, true, false
    META.sel = (which == "missions") and META.missionSel or 1
    META.fadeAt = now
    player.obj.active = false
end

-- The camera: across the deck at the ship, turning slowly on its pad.
function META.hangarCam()
    local hv = META.HANGAR_V
    local ex, ey, ez = W(-2.7, hv - 4.7, FLY_Y + 2.3)
    local tx, ty, tz = W(0.35, hv + 0.2, FLY_Y + 0.05)
    return ex, ey, ez, tx, ty, tz
end

function META.stepHangar(dt)
    local p = player
    p.alive, p.firing, p.focus = false, false, false
    p.obj.active, p.obj.blinkOff = true, false
    p.hitbox.active, p.shield.active = false, false
    p.u, p.v, p.roll = 0.0, META.hangarV or META.HANGAR_V, 0.0
    place(META.hangar, 0.0, META.HANGAR_V, FLY_Y)
    if game.setFocus then
        local d = 5.9 * RS
        game.setFocus(d * 1.25, d * 4.0)
    end
end

-- --- Menus ------------------------------------------------------------------------------------------

function META.buy(u)
    local t = META.tier(u.key)
    if t >= 3 then cue("hit", 0.1) return end
    local c = u.cost[t + 1]
    if META.P.credits < c then
        META.denyAt = now
        cue("hit", 0.1)
        return
    end
    META.P.credits = META.P.credits - c
    META.P.up[u.key] = t + 1
    META.boughtAt, META.boughtKey = now, u.key
    META.looksDirty = true
    META.save()
    cue("power")
    FX.flash(0.12, 0.4, 0.8, 1.0)
end

-- Up/down move, enter picks, backspace goes back. Nothing needs speed or aim:
-- a menu that forgives a slow or unsteady hand costs nobody anything.
function META.input(upHit, downHit, leftHit, rightHit, enterHit, backHit)
    local function move(n)
        if upHit then META.sel = (META.sel - 2) % n + 1 cue("item", 0.05) end
        if downHit then META.sel = META.sel % n + 1 cue("item", 0.05) end
    end
    if mode == "title" then
        move(#META.TITLE_ITEMS)
        if enterHit then
            if META.sel == 1 then META.launch(nil)
            elseif META.sel == 2 then META.toMenu("missions") cue("item")
            else META.enterHangar() end
        end
    elseif mode == "missions" then
        local n = #META.MISSIONS + 1
        move(n)
        META.missionSel = META.sel
        if backHit or (enterHit and META.sel == n) then META.toMenu("title") cue("item") return end
        if enterHit then
            local m = META.MISSIONS[META.sel]
            if META.P.cleared[m.id] and META.reachable(m) then META.launch(m)
            else META.denyAt = now cue("hit", 0.1) end
        end
    elseif mode == "hangar" then
        local n = #META.UPGRADES + 2          -- the gun, the upgrades, back
        move(n)
        if backHit or (enterHit and META.sel == n) then META.toMenu("title") cue("item") return end
        if META.sel == 1 and (leftHit or rightHit or enterHit) then
            SYS.weapon = (SYS.weapon == 1) and 2 or 1
            META.save()
            cue("item")
        elseif enterHit and META.sel >= 2 and META.sel < n then
            META.buy(META.UPGRADES[META.sel - 1])
        end
    end
end

-- --- Drawing ------------------------------------------------------------------------------------------

function META.fadeIn(t0, dur) return clamp((now - (t0 or 0)) / (dur or 0.35), 0.0, 1.0) end

function META.credits(x, y, size, align)
    txt(x, y, fmt(META.P.credits) .. " CR", size, C_GOLD, 1.0, align or 0.0, true)
end

function META.drawTitle(xl, xr)
    local cx = (xl + xr) * 0.5
    game.hudGradient(xl, 0, xr - xl, view.h, 0.0, 0.02, 0.06, 0.15, 0.0, 0.02, 0.06, 0.7)
    local y = 200
    for i = 1, 12 do
        local a = i / 12 * TAU
        txt(cx + cos(a) * 5, y + sin(a) * 5, "SKYSTRIKE", 130, C_CYAN, 0.12, 0.5, true)
    end
    txt(cx, y, "SKYSTRIKE", 130, { 1.0, 1.0, 1.0 }, 1.0, 0.5, true)
    txt(cx, y + 150, "V E R T I C A L    A S S A U L T", 22, C_CYAN, 0.95, 0.5, true)
    for i, name in ipairs(META.TITLE_ITEMS) do
        local on = META.sel == i
        local ry = 430 + (i - 1) * 78
        rect(cx - 230, ry, 460, 62, on and { 0.05, 0.12, 0.2 } or { 0.02, 0.04, 0.08 }, on and 0.88 or 0.5, 8)
        frameBox(cx - 230, ry, 460, 62, on and C_GOLD or C_DIM, on and 1.0 or 0.35, on and 2.5 or 1.5, 8)
        txt(cx, ry + 12, name, 34, on and C_GOLD or C_TEXT, on and 1.0 or 0.75, 0.5, true)
    end
    META.credits(cx, 680, 30, 0.5)
    txt(cx, 726, "HI-SCORE   " .. fmt(hiScore), 22, C_TEXT, 0.8, 0.5, true)
    txt(cx, 780, "UP / DOWN  choose     ENTER  go", 20, C_DIM, 0.85, 0.5, true)
    local rows = { { "ARROWS / WASD", "FLY" }, { "SHIFT", "FOCUS" }, { "SPACE / J", "FIRE" },
                   { "B / X / K", "BOMB" }, { "P", "PAUSE" } }
    for i, r in ipairs(rows) do
        local ry = 840 + (i - 1) * 30
        txt(cx - 16, ry, r[1], 19, C_DIM, 0.8, 1.0, true)
        txt(cx + 16, ry, r[2], 19, C_TEXT, 0.85, 0.0, true)
    end
end

function META.drawMissions(xl, xr)
    local cx = (xl + xr) * 0.5
    local a = META.fadeIn(META.fadeAt)
    game.hudGradient(xl, 0, xr - xl, view.h, 0.0, 0.02, 0.06, 0.55 * a, 0.0, 0.02, 0.06, 0.8 * a)
    txt(cx, 120, "MISSIONS", 80, C_TEXT, a, 0.5, true)
    txt(cx, 215, "Every mission cleared in the campaign can be flown again on its own.", 20, C_DIM, a, 0.5,
        false)
    local w = min(760, xr - xl - 60)
    for i, m in ipairs(META.MISSIONS) do
        local on = META.sel == i
        local open = META.P.cleared[m.id] and META.reachable(m)
        local ry = 270 + (i - 1) * 104
        rect(cx - w * 0.5, ry, w, 88, on and { 0.05, 0.12, 0.2 } or { 0.02, 0.04, 0.08 }, (on and 0.9 or 0.6) * a, 8)
        frameBox(cx - w * 0.5, ry, w, 88, on and (open and C_GOLD or C_RED) or C_DIM, (on and 1.0 or 0.35) * a,
                 on and 2.5 or 1.5, 8)
        txt(cx - w * 0.5 + 26, ry + 10, i .. "  " .. m.name, 34, open and C_TEXT or C_DIM, a, 0.0, true)
        txt(cx - w * 0.5 + 64, ry + 52, m.sub, 18, C_DIM, 0.9 * a, 0.0, false)
        local right = cx + w * 0.5 - 26
        if open then
            txt(right, ry + 14, "CLEARED", 22, C_GREEN, a, 1.0, true)
            txt(right, ry + 48, "BEST  " .. fmt(META.P.best[m.id] or 0), 20, C_GOLD, 0.9 * a, 1.0, true)
        elseif META.P.cleared[m.id] then
            txt(right, ry + 30, "IN ANOTHER SCENE", 20, C_DIM, a, 1.0, true)
        else
            txt(right, ry + 30, "LOCKED", 24, C_DIM, a, 1.0, true)
        end
    end
    local by = 270 + #META.MISSIONS * 104
    local on = META.sel == #META.MISSIONS + 1
    frameBox(cx - 120, by, 240, 54, on and C_GOLD or C_DIM, (on and 1.0 or 0.35) * a, on and 2.5 or 1.5, 8)
    txt(cx, by + 11, "BACK", 28, on and C_GOLD or C_TEXT, a, 0.5, true)
    local deny = META.denyAt and now - META.denyAt < 1.2
    txt(cx, by + 80, deny and "Clear this one in the campaign first" or "ENTER  fly     BACKSPACE  back", 22,
        deny and C_RED or C_DIM, a, 0.5, true)
end

function META.drawHangar()
    local a = META.fadeIn(META.fadeAt, 0.6)
    -- The swing down from the top-down view happens behind this.
    if a < 1.0 then rect(0, 0, view.w, view.h, { 0.0, 0.0, 0.0 }, 1.0 - a) end
    local s = clamp(view.h / 1080.0, 0.7, 1.0)
    -- Left: the pilot's credits and what the ship now is.
    local lx, lw = 40, 520
    game.hudGradient(0, 0, lx + lw + 30, view.h, 0.01, 0.03, 0.07, 0.86, 0.01, 0.02, 0.05, 0.55)
    txt(lx, 40, "HANGAR", 72, C_TEXT, 1.0, 0.0, true)
    label(lx, 140, 1.0, "CREDITS")
    local buyFlash = META.boughtAt and now - META.boughtAt < 0.5
    META.credits(lx, 168, 54, 0.0)
    if META.denyAt and now - META.denyAt < 1.2 then
        txt(lx, 236, "NOT ENOUGH CREDITS", 24, C_RED, 1.0, 0.0, true)
    elseif buyFlash then
        txt(lx, 236, "INSTALLED", 24, C_GREEN, 1.0, 0.0, true)
    end
    label(lx, 300, 1.0, "THE SHIP")
    local stats = {
        { "Main gun", SYS.weapon == 1 and "VULCAN" or "LASER" },
        { "Starts at", ({ "TWIN", "SPREAD", "OPTIONS", "MISSILES" })[META.basePower()] },
        { "Fire rate", string.format("%d %%", floor(100 / META.rateK() + 0.5)) },
        { "Missiles", META.missileCd() and string.format("every %.2f s", META.missileCd()) or "at power 4" },
        { "Shield", META.shieldMax() > 0 and string.format("%d hit, %d s", META.shieldMax(), META.shieldTime()) or "none" },
        { "Speed", string.format("%d %%", floor(100 * META.speedK() + 0.5)) },
        { "Bombs", string.format("%d, carries %d", max(0, floor(bombs)) + META.tier("bombs"), META.maxBombs()) },
        { "Ships", tostring(max(1, floor(ships)) + META.tier("reserve")) },
        { "Credits", string.format("x %.2f", META.creditK()) },
    }
    for i, r in ipairs(stats) do
        local ry = 336 + (i - 1) * 42
        txt(lx, ry, r[1], 22, C_DIM, 0.95, 0.0, false)
        txt(lx + lw, ry, r[2], 22, C_TEXT, 1.0, 1.0, true)
    end
    txt(lx, view.h - 70, "UP / DOWN  choose     ENTER  buy     BACKSPACE  back", 19, C_DIM, 0.9, 0.0, false)

    -- Right: the shop.
    local rw = 700
    local rx = view.w - rw - 40
    game.hudGradient(rx - 30, 0, rw + 70, view.h, 0.01, 0.03, 0.07, 0.55, 0.01, 0.03, 0.07, 0.88)
    local rowH = 84
    local y0 = 40
    -- The gun: free, either way.
    local gOn = META.sel == 1
    rect(rx, y0, rw, 70, gOn and { 0.05, 0.12, 0.2 } or { 0.02, 0.04, 0.08 }, gOn and 0.9 or 0.6, 8)
    frameBox(rx, y0, rw, 70, gOn and C_GOLD or C_DIM, gOn and 1.0 or 0.35, gOn and 2.5 or 1.5, 8)
    txt(rx + 22, y0 + 18, "MAIN GUN", 28, C_TEXT, 1.0, 0.0, true)
    for i, gname in ipairs({ "VULCAN", "LASER" }) do
        local on = SYS.weapon == i
        local gx = rx + rw - 330 + (i - 1) * 160
        rect(gx, y0 + 14, 148, 42, on and C_CYAN or C_DIM, on and 0.85 or 0.15, 6)
        txt(gx + 74, y0 + 22, gname, 22, on and { 0.02, 0.08, 0.14 } or C_DIM, 1.0, 0.5, true)
    end
    for i, u in ipairs(META.UPGRADES) do
        local row = i + 1
        local on = META.sel == row
        local ry = y0 + 70 + 14 + (i - 1) * rowH
        local t = META.tier(u.key)
        local cost = u.cost[t + 1]
        local afford = cost and META.P.credits >= cost
        local lit = META.boughtKey == u.key and META.boughtAt and now - META.boughtAt < 0.6
        rect(rx, ry, rw, rowH - 10, lit and { 0.1, 0.3, 0.2 } or (on and { 0.05, 0.12, 0.2 } or { 0.02, 0.04, 0.08 }),
             on and 0.92 or 0.6, 8)
        frameBox(rx, ry, rw, rowH - 10, on and C_GOLD or C_DIM, on and 1.0 or 0.3, on and 2.5 or 1.5, 8)
        txt(rx + 22, ry + 8, u.name, 26, t > 0 and C_TEXT or { 0.8, 0.84, 0.9 }, 1.0, 0.0, true)
        for k = 1, 3 do
            rect(rx + 22 + (k - 1) * 40, ry + 46, 32, 12, k <= t and C_CYAN or C_DIM, k <= t and 0.95 or 0.25, 3)
        end
        txt(rx + 150, ry + 40, t < 3 and u.tiers[t + 1] or "Fully upgraded", 18,
            on and C_TEXT or C_DIM, 0.95, 0.0, false)
        if cost then
            txt(rx + rw - 22, ry + 10, fmt(cost) .. " CR", 26, afford and C_GOLD or C_RED, 1.0, 1.0, true)
        else
            txt(rx + rw - 22, ry + 10, "MAX", 26, C_GREEN, 1.0, 1.0, true)
        end
    end
    local by = y0 + 70 + 14 + #META.UPGRADES * rowH + 6
    local bOn = META.sel == #META.UPGRADES + 2
    frameBox(rx, by, rw, 56, bOn and C_GOLD or C_DIM, bOn and 1.0 or 0.35, bOn and 2.5 or 1.5, 8)
    txt(rx + rw * 0.5, by + 12, "BACK", 28, bOn and C_GOLD or C_TEXT, 1.0, 0.5, true)
end

-- What a run paid, on the game-over screen and at a stage clear.
function META.drawEarned(cx, y, a)
    if not G.earned or G.earned <= 0 then return end
    txt(cx, y, "+" .. fmt(G.earned) .. " CR     TOTAL " .. fmt(META.P.credits) .. " CR", 28, C_GOLD, a, 0.5, true)
end

-- --- Lifecycle ---------------------------------------------------------------------------------

local ATTRACT = { { "dart", "hook" }, { "saucer", "arc" }, { "dart", "loop" }, { "saucer", "sweep" },
                  { "dart", "zig" }, { "dart", "cross" } }

function start(e)
    BOX, SPH, CYL, EMP = game.BOX, game.SPHERE, game.CYLINDER, game.EMPTY
    PLN = game.PLANE or 8
    KEYS = {
        left  = { game.KEY_LEFT, game.KEY_A },  right = { game.KEY_RIGHT, game.KEY_D },
        up    = { game.KEY_UP, game.KEY_W },    down  = { game.KEY_DOWN, game.KEY_S },
        focus = { game.KEY_LSHIFT, game.KEY_RSHIFT },
        fire  = { game.KEY_SPACE, game.KEY_J },
        bomb  = { game.KEY_B, game.KEY_X, game.KEY_K },
    }
    -- The sea floats a little above the carrier: level with the ground it
    -- would fight the terrain for every pixel.
    OX, OZ, BY = e.x, e.z, e.y + 2.0
    -- Over a scene with a terrain of its own the flight starts at the carrier
    -- and goes on up the screen (world -z) over that ground (see SYS.LAND).
    SYS.land = landscape and game.find ~= nil and game.find("Terrain") ~= nil
    if SYS.land then
        RS = SYS.LAND.scale
        BY = SYS.groundTop()
        SYS.startZ = OZ
        STAGES = SYS.LAND_STAGES
        -- The ground lies 200 m below the camera, far past the view's own
        -- focus: all of it sharp.
        if game.setFocus then game.setFocus(5000.0, 10000.0) end
    end
    math.randomseed(math.floor(os.time()))
    for key, def in pairs(MAT_DEFS) do
        local t = { name = "Shmup " .. key }
        for k, v in pairs(def) do t[k] = v end
        mats[key] = game.createMaterial(t)
    end
    -- A file just written by the generators may not be in the asset list of an
    -- editor that was already open: one rescan if anything is missing.
    local missing = false
    for _, def in pairs(TEX_DEFS) do
        if not game.findAsset(def.texture, "Texture") then missing = true end
    end
    for _, groups in pairs(CUES) do
        if not game.findAsset(groups[1][1], "Sound") then missing = true end
    end
    if missing and game.refreshAssets then game.refreshAssets() end
    for key, def in pairs(TEX_DEFS) do
        hasTex[key] = game.findAsset(def.texture, "Texture") ~= nil
        local t = { name = "Shmup " .. key }
        for k, v in pairs(hasTex[key] and def or (def.plain or MAT_DEFS.cloud)) do
            if k ~= "plain" then t[k] = v end
        end
        mats[key] = game.createMaterial(t)
    end
    for name, groups in pairs(CUES) do
        for _, group in ipairs(groups) do
            local found = {}
            for _, f in ipairs(group) do
                if game.findAsset(f, "Sound") then found[#found + 1] = f end
            end
            if #found > 0 then cueFile[name] = found break end
        end
    end

    objects, enemies, enemyPools, beams, pools = {}, {}, {}, {}, {}
    world = { sea = {}, islands = {}, clouds = {}, haze = {} }
    buildWorld()
    buildPlayer()
    META.buildLooks(player.obj)
    buildPools()
    META.buildHangar()
    for i, s in ipairs(enemyPools.sentinel.items) do s.beamObj = beams[i] end
    enemyPools.tempest.items[1].beamPairs = { beams[4], beams[5] }
    enemyPools.bastion.items[1].beamPairs = { beams[4] }
    pools.rain = newPool(60, function() return single("rain", BOX, 0.015, 0.8, 0.015, "rain", { 12, 0, 0 }) end)
    SYS.planGround(STAGES[1])
    game.setCamera(-1)
    if game.setCrosshair then game.setCrosshair(false) end
    loadHi()
    META.load()
    resetGame()
    mode = "title"
    noFire = true
    SYS.takeOver()
end

function update(e, dt, t)
    now = t
    frameNo = frameNo + 1
    dtReal = min(dt, 1.0 / 30.0)
    if now > slowUntil then timeScale = 1.0 end
    local gdt = paused and 0.0 or dtReal * timeScale
    G.clock = G.clock + gdt

    local startHit = pressed("start", game.keyDown(game.KEY_ENTER))
    local bombHit  = pressed("bomb", anyDown(KEYS.bomb))
    local pauseHit = pressed("pause", game.keyDown(game.KEY_P))

    local leftHit = pressed("wl", anyDown(KEYS.left))
    local rightHit = pressed("wr", anyDown(KEYS.right))
    local upHit = pressed("mu", anyDown(KEYS.up))
    local downHit = pressed("md", anyDown(KEYS.down))
    local backHit = pressed("back", game.keyDown(game.KEY_BACKSPACE or 259))
    META.input(upHit, downHit, leftHit, rightHit, startHit, backHit)
    META.applyLooks()
    if startHit and mode == "over" and now - G.overAt > 1.5 then
        META.toMenu("title")
    elseif startHit and mode == "continue" then
        SYS.continueGame()
    end
    if mode == "play" and pauseHit then paused = not paused end

    SYS.stepScroll(gdt)
    stepWorld(gdt)
    SYS.stepWeather(gdt)
    if not paused then
        -- Delayed calls (squadron members, boss debris, the respawn).
        local rest, due = {}, {}
        for _, q in ipairs(queue) do
            if G.clock >= q.at then due[#due + 1] = q else rest[#rest + 1] = q end
        end
        queue = rest
        for _, q in ipairs(due) do q.fn() end
    end
    if mode == "play" then
        if not paused then
            stepStage(gdt)
            stepPlayer(gdt)
            if bombHit then useBomb() end
            stepBomb(gdt)
            stepOptions(gdt)
            stepShots(gdt)
            SYS.stepLasers(gdt)
            stepEnemies(gdt)
            stepBullets(gdt)
            stepItems(gdt)
            checkExtend()
            if G.chain > 0 and G.clock - G.chainAt > CHAIN_WINDOW then G.chain = 0 end
        end
    else
        if mode == "hangar" then
            META.stepHangar(gdt)
        elseif mode == "title" or mode == "missions" then
            stepDemo(gdt)
            G.attractCd = (G.attractCd or 1.0) - gdt
            if G.attractCd <= 0.0 then
                G.attractCd = 3.2
                local pick = ATTRACT[math.random(1, #ATTRACT)]
                WAVE.squad({ kind = pick[1], path = pick[2], n = 5, mirror = math.random() < 0.5 })
            end
        else
            player.obj.active = false
            player.hitbox.active, player.shield.active = false, false
            if mode == "continue" then
                G.contT = G.contT - dtReal
                if G.contT <= 0.0 then gameOver() end
            end
        end
        stepOptions(gdt)
        stepShots(gdt)
        stepEnemies(gdt)
        stepBullets(gdt)
        stepItems(gdt)
    end
    stepEffects(gdt)
    SYS.stepFx(gdt)
    SYS.stepBlazes(gdt)
    SYS.stepWrecks(gdt)
    flash = max(0.0, flash - dtReal * 2.2)
    drawPlayer()
    SYS.playerTrail(gdt)
    syncVisibility()
    driveCamera(dtReal)
    HUD.draw()
end
