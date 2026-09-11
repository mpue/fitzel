-- ARKANOID -- the 1986 brick breaker, entirely in Lua.
--
-- Put this script on ONE object (an Empty is best) and press Play. The field
-- with its pipes, the bricks, the Vaus, the capsules and the drifting enemies
-- are all spawned around it and removed again when Play stops.
--
--   Left / Right, A / D   move the Vaus  (Shift: faster)
--   Space / Up / W        launch the ball, release a caught one, fire the laser
--   P                     pause
--   Enter                 start / play again
--
-- Clear every brick to finish the round. Silver bricks take several hits (more
-- in later rounds) and gold ones none -- they are walls. Where the ball meets
-- the Vaus decides where it goes: the ends send it off flat. It gets faster the
-- longer it is in play, and at once when it touches the ceiling.
--
-- Some bricks leave a capsule. Catch it with the Vaus:
--   S slow   the ball drops back to its starting speed
--   C catch  the ball sticks to the Vaus until you let it go
--   E expand a longer Vaus
--   L laser  fire at the bricks
--   D disruption  three balls (no capsules while more than one is in play)
--   B break  a gate opens in the right wall: go through it to skip the round
--   P player one more Vaus
-- C, E and L replace each other. Enemies drift out of the gates in the ceiling;
-- ball, laser or Vaus destroys them. Extra Vaus at 20.000 and every 60.000.
-- After the last round the first comes round again, faster.
--
-- Everything is pooled -- game.spawn only delivers at the end of the frame --
-- and switched with setActive; materials are set once a thing is visible.
-- The ball runs in arcade pixels with small substeps, bricks are a grid, so a
-- collision only ever looks at the few cells around the ball.
--
-- The camera is driven every frame. Make sure no Camera entity in the scene is
-- "active on start" -- that one would take the view back.

-- --- Inspector parameters (globals = editable fields on the Script component) --
ships       = 3       -- Vaus per game
startRound  = 1
difficulty  = 1.0     -- scales the ball's speed
paddleSpeed = 230.0   -- arcade pixels per second (Shift: half as fast again)
autoLaunch  = 5.0     -- seconds before a waiting ball launches itself (0 = never)
enemyShips  = true    -- the drifting enemies
camZoom     = 1.0     -- 1 = the field just fills the view's height
camRise     = 2.0     -- camera height above the field's middle, metres
camFov      = 60.0    -- the distance follows it, so the field always fits
sound       = true

-- --- Layout, in arcade pixels -----------------------------------------------------
-- u runs across the 208-pixel field (13 bricks), v up it; d is depth, towards
-- the camera. One arcade pixel is PX world metres.
local PX         = 0.1
local FIELD_W    = 208
local TOP_V      = 236         -- the ceiling
local BRICK_TOP  = 220         -- top edge of the first brick row
local COLS, ROWS = 13, 16
local PADDLE_V   = 20          -- bottom of the Vaus
local PADDLE_TOP = PADDLE_V + 6
local BALL_R     = 2.5
local HW_NORMAL, HW_LONG = 16, 24
local GATE_U     = { 52, 156 }
local ENEMY_R    = 5.0
local LASER_SPEED = 320.0
local CAPSULE_SPEED = 50.0
local EXTENDS    = { 20000 }
local EXTEND_EVERY = 60000
local BACK_D     = -40

local BRICKS = {
    w = { mat = "white",  pts = 50 },  o = { mat = "orange", pts = 60 },
    c = { mat = "cyan",   pts = 70 },  g = { mat = "green",  pts = 80 },
    r = { mat = "red",    pts = 90 },  b = { mat = "blue",   pts = 100 },
    p = { mat = "pink",   pts = 110 }, y = { mat = "yellow", pts = 120 },
    s = { mat = "silver", silver = true },
    G = { mat = "gold",   gold = true },
}

-- Thirteen columns, the first row at the top. Every breakable brick has to be
-- reachable: gold never closes a room completely.
local ROUNDS = {
    { ".............", "sssssssssssss", "rrrrrrrrrrrrr", "yyyyyyyyyyyyy",
      "bbbbbbbbbbbbb", "ppppppppppppp", "ggggggggggggg" },
    { "w............", "wo...........", "woc..........", "wocg.........",
      "wocgr........", "wocgrb.......", "wocgrbp......", "wocgrbpy.....",
      "wocgrbpyw....", "wocgrbpywo...", "wocgrbpywoc..", "wocgrbpywocg.",
      "ssssssssssssr" },
    { ".............", "......y......", ".....yry.....", "....yrcry....",
      "...yrcgcry...", "..yrcgsgcry..", ".yrcgsbsgcry.", "..yrcgsgcry..",
      "...yrcgcry...", "....yrcry....", ".....yry.....", "......y......" },
    { ".............", "ggggggggggggg", ".............", "GGGG.....GGGG",
      ".............", "rrrrrrrrrrrrr", ".............", "...GGGGGGG...",
      ".............", "ppppppppppppp", ".............", "bbbbbbbbbbbbb",
      "ccccccccccccc" },
    { ".............", "...p.....p...", "....p...p....", "...ppppppp...",
      "..pp.ppp.pp..", ".ppppppppppp.", ".p.ppppppp.p.", ".p.p.....p.p.",
      "....pp.pp...." },
    { ".............", ".o.o.o.o.o.o.", ".o.o.o.o.o.o.", ".c.c.c.c.c.c.",
      ".c.c.c.c.c.c.", ".g.g.g.g.g.g.", ".g.g.g.g.g.g.", ".b.b.b.b.b.b.",
      ".b.b.b.b.b.b.", ".s.s.s.s.s.s.", ".............", "GGG.......GGG" },
    { ".............", "r.y.g.c.b.p.w", ".r.y.g.c.b.p.", "r.y.g.c.b.p.w",
      ".r.y.g.c.b.p.", "r.y.g.c.b.p.w", ".r.y.g.c.b.p.", "r.y.g.c.b.p.w",
      ".r.y.g.c.b.p.", "r.y.g.c.b.p.w", ".r.y.g.c.b.p." },
    { ".............", "..pp.....pp..", ".pppp...pppp.", "prrrrp.prrrrp",
      "prrrrrprrrrrp", "prrrrrrrrrrrp", ".prrrrrrrrrp.", "..prrrrrrrp..",
      "...prrrrrp...", "....prrrp....", ".....prp.....", "......p......" },
    { ".............", "yyyyyyyyyyyyy", "..GGGG.GGGG..", "bbbbbbbbbbbbb",
      "bbbbbbbbbbbbb", ".GGGG...GGGG.", "ccccccccccccc", "ccccccccccccc",
      "..GGG...GGG..", "wwwwwwwwwwwww" },
    { ".............", "GGGGGGGGGGGGG", "G...........G", "G.sssssssss.G",
      "G.syyyyyyys.G", "G.syrrrrrys.G", "G.syrppprys.G", "G.syrrrrrys.G",
      "G.syyyyyyys.G", "G.sssssssss.G", "G...........G", "GGGGG...GGGGG" },
    { ".............", "sssssssssssss", "wwwwwwwwwwwww", "ooooooooooooo",
      "ccccccccccccc", "ggggggggggggg", "rrrrrrrrrrrrr", "bbbbbbbbbbbbb",
      "ppppppppppppp", "yyyyyyyyyyyyy", "sssssssssssss" },
}

