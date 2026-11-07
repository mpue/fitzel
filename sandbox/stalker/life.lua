-- stalker/life.lua -- the people and creatures of a level converted by
-- stalkerimport, and the player's pistol. Every figure is an object of the
-- scene with a model and the xr_human / xr_mutant animation graph; this module
-- moves them (game.moveCharacter) and sets the graph's `mode`.
--
-- People do the job the level's data gives them: walk a patrol path, sit at
-- a camp fire (one of them playing the guitar or the harmonica), stand guard,
-- sleep, or stay where they are. Close to the player they stop, turn to him
-- and say something. Mutants roam around their home, hunt the player when he
-- comes near and bite. Crows circle. All of them can be shot.
--
-- Written into the project by the converter; a new import overwrites it.

local L = {}

local D, P
local T = 0.0
local figs = {}
local camps = {}            -- path name -> {x, y, z, seats, players}
local ACTIVE = 80           -- metres: farther away a figure is hidden and still
local ACTIVE_CROW = 160
local WALK, RUN = 1.5, 4.2  -- people, m/s

local HM = {idle = 0, walk = 1, run = 2, sit = 3, kneel = 4, guitar = 5, harmonica = 6, sleep = 7,
            hello = 8, dead = 9}
local MM = {idle = 0, walk = 1, run = 2, attack = 3, eat = 4, rest = 5, threaten = 6, dead = 9}

-- Mutants: hits a pistol needs, bite, how near they notice, how near they bite.
local KIND = {
    dog = {tough = 2, bite = 0.05, aggro = 28, reach = 1.4},
    boar = {tough = 5, bite = 0.1, aggro = 22, reach = 1.7},
    flesh = {tough = 3, bite = 0.06, aggro = 14, reach = 1.6},
    pseudodog = {tough = 5, bite = 0.09, aggro = 30, reach = 1.6},
    snork = {tough = 4, bite = 0.1, aggro = 25, reach = 1.4},
    bloodsucker = {tough = 8, bite = 0.18, aggro = 25, reach = 1.7},
    gigant = {tough = 16, bite = 0.3, aggro = 30, reach = 2.6},
    controller = {tough = 7, bite = 0.1, aggro = 20, reach = 1.5},
    tushkano = {tough = 1, bite = 0.03, aggro = 12, reach = 0.9},
    zombie = {tough = 4, bite = 0.1, aggro = 15, reach = 1.5},
    rat = {tough = 1, bite = 0.02, aggro = 10, reach = 0.8},
    cat = {tough = 2, bite = 0.06, aggro = 18, reach = 1.1},
    burer = {tough = 8, bite = 0.15, aggro = 20, reach = 1.6},
    chimera = {tough = 12, bite = 0.25, aggro = 30, reach = 2.2},
    fracture = {tough = 6, bite = 0.15, aggro = 18, reach = 1.7},
    mutant = {tough = 4, bite = 0.1, aggro = 20, reach = 1.5},
}

-- The pistol.
local MAG = 8
local mag, shotCool, reloadT = MAG, 0.0, 0.0

local function pick(t)
    if t and #t > 0 then return t[math.random(#t)] end
end
local function rand(a, b) return a + math.random() * (b - a) end

local function setMode(f, m)
    if f.mode ~= m then
        f.mode = m
        game.animNumber(f.id, "mode", m)
    end
end

-- Turn to face (dx, dz). The figures look down their model's -Z.
local function face(f, dx, dz)
    if dx * dx + dz * dz < 1e-6 then return end
    f.yaw = math.deg(math.atan(-dx, -dz))
    game.setRot(f.id, 0, f.yaw, 0)
end

local function place(f)
    game.setPos(f.id, f.x, f.y + f.cy, f.z)
end

-- One step towards (tx, tz). True once there -- or once stuck for a while
-- (a fence the straight line runs into), so the caller moves on.
local function walkTo(f, tx, tz, speed, dt, arrive)
    local dx, dz = tx - f.x, tz - f.z
    local d = math.sqrt(dx * dx + dz * dz)
    if d < (arrive or 0.45) then f.stuck = 0 return true end
    local x, y, z = game.moveCharacter(f.id, dx / d * speed, dz / d * speed, dt, f.radius, f.height)
    if x then
        local moved = math.abs(x - f.x) + math.abs(z - f.z)
        f.stuck = (moved < speed * dt * 0.25) and (f.stuck or 0) + dt or 0
        f.x, f.y, f.z = x, y, z
        place(f)
    end
    face(f, dx, dz)
    if f.stuck > 3.0 then f.stuck = 0 return true end
    return false
end

local function say(f, list, near, far)
    local s = pick(list)
    if s then game.sound(s, 0.9, 1.0, f.x, f.y + 1.5, f.z, near or 4, far or 35, "SFX") end
end

