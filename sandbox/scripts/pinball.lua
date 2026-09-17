-- Flipper -- a complete pinball machine, entirely in Lua.
--
-- Put this script on ONE object (an Empty is best) and press Play. The machine
-- is built around that object -- cabinet, playfield, flippers, bumpers, lights
-- -- and removed again when Play stops. Nothing else has to be in the scene.
--
--   Left Shift / Left arrow / A / left mouse      left flipper
--   Right Shift / Right arrow / D / right mouse   right flipper
--   Space / Down / S   hold to pull the plunger, let go to shoot
--   Up / W             nudge the table (too often and it TILTs)
--   Enter              start a game (Space works too while none is running)
--
-- The ball does not use the engine's rigid bodies. A pinball lives or dies by
-- one contact -- ball against a flipper that is itself swinging at twenty
-- radians a second -- and that is easier to get right, and to keep right, in a
-- small dedicated simulation: the playfield is a plane, the ball a circle on
-- it, every wall a line segment or a circle, and the whole thing is stepped at
-- 720 Hz so nothing can pass through anything. The 3D objects only show where
-- the simulation says things are.
--
-- The rules, as on a real table:
--   * Pop bumpers and slingshots kick the ball away (100 / 10 points).
--   * The three top lanes light as the ball rolls through; all three raise the
--     bonus multiplier (up to x5). Either flipper button shifts the lit lanes.
--   * The first lane the plunged ball drops into after a launch is the skill
--     shot while it blinks (10,000).
--   * Knock down all three drop targets to light the saucer for an EXTRA BALL.
--   * The saucer on the right holds the ball, scores and throws it back up.
--   * For the first seconds of every ball a drained ball is served again.
--   * At the end of a ball the bonus is counted, times the multiplier.
--
-- The camera is driven every frame. Make sure no Camera entity in the scene is
-- "active on start" -- that one would take the view back.

-- --- Inspector parameters (globals = editable fields on the Script component) --
balls        = 3       -- balls per game
gravity      = 21.0    -- pull down the playfield, m/s^2 (a 6.5 degree slope,
                       -- scaled with the table, which is 20x a real one)
flipperSpeed = 20.0    -- how fast a flipper swings up, radians per second
ballSaveTime = 10.0    -- seconds after the launch in which a drain is forgiven
extraBallAt  = 75000   -- score that earns an extra ball (0 = never)
centerPost   = false   -- a post between the flippers: fewer drains, easier game
camHeight    = 17.0    -- camera height above the playfield
camBack      = 11.0    -- how far behind the bottom edge the camera stands
camFov       = 60.0
standHeight  = 3.0     -- playfield height above the carrier object
sound        = true    -- play the cues (silently skipped if a file is absent)

-- --- Table geometry, in playfield units (1 unit = 1 world metre) ----------------
-- u runs across the table (left to right), v up it (from the flippers to the
-- back). The playfield is u -5..5, the plunger lane u 5..6, and the back is a
-- half circle round (0.5, 15.5).
local BALL_R     = 0.27
local H          = 1.0 / 720.0      -- simulation step
local MAX_SPEED  = 55.0
local WALL_H     = 0.55             -- height of the guides above the playfield
local ARC_CU, ARC_CV, ARC_R = 0.5, 15.5, 5.5
local LANE_L, LANE_R = 5.0, 6.0     -- the plunger lane
local PLUNGER_REST, PLUNGER_TRAVEL = 0.6, 1.0
local DRAIN_V    = -0.9

local FLIP_LEN   = 1.85
local FLIP_R     = 0.22             -- the flipper's collision radius
local FLIP_REST  = math.rad(-30.0)
local FLIP_UP    = math.rad(32.0)
local FLIP_RETURN = 14.0            -- rad/s back down

-- --- Colours / materials (created in start, named "Pinball ...") -------------
local MAT_DEFS = {
    -- Whites stay well below 1: the playfield is dark, the auto exposure opens
    -- up for it, and a pure white part then burns out into a glowing blob.
    playfield = { color = {0.035, 0.06, 0.16}, roughness = 0.28, reflectivity = 0.08 },
    art       = { color = {0.05, 0.09, 0.24}, roughness = 0.35 },
    metal     = { color = {0.62, 0.63, 0.66}, roughness = 0.22, reflectivity = 0.85 },
    rubber    = { color = {0.62, 0.62, 0.58}, roughness = 0.65 },
    flipper   = { color = {0.52, 0.52, 0.52}, roughness = 0.40 },
    flipRub   = { color = {0.80, 0.08, 0.06}, roughness = 0.55 },
    wood      = { color = {0.30, 0.15, 0.06}, roughness = 0.55 },
    cabinet   = { color = {0.45, 0.04, 0.05}, roughness = 0.35, reflectivity = 0.15 },
    black     = { color = {0.02, 0.02, 0.02}, roughness = 0.8 },
    ball      = { color = {0.92, 0.92, 0.94}, roughness = 0.06, reflectivity = 1.0 },
    target    = { color = {0.95, 0.72, 0.08}, roughness = 0.35 },
    bumperOff = { color = {0.75, 0.10, 0.08}, roughness = 0.30 },
    bumperOn  = { color = {1.00, 0.55, 0.30}, emission = {1.0, 0.45, 0.15}, emissionStrength = 7.0 },
    lampOff   = { color = {0.20, 0.13, 0.06}, roughness = 0.15, reflectivity = 0.2 },
    lampAmber = { color = {1.00, 0.70, 0.20}, emission = {1.0, 0.62, 0.12}, emissionStrength = 5.0 },
    lampRed   = { color = {1.00, 0.25, 0.20}, emission = {1.0, 0.12, 0.08}, emissionStrength = 5.0 },
    lampGreen = { color = {0.30, 1.00, 0.40}, emission = {0.15, 1.0, 0.25}, emissionStrength = 5.0 },
    lampBlue  = { color = {0.35, 0.60, 1.00}, emission = {0.20, 0.45, 1.0}, emissionStrength = 5.0 },
    backglass = { color = {0.20, 0.05, 0.30}, emission = {0.5, 0.1, 0.8}, emissionStrength = 2.0 },
}
local mats = {}

