-- SKYSTRIKE -- a small vertical shoot'em up, entirely in Lua.
--
-- Put this script on ONE object (an Empty is best) and press Play. Sea, clouds,
-- your ship, the enemy squadrons and the boss are all spawned around it and
-- removed again when Play stops.
--
--   Arrows / WASD         fly
--   Shift (hold)          fly slowly, for threading through bullets
--   Space / J (hold)      fire          (or set autoFire and never hold it)
--   B / X / K             bomb: clears the bullets, hurts everything on screen
--   P                     pause
--   Enter                 start / play again
--
-- Every stage is a run of squadrons -- darts that dive, saucers that swoop in
-- from the sides, gunships that park and spray -- and ends with a boss. Beat it
-- and the next stage comes round faster and angrier. Orange orbs power up the
-- guns (four levels), green ones are a bomb, the pink one after a boss is a
-- ship. A lost ship costs one gun level.
--
-- Everything is pooled: the bullets, enemies and sparks are spawned once at the
-- start and switched on and off, because game.spawn only delivers at the end
-- of the frame -- a bullet that appears a frame late is a bullet that hit you
-- from nowhere. Collisions are circles on the flight plane, done here.
--
-- The camera is driven every frame. Make sure no Camera entity in the scene is
-- "active on start" -- that one would take the view back.

-- --- Inspector parameters (globals = editable fields on the Script component) --
ships       = 3       -- ships per game
bombs       = 3       -- bombs at the start (and at least 2 after a lost ship)
autoFire    = false   -- fire all the time, no button to hold
difficulty  = 1.0     -- scales enemy bullet speed and how often they fire
scrollSpeed = 5.0     -- how fast the sea slides by
camHeight   = 36.0    -- camera height above the flight plane
camFov      = 38.0
sound       = true

-- --- Layout (flight-plane units, 1 unit = 1 world metre) -------------------------
-- u runs across the screen, v up it. The sea lies 2 m above the carrier, the
-- flight plane FLY_Y above the sea, the clouds between the two.
local FLY_Y      = 5.0
local CLOUD_Y    = 2.5
local LIMIT_U    = 12.0
local LIMIT_V0, LIMIT_V1 = -10.0, 8.5
local SPAWN_V    = 13.5
local GONE_V     = 16.5
local PLAYER_R   = 0.3         -- the hitbox is the cockpit, not the wings
local SHOT_SPEED = 34.0
local MAX_BOMBS  = 5
-- Extra ships at these scores, then every EXTEND_EVERY. Kills score more in
-- later stages, so a fixed step would hand out a ship per stage there.
local EXTENDS    = { 40000, 120000 }
local EXTEND_EVERY = 250000

-- Kept darker than they look right: the sea is dark, the auto exposure opens
-- up for it, and anything pale then blooms. The clouds are opaque on purpose --
-- see-through ones were drawn over the shots flying above them.
local MAT_DEFS = {
    -- Matte: a glossy sea mirrors the sun straight up into a looking-down
    -- camera, a white blot right where the bullets are.
    sea       = { color = {0.0, 0.08, 0.26}, roughness = 0.45, reflectivity = 0.08 },
    foam      = { color = {0.12, 0.24, 0.30}, roughness = 0.6 },
    sand      = { color = {0.46, 0.39, 0.26}, roughness = 0.9 },
    grass     = { color = {0.12, 0.33, 0.10}, roughness = 0.85 },
    tree      = { color = {0.06, 0.20, 0.06}, roughness = 0.9 },
    cloud     = { color = {0.52, 0.54, 0.57}, roughness = 1.0 },
    hull      = { color = {0.62, 0.66, 0.72}, roughness = 0.3, reflectivity = 0.6 },
    wing      = { color = {0.12, 0.30, 0.70}, roughness = 0.35, reflectivity = 0.3 },
    glass     = { color = {0.10, 0.60, 0.80}, emission = {0.1, 0.6, 0.9}, emissionStrength = 1.5,
                  roughness = 0.05, reflectivity = 0.8 },
    engine    = { color = {1.0, 0.6, 0.2}, emission = {1.0, 0.5, 0.1}, emissionStrength = 7.0 },
    shot      = { color = {0.6, 1.0, 1.0}, emission = {0.3, 0.9, 1.0}, emissionStrength = 8.0 },
    bullet    = { color = {1.0, 0.3, 0.8}, emission = {1.0, 0.15, 0.6}, emissionStrength = 9.0 },
    spark     = { color = {1.0, 0.7, 0.3}, emission = {1.0, 0.55, 0.15}, emissionStrength = 9.0 },
    wave      = { color = {0.9, 0.95, 1.0}, emission = {0.6, 0.8, 1.0}, emissionStrength = 5.0 },
    dart      = { color = {0.75, 0.12, 0.10}, roughness = 0.35, reflectivity = 0.3 },
    darkMetal = { color = {0.16, 0.17, 0.19}, roughness = 0.4, reflectivity = 0.5 },
    eye       = { color = {1.0, 0.9, 0.2}, emission = {1.0, 0.85, 0.1}, emissionStrength = 6.0 },
    saucer    = { color = {0.45, 0.25, 0.65}, roughness = 0.25, reflectivity = 0.6 },
    olive     = { color = {0.26, 0.32, 0.18}, roughness = 0.6 },
    blueGlow  = { color = {0.3, 0.6, 1.0}, emission = {0.2, 0.5, 1.0}, emissionStrength = 6.0 },
    boss      = { color = {0.22, 0.20, 0.24}, roughness = 0.35, reflectivity = 0.5 },
    core      = { color = {1.0, 0.1, 0.1}, emission = {1.0, 0.1, 0.05}, emissionStrength = 5.0 },
    pwP       = { color = {1.0, 0.6, 0.1}, emission = {1.0, 0.5, 0.05}, emissionStrength = 6.0 },
    pwB       = { color = {0.3, 1.0, 0.3}, emission = {0.2, 1.0, 0.2}, emissionStrength = 6.0 },
    pw1       = { color = {1.0, 0.4, 0.7}, emission = {1.0, 0.3, 0.6}, emissionStrength = 6.0 },
    ring      = { color = {0.9, 0.9, 0.9}, roughness = 0.2, reflectivity = 0.9 },
}
local mats = {}