-- --- people ---------------------------------------------------------------------

local function nextPoint(f, path)
    local outs = {}
    for _, l in ipairs(path.links or {}) do
        if l[1] == f.pi then outs[#outs + 1] = l[2] end
    end
    if #outs > 0 then
        f.pi = outs[math.random(#outs)]
        return
    end
    if #path == 1 then return end
    if f.pi >= #path then f.dir = -1 elseif f.pi <= 1 then f.dir = 1 end
    f.pi = f.pi + (f.dir or 1)
end

local POINT_MODE = {sit = HM.sit, sit_ass = HM.sit, sit_knee = HM.kneel, sleep = HM.sleep, hide = HM.kneel,
                    sneak = HM.kneel}

local function walker(f, dt)
    local path = D.paths[f.d.path]
    if not path then return end
    if f.wait and f.wait > 0 then
        f.wait = f.wait - dt
        return
    end
    local pt = path[f.pi]
    setMode(f, HM.walk)
    if walkTo(f, pt.x, pt.z, WALK, dt) then
        local wait = pt.t or 0
        if pt.a or #path == 1 then wait = math.max(wait, rand(6, 14)) end
        if not path.links or #path.links == 0 then
            if f.pi == #path or f.pi == 1 then wait = math.max(wait, rand(4, 9)) end
        end
        nextPoint(f, path)
        if wait > 0 then
            f.wait = wait
            setMode(f, POINT_MODE[pt.a or ""] or HM.idle)
        end
    end
end

local function goAndDo(f, dt, x, z, mode, lookX, lookZ)
    if not f.there then
        setMode(f, HM.walk)
        if walkTo(f, x, z, WALK, dt, 0.35) then
            f.there = true
            if lookX then face(f, lookX - f.x, lookZ - f.z) end
            setMode(f, mode)
        end
    end
end

local function camp(f, dt)
    local c = camps[f.d.path]
    if not c then return end
    if not f.seat then
        c.n = c.n + 1
        local a = c.phase + (c.n - 1) * (2 * math.pi / math.max(4, c.count))
        local r = rand(1.9, 2.4)
        f.seat = {c.x + math.cos(a) * r, c.z + math.sin(a) * r}
        f.sitMode = math.random() < 0.7 and HM.sit or HM.kneel
    end
    goAndDo(f, dt, f.seat[1], f.seat[2], f.sitMode, c.x, c.z)
    if f.there and not c.player and f.mode ~= HM.walk then c.player = f end
end

-- The camp's musician plays a song, rests, plays the next.
local function campMusic(dt)
    for name, c in pairs(camps) do
        local f = c.player
        if f and not f.dead and f.there and f.active then
            c.songT = (c.songT or rand(4, 12)) - dt
            if c.songT <= 0 then
                if c.playing then
                    c.playing = false
                    setMode(f, f.sitMode)
                    c.songT = rand(25, 60)
                else
                    local songs = D.songs[f.d.com == "trader" and "stalker" or (f.d.com or "stalker")]
                    songs = songs or D.songs.stalker
                    local harmonica = songs and #songs.harmonica > 0 and math.random() < 0.3
                    local s = songs and pick(harmonica and songs.harmonica or songs.guitar)
                    if s then
                        setMode(f, harmonica and HM.harmonica or HM.guitar)
                        game.sound(s.f, 0.9, 1.0, f.x, f.y + 1.0, f.z, 5, 45, "SFX")
                        c.playing = true
                        c.songT = (s.len > 0 and s.len or 40)
                    else
                        c.songT = 9999
                    end
                end
            end
        end
    end
end

local function person(f, dt, px, py, pz)
    -- The player close by: stop and turn to him; one standing says something.
    local dx, dz = px - f.x, pz - f.z
    local d2 = dx * dx + dz * dz
    local seated = f.mode == HM.sit or f.mode == HM.kneel or f.mode == HM.sleep or f.mode == HM.guitar
                   or f.mode == HM.harmonica
    if d2 < 6.5 and math.abs(py - f.y) < 2.5 and not P.dead then
        if not seated then
            if f.mode == HM.walk or f.mode == HM.run then setMode(f, HM.idle) end
            face(f, dx, dz)
        end
        if T - (f.greetT or -99) > 45 then
            f.greetT = T
            if not seated and f.d.fam ~= "trader" then
                setMode(f, HM.hello)
                f.helloT = 2.8
            elseif f.d.fam == "trader" then
                setMode(f, HM.hello)
                f.helloT = 3.5
            end
            say(f, f.d.snd and f.d.snd.idle)
        end
    end
    if f.helloT then
        f.helloT = f.helloT - dt
        if f.helloT > 0 then return end
        f.helloT = nil
        setMode(f, HM.idle)
    end
    if d2 < 6.5 and not seated then return end

    local job = f.d.job
    if job == "walker" then
        walker(f, dt)
    elseif job == "kamp" then
        camp(f, dt)
    elseif job == "sleep" or job == "guard" then
        local path = D.paths[f.d.path or ""]
        local pt = path and path[1]
        local look = D.paths[f.d.look or ""]
        local lp = look and look[1]
        if pt then
            goAndDo(f, dt, pt.x, pt.z, job == "sleep" and HM.sleep or (POINT_MODE[pt.a or ""] or HM.idle),
                    lp and lp.x, lp and lp.z)
        elseif lp and not f.there then
            f.there = true
            face(f, lp.x - f.x, lp.z - f.z)
        end
    elseif job == "wander" then
        if f.wait and f.wait > 0 then
            f.wait = f.wait - dt
        else
            if not f.tx then
                local a, r = math.random() * 2 * math.pi, rand(1, 6)
                f.tx, f.tz = f.d.hx + math.cos(a) * r, f.d.hz + math.sin(a) * r
            end
            setMode(f, HM.walk)
            if walkTo(f, f.tx, f.tz, WALK * 0.8, dt) then
                f.tx = nil
                f.wait = rand(6, 16)
                setMode(f, HM.idle)
            end
        end
    end
end

-- --- mutants and crows ----------------------------------------------------------

local function mutant(f, dt, px, py, pz)
    local k = f.k
    local dx, dz = px - f.x, pz - f.z
    local d = math.sqrt(dx * dx + dz * dz)
    if f.state ~= "hunt" and not P.dead and math.abs(py - f.y) < 5 and (d < k.aggro or f.angry) then
        f.state = "hunt"
        say(f, f.d.snd and f.d.snd.attack, 6, 70)
    end
    if f.state == "hunt" then
        if P.dead or d > k.aggro * 2.5 + 15 then
            f.state, f.angry, f.tx = "home", false, nil
        elseif d < k.reach + 0.4 then
            face(f, dx, dz)
            setMode(f, MM.attack)
            f.biteT = (f.biteT or 0.4) - dt
            if f.biteT <= 0 then
                f.biteT = rand(1.2, 1.9)
                say(f, f.d.snd and f.d.snd.hit, 4, 40)
                P.hurt(k.bite)
            end
        else
            setMode(f, MM.run)
            walkTo(f, px, pz, f.d.run or 5, dt, k.reach)
        end
        return
    end
    -- Roaming: walk somewhere near home, then idle, eat or lie down a while.
    if f.wait and f.wait > 0 then
        f.wait = f.wait - dt
        if T > (f.growlT or 0) then
            f.growlT = T + rand(15, 45)
            say(f, f.d.snd and f.d.snd.idle, 4, 45)
        end
        return
    end
    if not f.tx then
        local a, r = math.random() * 2 * math.pi, rand(2, 14)
        f.tx, f.tz = f.d.hx + math.cos(a) * r, f.d.hz + math.sin(a) * r
    end
    setMode(f, MM.walk)
    if walkTo(f, f.tx, f.tz, f.d.walk or 1.4, dt) then
        f.tx = nil
        f.wait = rand(5, 18)
        local r = math.random()
        setMode(f, r < 0.5 and MM.idle or r < 0.75 and MM.eat or MM.rest)
    end
end

local function crow(f, dt)
    f.ang = f.ang + dt * f.speed / f.r
    local x = f.cx + math.cos(f.ang) * f.r
    local z = f.cz + math.sin(f.ang) * f.r
    local y = f.base + math.sin(f.ang * 0.7) * 3
    face(f, x - f.x, z - f.z)
    f.x, f.y, f.z = x, y, z
    place(f)
end

-- --- harm --------------------------------------------------------------------------

function L.damage(f, amount)
    if f.dead then return end
    f.hp = f.hp - amount
    if f.d.fam ~= "human" and f.d.fam ~= "trader" and f.d.fam ~= "crow" then
        f.angry = true
    end
    if f.hp > 0 then
        say(f, f.d.snd and f.d.snd.hurt, 4, 40)
        return
    end
    f.dead = true
    say(f, f.d.snd and f.d.snd.die, 4, 50)
    if f.d.fam == "crow" then
        game.setActive(f.id, false)
        return
    end
    setMode(f, f.human and HM.dead or MM.dead)
    game.removeCharacter(f.id)
    for _, c in pairs(camps) do
        if c.player == f then c.player = nil end
    end
end

local function shoot(dt)
    shotCool = math.max(0, shotCool - dt)
    if reloadT > 0 then
        reloadT = reloadT - dt
        if reloadT <= 0 then mag = MAG end
        return
    end
    if P.dead then return end
    if game.keyPressed(game.KEY_R) and mag < MAG then
        reloadT = 1.6
        if D.fx.reload ~= "" then game.sound(D.fx.reload, 0.9, 1.0, "SFX") end
        return
    end
    if not game.mousePressed(game.MOUSE_LEFT) or shotCool > 0 then return end
    if mag == 0 then
        if D.fx.empty ~= "" then game.sound(D.fx.empty, 0.9, 1.0, "SFX") end
        shotCool = 0.25
        return
    end
    mag = mag - 1
    shotCool = 0.28
    if D.fx.shot ~= "" then game.sound(D.fx.shot, 1.0, 0.97 + math.random() * 0.06, "SFX") end
    local ox, oy, oz = game.cameraPos()
    L.fire(ox, oy, oz, game.cameraDir())
end

-- A bullet from (ox, oy, oz) along the unit (dx, dy, dz): the first figure
-- whose skin it meets before a wall takes the hit. Returns that figure.
function L.fire(ox, oy, oz, dx, dy, dz)
    local _, _, _, _, _, _, _, wall = game.castRay(ox, oy, oz, dx, dy, dz, 150)
    local best, bestD = nil, (wall or 150) + 0.8
    for _, f in ipairs(figs) do
        if f.active and not f.dead then
            local cx, cy, cz = f.x - ox, f.y + f.cy - oy, f.z - oz
            local t = cx * dx + cy * dy + cz * dz
            if t > 0 and t < bestD then
                local qx, qy, qz = cx - dx * t, cy - dy * t, cz - dz * t
                local r = f.height * 0.6 + 0.5
                if qx * qx + qy * qy + qz * qz < r * r then
                    local hx, hy, hz = game.rayFigure(f.id, ox, oy, oz, dx, dy, dz, bestD)
                    if hx then
                        local hd = math.sqrt((hx - ox) ^ 2 + (hy - oy) ^ 2 + (hz - oz) ^ 2)
                        if hd < bestD then best, bestD = f, hd end
                    end
                end
            end
        end
    end
    if best then L.damage(best, 1) end
    return best
end

-- --- the module ------------------------------------------------------------------------

function L.start(data, player)
    D, P = data, player
    for _, d in ipairs(D.people) do
        local path = d.job == "kamp" and D.paths[d.path]
        if path then
            local c = camps[d.path]
            if not c then
                c = {x = path[1].x, y = path[1].y, z = path[1].z, n = 0, count = 0,
                     phase = math.random() * 2 * math.pi}
                camps[d.path] = c
            end
            c.count = c.count + 1
        end
    end
    for _, d in ipairs(D.people) do
        local id = game.find(d.n)
        if id then
            local f = {id = id, d = d, x = d.x, y = d.y, z = d.z, cy = d.cy, yaw = d.yaw, mode = -1,
                       height = math.max(0.5, d.h), stuck = 0, active = true, pi = 1, dir = 1}
            f.human = d.fam == "human" or d.fam == "trader"
            if f.human then
                f.radius = 0.32
                f.hp = 3
            elseif d.fam == "crow" then
                f.hp = 1
                f.cx, f.cz = d.x + rand(-4, 4), d.z + rand(-4, 4)
                f.r = rand(8, 22)
                f.speed = rand(5, 8)
                f.ang = math.random() * 2 * math.pi
                f.base = d.y + rand(12, 24)
            else
                f.k = KIND[d.fam] or KIND.mutant
                f.hp = f.k.tough
                f.radius = math.min(0.6, math.max(0.25, d.h * 0.3))
                f.state = "home"
            end
            -- A walker starts at the point of its path nearest to it.
            if d.job == "walker" and D.paths[d.path] then
                local best = 1e30
                for i, pt in ipairs(D.paths[d.path]) do
                    local q = (pt.x - d.x) ^ 2 + (pt.z - d.z) ^ 2
                    if q < best then best, f.pi = q, i end
                end
            end
            setMode(f, 0)
            figs[#figs + 1] = f
        end
    end
end

function L.update(dt, t)
    T = t
    local px, py, pz = P.feet()
    shoot(dt)
    for _, f in ipairs(figs) do
        local dx, dz = px - f.x, pz - f.z
        local reach = f.d.fam == "crow" and ACTIVE_CROW or ACTIVE
        local active = dx * dx + dz * dz < reach * reach
        if active ~= f.active then
            f.active = active
            game.setActive(f.id, active)
        end
        if active and not f.dead then
            if f.d.fam == "crow" then crow(f, dt)
            elseif f.human then person(f, dt, px, py, pz)
            else mutant(f, dt, px, py, pz) end
        end
    end
    campMusic(dt)
end

-- Every figure, for other scripts (shared.stalker.life.figures): its object id
-- `id`, data `d`, feet `x, y, z`, graph `mode`, `dead`, `active`.
L.figures = figs

function L.ammo()
    if reloadT > 0 then return "PM  ..." end
    return "PM  " .. mag .. " / " .. MAG
end

return L