-- Capsules: letter pixel art (3 x 5), colour, how often they come.
local CAPSULES = {
    { key = "S", name = "SLOW",       mat = "capS", weight = 3, art = { "###", "#..", "###", "..#", "###" } },
    { key = "C", name = "CATCH",      mat = "capC", weight = 2, art = { "###", "#..", "#..", "#..", "###" } },
    { key = "E", name = "EXPAND",     mat = "capE", weight = 3, art = { "###", "#..", "###", "#..", "###" } },
    { key = "L", name = "LASER",      mat = "capL", weight = 3, art = { "#..", "#..", "#..", "#..", "###" } },
    { key = "D", name = "DISRUPTION", mat = "capD", weight = 2, art = { "##.", "#.#", "#.#", "#.#", "##." } },
    { key = "B", name = "BREAK",      mat = "capB", weight = 1, art = { "##.", "#.#", "##.", "#.#", "##." } },
    { key = "P", name = "PLAYER",     mat = "capP", weight = 1, art = { "###", "#.#", "###", "#..", "#.." } },
}

-- Each round tints the grid behind the bricks.
local GRID_TINTS = { {0.1, 0.25, 0.9}, {0.1, 0.7, 0.5}, {0.7, 0.2, 0.8}, {0.9, 0.4, 0.1},
                     {0.2, 0.6, 1.0}, {0.8, 0.8, 0.2} }

-- The field is dark, so the auto exposure opens up: colours carry a little
-- emission of their own and the albedo stays moderate.
local MAT_DEFS = {
    -- Black METAL: a black dielectric still mirrors 4 % of the sun (F0).
    space  = { color = {0.0, 0.0, 0.0}, roughness = 1.0, reflectivity = 1.0 },
    panel  = { color = {0.01, 0.015, 0.04}, roughness = 0.7, reflectivity = 1.0,
               emission = {0.02, 0.04, 0.12}, emissionStrength = 1.0 },
    grid   = { color = {0.02, 0.04, 0.1}, emission = {0.1, 0.25, 0.9}, emissionStrength = 0.35 },
    pipe   = { color = {0.55, 0.57, 0.62}, roughness = 0.25, reflectivity = 1.0,
               emission = {0.25, 0.28, 0.35}, emissionStrength = 0.25 },
    gate   = { color = {0.22, 0.22, 0.25}, roughness = 0.35, reflectivity = 1.0 },
    warp   = { color = {0.5, 0.1, 0.5}, emission = {1.0, 0.3, 1.0}, emissionStrength = 3.0 },
    star1  = { color = {0.5, 0.5, 0.55}, emission = {0.8, 0.85, 1.0}, emissionStrength = 1.5 },
    star2  = { color = {0.5, 0.45, 0.35}, emission = {1.0, 0.85, 0.6}, emissionStrength = 1.2 },
    star3  = { color = {0.35, 0.4, 0.5}, emission = {0.6, 0.75, 1.0}, emissionStrength = 1.0 },
    -- Saturated and not too bright: at the exposure this dark field gets,
    -- anything brighter tonemaps to pastel.
    white  = { color = {0.50, 0.50, 0.52}, roughness = 0.3, emission = {0.9, 0.9, 1.0}, emissionStrength = 0.25 },
    orange = { color = {0.50, 0.16, 0.02}, roughness = 0.3, emission = {1.0, 0.35, 0.03}, emissionStrength = 0.25 },
    cyan   = { color = {0.02, 0.35, 0.45}, roughness = 0.3, emission = {0.05, 0.8, 1.0}, emissionStrength = 0.25 },
    green  = { color = {0.04, 0.40, 0.06}, roughness = 0.3, emission = {0.1, 1.0, 0.15}, emissionStrength = 0.25 },
    red    = { color = {0.50, 0.02, 0.02}, roughness = 0.3, emission = {1.0, 0.05, 0.03}, emissionStrength = 0.3 },
    blue   = { color = {0.02, 0.04, 0.50}, roughness = 0.3, emission = {0.03, 0.1, 1.0}, emissionStrength = 0.4 },
    pink   = { color = {0.50, 0.04, 0.35}, roughness = 0.3, emission = {1.0, 0.1, 0.7}, emissionStrength = 0.3 },
    yellow = { color = {0.50, 0.40, 0.02}, roughness = 0.3, emission = {1.0, 0.8, 0.05}, emissionStrength = 0.25 },
    silver = { color = {0.42, 0.44, 0.50}, roughness = 0.2, reflectivity = 1.0,
               emission = {0.4, 0.45, 0.6}, emissionStrength = 0.2 },
    gold   = { color = {0.70, 0.45, 0.08}, roughness = 0.25, reflectivity = 1.0,
               emission = {1.0, 0.55, 0.05}, emissionStrength = 0.25 },
    flash  = { color = {0.9, 0.9, 0.9}, emission = {1.0, 1.0, 1.0}, emissionStrength = 3.0 },
    vaus   = { color = {0.55, 0.57, 0.62}, roughness = 0.2, reflectivity = 1.0,
               emission = {0.3, 0.32, 0.4}, emissionStrength = 0.3 },
    vausEnd = { color = {0.6, 0.06, 0.05}, roughness = 0.3, emission = {1.0, 0.15, 0.1}, emissionStrength = 0.8 },
    vausLight = { color = {0.2, 0.6, 0.8}, emission = {0.3, 0.9, 1.0}, emissionStrength = 2.5 },
    gun    = { color = {0.3, 0.3, 0.32}, roughness = 0.3, emission = {1.0, 0.2, 0.1}, emissionStrength = 1.0 },
    ball   = { color = {0.6, 0.6, 0.6}, emission = {1.0, 0.95, 0.9}, emissionStrength = 2.0 },
    laser  = { color = {0.6, 0.1, 0.1}, emission = {1.0, 0.2, 0.15}, emissionStrength = 5.0 },
    enemy  = { color = {0.3, 0.5, 0.2}, roughness = 0.3, emission = {0.5, 1.0, 0.3}, emissionStrength = 1.2 },
    letter = { color = {0.8, 0.8, 0.8}, emission = {1.0, 1.0, 1.0}, emissionStrength = 1.0 },
    capS   = { color = {0.60, 0.30, 0.05}, roughness = 0.3, emission = {1.0, 0.5, 0.1}, emissionStrength = 0.3 },
    capC   = { color = {0.10, 0.50, 0.12}, roughness = 0.3, emission = {0.2, 1.0, 0.3}, emissionStrength = 0.3 },
    capE   = { color = {0.08, 0.15, 0.65}, roughness = 0.3, emission = {0.2, 0.35, 1.0}, emissionStrength = 0.3 },
    capL   = { color = {0.60, 0.05, 0.05}, roughness = 0.3, emission = {1.0, 0.12, 0.1}, emissionStrength = 0.3 },
    capD   = { color = {0.05, 0.45, 0.55}, roughness = 0.3, emission = {0.2, 0.85, 1.0}, emissionStrength = 0.3 },
    capB   = { color = {0.60, 0.10, 0.45}, roughness = 0.3, emission = {1.0, 0.25, 0.8}, emissionStrength = 0.3 },
    capP   = { color = {0.55, 0.55, 0.60}, roughness = 0.3, emission = {0.8, 0.8, 0.9}, emissionStrength = 0.3 },
}
local mats = {}