local CUE_FILES = {
    shot = "shot.wav", boom = "missile_hit.wav", bigBoom = "impact.wav",
    hit = "missile_deny.wav", pickup = "plopp.wav", bomb = "missile_launch.wav",
    warning = "missile_lock.wav",
}
local cues, lastCue = {}, {}

-- --- State ------------------------------------------------------------------------
local OX, OZ, BY = 0.0, 0.0, 0.0
local now, frameNo = 0.0, 0
local objects = {}          -- every pooled thing: { root, active, shown }
local shots, bullets, sparks, orbs = nil, nil, nil, nil
local enemyPools = {}
local enemies = {}          -- the active ones, whatever their kind
local scenery = {}
local player = nil
local wave = nil            -- the bomb's shock ring

local mode = "title"        -- title / play / over
local paused = false
local score, hiScore, extendNo, nextExtend = 0, 0, 1, EXTENDS[1]
local lives, bombCount, power = 3, 3, 1
local stage, stageT, waveIdx = 1, 0.0, 1
local bossAlive, clearAt = false, -1.0
local queue = {}            -- delayed spawns: { at, fn }
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

local function fmt(n)
    local out = tostring(math.floor(n)):reverse():gsub("(%d%d%d)", "%1."):reverse()
    if out:sub(1, 1) == "." then out = out:sub(2) end
    return out
end

local function rand(a, b) return a + (b - a) * math.random() end
local function clamp(x, a, b) if x < a then return a elseif x > b then return b end return x end

-- Flight-plane point -> world point.
local function W(u, v, y) return OX + u, BY + (y or FLY_Y), OZ - v end

-- Parked out of sight under the sea until a pool hands the thing out.
local function parkedAt() return W(0.0, 0.0, -40.0) end

local function spawn(kind, x, y, z, hx, hy, hz, mat, parent, name, rot)
    local id = game.spawn{
        type = kind, x = x, y = y, z = z, sx = hx, sy = hy, sz = hz,
        rx = rot and rot[1] or 0.0, ry = rot and rot[2] or 0.0, rz = rot and rot[3] or 0.0,
        material = mat and mats[mat] or nil, parent = parent or -1,
        physics = game.PHYSICS_NONE, name = name or "shmup",
    }
    return id
end