-- --- Sounds ---------------------------------------------------------------------
local CUE_FILES = {
    flipper = "swoosh.wav", bumper = "plopp.wav", sling = "impact.wav",
    target = "impact.wav", plunger = "shot.wav", saucer = "missile_lock.wav",
    drain = "missile_deny.wav", special = "missile_launch.wav",
}
local cues = {}

-- --- State ------------------------------------------------------------------------
local OX, OZ, BY, GROUND = 0.0, 0.0, 0.0, 0.0  -- table origin in the world
local spawned  = {}
local segs, circles, sensors, flippers = {}, {}, {}, {}
local lamps    = {}      -- key -> { id, want, applied }
local ball     = { x = 5.5, y = 1.2, vx = 0.0, vy = 0.0, live = false,
                   held = false, id = nil, still = 0.0 }
local plunger  = { y = PLUNGER_REST, vy = 0.0, pull = 0.0, fire = 0.0, ids = {} }
local now      = 0.0
local acc      = 0.0

-- Game state.
local mode       = "attract"   -- attract / play / bonus / over
local score, highScore = 0, 0
local ballNo, extraBalls = 1, 0
local bonus, mult = 0, 1
local laneLit    = { false, false, false }
local skillLane, skillLive = 0, false
local dropDown   = { false, false, false }
local dropResetAt = -1.0
local extraLit   = false
local extraAwarded = false
local saveUntil, saveArmed = -1.0, false
local tilt, tilted = 0.0, false
local message, messageUntil = "", 0.0
local bonusShown, bonusEnd = 0, 0.0
local saucerUntil = -1.0
local shake       = 0.0
local keysPrev    = {}
local lastCue     = {}

-- --- Small helpers ------------------------------------------------------------------

local function cue(name)
    if not sound then return end
    local f = CUE_FILES[name]
    if not f or not cues[f] then return end
    -- The same cue at most every 50 ms: a ball buzzing between two bumpers
    -- would otherwise stack dozens of one-shots.
    if lastCue[name] and now - lastCue[name] < 0.05 then return end
    lastCue[name] = now
    game.playSound(f)
end

local function say(text, secs)
    message, messageUntil = text, now + (secs or 2.0)
end

local function fmt(n)
    local s = tostring(math.floor(n))
    local out = s:reverse():gsub("(%d%d%d)", "%1."):reverse()
    if out:sub(1, 1) == "." then out = out:sub(2) end
    return out
end

local function addScore(n)
    if tilted or mode ~= "play" then return end
    local before = score
    score = score + n
    if extraBallAt > 0 and not extraAwarded and before < extraBallAt
       and score >= extraBallAt then
        extraAwarded = true
        extraBalls = extraBalls + 1
        say("REPLAY!  EXTRA BALL", 3.0)
        cue("special")
    end
end

-- Table point -> world point.
local function W(u, v, y)
    return OX + u, BY + (y or 0.0), OZ - v
end

-- The yaw that lays an object's local X along table direction (du, dv).
-- Entity rotation is Rz*Ry*Rx in degrees; Ry turns local X to (cos, 0, -sin),
-- and table v runs along world -z, so the angle is simply atan2(dv, du).
local function yawOf(du, dv)
    return math.deg(math.atan(dv, du))
end

