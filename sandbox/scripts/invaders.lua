-- SPACE INVADERS -- the 1978 arcade game, entirely in Lua.
--
-- Put this script on ONE object (an Empty is best) and press Play. The screen,
-- the rack of 55 invaders, the four bunkers, your laser base and the mystery
-- ship are all spawned around it and removed again when Play stops.
--
--   Left / Right, A / D   move
--   Space / Up / W        fire
--   P                     pause
--   Enter                 start / play again
--
-- The rules are the arcade's. The rack marches one invader per tick, bottom
-- left first -- that is what makes it ripple, and why it gets faster the fewer
-- are left: the last one moves every tick. When the rack touches a side it
-- steps down and turns. It stops while an invader explodes. You get one shot
-- on screen; they get three, one of each kind: the rolling shot aims at the
-- column above you, the plunger and the squiggly one take their columns from
-- the arcade's tables. The squiggly shot and the mystery ship share a slot, so
-- neither shows up while the other is out. The mystery ship's score follows
-- your shot count -- the 23rd shot and every 15th after it is worth 300.
-- Invaders that reach the bunkers eat them; invaders that reach your row end
-- the game, whatever lives are left. Every wave starts lower, and the bunkers
-- are rebuilt. One extra base at bonusLife points.
--
-- Everything is a voxel model: the sprites are the pixel art, every run of
-- pixels merged into one box. An invader's two animation frames share what
-- is the same and keep the rest under one Empty each, switched per step.
-- Everything is pooled -- game.spawn only delivers at the end of the frame --
-- and switched with setActive. Collisions are rectangles in arcade pixels.
--
-- The camera is driven every frame. Make sure no Camera entity in the scene is
-- "active on start" -- that one would take the view back.

-- --- Inspector parameters (globals = editable fields on the Script component) --
ships       = 3       -- laser bases per game
bonusLife   = 1500    -- one extra base at this score (0 = never)
difficulty  = 1.0     -- scales how fast and how often the invaders shoot back
marchSpeed  = 1.0     -- 1.0 = the arcade's 60 invader steps a second
playerSpeed = 70.0    -- arcade pixels per second (the arcade: 60)
holdToFire  = true    -- holding fire re-fires once the shot is gone (the arcade: press every time)
colorRows   = true    -- coloured rows; false = white invaders as on the 1978 screen
marchBeat   = true    -- a thump every time the whole rack has stepped
camZoom     = 1.0     -- 1 = the arcade screen just fills the view's height
camRise     = 2.0     -- camera height above the screen's middle, metres
camFov      = 60.0    -- the distance follows it, so the screen always fits
sound       = true

-- --- Layout, in arcade pixels -------------------------------------------------------
-- u runs across the 224-pixel screen, v up it; d is depth, towards the camera.
-- One arcade pixel is PX world metres.
local PX         = 0.1
local FIELD_W    = 224
local GROUND_V   = 18          -- the green line
local PLAYER_V   = 24          -- bottom of the laser base
local BUNKER_V   = 44          -- bottom of the bunkers
local BUNKER_U   = { 44, 89, 135, 180 }
local UFO_V      = 212
local TOP_V      = 232         -- a shot that gets here has missed
local RACK_V     = 120         -- bottom row at the start of the first wave
local RACK_DROPS = { 0, 16, 24, 32, 32, 32, 40, 40, 40 }   -- later waves start lower
local LEFT_EDGE, RIGHT_EDGE = 8, 216
local BACK_D     = -30         -- the backdrop, 3 m behind the screen
local VOXEL_D    = 1.0         -- half depth of a voxel
local TWIST      = 8.0         -- degrees an invader turns per step: shows it is solid
local SHOT_SPEED = 240.0       -- the arcade: 4 pixels a frame
local UFO_SPEED  = 48.0
local UFO_EVERY  = 25.0
local UFO_TABLE  = { 100, 50, 50, 100, 150, 100, 100, 50, 300, 100, 100, 100, 50, 150, 100 }
local PLUNGER_COLS  = { 1, 7, 1, 1, 1, 4, 11, 1, 6, 3, 1, 1, 11, 9, 2, 8 }
local SQUIGGLY_COLS = { 11, 1, 6, 3, 1, 1, 11, 9, 2, 8, 2, 11, 4, 7, 10 }
local POINTS     = { squid = 30, crab = 20, octopus = 10 }
local ROW_KIND   = { [0] = "octopus", "octopus", "crab", "crab", "squid" }   -- bottom row first

-- --- Pixel art (top row first) -------------------------------------------------------
local ART = {
    squid = {
        { "...##...", "..####..", ".######.", "##.##.##", "########", "..#..#..", ".#.##.#.", "#.#..#.#" },
        { "...##...", "..####..", ".######.", "##.##.##", "########", ".#.##.#.", "#......#", ".#....#." },
    },
    crab = {
        { "..#.....#..", "...#...#...", "..#######..", ".##.###.##.", "###########", "#.#######.#", "#.#.....#.#", "...##.##..." },
        { "..#.....#..", "#..#...#..#", "#.#######.#", "###.###.###", "###########", ".#########.", "..#.....#..", ".#.......#." },
    },
    octopus = {
        { "....####....", ".##########.", "############", "###..##..###", "############", "...##..##...", "..##.##.##..", "##........##" },
        { "....####....", ".##########.", "############", "###..##..###", "############", "..###..###..", ".##..##..##.", "..##....##.." },
    },
    cannon = {
        { "......#......", ".....###.....", ".....###.....", ".###########.", "#############", "#############", "#############", "#############" },
    },
    wreck = {
        { "...#.....#...", "#.....#.....#", "..#.#...#.#..", ".....###.....", "..#.#####....", ".###########.", "#.#########.#", "#############" },
        { "#.....#.....#", "...#.....#...", ".#...#.#...#.", "....#####.#..", ".#.#######...", "..#########.#", "#.#########..", ".###########." },
    },
    ufo = {
        { ".....######.....", "...##########...", "..############..", ".##.##.##.##.##.", "################", "..###..##..###..", "...#........#..." },
    },
    rolling  = { { ".#.", ".##", ".#.", "##.", ".#.", ".##", ".#." }, { ".#.", "##.", ".#.", ".##", ".#.", "##.", ".#." } },
    plunger  = { { ".#.", ".#.", "###", ".#.", ".#.", ".#.", ".#." }, { ".#.", ".#.", ".#.", ".#.", "###", ".#.", ".#." } },
    squiggly = { { "#..", ".#.", "..#", ".#.", "#..", ".#.", "..#" }, { "..#", ".#.", "#..", ".#.", "..#", ".#.", "#.." } },
}
-- Each bunker cell is 2x2 arcade pixels: 22 x 16 like the arcade's, but coarser.
local BUNKER_ART = { "...#####...", "..#######..", ".#########.", "###########",
                     "###########", "###########", "####...####", "###.....###" }

