-- Shooting with the weapon the figure carries: aim over the shoulder, fire,
-- reload from the rounds in the bag.
--
-- Hang it on the figure as a THIRD Script component, after tp_controller.lua and
-- inventory.lua. It works with what the inventory has equipped (a Collectible
-- of category Weapon), takes its rounds from the bag's Ammo, and has the
-- controller stand and face the way the camera looks while aiming.
--
--   right mouse   aim: the figure raises the weapon (graph state `aim`), the
--                 camera comes in over the right shoulder, a crosshair shows
--   left mouse    fire, while aiming (graph Trigger `shoot`, state `shoot`)
--   R             reload; an empty magazine reloads by itself on the next shot
--
-- A shot goes from the eye through the middle of the picture (game.castRay):
-- dust and sparks where it lands (game.emit on the FX objects), a push for a
-- loose physics body; glass breaks (game.shatter) and lets the shot through.
-- The flash and the smoke come out of the muzzle, a point
-- of the weapon in its own frame (MUZZLE), and light up what is near for a
-- moment. Every number below shows in the Inspector.

AIM_BUTTON    = 1            -- mouse button: 0 left, 1 right, 2 middle
FIRE_BUTTON   = 0
RELOAD_KEY    = "R"
MAG_SIZE      = 7            -- rounds in a magazine
FIRE_INTERVAL = 0.22         -- seconds between two shots at the fastest
RELOAD_TIME   = 1.5          -- seconds a reload takes
RANGE         = 150          -- metres a shot carries
IMPULSE       = 4.0          -- what a hit gives a loose physics body (N s)
SHOOT_PARAM   = "shoot"      -- graph Trigger for the recoil
HAND_BONE     = "CC_Base_R_Hand"
-- The weapon in the hand while aiming (bone space, like the inventory's
-- carrying grip): muzzle ahead, slide up, the grip in the palm of the raised
-- hand. Worked out from the `shooting` clip of a CC rig.
AIM_GRIP_POS  = {x = -0.059, y = 0.120, z = 0.040}
AIM_GRIP_ROT  = {x = -9.0, y = -78.0, z = -57.5}
-- The muzzle, from the weapon's centre in its own frame (the pistol's barrel
-- end, BARRELOUT in its model).
MUZZLE        = {x = 0.051, y = -0.004, z = 0.110}
-- The camera over the shoulder: metres from the pivot, the pivot to the right
-- and up, and the narrower view.
AIM_DIST      = 1.6
AIM_SIDE      = 0.6
AIM_UP        = 0.12
AIM_FOV       = 44
AIM_TIME      = 0.18         -- seconds to come up into the aim, and down
HINT_TIME     = 7            -- seconds the controls show after equipping
ACCENT_COLOR  = {r = 0.96, g = 0.72, b = 0.32}
IMPACT_FX     = "FX_Einschlag"  -- objects with a Particle burst (and a light)
SPARK_FX      = "FX_Funken"
MUZZLE_FX     = "FX_Muendung"
SMOKE_FX      = "FX_Rauch"
MUZZLE_LIGHT  = "Muendungslicht"
SHOT_SOUND    = "wpn_shot.wav"
DRY_SOUND     = "wpn_dry.wav"
RELOAD_SOUND  = "wpn_reload.wav"
HIT_SOUND     = "wpn_hit.wav"
-- Glass (a see-through material, or one with Glass ticked) breaks where it is
-- hit (game.shatter) and the shot goes on through the hole. GLASS = false: it
-- takes holes like a wall.
GLASS         = true
GLASS_SOUND   = "glass-shatter.wav"
-- Bullet holes (game.decal): how big, and the library material to paint them
-- with ("" = the engine's own). BULLET_HOLES = false for none.
BULLET_HOLES  = true
HOLE_SIZE     = 0.12
HOLE_MATERIAL = ""

local TEXT = {
    reload  = "NACHLADEN",
    empty   = "KEINE MUNITION",
    aim     = "Zielen",
    fire    = "Schießen",
    load    = "Nachladen",
    rmb     = "RMT",
    lmb     = "LMT",
}

local A                -- the accent colour
local _mags = {}       -- weapon id -> rounds in its magazine
local _weaponId        -- the weapon in the hand last frame
local _aiming = false
local _aimW = 0        -- 0 down .. 1 fully in the aim
local _gripAim = false -- the weapon sits in the aiming grip
local _cool = 0        -- seconds until the next shot may go
local _reload = 0      -- seconds left of a reload (0 = none)
local _kick = 0        -- recoil, 1 at the shot, dying away
local _spread = 0      -- the crosshair opening after shots
local _hitMark = 0     -- seconds the hit marker still shows
local _flash = 0       -- seconds the muzzle light still burns
local _hint = 0        -- seconds the controls have shown
local _later = {}      -- sounds still on their way (a far hit is heard later)
local _fx = {}
local _down, _was = {}, {}
local _t = 0

local function clamp(v, lo, hi) return math.max(lo, math.min(hi, v)) end
local function ease(u) u = clamp(u, 0, 1); return u * u * (3 - 2 * u) end
local function sound(n) if n and n ~= "" then game.playSound(n) end end

local function textSize(s, size, bold)
    local w, h = game.hudTextSize(s, size, bold)
    return w or 0, h or size
end

local function icon(tex, cx, cy, maxW, maxH, a)
    if not tex or tex == "" then return false end
    local w, h, u0, v0, u1, v1 = game.imageSize(tex)
    if not w then return false end
    local cw, ch = (u1 - u0) * w, (v1 - v0) * h
    local k = math.min(maxW / cw, maxH / ch)
    local dw, dh = cw * k, ch * k
    local x, y = cx - dw * 0.5, cy - dh * 0.5
    game.hudImage(tex, x + 3, y + 6, dw, dh, 0, 0, 0, 0.45 * a, u0, v0, u1, v1)
    game.hudImage(tex, x, y, dw, dh, 1, 1, 1, a, u0, v0, u1, v1)
    return true
end

local function keyLabel(x, y, h, label, a)
    local fs = h * 0.44
    local tw, th = textSize(label, fs, true)
    local w = math.max(h, tw + h * 0.6)
    game.hudRect(x, y, w, h, 1, 1, 1, 0.07 * a, h * 0.2)
    game.hudFrame(x, y, w, h, 1, 1, 1, 0.35 * a, 1.5, h * 0.2)
    game.hudText(x + w * 0.5, y + (h - th) * 0.5, label, fs, 0.95, 0.95, 0.95, a, 0.5, true)
    return w
end

local function api(name, self)
    return shared and shared[name] and shared[name][self.id]
end

-- Edges of our own, from mouseDown / keyDown (asked every frame).
local function edge(key, now)
    local was = _was[key]
    _was[key] = now
    return now and not was
end

function start(self, dt, t)
    A = {r = ACCENT_COLOR.r or ACCENT_COLOR[1], g = ACCENT_COLOR.g or ACCENT_COLOR[2],
         b = ACCENT_COLOR.b or ACCENT_COLOR[3]}
    _mags, _weaponId, _aiming, _aimW, _gripAim = {}, nil, false, 0, false
    _cool, _reload, _kick, _spread, _hitMark, _flash, _hint = 0, 0, 0, 0, 0, 0, 0
    _later, _was, _t = {}, {}, 0
    _fx = {}
    for key, name in pairs({impact = IMPACT_FX, spark = SPARK_FX, muzzle = MUZZLE_FX,
                            smoke = SMOKE_FX, light = MUZZLE_LIGHT}) do
        if name ~= "" then _fx[key] = game.find(name) end
    end
    if shared then
        shared.weapons = shared.weapons or {}
        shared.weapons[self.id] = {
            -- Rounds in a weapon's magazine, and how many it takes.
            magazine = function(id) return _mags[id] or MAG_SIZE, MAG_SIZE end,
            aiming = function() return _aiming end,
        }
    end
end

-- A sound from `metres` away, heard when it gets back -- as far as 60 m off.
local function heard(name, metres)
    if metres < 60 then _later[#_later + 1] = {t = metres / 340, name = name} end
end

local function burst(key, x, y, z)
    local id = _fx[key]
    if not id then return end
    game.setPos(id, x, y, z)
    game.emit(id)
end

-- One shot: the recoil, the bang, the flash at the muzzle, and where it goes.
local function shoot(self, weapon)
    _mags[weapon.id] = _mags[weapon.id] - 1
    _cool, _kick = FIRE_INTERVAL, 1
    _spread = math.min(_spread + 1, 2.5)
    game.animTrigger(self.id, SHOOT_PARAM)
    sound(SHOT_SOUND)
    local mx, my, mz = game.toWorld(weapon.id, MUZZLE.x, MUZZLE.y, MUZZLE.z)
    if mx then
        burst("muzzle", mx, my, mz)
        burst("smoke", mx, my, mz)
        if _fx.light then game.setPos(_fx.light, mx, my, mz) end
        _flash = 0.06
    end
    -- From the eye through the middle of the picture -- starting level with the
    -- figure, so nothing between the camera and its shoulder is hit.
    local cx, cy, cz = game.cameraPos()
    local dx, dy, dz = game.cameraDir()
    local fx, fy, fz = game.getPos(self.id)
    local ahead = math.max(0, (fx - cx) * dx + (fy - cy) * dy + (fz - cz) * dz)
    local ox, oy, oz = cx + dx * ahead, cy + dy * ahead, cz + dz * ahead
    local hx, hy, hz, nx, ny, nz, hit, dist = game.castRay(ox, oy, oz, dx, dy, dz, RANGE)
    -- Glass gives way, and the shot goes on: cast again, the broken pane is no
    -- longer there to meet.
    for _ = 1, 4 do
        if not (GLASS and hx and game.shatter and
                game.shatter(hit, hx, hy, hz, dx, dy, dz)) then break end
        heard(GLASS_SOUND, dist + ahead)
        hx, hy, hz, nx, ny, nz, hit, dist = game.castRay(ox, oy, oz, dx, dy, dz, RANGE)
    end
    if not hx then return end
    burst("impact", hx + nx * 0.03, hy + ny * 0.03, hz + nz * 0.03)
    -- A hole where it went in -- on whatever stands still there.
    if BULLET_HOLES and game.decal then
        game.decal(hx, hy, hz, nx, ny, nz, HOLE_SIZE, HOLE_MATERIAL, math.random() * 360)
    end
    if hit >= 0 then
        burst("spark", hx + nx * 0.02, hy + ny * 0.02, hz + nz * 0.02)
        -- Where it struck, too: a hanging thing (a Swing) swings from it.
        game.applyImpulse(hit, dx * IMPULSE, dy * IMPULSE, dz * IMPULSE, hx, hy, hz)
        _hitMark = 0.22
    end
    heard(HIT_SOUND, dist + ahead)
end

local function startReload(inv, weapon)
    if _reload > 0 or not inv or inv.ammo() <= 0 or _mags[weapon.id] >= MAG_SIZE then return end
    _reload = RELOAD_TIME
    sound(RELOAD_SOUND)
end

-- --- HUD --------------------------------------------------------------------------

local function drawCrosshair(W, a)
    if a <= 0.02 then return end
    local cx, cy = W * 0.5, 540
    local gap = 10 + _spread * 7 + (1 - a) * 16
    local len = 13
    local ticks = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}}
    for _, d in ipairs(ticks) do
        local x1, y1 = cx + d[1] * gap, cy + d[2] * gap
        local x2, y2 = cx + d[1] * (gap + len), cy + d[2] * (gap + len)
        game.hudLine(x1, y1, x2, y2, 0, 0, 0, 0.55 * a, 4)
        game.hudLine(x1, y1, x2, y2, 1, 1, 1, 0.95 * a, 2)
    end
    game.hudCircle(cx, cy, 2.6, 0, 0, 0, 0.5 * a)
    game.hudCircle(cx, cy, 1.6, 1, 1, 1, a)
    if _hitMark > 0 then
        -- A hit: four short strokes on the diagonals.
        local k = _hitMark / 0.22
        local r0, r1 = 7 + (1 - k) * 4, 16 + (1 - k) * 4
        for _, d in ipairs({{1, 1}, {-1, 1}, {1, -1}, {-1, -1}}) do
            game.hudLine(cx + d[1] * r0, cy + d[2] * r0, cx + d[1] * r1, cy + d[2] * r1,
                         1, 0.92, 0.85, k * a, 2.5)
        end
    end