local function spawnPart(kind, u, v, y, hx, hy, hz, yaw, mat, name)
    local x, wy, z = W(u, v, y)
    local id = game.spawn{
        type = kind, x = x, y = wy, z = z,
        sx = hx, sy = hy, sz = hz, ry = yaw or 0.0,
        material = mats[mat], physics = game.PHYSICS_NONE,
        name = name or "pinball",
    }
    spawned[#spawned + 1] = id
    return id
end

local function box(u, v, y, hx, hy, hz, yaw, mat, name)
    return spawnPart(game.BOX, u, v, y, hx, hy, hz, yaw, mat, name)
end
local function cyl(u, v, y, r, hy, mat, name)
    return spawnPart(game.CYLINDER, u, v, y, r, hy, r, 0.0, mat, name)
end

-- --- Colliders --------------------------------------------------------------------
-- seg:    a line segment with a radius (a capsule), bounciness `e`
-- circle: a post or a bumper
-- `kind` says what a hit means; `oneway` segments only stop a ball coming from
-- their left-normal side (the gate at the top of the plunger lane).

local function addSeg(ax, ay, bx, by, r, e, kind, extra)
    local dx, dy = bx - ax, by - ay
    local len = math.sqrt(dx * dx + dy * dy)
    -- Bounds grown by both radii: most segments are nowhere near the ball, and
    -- four comparisons turn them away before any real work.
    local m = r + BALL_R
    local s = { ax = ax, ay = ay, bx = bx, by = by, dx = dx / len, dy = dy / len,
                len = len, nx = -dy / len, ny = dx / len, r = r, e = e,
                x0 = math.min(ax, bx) - m, x1 = math.max(ax, bx) + m,
                y0 = math.min(ay, by) - m, y1 = math.max(ay, by) + m,
                kind = kind or "wall", active = true, cool = 0.0 }
    if extra then for k, v in pairs(extra) do s[k] = v end end
    segs[#segs + 1] = s
    return s
end

local function addCircle(x, y, r, e, kind, extra)
    local c = { x = x, y = y, r = r, e = e, kind = kind or "post", cool = 0.0 }
    if extra then for k, v in pairs(extra) do c[k] = v end end
    circles[#circles + 1] = c
    return c
end

local function addSensor(x, y, r, kind, index)
    local s = { x = x, y = y, r = r, kind = kind, index = index, inside = false }
    sensors[#sensors + 1] = s
    return s
end

-- Walls are drawn as boxes along their segment.
local function wallVis(s, mat, h)
    local du, dv = s.bx - s.ax, s.by - s.ay
    local hl = s.len * 0.5 + s.r * 0.6
    local hy = (h or WALL_H) * 0.5
    s.vis = box((s.ax + s.bx) * 0.5, (s.ay + s.by) * 0.5, hy,
                hl, hy, math.max(s.r, 0.05), yawOf(du, dv), mat, "guide")
    return s.vis
end

-- A chain of walls through a list of points {u1, v1, u2, v2, ...}.
local function wallChain(pts, r, e, mat, h)
    for i = 1, #pts - 3, 2 do
        wallVis(addSeg(pts[i], pts[i + 1], pts[i + 2], pts[i + 3], r, e), mat, h)
    end
end

-- --- Lamps --------------------------------------------------------------------------

local function lamp(key, u, v, r, onMat)
    local id = cyl(u, v, 0.012, r, 0.012, "lampOff", "lamp " .. key)
    lamps[key] = { id = id, on = onMat, want = false, applied = false }
end

local function setLamp(key, on)
    local l = lamps[key]
    if l then l.want = on end
end

-- Lamp materials are swapped only when a lamp changes. setMaterial fails on a
-- part spawned this frame (it appears at the end of the frame), so the applied
-- state only counts once the engine confirmed it.
local function syncLamps()
    for _, l in pairs(lamps) do
        if l.want ~= l.applied then
            if game.setMaterial(l.id, l.want and mats[l.on] or mats.lampOff) then
                l.applied = l.want
            end
        end
    end
end

-- --- Building the machine -------------------------------------------------------

local function buildTable()
    segs, circles, sensors, flippers, lamps = {}, {}, {}, {}, {}

    -- Playfield and cabinet.
    box(0.5, 10.5, -0.12, 5.75, 0.12, 11.6, 0, "playfield", "playfield")
    box(0.5, 10.5, -0.95, 6.2, 0.7, 12.0, 0, "cabinet", "cabinet")
    box(-5.45, 10.5, 0.25, 0.35, 0.5, 11.9, 0, "wood", "side rail")
    box( 6.45, 10.5, 0.25, 0.35, 0.5, 11.9, 0, "wood", "side rail")
    box(0.5, -1.35, 0.25, 6.3, 0.5, 0.45, 0, "wood", "front rail")
    -- The apron over the drain, where the ball disappears.
    box(0.5, -0.55, 0.3, 5.1, 0.3, 0.4, 0, "art", "apron")

    -- Legs down to the carrier's level, and the backbox.
    local legTop = BY - 1.65
    local legH = math.max(0.2, legTop - GROUND)
    for _, p in ipairs({{-5.2, -0.5}, {6.2, -0.5}, {-5.2, 21.5}, {6.2, 21.5}}) do
        local x, _, z = W(p[1], p[2])
        local id = game.spawn{ type = game.CYLINDER, x = x, y = GROUND + legH * 0.5,
                               z = z, sx = 0.28, sy = legH * 0.5, sz = 0.28,
                               material = mats.metal, physics = game.PHYSICS_NONE,
                               name = "leg" }
        spawned[#spawned + 1] = id
    end
    box(0.5, 22.6, 3.4, 6.2, 3.8, 0.55, 0, "cabinet", "backbox")
    box(0.5, 22.02, 3.6, 5.5, 3.0, 0.04, 0, "backglass", "backglass")

    -- Artwork on the playfield: a lighter band down the middle.
    box(0.0, 8.0, 0.002, 2.2, 0.002, 5.0, 0, "art", "art")

    -- Outer walls, the back arc, and the plunger lane.
    wallVis(addSeg(-5.0, 1.8, -5.0, ARC_CV, 0.1, 0.45), "metal")
    wallVis(addSeg(LANE_R, -0.6, LANE_R, ARC_CV, 0.1, 0.45), "metal")
    wallVis(addSeg(LANE_L, -0.6, LANE_L, 13.8, 0.1, 0.45), "metal")
    local N = 26
    for i = 0, N - 1 do
        local a0, a1 = math.pi * i / N, math.pi * (i + 1) / N
        wallVis(addSeg(ARC_CU + math.cos(a0) * ARC_R, ARC_CV + math.sin(a0) * ARC_R,
                       ARC_CU + math.cos(a1) * ARC_R, ARC_CV + math.sin(a1) * ARC_R,
                       0.1, 0.45), "metal")
    end
    -- One-way gate over the top of the lane: the plunged ball passes, a ball
    -- coming back down rolls off it into the playfield instead of the lane.
    wallVis(addSeg(LANE_L, 13.8, LANE_R, 14.6, 0.06, 0.3, "wall", { oneway = true }),
            "metal", 0.3)

    -- Aprons under the flippers, funnelling everything into the drain.
    wallVis(addSeg(-5.0, 1.8, -1.3, 0.1, 0.1, 0.3), "metal")
    wallVis(addSeg(LANE_L, 1.8, 1.3, 0.1, 0.1, 0.3), "metal")

    -- The orbit's exit: a ball coming round the back arc runs down the left
    -- wall, and without this it would run straight on into the left outlane --
    -- every full plunge would drain. One-way, so a shot UP the left side still
    -- gets into the orbit.
    wallVis(addSeg(-5.0, 13.0, -4.2, 12.2, 0.08, 0.5, "wall", { oneway = true }),
            "metal", 0.3)

    -- Outlane / inlane separators, each topped with a rubber post. The last
    -- stretch ends right over the flipper's pivot, its surface level with the
    -- top of the pivot: a ball rolling down it goes straight onto the bat.
    -- Ended short of the pivot it left a dip between the two, and a ball
    -- could lie in it for ever.
    for _, sd in ipairs({-1, 1}) do
        wallChain({4.2 * sd, 5.4, 4.2 * sd, 4.0, 2.2 * sd, 2.4 + FLIP_R - 0.08},
                  0.08, 0.4, "metal")
        addCircle(4.2 * sd, 5.4, 0.16, 0.7, "post")
        cyl(4.2 * sd, 5.4, WALL_H * 0.5, 0.16, WALL_H * 0.5, "rubber", "post")
    end

    -- Slingshots: two walls and a kicking rubber face toward the middle.
    for _, sd in ipairs({-1, 1}) do
        local ax, ay = 3.35 * sd, 5.4
        local bx, by = 3.35 * sd, 4.45
        local cx, cy = 2.3 * sd, 3.7
        wallVis(addSeg(ax, ay, bx, by, 0.08, 0.3), "metal")
        wallVis(addSeg(bx, by, cx, cy, 0.08, 0.3), "metal")
        local face = addSeg(ax, ay, cx, cy, 0.1, 0.5, "sling")
        -- The kicking normal points away from the triangle's third corner.
        if (bx - ax) * face.nx + (by - ay) * face.ny > 0 then
            face.kx, face.ky = -face.nx, -face.ny
        else
            face.kx, face.ky = face.nx, face.ny
        end
        face.vis = wallVis(face, "rubber", 0.4)
        -- A plate between the three walls so it reads as one solid block.
        box((ax + bx + cx) / 3, (ay + by + cy) / 3, 0.2, 0.32, 0.2, 0.45, 0,
            "cabinet", "sling body")
        lamp("sling" .. sd, (ax + bx + cx) / 3 - 0.1 * sd, (ay + by + cy) / 3 + 1.0,
             0.13, "lampRed")
    end

    -- Top rollover lanes.
    for _, gu in ipairs({-1.3, -0.1, 1.1, 2.3}) do
        wallVis(addSeg(gu, 17.2, gu, 18.4, 0.08, 0.4), "metal", 0.35)
        addCircle(gu, 17.2, 0.1, 0.5, "post")
    end
    for i, lu in ipairs({-0.7, 0.5, 1.7}) do
        addSensor(lu, 17.8, 0.4, "lane", i)
        lamp("lane" .. i, lu, 16.6, 0.2, "lampAmber")
    end

    -- Pop bumpers.
    for i, p in ipairs({{-0.9, 14.2}, {1.9, 14.2}, {0.5, 12.4}}) do
        local c = addCircle(p[1], p[2], 0.6, 0.6, "bumper", { index = i })
        cyl(p[1], p[2], 0.16, 0.66, 0.16, "metal", "bumper base")
        cyl(p[1], p[2], 0.42, 0.52, 0.1, "rubber", "bumper ring")
        c.cap = cyl(p[1], p[2], 0.62, 0.46, 0.1, "bumperOff", "bumper cap")
        c.lit = false
    end

    -- Drop targets on the left.
    for i, v0 in ipairs({8.0, 8.9, 9.8}) do
        local t = addSeg(-4.45, v0, -4.45, v0 + 0.7, 0.1, 0.25, "drop", { index = i })
        t.vis = box(-4.45, v0 + 0.35, 0.35, 0.1, 0.35, 0.35, 0, "target", "drop target")
        lamp("drop" .. i, -3.75, v0 + 0.35, 0.16, "lampGreen")
    end
    wallVis(addSeg(-4.45, 7.7, -5.0, 7.3, 0.08, 0.3), "metal")   -- bank ends
    wallVis(addSeg(-4.45, 10.8, -5.0, 11.2, 0.08, 0.3), "metal")

    -- Saucer on the right.
    addSensor(3.9, 9.5, 0.32, "saucer", 1)
    cyl(3.9, 9.5, 0.005, 0.42, 0.006, "metal", "saucer rim")
    cyl(3.9, 9.5, 0.012, 0.3, 0.008, "black", "saucer")
    lamp("extra", 3.2, 8.6, 0.22, "lampRed")

    -- Inlane / outlane rollovers.
    addSensor(-4.6, 4.4, 0.35, "outlane", 1)
    addSensor( 4.6, 4.4, 0.35, "outlane", 2)
    addSensor(-3.78, 5.0, 0.35, "inlane", 1)
    addSensor( 3.78, 5.0, 0.35, "inlane", 2)
    lamp("out1", -4.6, 3.6, 0.13, "lampRed");  lamp("out2", 4.6, 3.6, 0.13, "lampRed")
    lamp("in1", -3.78, 4.0, 0.13, "lampBlue"); lamp("in2", 3.78, 4.0, 0.13, "lampBlue")

    -- Bonus multiplier and shoot-again lamps down the middle.
    lamp("x2", -0.9, 6.2, 0.2, "lampAmber"); lamp("x3", -0.3, 6.5, 0.2, "lampAmber")
    lamp("x4",  0.3, 6.5, 0.2, "lampAmber"); lamp("x5",  0.9, 6.2, 0.2, "lampAmber")
    lamp("again", 0.0, 2.9, 0.26, "lampRed")
    lamp("save", 0.0, 4.3, 0.18, "lampGreen")

    if centerPost then
        addCircle(0.0, 0.9, 0.18, 0.5, "post")
        cyl(0.0, 0.9, WALL_H * 0.5, 0.18, WALL_H * 0.5, "rubber", "center post")
    end

    -- Flippers: a tapered bat, drawn as a box between two round ends.
    for _, sd in ipairs({-1, 1}) do
        local f = { px = 2.2 * sd, py = 2.4, side = -sd,  -- side +1: points right
                    a = FLIP_REST, w = 0.0, up = false }
        f.body  = box(f.px, f.py, 0.2, FLIP_LEN * 0.5, 0.18, 0.17, 0, "flipper", "flipper")
        f.rub   = box(f.px, f.py, 0.2, FLIP_LEN * 0.5, 0.12, 0.2, 0, "flipRub", "flipper rubber")
        f.base  = cyl(f.px, f.py, 0.2, FLIP_R, 0.19, "flipper", "flipper pivot")
        f.tip   = cyl(f.px, f.py, 0.2, FLIP_R - 0.05, 0.19, "flipper", "flipper tip")
        flippers[#flippers + 1] = f
    end

    -- Plunger: rod and knob, drawn where the simulated face is.
    plunger.ids.rod  = box(5.5, 0.0, 0.25, 0.08, 0.08, 0.5, 0, "metal", "plunger rod")
    plunger.ids.head = box(5.5, 0.0, 0.25, 0.36, 0.14, 0.08, 0, "rubber", "plunger")
    plunger.ids.knob = box(5.5, -1.9, 0.35, 0.2, 0.2, 0.2, 0, "flipRub", "plunger knob")

    -- The ball.
    ball.id = spawnPart(game.SPHERE, 5.5, 1.2, BALL_R, BALL_R, BALL_R, BALL_R, 0,
                        "ball", "ball")
end

-- --- Game flow ------------------------------------------------------------------------

local function flipperTip(f)
    return f.px + f.side * math.cos(f.a) * FLIP_LEN, f.py + math.sin(f.a) * FLIP_LEN
end

local function serveBall()
    ball.x, ball.y, ball.vx, ball.vy = 5.5, 1.3, 0.0, 0.0
    ball.live, ball.held, ball.still = true, false, 0.0
    ball.inLane, ball.launched = true, false
    tilt, tilted = 0.0, false
    bonus = 0
    saveArmed = true            -- the save clock starts when the ball leaves the lane
    saveUntil = -1.0
    skillLane = math.random(1, 3)
    skillLive = true
end

local function resetTargets()
    for _, s in ipairs(segs) do
        if s.kind == "drop" then
            s.active = true
            dropDown[s.index] = false
        end
    end
    dropResetAt = -1.0
end

local function startGame()
    score, ballNo, extraBalls, mult = 0, 1, 0, 1
    extraLit, extraAwarded = false, false
    laneLit = { false, false, false }
    resetTargets()
    mode = "play"
    serveBall()
    say("GOOD LUCK!", 2.0)
end

local function endOfBall()
    ball.live = false
    if saveUntil > now and not tilted then
        say("BALL SAVED", 2.0)
        cue("special")
        serveBall()
        saveArmed = false       -- one save per ball
        return
    end
    cue("drain")
    bonusShown = tilted and 0 or bonus * mult
    score = score + bonusShown
    bonusEnd = now + 2.2
    mode = "bonus"
end

local function afterBonus()
    if extraBalls > 0 then
        extraBalls = extraBalls - 1
        say("SHOOT AGAIN", 2.5)
        mode = "play"
        serveBall()
        return
    end
    ballNo = ballNo + 1
    mult = 1
    if ballNo > math.max(1, math.floor(balls)) then
        mode = "over"
        if score > highScore then
            highScore = score
            say("NEW HIGH SCORE!", 5.0)
        else
            say("GAME OVER", 5.0)
        end
        return
    end
    mode = "play"
    serveBall()
end

-- --- Scoring events ----------------------------------------------------------------------

local function laneCompleted()
    laneLit = { false, false, false }
    if mult < 5 then
        mult = mult + 1
        say(string.format("BONUS x%d", mult), 1.8)
    else
        addScore(5000)
        say("LANES 5.000", 1.8)
    end
    cue("special")
end

local function onSensor(s)
    if s.kind == "lane" then
        if skillLive and s.index == skillLane then
            addScore(10000)
            say("SKILL SHOT 10.000", 2.5)
            cue("special")
        end
        skillLive = false
        addScore(laneLit[s.index] and 100 or 500)
        bonus = bonus + 200
        laneLit[s.index] = true
        if laneLit[1] and laneLit[2] and laneLit[3] then laneCompleted() end
    elseif s.kind == "inlane" then
        addScore(250); bonus = bonus + 100
    elseif s.kind == "outlane" then
        addScore(1000)
    elseif s.kind == "saucer" then
        if not ball.held and ball.live then
            ball.held = true
            ball.x, ball.y, ball.vx, ball.vy = s.x, s.y, 0.0, 0.0
            saucerUntil = now + 1.3
            addScore(3000); bonus = bonus + 500
            if extraLit then
                extraLit = false
                extraBalls = extraBalls + 1
                say("EXTRA BALL!", 3.0)
                cue("special")
            else
                cue("saucer")
            end
        end
    end
end

local function onHit(kind, obj, speed)
    if obj.cool > now then return end
    obj.cool = now + 0.08
    if kind == "bumper" then
        addScore(100); bonus = bonus + 20
        obj.flashUntil = now + 0.12
        cue("bumper")
    elseif kind == "sling" then
        addScore(10)
        obj.flashUntil = now + 0.12
        cue("sling")
    elseif kind == "drop" then
        obj.active = false
        dropDown[obj.index] = true
        addScore(750); bonus = bonus + 300
        cue("target")
        if dropDown[1] and dropDown[2] and dropDown[3] then
            addScore(5000)
            extraLit = true
            say("EXTRA BALL IS LIT", 2.5)
            dropResetAt = now + 1.5
        end
    elseif kind == "post" and speed > 6.0 then
        addScore(10)
    end
end

-- --- The simulation ------------------------------------------------------------------------

-- Push the ball out of a round obstacle (surface point q, radius rad) and bounce
-- it off, relative to the obstacle's own surface velocity (svx, svy). Returns
-- the approach speed, or nil if they do not touch.
local function contact(qx, qy, rad, e, svx, svy)
    local dx, dy = ball.x - qx, ball.y - qy
    local R = rad + BALL_R
    local d2 = dx * dx + dy * dy
    if d2 >= R * R then return nil end
    local d = math.sqrt(d2)
    local nx, ny = 0.0, 1.0
    if d > 1e-6 then nx, ny = dx / d, dy / d end
    ball.x, ball.y = ball.x + nx * (R - d), ball.y + ny * (R - d)
    local rvx, rvy = ball.vx - svx, ball.vy - svy
    local vn = rvx * nx + rvy * ny
    if vn >= 0.0 then return 0.0, nx, ny end
    -- A ball merely rolling along a surface should not keep hopping off it.
    local bounce = (-vn > 1.2) and e or 0.0
    local tx, ty = -ny, nx
    local vt = rvx * tx + rvy * ty
    rvx = rvx - (1.0 + bounce) * vn * nx - 0.02 * vt * tx
    rvy = rvy - (1.0 + bounce) * vn * ny - 0.02 * vt * ty
    ball.vx, ball.vy = rvx + svx, rvy + svy
    return -vn, nx, ny
end

local function flippersEnabled()
    return mode == "play" and not tilted
end

local function substep(h, leftHeld, rightHeld)
    -- Flippers swing at a fixed rate; w is the bat's angular velocity in the
    -- plane (counter-clockwise positive), which is what the ball feels.
    for _, f in ipairs(flippers) do
        local held = rightHeld
        if f.side > 0 then held = leftHeld end
        held = held and flippersEnabled()
        local a0 = f.a
        if held then
            f.a = math.min(FLIP_UP, f.a + flipperSpeed * h)
        else
            f.a = math.max(FLIP_REST, f.a - FLIP_RETURN * h)
        end
        f.w = f.side * (f.a - a0) / h
    end

    -- The plunger face: pulled back by hand, or springing home after a release.
    local py0 = plunger.y
    if plunger.fire > 0.0 then
        plunger.y = math.min(PLUNGER_REST, plunger.y + plunger.fire * h)
        if plunger.y >= PLUNGER_REST then plunger.fire = 0.0 end
    else
        plunger.y = PLUNGER_REST - plunger.pull * PLUNGER_TRAVEL
    end
    plunger.vy = (plunger.y - py0) / h

    if not ball.live or ball.held then return end

    ball.vy = ball.vy - gravity * h
    local damp = 1.0 - 0.04 * h
    ball.vx, ball.vy = ball.vx * damp, ball.vy * damp
    ball.x, ball.y = ball.x + ball.vx * h, ball.y + ball.vy * h

    -- Walls, targets, slingshots, the gate.
    local bx, by = ball.x, ball.y
    for _, s in ipairs(segs) do
        if s.active and bx > s.x0 and bx < s.x1 and by > s.y0 and by < s.y1 then
            local px, py = ball.x - s.ax, ball.y - s.ay
            if not s.oneway or px * s.nx + py * s.ny > 0.0 then
                local t = px * s.dx + py * s.dy
                if t < 0.0 then t = 0.0 elseif t > s.len then t = s.len end
                local sp, nx, ny = contact(s.ax + s.dx * t, s.ay + s.dy * t, s.r, s.e, 0, 0)
                if sp then
                    if s.kind == "sling" and sp > 2.0 and not tilted
                       and nx * s.kx + ny * s.ky > 0.3 then
                        ball.vx, ball.vy = ball.vx + s.kx * 11.0, ball.vy + s.ky * 11.0
                        onHit("sling", s, sp)
                    elseif s.kind == "drop" and sp > 0.5 then
                        onHit("drop", s, sp)
                    end
                end
            end
        end
    end

    -- Posts and bumpers.
    for _, c in ipairs(circles) do
        local sp, nx, ny = contact(c.x, c.y, c.r, c.e, 0, 0)
        if sp then
            if c.kind == "bumper" and not tilted then
                -- A pop bumper fires the ball away whatever it came in with.
                local out = ball.vx * nx + ball.vy * ny
                if out < 13.0 then
                    ball.vx, ball.vy = ball.vx + nx * (13.0 - out), ball.vy + ny * (13.0 - out)
                end
                onHit("bumper", c, sp)
            else
                onHit(c.kind, c, sp)
            end
        end
    end

    -- Flippers: a capsule from the pivot to the tip, moving.
    for _, f in ipairs(flippers) do
        local tx, ty = flipperTip(f)
        local dx, dy = tx - f.px, ty - f.py
        local t = ((ball.x - f.px) * dx + (ball.y - f.py) * dy) / (FLIP_LEN * FLIP_LEN)
        if t < 0.0 then t = 0.0 elseif t > 1.0 then t = 1.0 end
        local qx, qy = f.px + dx * t, f.py + dy * t
        -- Surface velocity of the bat at that point: w x r.
        local svx, svy = -f.w * (qy - f.py), f.w * (qx - f.px)
        contact(qx, qy, FLIP_R - 0.05 * t, 0.35, svx, svy)
    end

    -- Plunger face (only reachable in the lane).
    if ball.x > LANE_L then
        local sp = contact(math.max(LANE_L + 0.12, math.min(LANE_R - 0.12, ball.x)),
                           plunger.y, 0.05, 0.1, 0, plunger.vy)
        if sp and plunger.vy > 5.0 then cue("plunger") end
    end

    local v2 = ball.vx * ball.vx + ball.vy * ball.vy
    if v2 > MAX_SPEED * MAX_SPEED then
        local k = MAX_SPEED / math.sqrt(v2)
        ball.vx, ball.vy = ball.vx * k, ball.vy * k
    end

    -- Rollovers: each fires once per pass.
    for _, s in ipairs(sensors) do
        local dx, dy = ball.x - s.x, ball.y - s.y
        local inside = dx * dx + dy * dy < s.r * s.r
        if inside and not s.inside then s.hitAt = now; onSensor(s) end
        s.inside = inside
    end
end

-- --- Input -------------------------------------------------------------------------------

local KEYS_LEFT  = { game.KEY_LSHIFT, game.KEY_LEFT,  game.KEY_A }
local KEYS_RIGHT = { game.KEY_RSHIFT, game.KEY_RIGHT, game.KEY_D }
local KEYS_PULL  = { game.KEY_SPACE,  game.KEY_DOWN,  game.KEY_S }
local KEYS_NUDGE = { game.KEY_UP,     game.KEY_W }

local function anyDown(keys)
    for _, k in ipairs(keys) do if game.keyDown(k) then return true end end
    return false
end

-- Edges are made here from keyDown rather than asked of keyPressed, which only
-- sees a press if it was also asked on the frame before.
local function pressed(name, down)
    local was = keysPrev[name]
    keysPrev[name] = down
    return down and not was
end

-- --- Drawing -----------------------------------------------------------------------------

local function drawFlipper(f)
    local ca, sa = f.side * math.cos(f.a), math.sin(f.a)
    local yaw = yawOf(ca, sa)
    local mx, my = f.px + ca * FLIP_LEN * 0.5, f.py + sa * FLIP_LEN * 0.5
    local tx, ty = flipperTip(f)
    local x, y, z = W(mx, my, 0.2)
    game.setPos(f.body, x, y, z);          game.setRot(f.body, 0, yaw, 0)
    game.setPos(f.rub, x, y - 0.02, z);    game.setRot(f.rub, 0, yaw, 0)
    game.setPos(f.tip, W(tx, ty, 0.2))
end

local function drawAll(t)
    if ball.live then
        game.setPos(ball.id, W(ball.x, ball.y, BALL_R))
    else
        game.setPos(ball.id, W(0.5, 10.0, -0.9))    -- tucked away inside the cabinet
    end
    for _, f in ipairs(flippers) do drawFlipper(f) end

    local py = plunger.y
    game.setPos(plunger.ids.head, W(5.5, py - 0.08, 0.25))
    game.setPos(plunger.ids.rod,  W(5.5, py - 0.6, 0.25))
    game.setPos(plunger.ids.knob, W(5.5, py - 2.5, 0.35))

    for _, s in ipairs(segs) do
        if s.kind == "drop" and s.vis then
            local sink = s.active and 0.35 or -0.5
            game.setPos(s.vis, W(s.ax, (s.ay + s.by) * 0.5, sink))
        end
    end
    for _, c in ipairs(circles) do
        if c.cap then
            local lit = (c.flashUntil or 0) > now
            if lit ~= c.lit and game.setMaterial(c.cap, lit and mats.bumperOn or mats.bumperOff) then
                c.lit = lit
            end
        end
    end

    -- Lamps.
    local blink = (math.floor(t * 4.0) % 2) == 0
    for i = 1, 3 do
        local on = laneLit[i]
        if skillLive and ball.inLane and i == skillLane then on = blink end
        setLamp("lane" .. i, on)
        setLamp("drop" .. i, not dropDown[i])
    end
    for m = 2, 5 do setLamp("x" .. m, mult >= m) end
    setLamp("extra", extraLit and blink)
    setLamp("again", extraBalls > 0)
    setLamp("save", saveUntil > now and (saveUntil - now > 3.0 or blink))
    for _, s in ipairs(segs) do
        if s.kind == "sling" then
            setLamp("sling" .. (s.ax < 0 and -1 or 1), (s.flashUntil or 0) > now)
        end
    end
    for _, s in ipairs(sensors) do
        local recent = (s.hitAt or -9.0) > now - 1.0
        if s.kind == "inlane" then setLamp("in" .. s.index, recent) end
        if s.kind == "outlane" then setLamp("out" .. s.index, recent) end
    end
    if mode ~= "play" then
        -- Attract mode: the lamps chase each other.
        local k = math.floor(t * 6.0) % 4
        for i = 1, 3 do setLamp("lane" .. i, (i - 1) == k) end
        for m = 2, 5 do setLamp("x" .. m, (m - 2) == k) end
    end
    syncLamps()
end

local backglassT = 0.0
local function driveBackglass(t)
    if t - backglassT < 0.1 then return end
    backglassT = t
    local r = 0.5 + 0.5 * math.sin(t * 0.7)
    local b = 0.5 + 0.5 * math.sin(t * 0.7 + 2.1)
    local strength = (mode == "play") and 1.6 or (1.6 + 1.2 * math.abs(math.sin(t * 2.0)))
    game.setMaterialProps(mats.backglass, { emission = {0.3 + 0.7 * r, 0.1, 0.3 + 0.7 * b},
                                            emissionStrength = strength })
end

local function driveCamera(dt)
    shake = math.max(0.0, shake - dt * 3.0)
    local sx = shake > 0 and (math.random() - 0.5) * shake * 0.4 or 0.0
    local cx, cy, cz = W(0.5 + sx, -camBack, camHeight)
    local lx, ly, lz = W(0.5, 8.5, 0.0)
    game.setCameraPos(cx, cy, cz)
    game.setCameraDir(lx - cx, ly - cy, lz - cz)
    game.setCameraFov(camFov)
end

-- --- HUD ----------------------------------------------------------------------------------

local function hud()
    local lines = {}
    if mode == "attract" or mode == "over" then
        lines[1] = string.format("FLIPPER      HIGH SCORE %s", fmt(highScore))
        lines[2] = (mode == "over") and string.format("Last game: %s", fmt(score))
                   or "Press ENTER to start"
        if mode == "over" then lines[2] = lines[2] .. "      ENTER: new game" end
    else
        lines[1] = string.format("SCORE %s      BALL %d/%d      BONUS %s x%d",
                                 fmt(score), math.min(ballNo, math.floor(balls)),
                                 math.floor(balls), fmt(bonus), mult)
        if mode == "bonus" then
            lines[2] = tilted and "TILT -- no bonus"
                       or string.format("BONUS %s", fmt(bonusShown))
        elseif tilted then
            lines[2] = "TILT"
        elseif now < messageUntil then
            lines[2] = message
        elseif ball.inLane then
            lines[2] = "Hold SPACE to pull the plunger, let go to shoot"
        else
            lines[2] = (extraBalls > 0) and "SHOOT AGAIN lit" or ""
        end
    end
    if now < messageUntil and (mode == "attract" or mode == "over") then
        lines[2] = message .. "      " .. lines[2]
    end
    lines[3] = "Shift / arrows / mouse: flippers    Space: plunger    Up: nudge"
    game.setHud(table.concat(lines, "\n"))
end

-- --- Lifecycle ---------------------------------------------------------------------------

function start(e)
    GROUND = e.y
    BY = e.y + standHeight
    OX, OZ = e.x - 0.5, e.z + 10.5
    math.randomseed(math.floor(os.time()))

    for key, def in pairs(MAT_DEFS) do
        local t = { name = "Pinball " .. key }
        for k, v in pairs(def) do t[k] = v end
        mats[key] = game.createMaterial(t)
    end
    for _, f in pairs(CUE_FILES) do
        cues[f] = game.findAsset(f, "Sound") ~= nil
    end

    buildTable()
    game.setCamera(-1)    -- take the view back from any active Camera component
    mode = "attract"
    ball.live = false
    say("Press ENTER to start", 3.0)
end

function update(e, dt, t)
    now = t
    dt = math.min(dt, 0.05)

    local leftHeld  = anyDown(KEYS_LEFT)  or game.mouseDown(game.MOUSE_LEFT)
    local rightHeld = anyDown(KEYS_RIGHT) or game.mouseDown(game.MOUSE_RIGHT)
    local pullHeld  = anyDown(KEYS_PULL)
    local startHit  = pressed("start", game.keyDown(game.KEY_ENTER))
    local leftHit   = pressed("left", leftHeld)
    local rightHit  = pressed("right", rightHeld)
    local pullHit   = pressed("pull", pullHeld)
    local nudgeHit  = pressed("nudge", anyDown(KEYS_NUDGE))

    if mode == "attract" or mode == "over" then
        if startHit or pullHit then startGame() end
        pullHeld = false
    end

    if mode == "play" then
        -- Flipper sounds, and lane change: a press shifts the lit lanes round.
        if (leftHit or rightHit) and flippersEnabled() then
            cue("flipper")
            if not ball.inLane then
                if leftHit then
                    laneLit = { laneLit[2], laneLit[3], laneLit[1] }
                else
                    laneLit = { laneLit[3], laneLit[1], laneLit[2] }
                end
            end
        end

        -- Plunger: pulled while held (a full pull takes about a second), fired
        -- on release at a speed that grows with how far it went back.
        if pullHeld and plunger.fire == 0.0 then
            plunger.pull = math.min(1.0, plunger.pull + dt * 1.1)
        elseif plunger.pull > 0.0 then
            plunger.fire = 8.0 + 32.0 * plunger.pull
            plunger.pull = 0.0
        end

        -- Nudge: shoves the ball a little. Every nudge fills the tilt meter,
        -- which drains again with time; overfill it and the ball is lost.
        if nudgeHit and ball.live and not tilted then
            ball.vx = ball.vx + (math.random() - 0.5) * 3.0
            ball.vy = ball.vy + 2.5
            shake = 1.0
            tilt = tilt + 1.0
            if tilt > 3.2 then
                tilted = true
                say("TILT", 3.0)
                cue("drain")
            elseif tilt > 2.0 then
                say("DANGER", 1.2)
            end
        end
        tilt = math.max(0.0, tilt - dt * 0.6)

        -- The saucer lets go after a moment, up toward the bumpers.
        if ball.held and now >= saucerUntil then
            ball.held = false
            ball.vx, ball.vy = -8.5 + (math.random() - 0.5), 11.0
            ball.x, ball.y = ball.x - 0.2, ball.y + 0.35
            cue("plunger")
        end
    end

    -- Drop targets reset a moment after the bank was completed.
    if dropResetAt > 0.0 and now >= dropResetAt then resetTargets() end

    -- Run the simulation in fixed steps.
    acc = acc + dt
    local steps = 0
    while acc >= H and steps < 80 do
        acc = acc - H
        steps = steps + 1
        substep(H, leftHeld, rightHeld)
    end

    if mode == "play" and ball.live then
        -- Leaving the lane starts the ball-save clock.
        if ball.inLane and ball.x < LANE_L - BALL_R and ball.y > 12.0 then
            ball.inLane = false
            if saveArmed then saveUntil = now + ballSaveTime; saveArmed = false end
        end
        if not ball.inLane and ball.x > LANE_L and ball.y < 13.0 then
            ball.inLane = true    -- rolled back into the lane (weak launch)
        end
        -- Ball search: a ball lying still in the playfield gets a kick. Not
        -- down at the flippers, where a still ball is one being held on a
        -- raised flipper on purpose.
        local speed = math.sqrt(ball.vx * ball.vx + ball.vy * ball.vy)
        if not ball.inLane and not ball.held and speed < 0.4 and ball.y > 5.0 then
            ball.still = ball.still + dt
            if ball.still > 4.0 then
                ball.vx, ball.vy = (math.random() - 0.5) * 6.0, 9.0
                ball.still = 0.0
            end
        else
            ball.still = 0.0
        end
        if ball.y < DRAIN_V then endOfBall() end
    elseif mode == "bonus" and now >= bonusEnd then
        afterBonus()
    end

    drawAll(t)
    driveBackglass(t)
    driveCamera(dt)
    hud()
end