-- A multi-part object: an Empty root with parts placed relative to it. Part
-- offsets are (across, up, forward) -- forward is up the screen, world -z.
local function assembly(name, parts, x, y, z)
    if not x then x, y, z = parkedAt() end
    local root = spawn(game.EMPTY, x, y, z, 0.3, 0.3, 0.3, nil, nil, name)
    local ids = {}
    for i, p in ipairs(parts) do
        ids[i] = spawn(p[1], p[2], p[3], -p[4], p[5], p[6], p[7], p[8], root,
                       name .. " part", p[9])
    end
    local o = { root = root, parts = ids, active = false, shown = true }
    objects[#objects + 1] = o
    return o
end

local function single(name, kind, hx, hy, hz, mat)
    local x, y, z = parkedAt()
    local o = { root = spawn(kind, x, y, z, hx, hy, hz, mat, nil, name),
                active = false, shown = true }
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

-- Pooled objects are switched on and off only when their state changes, and
-- only from the second frame: before that they do not exist in the scene yet.
local function syncVisibility()
    if frameNo < 2 then return end
    for _, o in ipairs(objects) do
        local want = o.active and not o.blinkOff
        if want ~= o.shown then
            game.setActive(o.root, want)
            o.shown = want
        end
    end
end

local function place(o, u, v, y)
    game.setPos(o.root, W(u, v, y))
end

-- --- Building ----------------------------------------------------------------------------

local B, S, C = nil, nil, nil     -- part kinds, set in start (game.* exists then)

local function buildShip()
    -- { kind, across, up, forward, hx, hy, hz, material, rot }
    local o = assembly("player", {
        {B,  0.0,  0.0,  0.0,  0.28, 0.2, 0.85, "hull"},
        {S,  0.0,  0.0,  0.85, 0.26, 0.18, 0.45, "hull"},
        {B,  0.0, -0.02, -0.15, 1.15, 0.06, 0.34, "wing"},
        {B,  0.0,  0.0, -0.75, 0.5, 0.05, 0.18, "wing"},
        {S,  0.0,  0.2,  0.3,  0.16, 0.13, 0.3, "glass"},
        {B, -0.95, 0.05, -0.1, 0.07, 0.1, 0.36, "hull"},
        {B,  0.95, 0.05, -0.1, 0.07, 0.1, 0.36, "hull"},
    })
    o.glow = spawn(B, 0.0, 0.0, 0.98, 0.16, 0.09, 0.12, "engine", o.root, "engine")
    player = { obj = o, u = 0.0, v = -7.0, roll = 0.0, alive = false,
               invuln = 0.0, respawn = 0.0, fireCd = 0.0 }
end

local ENEMY_SHAPES = {
    dart = function()
        return assembly("dart", {
            {S, 0.0, 0.0, 0.0, 0.34, 0.24, 0.8, "dart"},
            {B, 0.0, 0.0, 0.35, 0.75, 0.05, 0.2, "darkMetal"},
            {S, 0.0, 0.14, -0.45, 0.12, 0.1, 0.12, "eye"},
        })
    end,
    swoop = function()
        return assembly("saucer", {
            {C, 0.0, 0.0, 0.0, 0.85, 0.1, 0.85, "saucer"},
            {C, 0.0, -0.06, 0.0, 0.6, 0.1, 0.6, "darkMetal"},
            {S, 0.0, 0.14, 0.0, 0.38, 0.26, 0.38, "glass"},
            {S, 0.7, 0.05, 0.0, 0.1, 0.08, 0.1, "eye"},
            {S, -0.7, 0.05, 0.0, 0.1, 0.08, 0.1, "eye"},
        })
    end,
    gunship = function()
        return assembly("gunship", {
            {B, 0.0, 0.0, 0.0, 1.1, 0.35, 1.0, "olive"},
            {B, 0.0, -0.05, 0.25, 1.9, 0.1, 0.5, "darkMetal"},
            {C, -0.6, 0.4, -0.3, 0.3, 0.16, 0.3, "darkMetal"},
            {C, 0.6, 0.4, -0.3, 0.3, 0.16, 0.3, "darkMetal"},
            {B, -0.6, 0.45, -0.75, 0.07, 0.07, 0.3, "darkMetal"},
            {B, 0.6, 0.45, -0.75, 0.07, 0.07, 0.3, "darkMetal"},
            {B, -0.55, 0.0, 1.05, 0.22, 0.14, 0.1, "blueGlow"},
            {B, 0.55, 0.0, 1.05, 0.22, 0.14, 0.1, "blueGlow"},
        })
    end,
    boss = function()
        local o = assembly("boss", {
            {B, 0.0, 0.0, 0.0, 3.0, 0.6, 1.8, "boss"},
            {B, 0.0, 0.05, -1.9, 2.1, 0.5, 0.55, "darkMetal"},
            {B, 0.0, -0.1, 0.4, 4.8, 0.16, 1.0, "boss"},
            {B, -4.4, 0.0, 0.2, 0.35, 0.35, 1.3, "darkMetal"},
            {B, 4.4, 0.0, 0.2, 0.35, 0.35, 1.3, "darkMetal"},
            {C, -2.1, 0.7, -0.9, 0.45, 0.2, 0.45, "darkMetal"},
            {C, 2.1, 0.7, -0.9, 0.45, 0.2, 0.45, "darkMetal"},
            {B, -2.1, 0.75, -1.5, 0.1, 0.1, 0.5, "darkMetal"},
            {B, 2.1, 0.75, -1.5, 0.1, 0.1, 0.5, "darkMetal"},
            {B, -1.6, 0.0, 1.85, 0.4, 0.3, 0.12, "blueGlow"},
            {B, 1.6, 0.0, 1.85, 0.4, 0.3, 0.12, "blueGlow"},
            {B, -3.6, 0.0, 1.5, 0.3, 0.25, 0.12, "blueGlow"},
            {B, 3.6, 0.0, 1.5, 0.3, 0.25, 0.12, "blueGlow"},
        })
        o.core = spawn(S, 0.0, 0.7, 0.2, 0.7, 0.5, 0.7, "core", o.root, "boss core")
        return o
    end,
}

local ENEMY_KINDS = {
    dart    = { r = 0.7, hp = 1,   score = 100,   pool = 14 },
    swoop   = { r = 0.85, hp = 2,  score = 150,   pool = 12 },
    gunship = { r = 1.4, hp = 14,  score = 600,   pool = 3 },
    boss    = { r = 3.2, hp = 520, score = 10000, pool = 1 },
}

local function buildScenery()
    -- The sea and what floats on it scroll; the flight plane stays put.
    local x, y, z = W(0.0, 0.0, -0.3)
    spawn(B, x, y, z, 90.0, 0.3, 90.0, "sea", nil, "sea")
    for i = 1, 16 do
        local it = { u = rand(-26, 26), v = rand(-18, 22), y = 0.02, speed = 1.0 }
        it.id = spawn(B, x, BY, z, rand(0.6, 1.8), 0.02, rand(0.08, 0.16), "foam", nil, "foam")
        scenery[#scenery + 1] = it
    end
    for i = 1, 5 do
        local r = rand(2.0, 4.5)
        local it = { u = rand(-24, 24), v = rand(-18, 26), y = 0.0, speed = 1.0 }
        local o = assembly("island", {
            {C, 0.0, 0.1, 0.0, r, 0.12, r, "sand"},
            {C, 0.2, 0.28, 0.1, r * 0.78, 0.18, r * 0.72, "grass"},
            {S, r * 0.3, 0.7, 0.2, 0.55, 0.5, 0.55, "tree"},
            {S, -r * 0.25, 0.65, -r * 0.3, 0.5, 0.45, 0.5, "tree"},
            {S, r * 0.05, 0.6, r * 0.35, 0.45, 0.4, 0.45, "tree"},
        }, W(it.u, it.v, it.y))
        o.active = true
        it.obj = o
        scenery[#scenery + 1] = it
    end
    for i = 1, 6 do
        local it = { u = rand(-24, 24), v = rand(-18, 26), y = CLOUD_Y, speed = 1.6 }
        local s = rand(1.0, 1.8)
        local o = assembly("cloud", {
            {S, 0.0, 0.0, 0.0, 2.2 * s, 0.5, 1.5 * s, "cloud"},
            {S, 1.6 * s, 0.1, 0.4, 1.5 * s, 0.45, 1.1 * s, "cloud"},
            {S, -1.5 * s, -0.1, -0.3, 1.4 * s, 0.4, 1.0 * s, "cloud"},
        }, W(it.u, it.v, it.y))
        o.active = true
        it.obj = o
        scenery[#scenery + 1] = it
    end
end

-- A pool item IS its object: the game state (u, v, hp, ...) lives on the same
-- table as the entity ids and the active flag.
local function buildPools()
    shots   = newPool(72, function() return single("shot", B, 0.07, 0.07, 0.42, "shot") end)
    bullets = newPool(200, function() return single("bullet", S, 0.2, 0.2, 0.2, "bullet") end)
    sparks  = newPool(90, function() return single("spark", B, 0.2, 0.2, 0.2, "spark") end)
    orbs    = newPool(4, function()
        local o = assembly("orb", {
            {C, 0.0, 0.0, 0.0, 0.62, 0.05, 0.62, "ring"},
        })
        o.coreId = spawn(S, 0.0, 0.0, 0.0, 0.38, 0.38, 0.38, "pwP", o.root, "orb core")
        return o
    end)
    for kind, def in pairs(ENEMY_KINDS) do
        enemyPools[kind] = newPool(def.pool, function()
            local o = ENEMY_SHAPES[kind]()
            o.kind = kind
            return o
        end)
    end
    wave = single("shockwave", C, 1.0, 0.05, 1.0, "wave")
end

-- --- Effects -------------------------------------------------------------------------------

local function burst(u, v, n, size, speed)
    for i = 1, n do
        local s = take(sparks)
        if not s then return end
        local a = rand(0, math.pi * 2)
        local sp = rand(0.3, 1.0) * (speed or 7.0)
        s.u, s.v, s.y = u, v, FLY_Y
        s.vu, s.vv, s.vy = math.cos(a) * sp, math.sin(a) * sp, rand(-1.0, 3.0)
        s.life, s.max = rand(0.35, 0.7), 0.0
        s.max = s.life
        s.size = rand(0.12, 0.26) * (size or 1.0)
    end
end

local function explode(u, v, size)
    burst(u, v, math.floor(8 + 6 * size), size, 5.0 + 3.0 * size)
    cue(size > 2.0 and "bigBoom" or "boom", 0.06)
end

-- --- Enemy fire ------------------------------------------------------------------------------

-- Later stages fire faster bullets, and more often.
local function bulletSpeed(base)
    return base * (1.0 + 0.1 * (stage - 1)) * math.max(0.3, difficulty)
end
local function fireRate()
    return math.max(0.3, difficulty) * (1.0 + 0.15 * (stage - 1))
end

local function fire(u, v, ang, speed)
    local b = take(bullets)
    if not b then return end
    b.u, b.v = u, v
    b.vu, b.vv = math.cos(ang) * speed, math.sin(ang) * speed
end

local function aimAt(u, v)
    return math.atan(player.v - v, player.u - u)
end

local function fireSpread(u, v, n, spreadDeg, speed, ang)
    ang = ang or aimAt(u, v)
    local step = (n > 1) and math.rad(spreadDeg) / (n - 1) or 0.0
    local a0 = ang - step * (n - 1) * 0.5
    for i = 0, n - 1 do fire(u, v, a0 + step * i, speed) end
end

local function fireRing(u, v, n, speed, offset)
    for i = 0, n - 1 do fire(u, v, offset + i * math.pi * 2 / n, speed) end
end

-- --- Enemies -----------------------------------------------------------------------------

local function launch(kind, init)
    local e = take(enemyPools[kind])
    if not e then return nil end
    local def = ENEMY_KINDS[kind]
    e.hp = math.floor(def.hp * (1.0 + 0.35 * (stage - 1)))
    e.maxHp = e.hp
    e.t, e.fired, e.fireCd, e.flash = 0.0, false, 0.0, 0.0
    e.r = def.r
    for k, v in pairs(init) do e[k] = v end
    enemies[#enemies + 1] = e
    return e
end

local function dropOrb(u, v, kind)
    local o = take(orbs)
    if not o then return end
    o.u, o.v, o.t, o.kind = u, v, 0.0, kind
    game.setMaterial(o.coreId, mats["pw" .. kind])
end

local function killEnemy(e, scored)
    e.active = false
    if not scored then return end
    local def = ENEMY_KINDS[e.kind]
    score = score + def.score * stage
    if e.kind == "boss" then
        explode(e.u, e.v, 4.0)
        for i = 1, 6 do
            queue[#queue + 1] = { at = now + i * 0.15, fn = function()
                explode(e.u + rand(-3.5, 3.5), e.v + rand(-1.5, 1.5), 2.0)
            end }
        end
        bossAlive = false
        clearAt = now + 4.0
        score = score + 5000 * stage
        say(string.format("STAGE %d CLEAR   +%s", stage, fmt(5000 * stage)), 3.5)
        dropOrb(e.u, e.v, "1")
        for _, b in ipairs(bullets.items) do b.active = false end
    else
        explode(e.u, e.v, e.kind == "gunship" and 2.0 or 1.0)
        if e.kind == "gunship" then
            dropOrb(e.u, e.v, power < 4 and "P" or "B")
        elseif math.random() < 0.08 then
            dropOrb(e.u, e.v, math.random() < 0.7 and "P" or "B")
        end
    end
end

local function hurt(e, dmg)
    e.hp = e.hp - dmg
    e.flash = 0.06
    if e.hp <= 0 then killEnemy(e, true) end
end

local UPDATE = {}

function UPDATE.dart(e, dt)
    e.v = e.v - e.speed * dt
    if e.v > 2.0 then
        e.base = e.base + clamp(player.u - e.base, -1.0, 1.0) * 2.0 * dt
    end
    e.u = e.base + math.sin(e.t * 3.0 + e.phase) * 0.9
    if not e.fired and e.v < 6.0 and math.random() < 0.08 * fireRate() + 0.05 * (stage - 1) then
        fire(e.u, e.v, aimAt(e.u, e.v), bulletSpeed(9.0))
    end
    if e.v < 6.0 then e.fired = true end
    if e.v < -GONE_V then killEnemy(e, false) end
    return 0, 0, math.sin(e.t * 3.0 + e.phase) * 25.0   -- banks with its weave
end

function UPDATE.swoop(e, dt)
    local s = e.t * 8.5
    e.u = e.side * (15.5 - s)
    e.v = e.v0 - 6.5 * math.sin(math.pi * s / 31.0)
    if not e.fired and s > e.fireAt then
        e.fired = true
        if math.random() < 0.6 * difficulty + 0.1 * stage then
            fire(e.u, e.v, aimAt(e.u, e.v), bulletSpeed(8.5))
        end
    end
    if s > 31.0 then killEnemy(e, false) end
    return 0, e.t * 240.0, 0
end

function UPDATE.gunship(e, dt)
    if e.t < 1.6 then
        local k = e.t / 1.6
        e.v = SPAWN_V + (6.5 - SPAWN_V) * (1.0 - (1.0 - k) * (1.0 - k))
        e.u = e.u0
    elseif e.t < 9.5 then
        e.u = e.u0 + math.sin((e.t - 1.6) * 0.9) * 2.5
        e.fireCd = e.fireCd - dt
        if e.fireCd <= 0.0 then
            e.fireCd = 1.4 / fireRate()
            fireSpread(e.u, e.v - 0.8, stage >= 2 and 5 or 3, 40.0, bulletSpeed(8.0))
        end
    else
        e.v = e.v + 6.0 * dt       -- gives up and leaves
        if e.v > GONE_V then killEnemy(e, false) end
    end
    return 0, 0, 0
end

function UPDATE.boss(e, dt)
    if e.t < 3.0 then
        local k = e.t / 3.0
        e.v = 17.0 + (6.5 - 17.0) * (1.0 - (1.0 - k) ^ 3)
        e.u = 0.0
        return 0, 0, 0
    end
    local bt = e.t - 3.0
    e.u = math.sin(bt * 0.45) * 6.0
    local phase = math.floor(bt / 6.0) % 3
    local rate = fireRate()
    e.fireCd = e.fireCd - dt
    if phase == 0 then
        if e.fireCd <= 0.0 then
            e.fireCd = 1.3 / rate
            e.spin = (e.spin or 0.0) + 0.2
            fireRing(e.u, e.v, 18 + 2 * stage, bulletSpeed(7.0), e.spin)
        end
    elseif phase == 1 then
        if e.fireCd <= 0.0 then
            e.fireCd = 0.075 / rate
            e.spin = (e.spin or 0.0) + 0.33
            fire(e.u, e.v, e.spin, bulletSpeed(7.5))
            fire(e.u, e.v, e.spin + math.pi, bulletSpeed(7.5))
        end
    else
        if e.fireCd <= 0.0 then
            e.fireCd = 0.85 / rate
            fireSpread(e.u - 2.1, e.v + 0.9, 3, 24.0, bulletSpeed(10.0))
            fireSpread(e.u + 2.1, e.v + 0.9, 3, 24.0, bulletSpeed(10.0))
            fireSpread(e.u, e.v - 1.5, 5, 50.0, bulletSpeed(8.0))
        end
    end
    return 0, 0, math.sin(bt * 0.45) * -6.0
end

-- --- Waves ----------------------------------------------------------------------------------

local function after(delay, fn) queue[#queue + 1] = { at = now + delay, fn = fn } end

local WAVE_FNS = {
    darts = function(w)
        for i = 0, w.n - 1 do
            after(i * 0.32, function()
                launch("dart", { u = w.u, base = w.u + (i % 2 == 0 and 1 or -1) * 0.6 * (i % 3),
                                 v = SPAWN_V, speed = 7.5 + stage * 0.6, phase = i * 0.9 })
            end)
        end
    end,
    swoop = function(w)
        for i = 0, w.n - 1 do
            after(i * 0.38, function()
                launch("swoop", { side = w.side, v0 = w.v or 9.0, u = w.side * 15.5, v = 9.0,
                                  fireAt = rand(8.0, 20.0) })
            end)
        end
    end,
    gunship = function(w)
        launch("gunship", { u0 = w.u, u = w.u, v = SPAWN_V })
    end,
    boss = function()
        bossAlive = true
        say("WARNING -- A HUGE ENEMY APPROACHES", 3.0)
        cue("warning")
        after(1.5, function() launch("boss", { u = 0.0, v = 17.0 }) end)
    end,
}

local STAGE = {
    { t = 1.5,  kind = "darts", n = 5, u = -5 },
    { t = 4.0,  kind = "darts", n = 5, u = 5 },
    { t = 7.0,  kind = "swoop", n = 5, side = -1 },
    { t = 10.0, kind = "swoop", n = 5, side = 1, v = 8.0 },
    { t = 13.0, kind = "gunship", u = -5 },
    { t = 15.5, kind = "darts", n = 7, u = 0 },
    { t = 19.0, kind = "gunship", u = 5 },
    { t = 21.0, kind = "swoop", n = 6, side = -1, v = 10.0 },
    { t = 22.5, kind = "swoop", n = 6, side = 1, v = 7.5 },
    { t = 26.0, kind = "darts", n = 5, u = -8 },
    { t = 26.4, kind = "darts", n = 5, u = 8 },
    { t = 30.0, kind = "gunship", u = -6 },
    { t = 30.2, kind = "gunship", u = 6 },
    { t = 34.0, kind = "swoop", n = 8, side = -1 },
    { t = 35.5, kind = "swoop", n = 8, side = 1, v = 7.0 },
    { t = 39.0, kind = "darts", n = 9, u = 0 },
    { t = 44.0, kind = "boss" },
}

-- --- Player ---------------------------------------------------------------------------------

local GUNS = {
    { {-0.25, 0}, {0.25, 0} },
    { {-0.25, 0}, {0.25, 0}, {-0.55, -6}, {0.55, 6} },
    { {-0.25, 0}, {0.25, 0}, {-0.55, -6}, {0.55, 6}, {-0.8, -14}, {0.8, 14} },
    { {-0.25, 0}, {0.25, 0}, {-0.55, -6}, {0.55, 6}, {-0.8, -14}, {0.8, 14} },
}

local function shoot()
    for _, g in ipairs(GUNS[power]) do
        local s = take(shots)
        if s then
            local a = math.rad(g[2])
            s.u, s.v = player.u + g[1], player.v + 0.9
            s.vu, s.vv = math.sin(a) * SHOT_SPEED, math.cos(a) * SHOT_SPEED
            s.ry = -g[2]
        end
    end
    cue("shot", 0.22)
end

local function respawnPlayer()
    player.alive = true
    player.u, player.v = 0.0, -13.0
    player.respawn = 1.0            -- flies in from below
    player.invuln = 3.0
    player.obj.active = true
end

local function loseShip()
    explode(player.u, player.v, 2.5)
    cue("hit")
    player.alive = false
    player.obj.active = false
    lives = lives - 1
    power = math.max(1, power - 1)
    bombCount = math.max(bombCount, 2)
    if lives <= 0 then
        mode = "over"
        if score > hiScore then hiScore = score; say("NEW HIGH SCORE!", 6.0)
        else say("GAME OVER", 6.0) end
        return
    end
    after(1.2, function() if mode == "play" then respawnPlayer() end end)
end

local function useBomb()
    if bombCount <= 0 or not player.alive or player.respawn > 0.0 then return end
    bombCount = bombCount - 1
    for _, b in ipairs(bullets.items) do
        if b.active then burst(b.u, b.v, 1, 0.6, 2.0); b.active = false end
    end
    for _, e in ipairs(enemies) do
        if e.active and e.v < SPAWN_V then hurt(e, e.kind == "boss" and 25 or 30) end
    end
    wave.active, wave.t, wave.u, wave.v = true, 0.0, player.u, player.v
    player.invuln = math.max(player.invuln, 1.5)
    cue("bomb")
end

local function addScoreChecks()
    if score >= nextExtend then
        extendNo = extendNo + 1
        nextExtend = EXTENDS[extendNo] or (nextExtend + EXTEND_EVERY)
        lives = lives + 1
        say("EXTEND!  +1 SHIP", 2.5)
        cue("pickup")
    end
end

-- --- Game flow ------------------------------------------------------------------------------

local function clearField()
    for _, e in ipairs(enemies) do e.active = false end
    enemies = {}
    for _, p in ipairs({shots, bullets, orbs}) do
        for _, it in ipairs(p.items) do it.active = false end
    end
    queue = {}
end

local function startGame()
    clearField()
    score, extendNo, nextExtend = 0, 1, EXTENDS[1]
    lives, bombCount, power = math.max(1, math.floor(ships)), math.max(0, math.floor(bombs)), 1
    stage, stageT, waveIdx = 1, 0.0, 1
    bossAlive, clearAt = false, -1.0
    mode = "play"
    respawnPlayer()
    say("STAGE 1", 2.0)
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

-- --- Per-frame -------------------------------------------------------------------------------

local function stepPlayer(dt)
    if not player.alive then return end
    local l = anyDown(KEYS.left)  and 1 or 0
    local r = anyDown(KEYS.right) and 1 or 0
    local u = anyDown(KEYS.up)    and 1 or 0
    local d = anyDown(KEYS.down)  and 1 or 0
    local mu, mv = r - l, u - d
    if mu ~= 0 and mv ~= 0 then mu, mv = mu * 0.7071, mv * 0.7071 end
    local speed = anyDown(KEYS.focus) and 6.0 or 13.0

    if player.respawn > 0.0 then
        player.respawn = player.respawn - dt
        player.v = player.v + 6.0 * dt
    else
        player.u = clamp(player.u + mu * speed * dt, -LIMIT_U, LIMIT_U)
        player.v = clamp(player.v + mv * speed * dt, LIMIT_V0, LIMIT_V1)
    end
    player.roll = player.roll + (-mu * 28.0 - player.roll) * math.min(1.0, dt * 10.0)

    player.fireCd = player.fireCd - dt
    local firing = autoFire or anyDown(KEYS.fire)
    if firing and player.fireCd <= 0.0 and player.respawn <= 0.0 then
        player.fireCd = (power >= 4) and 0.07 or 0.1
        shoot()
    end

    player.invuln = math.max(0.0, player.invuln - dt)
    -- Blink while untouchable.
    player.obj.blinkOff = player.invuln > 0.0 and (math.floor(now * 14.0) % 2 == 0)
end

local function stepShots(dt)
    for _, s in ipairs(shots.items) do
        if s.active then
            s.u, s.v = s.u + s.vu * dt, s.v + s.vv * dt
            if s.v > GONE_V or math.abs(s.u) > 24 then
                s.active = false
            else
                for _, e in ipairs(enemies) do
                    if e.active and e.v < SPAWN_V + 1.0 then
                        local du, dv = s.u - e.u, s.v - e.v
                        local rr = e.r + 0.2
                        if du * du + dv * dv < rr * rr then
                            s.active = false
                            burst(s.u, s.v, 1, 0.6, 3.0)
                            hurt(e, 1)
                            break
                        end
                    end
                end
            end
        end
    end
end

local function stepBullets(dt)
    local pr = PLAYER_R + 0.18
    for _, b in ipairs(bullets.items) do
        if b.active then
            b.u, b.v = b.u + b.vu * dt, b.v + b.vv * dt
            if b.v < -15 or b.v > GONE_V or math.abs(b.u) > 24 then
                b.active = false
            elseif player.alive and player.invuln <= 0.0 then
                local du, dv = b.u - player.u, b.v - player.v
                if du * du + dv * dv < pr * pr then
                    b.active = false
                    loseShip()
                end
            end
        end
    end
end

local function stepEnemies(dt)
    local keep = {}
    for _, e in ipairs(enemies) do
        if e.active then
            e.t = e.t + dt
            local rx, ry, rz = UPDATE[e.kind](e, dt)
            if e.active then
                e.flash = math.max(0.0, e.flash - dt)
                place(e, e.u, e.v + (e.flash > 0 and 0.05 or 0.0))
                game.setRot(e.root, rx, ry, rz)
                -- Ramming: costs the ship, and the enemy a good chunk.
                if player.alive and player.invuln <= 0.0 then
                    local du, dv = e.u - player.u, e.v - player.v
                    local rr = e.r * 0.8 + PLAYER_R
                    if du * du + dv * dv < rr * rr then
                        loseShip()
                        hurt(e, 6)
                    end
                end
                if e.kind == "boss" then
                    local pulse = 4.0 + 3.0 * math.sin(now * 6.0) + (e.flash > 0 and 8.0 or 0.0)
                    game.setMaterialProps(mats.core, { emissionStrength = pulse })
                end
            end
            if e.active then keep[#keep + 1] = e end
        end
    end
    enemies = keep
end

local function stepOrbs(dt)
    for _, o in ipairs(orbs.items) do
        if o.active then
            o.t = o.t + dt
            o.v = o.v - 2.2 * dt
            o.u = o.u + math.sin(o.t * 2.0) * 1.5 * dt
            place(o, o.u, o.v)
            game.setRot(o.root, 0, o.t * 180.0, 20.0)
            if o.v < -14 then o.active = false end
            if player.alive then
                local du, dv = o.u - player.u, o.v - player.v
                if du * du + dv * dv < 1.3 * 1.3 then
                    o.active = false
                    cue("pickup")
                    if o.kind == "P" then
                        if power < 4 then power = power + 1; say("POWER UP", 1.2)
                        else score = score + 2000; say("+2.000", 1.0) end
                    elseif o.kind == "B" then
                        if bombCount < MAX_BOMBS then bombCount = bombCount + 1; say("BOMB +1", 1.2)
                        else score = score + 2000; say("+2.000", 1.0) end
                    else
                        lives = lives + 1; say("+1 SHIP", 1.5)
                    end
                end
            end
        end
    end
end

local function stepSparks(dt)
    for _, s in ipairs(sparks.items) do
        if s.active then
            s.life = s.life - dt
            if s.life <= 0.0 then
                s.active = false
            else
                s.u, s.v, s.y = s.u + s.vu * dt, s.v + s.vv * dt, s.y + s.vy * dt
                s.vy = s.vy - 9.0 * dt
                local k = s.life / s.max
                game.setPos(s.root, W(s.u, s.v, s.y))
                game.setScale(s.root, s.size * k + 0.02)
            end
        end
    end
end

local function stepScenery(dt)
    local sp = paused and 0.0 or ((mode == "play") and scrollSpeed or scrollSpeed * 0.6)
    for _, it in ipairs(scenery) do
        it.v = it.v - sp * it.speed * dt
        if it.v < -20.0 then
            it.v = it.v + 44.0
            it.u = rand(-24, 24)
        end
        local id = it.obj and it.obj.root or it.id
        game.setPos(id, W(it.u, it.v, it.y))
    end
end

local function stepStage(dt)
    stageT = stageT + dt
    while STAGE[waveIdx] and not bossAlive and stageT >= STAGE[waveIdx].t do
        local w = STAGE[waveIdx]
        WAVE_FNS[w.kind](w)
        waveIdx = waveIdx + 1
    end
    if clearAt > 0.0 and now >= clearAt then
        clearAt = -1.0
        stage = stage + 1
        stageT, waveIdx = 0.0, 1
        say(string.format("STAGE %d", stage), 2.0)
    end
end

local function draw(dt)
    if player.obj.active then
        place(player.obj, player.u, player.v)
        game.setRot(player.obj.root, 0, 0, player.roll)
        local g = 0.12 + 0.05 * math.sin(now * 60.0)
        game.setScale(player.obj.glow, 0.16, 0.09, g)
    end
    for _, s in ipairs(shots.items) do
        if s.active then
            place(s, s.u, s.v)
            game.setRot(s.root, 0, s.ry or 0, 0)
        end
    end
    for _, b in ipairs(bullets.items) do
        if b.active then place(b, b.u, b.v) end
    end
    if wave.active then
        wave.t = wave.t + dt
        if wave.t > 0.55 then
            wave.active = false
        else
            local r = 1.0 + wave.t * 45.0
            place(wave, wave.u, wave.v, FLY_Y - 0.5)
            game.setScale(wave.root, r, 0.04, r)
        end
    end
end

local function driveCamera()
    local cx, cy, cz = W(0.0, -4.0, FLY_Y + camHeight)
    local lx, ly, lz = W(0.0, 0.0, FLY_Y)
    game.setCameraPos(cx, cy, cz)
    game.setCameraDir(lx - cx, ly - cy, lz - cz)
    game.setCameraFov(camFov)
end

local function hud()
    local lines = {}
    if mode == "title" or mode == "over" then
        lines[1] = string.format("SKYSTRIKE      HI %s", fmt(hiScore))
        lines[2] = (mode == "over") and string.format("GAME OVER   score %s   --   ENTER: play again", fmt(score))
                   or "Press ENTER to start"
        lines[3] = "Arrows/WASD fly   Shift slow   Space fire   B bomb   P pause"
    else
        lines[1] = string.format("SCORE %s     HI %s     STAGE %d",
                                 fmt(score), fmt(math.max(hiScore, score)), stage)
        local boss = nil
        for _, e in ipairs(enemies) do if e.kind == "boss" then boss = e end end
        lines[2] = string.format("SHIPS %d   BOMBS %d   POWER %d", lives, bombCount, power)
        if boss then
            local n = math.floor(20 * boss.hp / boss.maxHp + 0.5)
            lines[2] = lines[2] .. "     BOSS [" .. string.rep("#", n) .. string.rep("-", 20 - n) .. "]"
        end
        lines[3] = paused and "PAUSED -- P to go on" or (now < messageUntil and message or "")
    end
    if (mode ~= "play") and now < messageUntil then lines[2] = message .. "     " .. lines[2] end
    game.setHud(table.concat(lines, "\n"))
end

-- --- Lifecycle ---------------------------------------------------------------------------------

function start(e)
    B, S, C = game.BOX, game.SPHERE, game.CYLINDER
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
    math.randomseed(math.floor(os.time()))
    for key, def in pairs(MAT_DEFS) do
        local t = { name = "Shmup " .. key }
        for k, v in pairs(def) do t[k] = v end
        mats[key] = game.createMaterial(t)
    end
    for _, f in pairs(CUE_FILES) do cues[f] = game.findAsset(f, "Sound") ~= nil end

    objects, scenery, enemies, enemyPools = {}, {}, {}, {}
    buildScenery()
    buildShip()
    buildPools()
    game.setCamera(-1)
    mode = "title"
end

function update(e, dt, t)
    now = t
    frameNo = frameNo + 1
    dt = math.min(dt, 1.0 / 30.0)

    local startHit = pressed("start", game.keyDown(game.KEY_ENTER))
    local bombHit  = pressed("bomb", anyDown(KEYS.bomb))
    local pauseHit = pressed("pause", game.keyDown(game.KEY_P))

    if mode ~= "play" and startHit then startGame() end
    if mode == "play" and pauseHit then paused = not paused end

    stepScenery(dt)
    if mode == "play" and not paused then
        -- Delayed spawns (squadron members, boss debris, respawn).
        local rest = {}
        for _, q in ipairs(queue) do
            if now >= q.at then q.fn() else rest[#rest + 1] = q end
        end
        queue = (#queue == 0) and queue or rest
        stepStage(dt)
        stepPlayer(dt)
        if bombHit then useBomb() end
        stepShots(dt)
        stepEnemies(dt)
        stepBullets(dt)
        stepOrbs(dt)
        addScoreChecks()
    elseif mode ~= "play" then
        -- Title / game over: the squadrons still in the air carry on without you.
        player.obj.blinkOff = false
        stepEnemies(dt)
        stepBullets(dt)
    end
    if not paused then stepSparks(dt) end
    draw(dt)
    syncVisibility()
    driveCamera()
    hud()
end