end

local function drawCard(W, weapon, inv, a)
    if a <= 0.01 then return end
    local w, h = 380, 118
    local x, y = W - w - 40, 1080 - h - 40 + (1 - a) * 16
    game.hudRect(x, y, w, h, 0.05, 0.055, 0.07, 0.80 * a, 12)
    game.hudFrame(x, y, w, h, 1, 1, 1, 0.10 * a, 1.2, 12)
    if not icon(weapon.icon, x + 74, y + 48, 116, 66, a) then
        game.hudText(x + 74, y + 30, weapon.name:sub(1, 1), 36, 1, 1, 1, a, 0.5, true)
    end
    game.hudText(x + 148, y + 14, weapon.name, 22, 0.97, 0.97, 0.98, a, 0, true)
    local mag = _mags[weapon.id] or MAG_SIZE
    local rest = inv and inv.ammo() or 0
    local s = tostring(mag)
    local mw = textSize(s, 46, true)
    local low = mag == 0
    local cr, cg, cb = 0.98, 0.98, 0.99
    if low then cr, cg, cb = 0.97, 0.45, 0.40 end
    game.hudText(x + 148, y + 38, s, 46, cr, cg, cb, a, 0, true)
    game.hudText(x + 148 + mw + 8, y + 58, "/ " .. rest, 22, 0.70, 0.72, 0.76, a, 0, true)
    -- The rounds in the magazine, one mark each.
    local pw, ph, gap = 9, 20, 5
    local px = x + w - 24 - MAG_SIZE * (pw + gap) + gap
    for i = 1, MAG_SIZE do
        local full = i <= mag
        game.hudRect(px + (i - 1) * (pw + gap), y + 48, pw, ph,
                     full and A.r or 1, full and A.g or 1, full and A.b or 1,
                     (full and 0.95 or 0.12) * a, 2)
    end
    -- Underneath: what it is doing, or what it needs.
    local ly = y + h - 30
    if _reload > 0 then
        local u = 1 - _reload / RELOAD_TIME
        game.hudText(x + 20, ly - 4, TEXT.reload, 15, A.r, A.g, A.b, a, 0, true)
        game.hudRect(x + 120, ly + 3, w - 140, 5, 1, 1, 1, 0.12 * a, 2)
        game.hudRect(x + 120, ly + 3, (w - 140) * u, 5, A.r, A.g, A.b, 0.95 * a, 2)
    elseif low and rest > 0 then
        local blink = 0.55 + 0.45 * math.sin(_t * 6)
        local kw = keyLabel(x + 20, ly - 6, 24, RELOAD_KEY:upper(), a * blink)
        game.hudText(x + 20 + kw + 8, ly - 3, TEXT.reload, 15, 0.95, 0.95, 0.95, a * blink, 0, true)
    elseif low then
        game.hudText(x + 20, ly - 4, TEXT.empty, 15, 0.97, 0.45, 0.40, a, 0, true)
    end