local CUE_FILES = {
    paddle = "plopp.wav", brick = "missile_deny.wav", hard = "missile_lock.wav",
    wall = "missile_seek.wav", laser = "shot.wav", capsule = "swoosh.wav",
    die = "impact.wav", enemy = "impact.wav", extra = "energy_low.wav",
    warp = "missile_launch.wav", clear = "missile_hit.wav",
}
local cues, lastCue = {}, {}

-- --- State --------------------------------------------------------------------------
local OX, OZ, BY = 0.0, 0.0, 0.0
local now, frameNo = 0.0, 0
local objects = {}          -- everything that is switched on and off
local bricks = {}           -- bricks[row][col], row 0 at the top
local flashing = {}
local balls, lasers, enemyPool, sparks = nil, nil, nil, nil
local capsule, vaus, warpBox = nil, nil, nil
local gates, lifeIcons, gridMat = {}, {}, nil
local starMats = {}

local mode = "title"        -- title / play / over
local state = "ready"       -- in play: ready / play / dying / clear
local stateT = 0.0
local paused = false
local score, hiScore, lives, round = 0, 0, 3, 1
local extendNo, nextExtend = 1, EXTENDS[1]
local breakable = 0
local power = nil           -- "C", "E", "L" or nil
local warpOpen, warped = false, false
local serveT, catchT, laserCd, enemyT = 0.0, 0.0, 0.0, 8.0
local message, messageUntil = "", 0.0
local keysPrev = {}

-- --- Helpers ------------------------------------------------------------------------

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
local function fmt(n)
    local out = tostring(math.floor(n)):reverse():gsub("(%d%d%d)", "%1."):reverse()
    if out:sub(1, 1) == "." then out = out:sub(2) end
    return out
end

-- Arcade pixel -> world point. The field stands upright, facing +z.
local function W(u, v, d) return OX + (u - FIELD_W * 0.5) * PX, BY + v * PX, OZ + (d or 0.0) * PX end

-- Out of sight behind the backdrop until a pool hands the thing out.
local function parkedAt() return W(FIELD_W * 0.5, 120.0, BACK_D - 40.0) end

local function spawn(kind, x, y, z, hx, hy, hz, mat, parent, name, rz)
    return game.spawn{
        type = kind, x = x, y = y, z = z, sx = hx, sy = hy, sz = hz, rz = rz or 0.0,
        material = mat and mats[mat] or nil, parent = parent or -1,
        physics = game.PHYSICS_NONE, name = name or "arkanoid",
    }
end

local function single(name, kind, hx, hy, hz, mat, x, y, z, rz)
    if not x then x, y, z = parkedAt() end
    local o = { root = spawn(kind, x, y, z, hx, hy, hz, mat, nil, name, rz),
                active = false, shown = true, mat = mat }
    o.parts = { o.root }
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
        if not it.active then
            pool.cursor = i % n + 1
            it.active = true
            return it
        end
    end
    return nil
end

local function paint(o, mat)
    for _, id in ipairs(o.parts) do game.setMaterial(id, mats[mat]) end
    o.mat = mat
end

-- Switched only when the wanted state changes, and only from the second frame:
-- before that the pooled entities do not exist in the scene yet. A material
-- asked for (wantMat) is put on when the thing is shown.
local function syncVisibility()
    if frameNo < 2 then return end
    for _, o in ipairs(objects) do
        local want = o.active and not o.blinkOff
        if want and o.wantMat and o.wantMat ~= o.mat then paint(o, o.wantMat) end
        if want ~= o.shown then
            game.setActive(o.root, want)
            o.shown = want
        end
        if want and o.frames then
            for i = 1, #o.frames do
                local f = (o.frame == i)
                if f ~= o.frameShown[i] then
                    game.setActive(o.frames[i], f)
                    o.frameShown[i] = f
                end
            end
        end
    end
end

-- --- Voxel letters --------------------------------------------------------------------