-- Emission carries the look, and it is kept low: the screen is nearly all
-- black, so the auto exposure opens right up to its limit, and at twice these
-- strengths the rows bloomed into solid bands of colour.
local MAT_DEFS = {
    -- Black METAL: a black dielectric still mirrors 4 % of the sun (F0), and
    -- that turned the whole sky brown. A metal's F0 is its colour -- nothing.
    space   = { color = {0.0, 0.0, 0.0}, roughness = 1.0, reflectivity = 1.0 },
    bezel   = { color = {0.03, 0.035, 0.05}, emission = {0.25, 0.3, 0.5}, emissionStrength = 0.3 },
    star1   = { color = {0.5, 0.5, 0.55}, emission = {0.8, 0.85, 1.0}, emissionStrength = 1.5 },
    star2   = { color = {0.5, 0.45, 0.35}, emission = {1.0, 0.85, 0.6}, emissionStrength = 1.2 },
    star3   = { color = {0.35, 0.4, 0.5}, emission = {0.6, 0.75, 1.0}, emissionStrength = 1.0 },
    squid   = { color = {0.45, 0.12, 0.38}, emission = {1.0, 0.12, 0.7}, emissionStrength = 1.5, roughness = 0.4 },
    crab    = { color = {0.10, 0.38, 0.48}, emission = {0.3, 0.9, 1.0}, emissionStrength = 1.5, roughness = 0.4 },
    octopus = { color = {0.48, 0.34, 0.06}, emission = {1.0, 0.72, 0.2}, emissionStrength = 1.5, roughness = 0.4 },
    white   = { color = {0.5, 0.5, 0.52}, emission = {0.9, 0.9, 0.95}, emissionStrength = 1.3, roughness = 0.4 },
    cannon  = { color = {0.08, 0.45, 0.14}, emission = {0.2, 1.0, 0.3}, emissionStrength = 1.4, roughness = 0.35 },
    bunker  = { color = {0.08, 0.40, 0.12}, emission = {0.15, 0.9, 0.25}, emissionStrength = 0.9, roughness = 0.6 },
    ground  = { color = {0.08, 0.40, 0.12}, emission = {0.15, 0.9, 0.25}, emissionStrength = 1.3 },
    ufo     = { color = {0.55, 0.04, 0.04}, emission = {1.0, 0.1, 0.08}, emissionStrength = 2.2, roughness = 0.3 },
    shot    = { color = {0.6, 0.9, 0.7}, emission = {0.7, 1.0, 0.8}, emissionStrength = 3.5 },
    bomb    = { color = {0.6, 0.5, 0.3}, emission = {1.0, 0.8, 0.45}, emissionStrength = 3.0 },
    boom    = { color = {0.6, 0.55, 0.45}, emission = {1.0, 0.9, 0.7}, emissionStrength = 3.5 },
}
local mats = {}

local CUE_FILES = {
    fire = "shot.wav", kill = "missile_hit.wav", die = "impact.wav", ufo = "missile_seek.wav",
    ufoHit = "missile_lock.wav", beat = "plopp.wav", extra = "swoosh.wav",
}
local cues, lastCue = {}, {}

-- --- State -------------------------------------------------------------------------
local OX, OZ, BY = 0.0, 0.0, 0.0
local now, frameNo = 0.0, 0
local objects = {}          -- everything that is switched on and off: { root, active, shown }
local rack = {}             -- the 55 invaders, bottom left first: the order they march in
local bunkers = {}          -- { u0, grid[row][col] = cell }, row 0 at the bottom
local cells = {}
local sparks, booms = nil, nil
local player, pshot, ufo, wreck = nil, nil, nil, nil
local alienShots = {}       -- rolling, plunger, squiggly
local lifeIcons = {}
local starMats = {}

local mode = "title"        -- title / play / over
local paused = false
local score, hiScore, lives, wave = 0, 0, 3, 1
local bonusGiven = false
local aliveCount = 0
local marchDir, dropPass, cursor, marchAcc, marchHold = 1, false, 0, 0.0, 0.0
local reloadT, fireTurn, plungerIdx, squigglyIdx = 1.0, 1, 1, 1
local shotCount, ufoTimer = 0, UFO_EVERY
local clearAt, landed = -1.0, false
local message, messageUntil = "", 0.0
local keysPrev = {}

-- --- Helpers -----------------------------------------------------------------------

local function cue(name, gap)
    if not sound then return end
    local f = CUE_FILES[name]
    if not f or not cues[f] then return end
    if lastCue[name] and now - lastCue[name] < (gap or 0.05) then return end
    lastCue[name] = now
    game.playSound(f)
end

