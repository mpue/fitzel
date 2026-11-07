-- stalker/world.lua -- the places of a level converted by stalkerimport: its
-- anomalies, radiation and mine fields, its level changers, its sound sources,
-- the ambience and the music. Written into the project by the converter; a new
-- import overwrites it.

local W = {}

local D, P
local T = 0.0
local fxId = {}            -- zone name -> the entity with its particles (and a fire's light)
local blowId = {}          -- zone name -> the entity whose burst is a discharge
local loops = {}           -- zone loop key -> true while it plays
local armed = false        -- exits work once the player has stood outside all of them
local geigerT = 0.0
local ambientT, musicT = 8.0, 25.0

local function pick(t)
    if t and #t > 0 then return t[math.random(#t)] end
end
local function rand(a, b) return a + math.random() * (b - a) end

-- Is (x, y, z) inside shape `s`? A sphere scaled by `scale`, or a box.
local function inside(s, x, y, z, scale)
    local dx, dy, dz = x - s.x, y - s.y, z - s.z
    if s.box then
        for i = 1, 3 do
            local a = s.box[i]
            if math.abs(dx * a[1] + dy * a[2] + dz * a[3]) > a[4] then return false end
        end
        return true
    end
    local r = s.r * (scale or 1)
    return dx * dx + dy * dy + dz * dz < r * r
end

local function insideAny(shapes, x, y, z, scale)
    for _, s in ipairs(shapes) do
        if inside(s, x, y, z, scale) then return s end
    end
end

-- What one blowout of an anomaly does to the player, by its power.
local HIT = {electra = 0.35, gravi = 0.5, mincer = 0.6, burner = 0.3, fuzz = 0.15, acid = 0.2,
             emi = 0.25, teleport = 0.0}

function W.start(data, player)
    D, P = data, player
    for _, z in ipairs(D.zones) do
        z.cool = 0.0
        if z.fx then
            fxId[z.n] = game.find(z.fx)
            blowId[z.n] = game.find(z.fx .. "_blow")
        end
    end
    for _, s in ipairs(D.sounds) do s.next = rand(s.pause[1], s.pause[2]) end
end

local function blow(z, s)
    z.cool = (z.k == "fire") and 0.5 or 1.6
    if z.k == "fire" then
        P.hurt(0.04 * z.p)
        return
    end
    if z.k == "mines" then
        if D.fx.explode ~= "" then game.sound(D.fx.explode, 1.0, 1.0, "SFX") end
        P.hurt(0.65)
        z.cool = 2.5
        return
    end
    local snd = z.blow or z.hit
    if snd then game.sound(snd, 1.0, 1.0, s.x, s.y + 0.5, s.z, 4, 60, "SFX") end
    local id = blowId[z.n]
    if id then game.emit(id) end
    P.hurt((HIT[z.k] or 0.3) * z.p)
end

-- Into the level an exit leads to: where to stand there goes into the save,
-- which that level's player.lua reads when it starts.
function W.travel(e)
    game.saveData("stalker_travel", {to = e.to, gx = e.gx, gy = e.gy, gz = e.gz, yaw = e.yaw,
                                     health = P.health, rad = P.rad})
    game.loadScene(e.to)
end

function W.update(dt, t)
    T = t
    local px, py, pz = P.feet()
    local cy = py + 0.9

    -- Anomalies, radiation, mines.
    local near = {}
    local radiation = 0.0
    for i, z in ipairs(D.zones) do
        z.cool = z.cool - dt
        local s0 = z.s[1]
        local dx, dz = px - s0.x, pz - s0.z
        local d2 = dx * dx + dz * dz
        if z.idle and d2 < 900 then near[#near + 1] = {d2 = d2, i = i, z = z} end
        local reach = s0.r + 6
        if d2 < reach * reach and not P.dead then
            if z.k == "radiation" then
                local s = insideAny(z.s, px, cy, pz, 1.0)
                if s then
                    radiation = radiation + z.p
                    P.irradiate(z.p * 0.9 * dt)
                elseif not s0.box then
                    -- The counter hears it a few metres before it starts.
                    local d = math.sqrt(d2)
                    radiation = radiation + z.p * math.max(0, 1 - (d - s0.r) / 6) * 0.3
                end
            else
                local s = insideAny(z.s, px, cy, pz, z.eff)
                if s and z.cool <= 0 then blow(z, s) end
            end
        end
        -- A fire's light lives.
        if z.k == "fire" and d2 < 1600 then
            local id = fxId[z.n]
            if id then game.setLight(id, {intensity = 5.0 + math.random() * 2.5}) end
        end
    end
    -- The idle sounds of the nearest few.
    table.sort(near, function(a, b) return a.d2 < b.d2 end)
    local want = {}
    for k = 1, math.min(6, #near) do
        local z, s = near[k].z, near[k].z.s[1]
        local key = "zone" .. near[k].i
        want[key] = true
        game.loop(key, z.idle, z.k == "fire" and 0.7 or 0.9, 1.0, s.x, s.y + 0.4, s.z, 2, 28, "Ambient")
    end
    for key in pairs(loops) do
        if not want[key] then game.stopLoop(key) end
    end
    loops = want

    -- The Geiger counter.
    if radiation > 0.001 and #D.fx.geiger > 0 then
        geigerT = geigerT - dt
        if geigerT <= 0 then
            game.sound(pick(D.fx.geiger), math.min(1.0, 0.4 + radiation * 6), 1.0, "SFX")
            geigerT = rand(0.05, 0.25) / math.min(4.0, 0.3 + radiation * 30)
        end
    end

    -- Level changers.
    local inExit = nil
    for _, e in ipairs(D.exits) do
        if insideAny(e.s, px, cy, pz, 1.0) then inExit = e break end
    end
    if not inExit then armed = true
    elseif armed and not P.dead then
        P.say("E - weiter nach " .. inExit.label, 0.1)
        if game.keyPressed(game.KEY_E) then W.travel(inExit) end
    end
    W.exitHere = inExit

    -- The level's own sound sources that play now and then.
    for _, s in ipairs(D.sounds) do
        s.next = s.next - dt
        if s.next <= 0 then
            game.sound(s.file, s.vol, 1.0, s.x, s.y, s.z, 3, 35, "Ambient")
            s.next = rand(s.pause[1], s.pause[2]) + 2
        end
    end

    -- The ambience: a sound now and then from somewhere around.
    ambientT = ambientT - dt
    if ambientT <= 0 and #D.ambient.sounds > 0 then
        local a = math.random() * 2 * math.pi
        local d = rand(D.ambient.dist[1], D.ambient.dist[2])
        game.sound(pick(D.ambient.sounds), 0.75, 1.0, px + math.cos(a) * d, py + 3, pz + math.sin(a) * d,
                   12, 160, "Ambient")
        ambientT = rand(D.ambient.period[1], D.ambient.period[2])
    end

    -- The music of the hour, with a pause after each piece.
    musicT = musicT - dt
    if musicT <= 0 and #D.music > 0 then
        local hour = game.timeOfDay()
        local fit = {}
        for _, m in ipairs(D.music) do
            if hour >= m.from and hour < m.to then fit[#fit + 1] = m end
        end
        local m = pick(fit)
        if m then
            game.sound(m.l, m.vol * 0.6, 1.0, "Music")
            if m.r then game.sound(m.r, m.vol * 0.6, 1.0, "Music") end
            musicT = (m.len > 0 and m.len or 120) + rand(m.pause[1], m.pause[2])
        else
            musicT = 60
        end
    end
end

return W