local function parse(rows)
    local on = {}
    for y, row in ipairs(rows) do
        assert(#row == #rows[1], "ragged sprite row: " .. row)
        on[y] = {}
        for x = 1, #row do on[y][x] = row:sub(x, x) == "#" end
    end
    return on
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

-- One Empty per frame under `parent`, each holding its letter's pixels; the
-- owner picks one with .frame.
local function letterSet(parent, arts, d)
    local o = { root = parent, active = true, shown = true, frame = 1, frames = {}, frameShown = {} }
    for i, art in ipairs(arts) do
        local w, h = #art[1], #art
        o.frames[i] = spawn(game.EMPTY, 0.0, 0.0, 0.0, 0.1, 0.1, 0.1, nil, parent, "letter")
        o.frameShown[i] = true
        for _, r in ipairs(cover(parse(art), w, h)) do
            local cx = (r.x + r.w * 0.5 - w * 0.5) * PX
            local cy = (h * 0.5 - r.y - r.h * 0.5) * PX
            spawn(game.BOX, cx, cy, d * PX, r.w * 0.5 * PX, r.h * 0.5 * PX, 0.4 * PX,
                  "letter", o.frames[i], "letter pixels")
        end
    end
    objects[#objects + 1] = o
    return o
end

-- --- Building --------------------------------------------------------------------------

local function brickCentre(row, col) return col * 16 + 8, BRICK_TOP - row * 8 - 4 end

local function buildScene()
    local B, S, C = game.BOX, game.SPHERE, game.CYLINDER
    local x, y, z = W(FIELD_W * 0.5, 115.0, BACK_D)
    spawn(B, x, y, z, 70.0, 40.0, 0.2, "space", nil, "arkanoid backdrop")
    for i = 1, 30 do
        local k = math.random(1, 3)
        local sx, sy, sz = W(rand(-300, 508), rand(-90, 330), BACK_D + 4.0)
        local r = rand(0.03, 0.08)
        spawn(S, sx, sy, sz, r, r, r * 0.3, "star" .. k, nil, "star")
    end
    -- The field: a dark panel with a grid in the brick spacing.
    x, y, z = W(FIELD_W * 0.5, 114.0, -8.0)
    spawn(B, x, y, z, FIELD_W * 0.5 * PX, 122.0 * PX, 1.0 * PX, "panel", nil, "field")
    for k = 1, COLS - 1 do
        x, y, z = W(k * 16, 114.0, -6.8)
        spawn(B, x, y, z, 0.25 * PX, 122.0 * PX, 0.3 * PX, "grid", nil, "grid")
    end
    for k = 0, 14 do
        x, y, z = W(FIELD_W * 0.5, 4 + k * 16, -6.8)
        spawn(B, x, y, z, FIELD_W * 0.5 * PX, 0.25 * PX, 0.3 * PX, "grid", nil, "grid")
    end
    -- The pipes round it, and the two gates in the ceiling.
    x, y, z = W(-4, 116.0, 0.0)
    spawn(C, x, y, z, 4.0 * PX, 128.0 * PX, 4.0 * PX, "pipe", nil, "pipe")
    x, y, z = W(FIELD_W + 4, 116.0, 0.0)
    spawn(C, x, y, z, 4.0 * PX, 128.0 * PX, 4.0 * PX, "pipe", nil, "pipe")
    x, y, z = W(FIELD_W * 0.5, TOP_V + 4, 0.0)
    spawn(C, x, y, z, 4.0 * PX, (FIELD_W * 0.5 + 8) * PX, 4.0 * PX, "pipe", nil, "pipe", 90.0)
    for i, gu in ipairs(GATE_U) do
        gates[i] = single("gate", B, 12.0 * PX, 4.5 * PX, 5.0 * PX, "gate", W(gu, TOP_V + 4, 0.5))
        gates[i].active, gates[i].openT = true, 0.0
    end
    warpBox = single("warp", B, 4.6 * PX, 9.0 * PX, 4.6 * PX, "warp", W(FIELD_W + 4, PADDLE_V + 3, 0.0))
end

local function buildBricks()
    for row = 0, ROWS - 1 do
        bricks[row] = {}
        for col = 0, COLS - 1 do
            local u, v = brickCentre(row, col)
            local b = single("brick", game.BOX, 7.5 * PX, 3.5 * PX, 3.0 * PX, "white", W(u, v, 0.0))
            b.row, b.col = row, col
            bricks[row][col] = b
        end
    end
end

local function buildVaus()
    local B, S, C = game.BOX, game.SPHERE, game.CYLINDER
    vaus = { u = FIELD_W * 0.5, hw = HW_NORMAL, visible = true }
    vaus.body   = single("vaus", C, 3.0 * PX, 10.0 * PX, 3.0 * PX, "vaus", nil, nil, nil, 90.0)
    vaus.endL   = single("vaus end", S, 3.3 * PX, 3.3 * PX, 3.3 * PX, "vausEnd")
    vaus.endR   = single("vaus end", S, 3.3 * PX, 3.3 * PX, 3.3 * PX, "vausEnd")
    vaus.lightL = single("vaus light", C, 3.2 * PX, 0.7 * PX, 3.2 * PX, "vausLight", nil, nil, nil, 90.0)
    vaus.lightR = single("vaus light", C, 3.2 * PX, 0.7 * PX, 3.2 * PX, "vausLight", nil, nil, nil, 90.0)
    vaus.gunL   = single("vaus gun", B, 1.0 * PX, 2.5 * PX, 1.0 * PX, "gun")
    vaus.gunR   = single("vaus gun", B, 1.0 * PX, 2.5 * PX, 1.0 * PX, "gun")
    vaus.parts  = { vaus.body, vaus.endL, vaus.endR, vaus.lightL, vaus.lightR }
    for i = 1, 5 do
        local ix, iy, iz = W(6 + i * 14, -16, 0.0)
        lifeIcons[i] = single("spare vaus", C, 2.2 * PX, 5.5 * PX, 2.2 * PX, "vausEnd", ix, iy, iz, 90.0)
    end
end

local function buildPools()
    local B, S = game.BOX, game.SPHERE
    balls  = newPool(3, function() return single("ball", S, BALL_R * PX, BALL_R * PX, BALL_R * PX, "ball") end)
    lasers = newPool(8, function() return single("laser", B, 0.6 * PX, 3.0 * PX, 0.6 * PX, "laser") end)
    sparks = newPool(70, function()
        return single("debris", B, 0.8 * PX, 0.8 * PX, 0.8 * PX, "white")
    end)
    enemyPool = newPool(3, function()
        local x, y, z = parkedAt()
        local o = { root = spawn(game.EMPTY, x, y, z, 0.1, 0.1, 0.1, nil, nil, "enemy"),
                    active = false, shown = true, parts = {} }
        for k = 0, 2 do
            local a = k * math.pi * 2 / 3
            o.parts[k + 1] = spawn(S, math.cos(a) * 2.4 * PX, math.sin(a) * 2.4 * PX, 0.0,
                                   2.4 * PX, 2.4 * PX, 2.4 * PX, "enemy", o.root, "enemy ball")
        end
        o.mat = "enemy"
        objects[#objects + 1] = o
        return o
    end)
    -- The capsule: a pill with its letter on the front.
    local x, y, z = parkedAt()
    capsule = { root = spawn(game.EMPTY, x, y, z, 0.1, 0.1, 0.1, nil, nil, "capsule"),
                active = false, shown = true, mat = "capS" }
    capsule.parts = {
        spawn(game.CYLINDER, 0.0, 0.0, 0.0, 3.5 * PX, 4.5 * PX, 3.5 * PX, "capS", capsule.root, "capsule", 90.0),
        spawn(S, -4.5 * PX, 0.0, 0.0, 3.5 * PX, 3.5 * PX, 3.5 * PX, "capS", capsule.root, "capsule end"),
        spawn(S, 4.5 * PX, 0.0, 0.0, 3.5 * PX, 3.5 * PX, 3.5 * PX, "capS", capsule.root, "capsule end"),
    }
    objects[#objects + 1] = capsule
    local arts = {}
    for i, c in ipairs(CAPSULES) do arts[i] = c.art end
    capsule.letter = letterSet(capsule.root, arts, 3.6)
end

-- --- Effects ----------------------------------------------------------------------------

local function burst(u, v, n, mat, speed, life)
    for i = 1, n do
        local s = take(sparks)
        if not s then return end
        s.wantMat = mat
        local a = rand(0.0, math.pi * 2.0)
        local sp = rand(0.3, 1.0) * speed
        s.u, s.v, s.d = u + rand(-4, 4), v + rand(-2, 2), 0.0
        s.vu, s.vv, s.vd = math.cos(a) * sp, math.sin(a) * sp, rand(0.3, 1.0) * speed
        s.life = rand(0.4, 0.8) * (life or 1.0)
        s.max = s.life
        s.size = rand(0.6, 1.3)
        s.spinA, s.spinB = rand(-500, 500), rand(-500, 500)
    end
end

local function flash(b)
    b.wantMat = "flash"
    b.flashT = 0.08
    flashing[#flashing + 1] = b
end

-- --- Score ----------------------------------------------------------------------------------

local function addScore(n)
    score = score + n
    if score >= nextExtend then
        extendNo = extendNo + 1
        nextExtend = EXTENDS[extendNo] or (nextExtend + EXTEND_EVERY)
        lives = lives + 1
        say("ONE MORE VAUS", 2.0)
        cue("extra")
    end
end

-- --- Speed ----------------------------------------------------------------------------------

local function loopNo() return math.floor((round - 1) / #ROUNDS) end
local function baseSpeed() return 125.0 * math.max(0.3, difficulty) * (1.0 + 0.12 * loopNo()) end
local function maxSpeed() return 270.0 * math.max(0.3, difficulty) * (1.0 + 0.08 * loopNo()) end

-- Never let the ball go (nearly) flat: it would crawl from wall to wall.
local function fixAngle(b)
    if math.abs(b.dv) < 0.28 then
        b.dv = (b.dv < 0) and -0.28 or 0.28
        b.du = ((b.du < 0) and -1 or 1) * math.sqrt(1.0 - b.dv * b.dv)
    end
end

local function countBalls()
    local n = 0
    for _, b in ipairs(balls.items) do if b.active then n = n + 1 end end
    return n
end

-- --- Capsules -------------------------------------------------------------------------------

local function dropCapsule(u, v)
    if capsule.active or countBalls() > 1 or math.random() > 0.2 then return end
    local total = 0
    for _, c in ipairs(CAPSULES) do total = total + c.weight end
    local pick, i = math.random() * total, 1
    while pick > CAPSULES[i].weight do pick = pick - CAPSULES[i].weight; i = i + 1 end
    capsule.active, capsule.kind = true, i
    capsule.u, capsule.v, capsule.t = u, v, 0.0
    capsule.wantMat = CAPSULES[i].mat
    capsule.letter.frame = i
end

local function releaseBalls()
    for _, b in ipairs(balls.items) do
        if b.active and b.stuck then
            b.stuck, b.caught = false, false
            local f = clamp(b.stuckOff / vaus.hw, -1.0, 1.0)
            local ang = math.rad(((f < 0) and -1 or 1) * math.max(20.0, math.abs(f) * 62.0))
            b.du, b.dv = math.sin(ang), math.cos(ang)
        end
    end
end

local function split()
    local src = nil
    for _, b in ipairs(balls.items) do if b.active then src = b; break end end
    if not src then return end
    local base = math.atan(src.du, src.dv)
    for _, off in ipairs({ -0.45, 0.45 }) do
        local b = take(balls)
        if not b then break end
        b.u, b.v, b.speed, b.stuck = src.u, src.v, src.speed, false
        b.du, b.dv = math.sin(base + off), math.cos(base + off)
        if src.stuck then b.dv = math.abs(b.dv) end
        fixAngle(b)
    end
    if src.stuck then releaseBalls() end
end

local function applyCapsule(i)
    local c = CAPSULES[i]
    addScore(1000)
    cue("capsule")
    if c.key == "S" then
        for _, b in ipairs(balls.items) do if b.active then b.speed = baseSpeed() end end
    elseif c.key == "C" or c.key == "E" or c.key == "L" then
        if power == "C" and c.key ~= "C" then releaseBalls() end
        power = c.key
    elseif c.key == "D" then
        split()
    elseif c.key == "B" then
        warpOpen = true
    elseif c.key == "P" then
        lives = lives + 1
    end
    say(c.name, 1.5)
end

local function stepCapsule(dt)
    if not capsule.active then return end
    capsule.t = capsule.t + dt
    capsule.v = capsule.v - CAPSULE_SPEED * dt
    if capsule.v < -12 then capsule.active = false; return end
    if vaus.visible and math.abs(capsule.u - vaus.u) < vaus.hw + 7
       and capsule.v - 3.5 <= PADDLE_TOP and capsule.v + 3.5 >= PADDLE_V then
        capsule.active = false
        applyCapsule(capsule.kind)
    end
end

-- --- Bricks --------------------------------------------------------------------------------

local function rowOf(v) return math.floor((BRICK_TOP - v) / 8) end

local function brickAt(row, col)
    local r = bricks[row]
    local b = r and r[col]
    if b and b.active then return b end
    return nil
end

local function hitBrick(b)
    local def = BRICKS[b.kind]
    if def.gold then flash(b); cue("hard", 0.03); return end
    b.hits = b.hits - 1
    if b.hits > 0 then flash(b); cue("hard", 0.03); return end
    b.active = false
    breakable = breakable - 1
    addScore(def.silver and 50 * round or def.pts)
    local u, v = brickCentre(b.row, b.col)
    burst(u, v, 5, def.mat, 70.0)
    cue("brick", 0.02)
    if not def.silver then dropCapsule(u, v) end
end

-- The brick the ball overlaps most, if any: bounce off the side it came
-- through and push it back out.
local function collideBricks(b)
    local r = BALL_R
    local c0, c1 = math.max(0, math.floor((b.u - r) / 16)), math.min(COLS - 1, math.floor((b.u + r) / 16))
    local r0, r1 = math.max(0, rowOf(b.v + r)), math.min(ROWS - 1, rowOf(b.v - r))
    local best, pen, nx, ny = nil, 0.0, 0.0, 0.0
    for row = r0, r1 do
        for col = c0, c1 do
            local k = brickAt(row, col)
            if k then
                local u0, top = col * 16, BRICK_TOP - row * 8
                local dx = b.u - clamp(b.u, u0, u0 + 16)
                local dy = b.v - clamp(b.v, top - 8, top)
                local d2 = dx * dx + dy * dy
                if d2 < r * r then
                    local p = r - math.sqrt(d2)
                    if not best or p > pen then best, pen, nx, ny = k, p, dx, dy end
                end
            end
        end
    end
    if not best then return end
    if nx == 0.0 and ny == 0.0 then nx, ny = -b.du, -b.dv end   -- centre inside: back out the way it came
    if math.abs(nx) > math.abs(ny) then
        b.du = (nx > 0) and math.abs(b.du) or -math.abs(b.du)
        b.u = b.u + ((nx > 0) and pen or -pen)
    else
        b.dv = (ny > 0) and math.abs(b.dv) or -math.abs(b.dv)
        b.v = b.v + ((ny > 0) and pen or -pen)
    end
    fixAngle(b)
    hitBrick(best)
end

-- --- Enemies -------------------------------------------------------------------------------

local function blockedAt(u, v, r)
    if u - r < 0 or u + r > FIELD_W or v + r > TOP_V + 4 then return true end
    local c0, c1 = math.max(0, math.floor((u - r) / 16)), math.min(COLS - 1, math.floor((u + r) / 16))
    for row = math.max(0, rowOf(v + r)), math.min(ROWS - 1, rowOf(v - r)) do
        for col = c0, c1 do
            if brickAt(row, col) then return true end
        end
    end
    return false
end

local function killEnemy(e)
    e.active = false
    addScore(100)
    burst(e.u, e.v, 8, "enemy", 90.0)
    cue("enemy", 0.05)
end

-- They drift, mostly downwards, and turn when a brick is in the way -- which
-- above a full wall means wandering along it until there is a gap.
local ENEMY_DIRS = { {0, -1}, {0, -1}, {-1, 0}, {1, 0}, {-0.7, -0.7}, {0.7, -0.7} }

local function stepEnemies(dt)
    if enemyShips then
        enemyT = enemyT - dt
        if enemyT <= 0.0 then
            enemyT = rand(6.0, 11.0)
            local e = take(enemyPool)
            if e then
                local g = math.random(1, 2)
                e.u, e.v, e.t, e.dirT = GATE_U[g], TOP_V - 3, 0.0, 0.6
                e.vu, e.vv = 0.0, -20.0
                gates[g].openT = 0.9
            end
        end
    end
    for _, e in ipairs(enemyPool.items) do
        if e.active then
            e.t, e.dirT = e.t + dt, e.dirT - dt
            if e.dirT <= 0.0 then
                local d = ENEMY_DIRS[math.random(1, #ENEMY_DIRS)]
                e.vu, e.vv, e.dirT = d[1] * 22.0, d[2] * 22.0, rand(0.8, 2.0)
            end
            local nu, nv = e.u + e.vu * dt, e.v + e.vv * dt
            if blockedAt(nu, nv, ENEMY_R) and not blockedAt(e.u, e.v, ENEMY_R) then
                e.dirT = 0.0                                  -- try another way next frame
            else
                e.u, e.v = nu, nv
            end
            if e.v < -12 then e.active = false end
            if e.active and vaus.visible and math.abs(e.u - vaus.u) < vaus.hw + ENEMY_R
               and e.v - ENEMY_R <= PADDLE_TOP and e.v + ENEMY_R >= PADDLE_V then
                killEnemy(e)
            end
        end
    end
end

-- --- Ball ------------------------------------------------------------------------------------

local function bounceOffVaus(b)
    local f = clamp((b.u - vaus.u) / vaus.hw, -1.0, 1.0)
    local sgn = (f < 0) and -1 or 1
    if math.abs(f) < 0.05 then sgn = (b.du < 0) and -1 or 1 end
    local ang = math.rad(sgn * math.max(12.0, math.abs(f) * 62.0))
    b.du, b.dv = math.sin(ang), math.cos(ang)
    b.v = PADDLE_TOP + BALL_R
    cue("paddle", 0.04)
    if power == "C" then
        b.stuck, b.caught, b.stuckOff = true, true, b.u - vaus.u
        catchT = 3.0
    end
end

local function stepBall(b, dt)
    if b.stuck then
        b.u, b.v = vaus.u + b.stuckOff, PADDLE_TOP + BALL_R
        return
    end
    b.speed = math.min(maxSpeed(), b.speed + 2.5 * dt)
    local dist = b.speed * dt
    local n = math.max(1, math.ceil(dist / 1.5))
    local step = dist / n
    for _ = 1, n do
        b.u, b.v = b.u + b.du * step, b.v + b.dv * step
        if b.u < BALL_R then
            b.u, b.du = BALL_R, math.abs(b.du); cue("wall", 0.04)
        elseif b.u > FIELD_W - BALL_R then
            b.u, b.du = FIELD_W - BALL_R, -math.abs(b.du); cue("wall", 0.04)
        end
        if b.v > TOP_V - BALL_R then
            b.v, b.dv = TOP_V - BALL_R, -math.abs(b.dv)
            b.speed = math.max(b.speed, math.min(maxSpeed(), baseSpeed() * 1.3))   -- the ceiling wakes it up
            cue("wall", 0.04)
        end
        collideBricks(b)
        for _, e in ipairs(enemyPool.items) do
            if e.active then
                local dx, dy = b.u - e.u, b.v - e.v
                local d = math.sqrt(dx * dx + dy * dy)
                if d < BALL_R + ENEMY_R and d > 0.0 then
                    local nx, ny = dx / d, dy / d
                    local dot = b.du * nx + b.dv * ny
                    if dot < 0 then b.du, b.dv = b.du - 2 * dot * nx, b.dv - 2 * dot * ny end
                    fixAngle(b)
                    killEnemy(e)
                end
            end
        end
        if vaus.visible and b.dv < 0 and b.v - BALL_R <= PADDLE_TOP and b.v >= PADDLE_V
           and math.abs(b.u - vaus.u) <= vaus.hw + BALL_R * 0.5 then
            bounceOffVaus(b)
            if b.stuck then return end
        end
        if b.v < -6 then b.active = false; return end
    end
end

-- --- Laser ------------------------------------------------------------------------------------

local function fireLaser()
    if laserCd > 0.0 then return end
    laserCd = 0.2
    for _, off in ipairs({ -1, 1 }) do
        local s = take(lasers)
        if s then s.u, s.v = vaus.u + off * (vaus.hw - 3.5), PADDLE_TOP + 3 end
    end
    cue("laser", 0.08)
end

local function stepLasers(dt)
    laserCd = math.max(0.0, laserCd - dt)
    for _, s in ipairs(lasers.items) do
        if s.active then
            local dist = LASER_SPEED * dt
            local n = math.max(1, math.ceil(dist / 3.0))
            for _ = 1, n do
                s.v = s.v + dist / n
                local tip = s.v + 3
                if tip >= TOP_V then s.active = false; break end
                local b = brickAt(rowOf(tip), math.floor(s.u / 16))
                if b and tip <= BRICK_TOP then
                    s.active = false
                    hitBrick(b)
                    break
                end
                for _, e in ipairs(enemyPool.items) do
                    if e.active and math.abs(e.u - s.u) < ENEMY_R + 1 and math.abs(e.v - tip) < ENEMY_R + 3 then
                        s.active = false
                        killEnemy(e)
                        break
                    end
                end
                if not s.active then break end
            end
        end
    end
end

-- --- Rounds ------------------------------------------------------------------------------------

local function clearMovers()
    for _, p in ipairs({ balls, lasers, enemyPool }) do
        for _, it in ipairs(p.items) do it.active = false end
    end
    capsule.active = false
end

local function resetVaus()
    vaus.u, vaus.hw, vaus.visible = FIELD_W * 0.5, HW_NORMAL, true
    power, warpOpen, warped = nil, false, false
end

local function loadRound()
    local map = ROUNDS[(round - 1) % #ROUNDS + 1]
    breakable = 0
    for row = 0, ROWS - 1 do
        local line = map[row + 1] or ""
        assert(line == "" or #line == COLS, "round row is not 13 wide: " .. line)
        for col = 0, COLS - 1 do
            local b = bricks[row][col]
            local def = BRICKS[line:sub(col + 1, col + 1)]
            b.active = def ~= nil
            if def then
                b.kind, b.wantMat = line:sub(col + 1, col + 1), def.mat
                b.hits = def.silver and (2 + math.floor((round - 1) / 8)) or 1
                if not def.gold then breakable = breakable + 1 end
            end
        end
    end
    local tint = GRID_TINTS[(round - 1) % #GRID_TINTS + 1]
    if gridMat then game.setMaterialProps(gridMat, { emission = tint }) end
    clearMovers()
    resetVaus()
    enemyT = rand(5.0, 9.0)
end

local function serve()
    local b = take(balls)
    if not b then return end
    b.stuck, b.caught, b.stuckOff, b.speed = true, false, 6.0, baseSpeed()
    b.u, b.v, b.du, b.dv = vaus.u + 6.0, PADDLE_TOP + BALL_R, 0.5, 0.866
    serveT = 0.0
end

local function gameOver()
    mode = "over"
    if score > hiScore then
        hiScore = score
        say("NEW HIGH SCORE!", 8.0)
    else
        say("GAME OVER", 8.0)
    end
end

local function loseVaus()
    vaus.visible = false
    burst(vaus.u, PADDLE_V + 3, 18, "vausEnd", 110.0, 1.3)
    burst(vaus.u, PADDLE_V + 3, 10, "vaus", 90.0, 1.3)
    cue("die")
    clearMovers()
    power, warpOpen = nil, false
    lives = lives - 1
    state, stateT = "dying", 1.8
end

local function roundClear(viaWarp)
    state, stateT, warped = "clear", 2.5, viaWarp
    for _, b in ipairs(balls.items) do b.active = false end
    for _, s in ipairs(lasers.items) do s.active = false end
    for _, e in ipairs(enemyPool.items) do e.active = false end
    capsule.active = false
    if viaWarp then
        addScore(10000)
        say("BREAK!  +10.000", 2.5)
        cue("warp")
    else
        say(string.format("ROUND %d CLEAR", round), 2.5)
        cue("clear")
    end
end

local function startGame()
    score, lives = 0, math.max(1, math.floor(ships))
    round = math.max(1, math.floor(startRound))
    extendNo, nextExtend = 1, EXTENDS[1]
    paused = false
    mode = "play"
    loadRound()
    state, stateT = "ready", 1.8
    say(string.format("ROUND %d", round), 1.8)
end

-- --- Input --------------------------------------------------------------------------------------

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

local function stepVaus(dt)
    local l = anyDown(KEYS.left) and 1 or 0
    local r = anyDown(KEYS.right) and 1 or 0
    local speed = paddleSpeed * (anyDown(KEYS.fast) and 1.5 or 1.0)
    local target = (power == "E") and HW_LONG or HW_NORMAL
    vaus.hw = vaus.hw + (target - vaus.hw) * math.min(1.0, dt * 10.0)
    vaus.u = clamp(vaus.u + (r - l) * speed * dt, vaus.hw, FIELD_W - vaus.hw)
    if warpOpen and state == "play" and r == 1 and vaus.u >= FIELD_W - vaus.hw - 0.5 then
        roundClear(true)
    end
end

-- --- Per-frame -----------------------------------------------------------------------------------

local function stepSparks(dt)
    for _, s in ipairs(sparks.items) do
        if s.active then
            s.life = s.life - dt
            if s.life <= 0.0 then
                s.active = false
            else
                s.u, s.v, s.d = s.u + s.vu * dt, s.v + s.vv * dt, s.d + s.vd * dt
                s.vv = s.vv - 260.0 * dt
                local h = (0.25 + 0.75 * s.life / s.max) * s.size * PX
                game.setPos(s.root, W(s.u, s.v, s.d))
                game.setScale(s.root, h, h, h)
                game.setRot(s.root, s.spinA * s.life, s.spinB * s.life, 0.0)
            end
        end
    end
end

local function stepFlashes(dt)
    local keep = {}
    for _, b in ipairs(flashing) do
        b.flashT = b.flashT - dt
        if b.flashT <= 0.0 then
            if b.kind then b.wantMat = BRICKS[b.kind].mat end
        else
            keep[#keep + 1] = b
        end
    end
    flashing = keep
end

local function drawVaus()
    local show = vaus.visible and mode ~= "over"
    for _, p in ipairs(vaus.parts) do p.active = show end
    vaus.gunL.active, vaus.gunR.active = show and power == "L", show and power == "L"
    if not show or frameNo < 2 then return end
    local u, v, hw = vaus.u, PADDLE_V + 3, vaus.hw
    game.setPos(vaus.body.root, W(u, v, 0.0))
    game.setScale(vaus.body.root, 3.0 * PX, (hw - 3.3) * PX, 3.0 * PX)
    game.setPos(vaus.endL.root, W(u - hw + 3.3, v, 0.0))
    game.setPos(vaus.endR.root, W(u + hw - 3.3, v, 0.0))
    game.setPos(vaus.lightL.root, W(u - hw + 8.0, v, 0.0))
    game.setPos(vaus.lightR.root, W(u + hw - 8.0, v, 0.0))
    if power == "L" then
        game.setPos(vaus.gunL.root, W(u - hw + 3.5, v + 4.5, 0.0))
        game.setPos(vaus.gunR.root, W(u + hw - 3.5, v + 4.5, 0.0))
    end
end

local function draw(dt)
    drawVaus()
    for i, g in ipairs(gates) do
        g.openT = math.max(0.0, g.openT - dt)
        g.active = g.openT <= 0.0
    end
    warpBox.active = warpOpen and mode == "play"
    for i, icon in ipairs(lifeIcons) do icon.active = (mode == "play") and (lives - 1 >= i) end
    if frameNo < 2 then return end
    for _, b in ipairs(balls.items) do
        if b.active then game.setPos(b.root, W(b.u, b.v, 0.0)) end
    end
    for _, s in ipairs(lasers.items) do
        if s.active then game.setPos(s.root, W(s.u, s.v, 0.0)) end
    end
    for _, e in ipairs(enemyPool.items) do
        if e.active then
            game.setPos(e.root, W(e.u, e.v, 0.0))
            game.setRot(e.root, e.t * 70.0, e.t * 110.0, e.t * 150.0)
        end
    end
    if capsule.active then
        game.setPos(capsule.root, W(capsule.u, capsule.v, 0.0))
        game.setRot(capsule.root, math.sin(capsule.t * 5.0) * 15.0, 0.0, 0.0)
    end
    if warpOpen then
        game.setMaterialProps(mats.warp, { emissionStrength = 2.0 + 1.5 * math.sin(now * 8.0) })
    end
    for i, m in ipairs(starMats) do
        game.setMaterialProps(m, { emissionStrength = 1.2 + 0.6 * math.sin(now * (1.3 + i * 0.9) + i) })
    end
end

local function driveCamera()
    local lx, ly, lz = W(FIELD_W * 0.5, 113.0, 0.0)
    local fit = 13.9       -- half the height from the spare Vaus to the ceiling, plus a margin
    local dist = fit / math.tan(math.rad(clamp(camFov, 10.0, 140.0)) * 0.5) / math.max(0.2, camZoom)
    local cx, cy, cz = lx, ly + camRise, lz + dist
    game.setCameraPos(cx, cy, cz)
    game.setCameraDir(lx - cx, ly - cy, lz - cz)
    game.setCameraFov(camFov)
end

local function hud()
    local lines = {}
    local msg = (now < messageUntil) and message or ""
    if mode == "title" then
        lines[1] = string.format("A R K A N O I D          HI-SCORE %s", fmt(hiScore))
        lines[2] = "PRESS ENTER TO PLAY"
        lines[3] = "Left/Right or A/D move (Shift faster)    Space launch / laser    P pause"
        lines[4] = "CAPSULES:  S slow   C catch   E expand   L laser   D disruption   B break   P player"
    else
        local pw = (power == "C" and "   CATCH") or (power == "E" and "   EXPAND") or (power == "L" and "   LASER") or ""
        lines[1] = string.format("SCORE %s      HI-SCORE %s      ROUND %d      VAUS %d%s",
                                 fmt(score), fmt(math.max(hiScore, score)), round, math.max(0, lives), pw)
        if mode == "over" then
            lines[2] = msg ~= "" and msg or "GAME OVER"
            lines[3] = "PRESS ENTER TO PLAY AGAIN"
        elseif paused then
            lines[2] = "PAUSED -- P to go on"
        else
            lines[2] = msg
            for _, b in ipairs(balls.items) do
                if b.active and b.stuck and state == "play" then
                    lines[2] = (msg ~= "" and msg .. "      " or "") .. "SPACE: launch"
                    break
                end
            end
        end
    end
    game.setHud(table.concat(lines, "\n"))
end

-- --- Lifecycle -----------------------------------------------------------------------------------

function start(e)
    KEYS = {
        left  = { game.KEY_LEFT, game.KEY_A },
        right = { game.KEY_RIGHT, game.KEY_D },
        fast  = { game.KEY_LSHIFT, game.KEY_RSHIFT },
        fire  = { game.KEY_SPACE, game.KEY_UP, game.KEY_W },
    }
    -- The field stands 20 m above the carrier, clear of whatever terrain is there.
    OX, OZ, BY = e.x, e.z, e.y + 20.0
    math.randomseed(math.floor(os.time()))
    for key, def in pairs(MAT_DEFS) do
        local t = { name = "Arkanoid " .. key, reflectivity = 0.0 }
        for k, v in pairs(def) do t[k] = v end
        mats[key] = game.createMaterial(t)
    end
    starMats = { mats.star1, mats.star2, mats.star3 }
    gridMat = mats.grid
    for _, f in pairs(CUE_FILES) do cues[f] = game.findAsset(f, "Sound") ~= nil end

    objects, bricks, flashing, gates, lifeIcons = {}, {}, {}, {}, {}
    buildScene()
    buildBricks()
    buildVaus()
    buildPools()
    game.setCamera(-1)
    mode = "title"
    round = math.max(1, math.floor(startRound))
    loadRound()
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

    if not paused and mode == "play" then
        if state == "ready" then
            stepVaus(dt)
            stateT = stateT - dt
            if stateT <= 0.0 then state = "play"; serve() end
        elseif state == "play" then
            stepVaus(dt)
            if state == "play" then
                local stuck, caught = false, false
                for _, b in ipairs(balls.items) do
                    if b.active and b.stuck then
                        stuck = true
                        caught = caught or b.caught
                    end
                end
                if stuck then
                    serveT = serveT + dt
                    catchT = catchT - dt
                    -- A caught ball lets go by itself after a while, a served one if autoLaunch says so.
                    if fireHit or (autoLaunch > 0.0 and serveT > autoLaunch) or (caught and catchT <= 0.0) then
                        releaseBalls()
                        serveT = 0.0
                    end
                elseif power == "L" and fireHeld then
                    fireLaser()
                end
                for _, b in ipairs(balls.items) do
                    if b.active then stepBall(b, dt) end
                end
                stepLasers(dt)
                stepCapsule(dt)
                stepEnemies(dt)
                if breakable <= 0 then
                    roundClear(false)
                elseif countBalls() == 0 then
                    loseVaus()
                end
            end
        elseif state == "dying" then
            stateT = stateT - dt
            if stateT <= 0.0 then
                if lives <= 0 then
                    gameOver()
                else
                    resetVaus()
                    state, stateT = "ready", 1.2
                    say("READY", 1.2)
                end
            end
        elseif state == "clear" then
            if warped then vaus.u = vaus.u + 60.0 * dt else stepVaus(dt) end
            stateT = stateT - dt
            if stateT <= 0.0 then
                round = round + 1
                loadRound()
                state, stateT = "ready", 1.8
                say(string.format("ROUND %d", round), 1.8)
            end
        end
    end
    if not paused then
        stepSparks(dt)
        stepFlashes(dt)
    end
    draw(dt)
    syncVisibility()
    driveCamera()
    hud()
end