end

local function drawHint(W, a)
    if a <= 0.01 then return end
    local h, y = 30, 1080 - 40 - 118 - 14 - 30
    local parts = {{TEXT.rmb, TEXT.aim}, {TEXT.lmb, TEXT.fire}, {RELOAD_KEY:upper(), TEXT.load}}
    local total = 0
    for _, p in ipairs(parts) do
        total = total + math.max(h, textSize(p[1], h * 0.44, true) + h * 0.6) + 8 +
                textSize(p[2], 16, true) + 18
    end
    local x = W - 40 - total + 18
    for _, p in ipairs(parts) do
        x = x + keyLabel(x, y, h, p[1], a) + 8
        game.hudText(x, y + 6, p[2], 16, 0.85, 0.86, 0.9, a, 0, true)
        x = x + textSize(p[2], 16, true) + 18
    end
end

-- --- update ---------------------------------------------------------------------------

function update(self, dt, t)
    _t = _t + dt
    local inv = api("inventories", self)
    local fig = api("figures", self)
    local eq = inv and inv.equipped()
    local weapon = eq and eq.category == "weapon" and eq or nil
    local open = inv and inv.isOpen and inv.isOpen()
    local W = game.hudSize()

    if weapon and _mags[weapon.id] == nil then _mags[weapon.id] = MAG_SIZE end
    if (weapon and weapon.id) ~= _weaponId then
        -- Taken in hand (in the carrying grip) or put away: a fresh start.
        if not weapon and not open then game.setCrosshair(true) end
        _weaponId, _gripAim, _reload, _hint = weapon and weapon.id, false, 0, 0
    end

    -- --- Aim ----------------------------------------------------------------------
    local rmb = game.mouseDown(AIM_BUTTON)
    local want = weapon ~= nil and rmb and not open
    if want and fig then
        local dx, _, dz = game.cameraDir()
        want = fig.hold(math.deg(math.atan(dx, dz)))
    end
    _aiming = want
    _aimW = clamp(_aimW + (want and dt or -dt) / AIM_TIME, 0, 1)
    local e = ease(_aimW)
    if e > 0 then
        game.orbitFrame(e, AIM_DIST, AIM_SIDE, AIM_UP + _kick * 0.035, AIM_FOV + _kick * 1.5)
    end
    -- The weapon turns in the hand as the arms come up, and back as they go down.
    if weapon and want ~= _gripAim then
        local p, r = AIM_GRIP_POS, AIM_GRIP_ROT
        if not want then p, r = inv.carryGrip() end
        game.attach(weapon.id, self.id, HAND_BONE, p.x, p.y, p.z, r.x, r.y, r.z, AIM_TIME)
        _gripAim = want
    end

    -- --- Clocks ----------------------------------------------------------------------
    _cool = math.max(0, _cool - dt)
    _kick = _kick * math.exp(-14 * dt)
    _spread = _spread * math.exp(-5 * dt)
    _hitMark = math.max(0, _hitMark - dt)
    for i = #_later, 1, -1 do
        _later[i].t = _later[i].t - dt
        if _later[i].t <= 0 then sound(_later[i].name); table.remove(_later, i) end
    end
    if _fx.light then
        _flash = math.max(0, _flash - dt)
        game.setLight(_fx.light, {intensity = _flash > 0 and 40 * (_flash / 0.06) or 0})
    end

    -- --- Reload and fire ------------------------------------------------------------
    local fire = edge("fire", game.mouseDown(FIRE_BUTTON))
    local reloadKey = edge("reload", game.keyDown(game["KEY_" .. RELOAD_KEY:upper()]))
    if weapon then
        if _reload > 0 then
            _reload = _reload - dt
            if _reload <= 0 then
                _reload = 0
                _mags[weapon.id] = _mags[weapon.id] + inv.takeAmmo(MAG_SIZE - _mags[weapon.id])
            end
        elseif reloadKey then
            startReload(inv, weapon)
        end
        if fire and _aimW > 0.7 and _cool <= 0 and _reload <= 0 then
            if _mags[weapon.id] > 0 then
                shoot(self, weapon)
            else
                sound(DRY_SOUND)
                _cool = 0.3
                startReload(inv, weapon)
            end
        end
    end

    -- --- Draw -------------------------------------------------------------------------
    if weapon and not open then
        game.setCrosshair(false)     -- the weapon draws its own
        drawCrosshair(W, e)
        drawCard(W, weapon, inv, 1)
        _hint = _hint + dt
        local ha = (_hint < HINT_TIME and _aimW < 0.5) and (1 - ease((_hint - HINT_TIME + 0.6) / 0.6)) or 0
        drawHint(W, ha)
    end
end