local function say(text, secs) message, messageUntil = text, now + (secs or 2.0) end
local function rand(a, b) return a + (b - a) * math.random() end
local function clamp(x, a, b) if x < a then return a elseif x > b then return b end return x end

-- Arcade pixel -> world point. The screen stands upright, facing +z.
local function W(u, v, d) return OX + (u - FIELD_W * 0.5) * PX, BY + v * PX, OZ + (d or 0.0) * PX end

-- Out of sight behind the backdrop until a pool hands the thing out.
local function parkedAt() return W(FIELD_W * 0.5, 120.0, BACK_D - 40.0) end

local function spawn(kind, x, y, z, hx, hy, hz, mat, parent, name, rz)
    return game.spawn{
        type = kind, x = x, y = y, z = z, sx = hx, sy = hy, sz = hz, rz = rz or 0.0,
        material = mat and mats[mat] or nil, parent = parent or -1,
        physics = game.PHYSICS_NONE, name = name or "invaders",
    }
end

local function take(pool)
    local n = #pool.items
    for k = 0, n - 1 do
        local i = (pool.cursor - 1 + k) % n + 1
        local it = pool.items[i]
        if not it.active then
            pool.cursor = i % n + 1
            it.active = true
            return it
        end
    end
    return nil
end

-- Switched only when the wanted state changes, and only from the second frame:
-- before that the pooled entities do not exist in the scene yet.
local function syncVisibility()
    if frameNo < 2 then return end
    for _, o in ipairs(objects) do
        local want = o.active and not o.blinkOff
        if want ~= o.shown then
            game.setActive(o.root, want)
            o.shown = want
        end
        if want and o.frames then
            for i = 1, 2 do
                local f = (o.frame == i)
                if f ~= o.frameShown[i] then
                    game.setActive(o.frames[i], f)
                    o.frameShown[i] = f
                end
            end
        end
    end
end

-- --- Voxel sprites -------------------------------------------------------------------

local function parse(rows)
    local on = {}
    for y, row in ipairs(rows) do
        assert(#row == #rows[1], "ragged sprite row: " .. row)
        on[y] = {}
        for x = 1, #row do on[y][x] = row:sub(x, x) == "#" end
    end
    return on
end

local function combine(a, b, f)
    local out = {}
    for y = 1, #a do
        out[y] = {}
        for x = 1, #a[y] do out[y][x] = f(a[y][x], b[y][x]) end
    end
    return out
end

-- Greedy cover: the widest run first, then as far down as the whole run goes.
local function cover(on, w, h)
    local used, out = {}, {}
    for y = 1, h do used[y] = {} end
    local function free(x, y) return on[y][x] and not used[y][x] end
    for y = 1, h do
        for x = 1, w do
            if free(x, y) then
                local x1 = x
                while x1 < w and free(x1 + 1, y) do x1 = x1 + 1 end
                local y1 = y
                while y1 < h do
                    local ok = true
                    for xx = x, x1 do if not free(xx, y1 + 1) then ok = false; break end end
                    if not ok then break end
                    y1 = y1 + 1
                end
                for yy = y, y1 do for xx = x, x1 do used[yy][xx] = true end end
                out[#out + 1] = { x = x - 1, y = y - 1, w = x1 - x + 1, h = y1 - y + 1 }
            end
        end
    end
    return out
end

-- Every box is a draw call in every pass, so try the columns-first cover too
-- and keep whichever needs fewer.
local function coverBest(on, w, h)
    local rows = cover(on, w, h)
    local t = {}
    for x = 1, w do
        t[x] = {}
        for y = 1, h do t[x][y] = on[y][x] end
    end
    local cols = cover(t, h, w)
    if #cols >= #rows then return rows end
    for _, r in ipairs(cols) do r.x, r.y, r.w, r.h = r.y, r.x, r.h, r.w end
    return cols
end

-- An Empty root at the sprite's centre with the voxels around it. With two
-- frames, what both share hangs on the root and the rest under one Empty each.
local function sprite(name, frames, mat, x, y, z)
    if not x then x, y, z = parkedAt() end
    local w, h = #frames[1][1], #frames[1]
    local o = { w = w, h = h, parts = {}, active = false, shown = true, frame = 1, mat = mat }
    o.root = spawn(game.EMPTY, x, y, z, 0.1, 0.1, 0.1, nil, nil, name)
    local function boxes(on, parent)
        for _, r in ipairs(coverBest(on, w, h)) do
            local cx = (r.x + r.w * 0.5 - w * 0.5) * PX
            local cy = (h * 0.5 - r.y - r.h * 0.5) * PX
            o.parts[#o.parts + 1] = spawn(game.BOX, cx, cy, 0.0, r.w * 0.5 * PX, r.h * 0.5 * PX,
                                          VOXEL_D * PX, mat, parent, name .. " voxels")
        end
    end
    local a = parse(frames[1])
    if frames[2] then
        local b = parse(frames[2])
        boxes(combine(a, b, function(p, q) return p and q end), o.root)
        o.frames, o.frameShown = {}, {}
        local own = { combine(a, b, function(p, q) return p and not q end),
                      combine(a, b, function(p, q) return q and not p end) }
        for i = 1, 2 do
            o.frames[i] = spawn(game.EMPTY, 0.0, 0.0, 0.0, 0.1, 0.1, 0.1, nil, o.root, name .. " frame")
            o.frameShown[i] = true
            boxes(own[i], o.frames[i])
        end
    else
        boxes(a, o.root)
    end
    objects[#objects + 1] = o
    return o
end

local function paint(o, mat)
    if o.mat == mat then return end
    for _, id in ipairs(o.parts) do game.setMaterial(id, mats[mat]) end
    o.mat = mat
end

local function single(name, kind, hx, hy, hz, mat, x, y, z)
    if not x then x, y, z = parkedAt() end
    local o = { root = spawn(kind, x, y, z, hx, hy, hz, mat, nil, name), active = false, shown = true }
    objects[#objects + 1] = o
    return o
end

-- --- Building ------------------------------------------------------------------------

local function invaderMat(kind) return colorRows and kind or "white" end

local function buildScreen()
    local cx, cy, cz = W(FIELD_W * 0.5, 120.0, BACK_D)
    spawn(game.BOX, cx, cy, cz, 70.0, 40.0, 0.2, "space", nil, "invaders backdrop")
    for i = 1, 30 do
        local k = math.random(1, 3)
        local x, y, z = W(rand(-300, 524), rand(-90, 330), BACK_D + 4.0)   -- just clear of its face
        local r = rand(0.03, 0.08)
        spawn(game.SPHERE, x, y, z, r, r, r * 0.3, "star" .. k, nil, "star")
    end
    -- The bezel: the edges of the arcade screen.
    local function bar(u, v, hu, hv)
        local x, y, z = W(u, v, -2.0)
        spawn(game.BOX, x, y, z, hu * PX, hv * PX, 0.5 * PX, "bezel", nil, "invaders bezel")
    end
    bar(-4, 121, 1, 125)
    bar(FIELD_W + 4, 121, 1, 125)
    bar(FIELD_W * 0.5, 246, FIELD_W * 0.5 + 5, 1)
    bar(FIELD_W * 0.5, -4, FIELD_W * 0.5 + 5, 1)
    local gx, gy, gz = W(FIELD_W * 0.5, GROUND_V + 0.5, 0.0)
    spawn(game.BOX, gx, gy, gz, FIELD_W * 0.5 * PX, 0.5 * PX, VOXEL_D * PX, "ground", nil, "invaders ground")
end

local function rackHome(a, drop)
    a.u = 24 + a.col * 16 + (16 - a.w) * 0.5
    a.v = RACK_V - drop + a.row * 16
end

local function buildRack()
    for row = 0, 4 do
        local kind = ROW_KIND[row]
        for col = 0, 10 do
            local a = { row = row, col = col, w = #ART[kind][1][1], h = 8 }
            rackHome(a, 0)
            local o = sprite(kind, ART[kind], invaderMat(kind), W(a.u + a.w * 0.5, a.v + 4, 0.0))
            for k, v in pairs(a) do o[k] = v end
            o.kind, o.active, o.alive = kind, true, true
            rack[#rack + 1] = o
        end
    end
    aliveCount = #rack
end

-- The cells are bookkeeping only. What is drawn is one box per run of intact
-- cells in a row (a row of 11 has at most 6 runs), laid out again whenever the
-- row changes: 40 boxes for four whole bunkers instead of 272.
local function buildBunkers()
    local on = parse(BUNKER_ART)
    for b, bu in ipairs(BUNKER_U) do
        local bk = { u0 = bu - 11, grid = {}, boxes = {}, dirty = {} }
        for y = 1, 8 do
            local row = 8 - y
            bk.grid[row], bk.boxes[row], bk.dirty[row] = {}, {}, true
            for x = 1, 11 do
                if on[y][x] then
                    local c = { alive = true, u = bk.u0 + (x - 1) * 2, v = BUNKER_V + row * 2,
                                bk = bk, row = row }
                    bk.grid[row][x - 1] = c
                    cells[#cells + 1] = c
                end
            end
            for i = 1, 6 do
                bk.boxes[row][i] = single("bunker", game.BOX, PX, PX, VOXEL_D * 1.3 * PX, "bunker")
            end
        end
        bunkers[b] = bk
    end
end

local function layoutRow(bk, row)
    local cellsOf, runs, col = bk.grid[row], {}, 0
    while col <= 10 do
        local c = cellsOf[col]
        if c and c.alive then
            local c0 = col
            while cellsOf[col + 1] and cellsOf[col + 1].alive do col = col + 1 end
            runs[#runs + 1] = { c0, col }
        end
        col = col + 1
    end
    for i, box in ipairs(bk.boxes[row]) do
        local r = runs[i]
        box.active = r ~= nil
        if r then
            local u, w = bk.u0 + r[1] * 2, (r[2] - r[1] + 1) * 2
            game.setPos(box.root, W(u + w * 0.5, BUNKER_V + row * 2 + 1, 0.0))
            game.setScale(box.root, w * 0.5 * PX, PX, VOXEL_D * 1.3 * PX)
        end
    end
end

local function breakCell(c)
    if c and c.alive then
        c.alive = false
        c.bk.dirty[c.row] = true
    end
end

local function buildActors()
    local o = sprite("laser base", ART.cannon, "cannon", W(FIELD_W * 0.5, PLAYER_V + 4, 0.0))
    o.active = true
    player = { obj = o, u = (FIELD_W - 13) * 0.5, alive = false, dying = 0.0 }
    wreck = sprite("wreck", ART.wreck, "cannon")
    for i = 1, 4 do
        lifeIcons[i] = sprite("spare base", ART.cannon, "cannon", W(18 + i * 17, 7, 0.0))
    end
    pshot = single("shot", game.BOX, 0.5 * PX, 2.0 * PX, 0.5 * PX, "shot")
    pshot.u, pshot.v = 0.0, 0.0
    local spins = { 600.0, 0.0, 200.0 }
    for i, kind in ipairs({ "rolling", "plunger", "squiggly" }) do
        local s = sprite(kind .. " shot", ART[kind], "bomb")
        s.kind, s.spin, s.u, s.v = kind, spins[i], 0.0, 0.0
        alienShots[i] = s
    end
    ufo = sprite("mystery ship", ART.ufo, "ufo")
    ufo.u, ufo.dir = 0.0, 1
    -- The arcade's explosion is a sparse star of pixels: here four rays that
    -- flare and fade, the debris doing the rest.
    booms = { items = {}, cursor = 1 }
    for i = 1, 6 do
        local x, y, z = parkedAt()
        local o = { parts = {}, active = false, shown = true, mat = "boom" }
        o.root = spawn(game.EMPTY, x, y, z, 0.1, 0.1, 0.1, nil, nil, "explosion")
        for r = 0, 3 do
            o.parts[r + 1] = spawn(game.BOX, 0.0, 0.0, 0.0, 6.5 * PX, 0.5 * PX, VOXEL_D * PX,
                                   "boom", o.root, "explosion ray", r * 45.0)
        end
        objects[#objects + 1] = o
        booms.items[i] = o
    end
    sparks = { items = {}, cursor = 1 }
    for i = 1, 80 do
        local s = single("debris", game.BOX, 0.8 * PX, 0.8 * PX, 0.8 * PX, "boom")
        s.mat = "boom"
        sparks.items[i] = s
    end
end

-- --- Effects -------------------------------------------------------------------------

-- Voxel debris: flies out of the screen at you, the one thing the arcade could not do.
local function burst(u, v, n, mat, speed, life)
    for i = 1, n do
        local s = take(sparks)
        if not s then return end
        if s.mat ~= mat then game.setMaterial(s.root, mats[mat]); s.mat = mat end
        local a = rand(0.0, math.pi * 2.0)
        local sp = rand(0.3, 1.0) * speed
        s.u, s.v, s.d = u + rand(-2, 2), v + rand(-2, 2), 0.0
        s.vu, s.vv, s.vd = math.cos(a) * sp, math.sin(a) * sp, rand(0.2, 1.0) * speed
        s.life = rand(0.45, 0.85) * (life or 1.0)
        s.max = s.life
        s.size = rand(0.5, 1.1)
        s.spinA, s.spinB = rand(-500, 500), rand(-500, 500)
    end
end

local function boom(u, v, mat, secs)
    local x = take(booms)
    if not x then return end
    paint(x, mat)
    x.u, x.v, x.t = u, v, secs or 0.27
    x.max, x.size = x.t, (secs and secs > 0.3) and 1.5 or 1.0
end

-- --- The rack ----------------------------------------------------------------------------

local function setupRack()
    local drop = RACK_DROPS[math.min(wave, #RACK_DROPS)]
    for _, a in ipairs(rack) do
        rackHome(a, drop)
        a.alive, a.active, a.frame, a.dirty = true, true, 1, true
    end
    aliveCount = #rack
    marchDir, dropPass, cursor, marchAcc, marchHold = 1, false, 0, 0.0, 0.0
end

local function placeInvader(a)
    game.setPos(a.root, W(a.u + a.w * 0.5, a.v + a.h * 0.5, 0.0))
    game.setRot(a.root, 0.0, a.frame == 1 and TWIST or -TWIST, 0.0)
end

local function restoreBunkers()
    for _, c in ipairs(cells) do
        c.alive = true
        c.bk.dirty[c.row] = true
    end
end

-- Invaders low enough to touch a bunker eat what they touch.
local function eatBunkers(a)
    if a.v > BUNKER_V + 16 then return end
    for _, c in ipairs(cells) do
        if c.alive and c.u + 2 > a.u and c.u < a.u + a.w and c.v + 2 > a.v and c.v < a.v + a.h then
            breakCell(c)
        end
    end
end

local killPlayer   -- forward

-- The pass just ended. Did anyone reach a side? Then the next pass steps the
-- whole rack down, and the one after marches the other way.
local function nextPass(allowDrop)
    local reach = false
    for _, a in ipairs(rack) do
        if a.alive and ((marchDir > 0 and a.u + a.w >= RIGHT_EDGE) or (marchDir < 0 and a.u <= LEFT_EDGE)) then
            reach = true
            break
        end
    end
    if reach then marchDir = -marchDir end
    dropPass = reach and allowDrop
    if marchBeat and mode == "play" then cue("beat", 0.09) end
end

-- One tick moves one invader: the next living one after the last that moved.
local function marchTick(allowDrop)
    local n = #rack
    for _ = 1, n do
        cursor = cursor + 1
        if cursor > n then
            cursor = 1
            nextPass(allowDrop)
        end
        local a = rack[cursor]
        if a.alive then
            if dropPass then a.v = a.v - 8 else a.u = a.u + 2 * marchDir end
            a.frame = 3 - a.frame
            a.dirty = true
            eatBunkers(a)
            if allowDrop and a.v <= PLAYER_V + 8 then
                landed = true
                killPlayer()
            end
            return
        end
    end
end

local function stepMarch(dt, allowDrop)
    if aliveCount == 0 then return end
    if marchHold > 0.0 then marchHold = marchHold - dt; return end
    local tick = 1.0 / (60.0 * math.max(0.1, marchSpeed))
    marchAcc = marchAcc + dt
    local n = 0
    while marchAcc >= tick and n < 6 and not landed do
        marchAcc = marchAcc - tick
        marchTick(allowDrop)
        n = n + 1
    end
    if n == 6 then marchAcc = 0.0 end
end

local function lowestIn(col)
    for row = 0, 4 do
        local a = rack[row * 11 + col + 1]
        if a.alive then return a end
    end
    return nil
end

-- --- Scoring -----------------------------------------------------------------------------

local function addScore(n)
    score = score + n
    if not bonusGiven and bonusLife > 0 and score >= bonusLife then
        bonusGiven = true
        lives = lives + 1
        say("EXTRA LASER BASE", 2.5)
        cue("extra")
    end
end

local function killInvader(a)
    a.alive, a.active = false, false
    aliveCount = aliveCount - 1
    addScore(POINTS[a.kind])
    local cu, cv = a.u + a.w * 0.5, a.v + 4
    boom(cu, cv, invaderMat(a.kind))
    burst(cu, cv, 9, invaderMat(a.kind), 90.0)
    cue("kill", 0.03)
    marchHold = 0.27               -- the rack waits while one of them explodes
    if aliveCount == 0 then
        clearAt = now + 2.2
        say("WAVE " .. wave .. " CLEARED", 2.0)
        for _, s in ipairs(alienShots) do s.active = false end
        ufo.active = false
    end
end

local function killUfo()
    ufo.active = false
    local pts = UFO_TABLE[shotCount % 15 + 1]
    addScore(pts)
    boom(ufo.u + 8, UFO_V + 3.5, "ufo", 0.5)
    burst(ufo.u + 8, UFO_V + 3.5, 16, "ufo", 110.0)
    say(string.format("MYSTERY SHIP  %d", pts), 2.0)
    cue("ufoHit")
end

-- --- Bunkers -------------------------------------------------------------------------------

-- A hit takes the cell and, by chance, some of its neighbours -- more of them
-- on the far side, where the shot was going.
local function erode(bk, col, row, dir)
    local function kill(x, y) breakCell(bk.grid[y] and bk.grid[y][x]) end
    kill(col, row)
    for dy = -1, 1 do
        for dx = -1, 1 do
            if (dx ~= 0 or dy ~= 0) and math.random() < ((dy == dir) and 0.55 or 0.22) then
                kill(col + dx, row + dy)
            end
        end
    end
    burst(bk.u0 + col * 2 + 1, BUNKER_V + row * 2 + 1, 3, "bunker", 60.0, 0.6)
end

local function hitBunker(u, v, dir)
    if v < BUNKER_V or v >= BUNKER_V + 16 then return false end
    for _, bk in ipairs(bunkers) do
        if u >= bk.u0 - 0.5 and u < bk.u0 + 22.5 then
            local col = clamp(math.floor((u - bk.u0) / 2), 0, 10)
            local row = math.floor((v - BUNKER_V) / 2)
            local c = bk.grid[row] and bk.grid[row][col]
            if c and c.alive then
                erode(bk, col, row, dir)
                return true
            end
            return false
        end
    end
    return false
end

-- --- Shots -------------------------------------------------------------------------------------

local function clearShots()
    pshot.active = false
    for _, s in ipairs(alienShots) do s.active = false end
end

local function fire()
    pshot.active = true
    pshot.u, pshot.v = player.u + 6.5, PLAYER_V + 8
    shotCount = shotCount + 1
    cue("fire", 0.05)
end

local function stepPlayerShot(dt)
    if not pshot.active then return end
    local dist = SHOT_SPEED * dt
    local n = math.max(1, math.ceil(dist / 2.0))
    for _ = 1, n do
        pshot.v = pshot.v + dist / n
        local u, tip = pshot.u, pshot.v + 4
        if tip >= TOP_V then
            pshot.active = false
            burst(u, TOP_V, 3, "shot", 50.0, 0.5)
            return
        end
        if hitBunker(u, tip, 1) then pshot.active = false; return end
        for _, s in ipairs(alienShots) do
            if s.active and math.abs(s.u - u) <= 2.0 and tip >= s.v and pshot.v <= s.v + 7 then
                s.active, pshot.active = false, false
                burst(u, tip, 5, "bomb", 70.0, 0.6)
                return
            end
        end
        for _, a in ipairs(rack) do
            if a.alive and u >= a.u - 0.5 and u <= a.u + a.w + 0.5 and tip >= a.v and pshot.v <= a.v + a.h then
                pshot.active = false
                killInvader(a)
                return
            end
        end
        if ufo.active and u >= ufo.u and u <= ufo.u + 16 and tip >= UFO_V then
            pshot.active = false
            killUfo()
            return
        end
    end
end

local function hitsPlayer(s)
    if not player.alive then return false end
    local h = s.v - PLAYER_V
    if h > 8 or s.v + 7 < PLAYER_V then return false end
    local c = s.u - player.u
    if h < 5 then return c >= -1.0 and c <= 14.0 end
    return c >= 4.0 and c <= 9.0            -- only the turret is up there
end

-- Rolling: the column nearest you. Plunger and squiggly: the next column in
-- their table that still has someone in it.
local function shooterFor(kind)
    if kind == "rolling" then
        local pc, best, bd = player.u + 6.5, nil, 1e9
        for col = 0, 10 do
            local a = lowestIn(col)
            if a then
                local d = math.abs(a.u + a.w * 0.5 - pc)
                if d < bd then best, bd = a, d end
            end
        end
        return best
    end
    local tbl = (kind == "plunger") and PLUNGER_COLS or SQUIGGLY_COLS
    for _ = 1, #tbl do
        local col
        if kind == "plunger" then
            col = tbl[plungerIdx]; plungerIdx = plungerIdx % #tbl + 1
        else
            col = tbl[squigglyIdx]; squigglyIdx = squigglyIdx % #tbl + 1
        end
        local a = lowestIn(col - 1)
        if a then return a end
    end
    return nil
end

local function reloadTime()
    local r = (score < 200 and 1.1) or (score < 1000 and 0.8) or (score < 2000 and 0.6)
              or (score < 3000 and 0.5) or 0.42
    return r * rand(0.8, 1.2) / math.max(0.25, difficulty)
end

local function stepAlienFire(dt)
    if aliveCount == 0 or not player.alive then return end
    reloadT = reloadT - dt
    if reloadT > 0.0 then return end
    for _ = 1, 3 do
        local s = alienShots[fireTurn]
        fireTurn = fireTurn % 3 + 1
        local blocked = s.active
            or (s.kind == "plunger" and aliveCount == 1)       -- the last one only rolls
            or (s.kind == "squiggly" and ufo.active)          -- they share a slot
        if not blocked then
            local a = shooterFor(s.kind)
            if a then
                s.active, s.u, s.v = true, a.u + a.w * 0.5, a.v - 7
                reloadT = reloadTime()
                return
            end
        end
    end
end

local function stepAlienShots(dt)
    local speed = ((aliveCount <= 8) and 100.0 or 80.0) * (0.6 + 0.4 * math.max(0.25, difficulty))
    for _, s in ipairs(alienShots) do
        if s.active then
            local dist = speed * dt
            local n = math.max(1, math.ceil(dist / 2.0))
            for _ = 1, n do
                s.v = s.v - dist / n
                if s.v <= GROUND_V + 1 then
                    s.active = false
                    burst(s.u, GROUND_V + 1, 4, "bomb", 50.0, 0.5)
                    break
                end
                if hitBunker(s.u, s.v, -1) then s.active = false; break end
                if hitsPlayer(s) then
                    s.active = false
                    killPlayer()
                    break
                end
            end
        end
    end
end

-- --- Mystery ship ----------------------------------------------------------------------------

local function stepUfo(dt)
    if ufo.active then
        ufo.u = ufo.u + ufo.dir * UFO_SPEED * dt
        if ufo.u > FIELD_W + 4 or ufo.u < -20 then ufo.active = false end
        return
    end
    ufoTimer = ufoTimer - dt
    if ufoTimer > 0.0 then return end
    if alienShots[3].active then ufoTimer = 0.5; return end     -- the squiggly shot has the slot
    ufoTimer = UFO_EVERY
    if aliveCount < 8 then return end
    ufo.active = true
    ufo.dir = (shotCount % 2 == 0) and 1 or -1
    ufo.u = (ufo.dir > 0) and -16 or FIELD_W
    cue("ufo")
end

-- --- Player ------------------------------------------------------------------------------------

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

local function respawn()
    player.alive, player.dying = true, 0.0
    player.u = 16
    player.obj.active = true
    reloadT = math.max(reloadT, 1.0)
end

local function gameOver()
    mode = "over"
    if score > hiScore then
        hiScore = score
        say(landed and "THE INVADERS HAVE LANDED -- NEW HIGH SCORE!" or "NEW HIGH SCORE!", 8.0)
    else
        say(landed and "THE INVADERS HAVE LANDED" or "GAME OVER", 8.0)
    end
end

killPlayer = function()
    if not player.alive then return end
    player.alive = false
    player.obj.active = false
    player.dying = 1.8
    wreck.active, wreck.u = true, player.u
    burst(player.u + 6.5, PLAYER_V + 4, 24, "cannon", 120.0, 1.4)
    cue("die")
    clearShots()
    ufo.active = false
    if landed then lives = 1 end    -- no spare base helps against that
end

local function stepPlayer(dt, fireHit, fireHeld)
    local l = anyDown(KEYS.left) and 1 or 0
    local r = anyDown(KEYS.right) and 1 or 0
    player.u = clamp(player.u + (r - l) * playerSpeed * dt, LEFT_EDGE, RIGHT_EDGE - 13)
    if not pshot.active and (fireHit or (holdToFire and fireHeld)) then fire() end
end

local function stepDying(dt)
    player.dying = player.dying - dt
    wreck.frame = math.floor(now * 10.0) % 2 + 1
    if player.dying > 0.0 then return end
    wreck.active = false
    lives = lives - 1
    if lives <= 0 then gameOver() else respawn() end
end

-- --- Game flow ---------------------------------------------------------------------------------

local function startGame()
    score, lives, wave, bonusGiven = 0, math.max(1, math.floor(ships)), 1, false
    landed, clearAt, paused = false, -1.0, false
    setupRack()
    restoreBunkers()
    clearShots()
    for _, s in ipairs(sparks.items) do s.active = false end
    ufo.active, ufoTimer, shotCount = false, UFO_EVERY, 0
    reloadT, fireTurn, plungerIdx, squigglyIdx = 1.5, 1, 1, 1
    mode = "play"
    respawn()
    say("WAVE 1", 2.0)
end

local function nextWave()
    clearAt = -1.0
    wave = wave + 1
    setupRack()
    restoreBunkers()
    clearShots()
    ufo.active, ufoTimer, shotCount = false, UFO_EVERY, 0
    reloadT = 1.5
    say("WAVE " .. wave, 2.0)
end

-- --- Per-frame ---------------------------------------------------------------------------------

local function stepSparks(dt)
    for _, s in ipairs(sparks.items) do
        if s.active then
            s.life = s.life - dt
            if s.life <= 0.0 then
                s.active = false
            else
                s.u, s.v, s.d = s.u + s.vu * dt, s.v + s.vv * dt, s.d + s.vd * dt
                s.vv = s.vv - 260.0 * dt
                local k = s.life / s.max
                local h = (0.25 + 0.75 * k) * s.size * PX
                game.setPos(s.root, W(s.u, s.v, s.d))
                game.setScale(s.root, h, h, h)
                game.setRot(s.root, s.spinA * s.life, s.spinB * s.life, 0.0)
            end
        end
    end
end

local function stepBooms(dt)
    for _, x in ipairs(booms.items) do
        if x.active then
            x.t = x.t - dt
            if x.t <= 0.0 then x.active = false end
        end
    end
end

local function draw()
    if frameNo < 2 then return end
    for _, a in ipairs(rack) do
        if a.dirty and a.active then placeInvader(a); a.dirty = false end
    end
    for _, bk in ipairs(bunkers) do
        for row = 0, 7 do
            if bk.dirty[row] then layoutRow(bk, row); bk.dirty[row] = false end
        end
    end
    local p = player.obj
    if p.active then game.setPos(p.root, W(player.u + 6.5, PLAYER_V + 4, 0.0)) end
    if wreck.active then game.setPos(wreck.root, W(wreck.u + 6.5, PLAYER_V + 4, 0.0)) end
    if pshot.active then game.setPos(pshot.root, W(pshot.u, pshot.v + 2, 0.0)) end
    for _, s in ipairs(alienShots) do
        if s.active then
            s.frame = math.floor(now * 10.0) % 2 + 1
            game.setPos(s.root, W(s.u, s.v + 3.5, 0.0))
            game.setRot(s.root, 0.0, s.spin * now, 0.0)
        end
    end
    if ufo.active then
        game.setPos(ufo.root, W(ufo.u + 8, UFO_V + 3.5 + math.sin(now * 3.0) * 0.6, 0.0))
        game.setRot(ufo.root, 0.0, math.sin(now * 2.0) * 25.0, 0.0)
    end
    for _, x in ipairs(booms.items) do
        if x.active then
            local k = 1.0 - x.t / x.max                      -- 0 -> 1 over its life
            local len = (3.0 + 5.0 * math.sqrt(k)) * x.size * PX
            local thick = (0.9 - 0.5 * k) * PX
            game.setPos(x.root, W(x.u, x.v, 0.0))
            game.setRot(x.root, 0.0, 0.0, k * 20.0)
            for _, id in ipairs(x.parts) do game.setScale(id, len, thick, VOXEL_D * PX) end
        end
    end
    for i, icon in ipairs(lifeIcons) do icon.active = (mode ~= "title") and (lives - 1 >= i) end
    for i, m in ipairs(starMats) do
        game.setMaterialProps(m, { emissionStrength = 1.2 + 0.6 * math.sin(now * (1.3 + i * 0.9) + i) })
    end
end

-- Half the screen's height (bezel to bezel, 25 m) plus a margin, in metres.
local FIT_HALF = 13.6

local function driveCamera()
    local lx, ly, lz = W(FIELD_W * 0.5, 121.0, 0.0)
    local dist = FIT_HALF / math.tan(math.rad(clamp(camFov, 10.0, 140.0)) * 0.5) / math.max(0.2, camZoom)
    local cx, cy, cz = lx, ly + camRise, lz + dist
    game.setCameraPos(cx, cy, cz)
    game.setCameraDir(lx - cx, ly - cy, lz - cz)
    game.setCameraFov(camFov)
end

local function hud()
    local lines = {}
    local msg = (now < messageUntil) and message or ""
    if mode == "title" then
        lines[1] = string.format("S P A C E   I N V A D E R S          HI-SCORE %04d", hiScore)
        lines[2] = "PRESS ENTER TO PLAY"
        lines[3] = "Left/Right or A/D move    Space/Up/W fire    P pause"
        lines[4] = "SCORE ADVANCE TABLE:   mystery ship = ?    top row = 30    middle = 20    bottom = 10 POINTS"
    else
        lines[1] = string.format("SCORE<1> %04d      HI-SCORE %04d      WAVE %d      LIVES %d",
                                 score, math.max(hiScore, score), wave, math.max(0, lives))
        if mode == "over" then
            lines[2] = msg ~= "" and msg or "GAME OVER"
            lines[3] = "PRESS ENTER TO PLAY AGAIN"
        else
            lines[2] = paused and "PAUSED -- P to go on" or msg
        end
    end
    game.setHud(table.concat(lines, "\n"))
end

-- --- Lifecycle ---------------------------------------------------------------------------------

function start(e)
    KEYS = {
        left  = { game.KEY_LEFT, game.KEY_A },
        right = { game.KEY_RIGHT, game.KEY_D },
        fire  = { game.KEY_SPACE, game.KEY_UP, game.KEY_W },
    }
    -- The screen stands 20 m above the carrier, clear of whatever terrain is there.
    OX, OZ, BY = e.x, e.z, e.y + 20.0
    math.randomseed(math.floor(os.time()))
    for key, def in pairs(MAT_DEFS) do
        -- Matte unless said otherwise (the space): any reflective material
        -- makes the engine render an environment cubemap every frame.
        local t = { name = "Invaders " .. key, reflectivity = 0.0 }
        for k, v in pairs(def) do t[k] = v end
        mats[key] = game.createMaterial(t)
    end
    starMats = { mats.star1, mats.star2, mats.star3 }
    for _, f in pairs(CUE_FILES) do cues[f] = game.findAsset(f, "Sound") ~= nil end

    objects, rack, bunkers, cells, alienShots, lifeIcons = {}, {}, {}, {}, {}, {}
    buildScreen()
    buildRack()
    buildBunkers()
    buildActors()
    game.setCamera(-1)
    mode = "title"
end

function update(e, dt, t)
    now = t
    frameNo = frameNo + 1
    dt = math.min(dt, 1.0 / 30.0)

    local startHit = pressed("start", game.keyDown(game.KEY_ENTER))
    local pauseHit = pressed("pause", game.keyDown(game.KEY_P))
    local fireHeld = anyDown(KEYS.fire)
    local fireHit  = pressed("fire", fireHeld)

    if mode ~= "play" and startHit then startGame(); fireHit = false end
    if mode == "play" and pauseHit then paused = not paused end

    if not paused then
        if mode == "play" then
            if player.dying > 0.0 then
                stepDying(dt)
            elseif clearAt > 0.0 then
                stepPlayer(dt, false, false)
                if now >= clearAt then nextWave() end
            else
                stepPlayer(dt, fireHit, fireHeld)
                stepMarch(dt, true)
                if mode == "play" and player.alive then
                    stepAlienFire(dt)
                    stepUfo(dt)
                    stepPlayerShot(dt)
                    stepAlienShots(dt)
                end
            end
        else
            -- Title and game over: the rack keeps marching, but never down.
            stepMarch(dt, false)
        end
        stepBooms(dt)
        stepSparks(dt)
    end
    draw()
    syncVisibility()
    driveCamera()
    hud()
end
