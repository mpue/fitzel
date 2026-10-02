-- An inventory for a figure walked with tp_controller.lua: pick things up off
-- the ground, keep them, carry a weapon in the hand, throw things away again.
--
-- Hang it on the figure as a SECOND Script component, after the controller.
-- What it can pick up are objects with a Collectible component whose
-- "Inventory item" box is ticked: their name, icon, category and description
-- are what the bag shows. Within the Collectible's pickup radius of the figure
-- a prompt appears over the thing; PICKUP_KEY has the figure turn, step into
-- place, bend down and take it (the controller does the walking -- see
-- PICKING THINGS UP there; without it, the thing goes straight into the bag).
--
--   E            pick up what is offered
--   I or Tab     open / close the bag (Esc closes it too)
--   in the bag:  arrows or WASD choose, mouse too; E / Enter equips a weapon
--                (or puts it away again), X throws the chosen thing away
--
-- A weapon that is equipped hangs in the right hand (HAND_BONE), placed by
-- GRIP_POS / GRIP_ROT in the bone's space. Something thrown away flies out in
-- front of the figure and lands where it can be picked up again -- it is the
-- very object, deactivated while it is in the bag, so nothing about it is lost.

OPEN_KEY     = "I"              -- opens and closes the bag (Tab does as well)
PICKUP_KEY   = "E"              -- picks up; in the bag: equip / put away
DROP_KEY     = "X"              -- in the bag: throw the chosen thing away
SLOTS        = 12               -- places in the bag, four to a row
HAND_BONE    = "CC_Base_R_Hand" -- where a weapon is carried
GRAB_BONE    = "CC_Base_L_Hand" -- the hand the pickup clip reaches with
-- The weapon in the hand, in the bone's space: metres, and its turn in degrees.
-- Worked out for a CC rig (the hand's Y runs along the fingers, Z to the thumb,
-- the palm faces -X) and a pistol modelled lying on its side (muzzle +Z, slide
-- +X): the grip across the palm, raked 19 degrees, the slide on the thumb side.
GRIP_POS     = {x = -0.031, y = 0.112, z = 0.063}
GRIP_ROT     = {x = -0.1, y = -57.5, z = -90.3}
ACCENT_COLOR = {r = 0.96, g = 0.72, b = 0.32}
HINT_TIME    = 8                -- seconds the "I Inventory" hint shows after Play starts
STOW_SOUND   = "inv_stow.wav"
EQUIP_SOUND  = "inv_equip.wav"
DROP_SOUND   = "inv_drop.wav"
OPEN_SOUND   = "inv_open.wav"
CLICK_SOUND  = "inv_click.wav"

-- What the player reads. One place, so the bag speaks another language by
-- changing these lines.
local TEXT = {
    title      = "INVENTAR",
    places     = "%d / %d PLÄTZE",
    take       = "%s aufheben",
    full       = "Kein Platz mehr im Inventar",
    stowed     = "ins Inventar gelegt",
    dropped    = "weggeworfen",
    equip      = "AUSRÜSTEN",
    unequip    = "ABLEGEN",
    drop       = "WEGWERFEN",
    equipped   = "AUSGERÜSTET",
    empty      = "Leerer Platz",
    emptyHint  = "Gegenstände in der Welt mit %s aufheben.",
    close      = "SCHLIESSEN",
    choose     = "AUSWAHL",
    arrows     = "PFEILE",
    bag        = "Inventar",
    category   = {misc = "GEGENSTAND", weapon = "WAFFE", ammo = "MUNITION",
                  health = "HEILUNG", key = "SCHLÜSSEL", document = "DOKUMENT"},
}

local COLS = 4
local STOW_TIME = 0.22    -- the thing shrinking into the bag after the pickup
local TOSS_TIME = 0.55    -- in the air when thrown away
local TOAST_LIFE = 3.2

local _items     -- the bag, in slot order: {id, name, icon, category, ...}
local _equipped  -- id of the thing in the hand (it is in the bag as well)
local _open, _openT, _sel
local _pick      -- a pickup under way: {id, info, job, attached}
local _stow      -- the thing going into the bag: {id, info, t, half}
local _tosses    -- things thrown away, still in the air
local _toasts    -- messages sliding in on the right
local _offer, _offerInfo, _offerA
local _equipA, _hintT
local _down, _press = {}, {}
local _mouseWas = false
local _t = 0
local _wrapCache = {}

local function clamp(v, lo, hi) return math.max(lo, math.min(hi, v)) end
local function ease(u) u = clamp(u, 0, 1); return u * u * (3 - 2 * u) end
local function lerp(a, b, u) return a + (b - a) * u end

local function key(name)
    if name == "Tab" or name == "TAB" then return game.KEY_TAB end
    return game["KEY_" .. tostring(name):upper()]
end

-- Our own key edges: game.keyPressed only knows a key that was also asked the
-- frame before, and these are read every frame anyway.
local function poll()
    local watch = {key(OPEN_KEY), game.KEY_TAB, game.KEY_ESCAPE, key(PICKUP_KEY),
                   key(DROP_KEY), game.KEY_ENTER, game.KEY_LEFT, game.KEY_RIGHT,
                   game.KEY_UP, game.KEY_DOWN, game.KEY_W, game.KEY_A, game.KEY_S,
                   game.KEY_D, game.KEY_DELETE}
    for _, k in ipairs(watch) do
        if k then
            local now = game.keyDown(k)
            _press[k] = now and not _down[k]
            _down[k] = now
        end
    end
end
local function pressed(k) return k and _press[k] end

local function sound(name)
    if name and name ~= "" then game.playSound(name) end
end

local function inBag(id)
    for i, it in ipairs(_items) do if it.id == id then return i end end
    return nil
end
local function flying(id)
    for _, t in ipairs(_tosses) do if t.id == id then return true end end
    return false
end

local function toast(title, sub, icon)
    table.insert(_toasts, 1, {title = title, sub = sub, icon = icon, t = 0})
    while #_toasts > 3 do table.remove(_toasts) end
end

local function figureApi(self)
    return shared and shared.figures and shared.figures[self.id]
end

-- --- Drawing helpers --------------------------------------------------------------

local A -- the accent colour, read from ACCENT_COLOR in start()

local function textSize(s, size, bold)
    local w, h = game.hudTextSize(s, size, bold)
    return w or 0, h or size
end

-- Text with its letters spread apart: a title set like a title.
local function spaced(x, y, s, size, r, g, b, a, gap, bold)
    for ch in s:gmatch("[%z\1-\127\194-\244][\128-\191]*") do
        game.hudText(x, y, ch, size, r, g, b, a, 0, bold)
        x = x + textSize(ch, size, bold) + gap
    end
    return x
end

local function wrap(text, size, maxW)
    local k = text .. "|" .. size .. "|" .. maxW
    if _wrapCache[k] then return _wrapCache[k] end
    local lines, line = {}, ""
    for word in text:gmatch("%S+") do
        local test = line == "" and word or (line .. " " .. word)
        if line ~= "" and textSize(test, size) > maxW then
            lines[#lines + 1] = line
            line = word
        else
            line = test
        end
    end
    if line ~= "" then lines[#lines + 1] = line end
    _wrapCache[k] = lines
    return lines
end

-- A picture fitted into a box around (cx, cy): only its visible part counts,
-- so an icon rendered with a wide margin still fills its slot. A soft shadow
-- below it lifts it off the panel.
local function icon(tex, cx, cy, maxW, maxH, a, shadow)
    if not tex or tex == "" then return false end
    local w, h, u0, v0, u1, v1 = game.imageSize(tex)
    if not w then return false end
    local cw, ch = (u1 - u0) * w, (v1 - v0) * h
    local k = math.min(maxW / cw, maxH / ch)
    local dw, dh = cw * k, ch * k
    local x, y = cx - dw * 0.5, cy - dh * 0.5
    if shadow then
        game.hudImage(tex, x + 3, y + 7, dw, dh, 0, 0, 0, 0.45 * a, u0, v0, u1, v1)
    end
    game.hudImage(tex, x, y, dw, dh, 1, 1, 1, a, u0, v0, u1, v1)
    return true
end

-- An item without a picture: its first letter in a ring.
local function noIcon(name, cx, cy, size, a)
    game.hudCircle(cx, cy, size * 0.5, 1, 1, 1, 0.06 * a)
    game.hudCircle(cx, cy, size * 0.5, 1, 1, 1, 0.25 * a, 2)
    local letter = (name or "?"):match("[%z\1-\127\194-\244][\128-\191]*") or "?"
    local _, th = textSize(letter, size * 0.5, true)
    game.hudText(cx, cy - th * 0.5, letter, size * 0.5, 1, 1, 1, 0.8 * a, 0.5, true)
end

-- A key cap: filled with the accent, outlined on dark, or (`onAccent`) dark on
-- an accent-coloured button, where a light outline would vanish.
local function keycap(x, y, s, label, a, filled, onAccent)
    local fs = label:len() > 1 and s * 0.42 or s * 0.55
    local c = 0.95
    if filled then
        game.hudRect(x, y, s, s, A.r, A.g, A.b, 0.95 * a, s * 0.2)
        c = 0.08
    elseif onAccent then
        game.hudRect(x, y, s, s, 0, 0, 0, 0.16 * a, s * 0.2)
        game.hudFrame(x, y, s, s, 0, 0, 0, 0.45 * a, 1.5, s * 0.2)
        c = 0.08
    else
        game.hudRect(x, y, s, s, 1, 1, 1, 0.07 * a, s * 0.2)
        game.hudFrame(x, y, s, s, 1, 1, 1, 0.35 * a, 1.5, s * 0.2)
    end
    local tw, th = textSize(label, fs, true)
    game.hudText(x + s * 0.5, y + (s - th) * 0.5, label, fs, c, c, c, a, 0.5, true)
end

-- A key cap that may be wider than square (PFEILE).
local function keyLabel(x, y, h, label, a)
    local fs = h * 0.46
    local tw, th = textSize(label, fs, true)
    local w = math.max(h, tw + h * 0.6)
    game.hudRect(x, y, w, h, 1, 1, 1, 0.07 * a, h * 0.2)
    game.hudFrame(x, y, w, h, 1, 1, 1, 0.35 * a, 1.5, h * 0.2)
    game.hudText(x + w * 0.5, y + (h - th) * 0.5, label, fs, 0.95, 0.95, 0.95, a, 0.5, true)
    return w
end

local function diamond(cx, cy, r, cr, cg, cb, a)
    game.hudTri(cx, cy - r, cx + r, cy, cx, cy + r, cr, cg, cb, a)
    game.hudTri(cx, cy - r, cx - r, cy, cx, cy + r, cr, cg, cb, a)
end

local function pill(x, y, label, a, cr, cg, cb)
    local fs = 17
    local tw, th = textSize(label, fs, true)
    local w, h = tw + 22, 28
    game.hudRect(x, y, w, h, cr, cg, cb, 0.16 * a, 14)
    game.hudFrame(x, y, w, h, cr, cg, cb, 0.55 * a, 1.2, 14)
    game.hudText(x + w * 0.5, y + (h - th) * 0.5, label, fs, cr, cg, cb, a, 0.5, true)
    return w
end

-- --- The bag ------------------------------------------------------------------------

local function isWeapon(it) return it and it.category == "weapon" end

local function equip(self, it)
    if _equipped and _equipped ~= it.id then
        game.detach(_equipped)
        game.setActive(_equipped, false)
    end
    game.setActive(it.id, true)
    game.setScale(it.id, it.half[1], it.half[2], it.half[3])
    local ok = game.attach(it.id, self.id, HAND_BONE, GRIP_POS.x, GRIP_POS.y, GRIP_POS.z,
                           GRIP_ROT.x, GRIP_ROT.y, GRIP_ROT.z)
    if not ok then
        game.setActive(it.id, false)
        game.log("inventory: no bone '" .. HAND_BONE .. "' on this figure")
        return
    end
    _equipped = it.id
    sound(EQUIP_SOUND)
end

local function unequip()
    if not _equipped then return end
    game.detach(_equipped)
    game.setActive(_equipped, false)
    _equipped = nil
    sound(CLICK_SOUND)
end

-- Out of the bag and out in front: from the hand (or the chest), in an arc,
-- turning over, onto the ground half a step ahead -- where the prompt will
-- offer it again.
local function drop(self, index)
    local it = table.remove(_items, index)
    if not it then return end
    local fx, fy, fz = game.getPos(self.id)
    local _, ry = game.getRot(self.id)
    ry = ry or 0
    local r = math.rad(ry)
    local sx, sz = math.sin(r), math.cos(r)        -- ahead
    local lx, lz = math.cos(r), -math.sin(r)       -- to the left
    local x0, y0, z0 = fx + sx * 0.3, fy + 0.35, fz + sz * 0.3
    local rot0 = {it.rest[1], it.rest[2], it.rest[3]}
    if _equipped == it.id then
        local hx, hy, hz, rx, rry, rz = game.bonePos(self.id, HAND_BONE)
        if hx then
            x0, y0, z0 = hx, hy, hz
            rot0 = {rx, rry, rz}
        end
        game.detach(it.id)
        _equipped = nil
    end
    local tx = fx + sx * 0.8 - lx * 0.12
    local tz = fz + sz * 0.8 - lz * 0.12
    -- Where it lands: on whatever is there -- the road, a bridge deck, a floor
    -- -- or else the terrain. Looked for from a little above the feet, so a
    -- bridge overhead is not where it ends up.
    local feetY = fy - 0.9
    local fig = figureApi(self)
    if fig then
        local _, y = fig.feet()
        if y then feetY = y end
    end
    local ground = (game.groundHeight and game.groundHeight(tx, feetY + 0.6, tz))
                   or game.terrainHeight(tx, tz)
    game.setScale(it.id, it.half[1], it.half[2], it.half[3])
    game.setPos(it.id, x0, y0, z0)
    game.setRot(it.id, rot0[1], rot0[2], rot0[3])
    game.setActive(it.id, true)
    table.insert(_tosses, {
        id = it.id, t = 0, from = {x0, y0, z0}, to = {tx, ground + it.lift, tz},
        rot0 = rot0, rot1 = {it.rest[1], ry + 90 + math.random(-30, 30), it.rest[3]},
        spin = (math.random() < 0.5 and -1 or 1) * 360,
    })
    _sel = clamp(_sel, 1, math.max(1, #_items))
    toast(it.name, TEXT.dropped, it.icon)
end

local function stowNow(id, info, half)
    local x, y, z = game.getPos(id)
    local rx, ry, rz = game.getRot(id)
    local hx, hy, hz = table.unpack(half)
    -- How high it lay above what it lay on (a road is not the terrain), so it
    -- comes to rest the same way when it is thrown down again.
    local x0, y0, z0 = info.restX or x, info.restY or y, info.restZ or z
    local base = (game.groundHeight and game.groundHeight(x0, y0 + 0.3, z0))
                 or game.terrainHeight(x0, z0)
    local lift = clamp(y0 - base, hy, hy + 0.05)
    _items[#_items + 1] = {
        id = id, name = info.item, icon = info.icon, category = info.category,
        description = info.description, count = info.count,
        half = {hx, hy, hz}, rest = info.rest or {rx, ry, rz}, lift = lift,
    }
    game.detach(id)
    game.setActive(id, false)
    game.setScale(id, hx, hy, hz)
    toast(info.item, TEXT.stowed, info.icon)
end

-- What lies within reach right now: the nearest inventory item whose pickup
-- radius the figure stands in.
local function nearest(self)
    local fx, fy, fz = game.getPos(self.id)
    if not fx then return nil end
    local best, bestD, bestInfo
    for _, id in ipairs(game.collectibles()) do
        if not inBag(id) and not flying(id) and (not _pick or _pick.id ~= id)
           and (not _stow or _stow.id ~= id) then
            local info = game.collectible(id)
            if info and info.inventory then
                local x, y, z = game.getPos(id)
                local d = math.sqrt((x - fx) ^ 2 + (z - fz) ^ 2)
                if d <= info.radius and math.abs(y - fy) < 2.0 and (not bestD or d < bestD) then
                    best, bestD, bestInfo = id, d, info
                end
            end
        end
    end
    return best, bestInfo
end

-- --- start / update -----------------------------------------------------------------

function start(self, dt, t)
    _items, _equipped = {}, nil
    _open, _openT, _sel = false, 0, 1
    _pick, _stow, _tosses, _toasts = nil, nil, {}, {}
    _offer, _offerInfo, _offerA = nil, nil, 0
    _equipA, _hintT = 0, 0
    _down, _press, _mouseWas, _t, _wrapCache = {}, {}, false, 0, {}
    A = {r = ACCENT_COLOR.r or ACCENT_COLOR[1], g = ACCENT_COLOR.g or ACCENT_COLOR[2],
         b = ACCENT_COLOR.b or ACCENT_COLOR[3]}
end

local function setOpen(on)
    if _open == on then return end
    _open = on
    game.showCursor(on)
    game.setCrosshair(not on)
    sound(OPEN_SOUND)
    if on then
        _sel = 1
        for i, it in ipairs(_items) do if it.id == _equipped then _sel = i end end
    end
end

-- The pickup and the stowing, frame by frame.
local function tickPickup(self, dt)
    if _pick then
        local job = _pick.job
        if job.grabbed and not _pick.attached then
            -- The hand is on it: from now on it goes where the hand goes.
            _pick.attached = game.attach(_pick.id, self.id, GRAB_BONE)
        end
        if job.done then
            if job.cancelled and not _pick.attached then
                _pick = nil
            else
                local hx, hy, hz = game.getScale(_pick.id)
                _stow = {id = _pick.id, info = _pick.info, t = 0, half = {hx, hy, hz}}
                _pick = nil
                sound(STOW_SOUND)
            end
        end
    end
    if _stow then
        _stow.t = _stow.t + dt
        local u = _stow.t / STOW_TIME
        local k = 1 - 0.95 * ease(u)
        local h = _stow.half
        game.setScale(_stow.id, h[1] * k, h[2] * k, h[3] * k)
        if u >= 1 then
            stowNow(_stow.id, _stow.info, _stow.half)
            _stow = nil
        end
    end
end

local function tickTosses(dt)
    for i = #_tosses, 1, -1 do
        local s = _tosses[i]
        s.t = s.t + dt
        local u = clamp(s.t / TOSS_TIME, 0, 1)
        local x = lerp(s.from[1], s.to[1], u)
        local z = lerp(s.from[3], s.to[3], u)
        local y = lerp(s.from[2], s.to[2], u * u) + 0.32 * 4 * u * (1 - u)
        game.setPos(s.id, x, y, z)
        local e = ease(u)
        game.setRot(s.id, lerp(s.rot0[1], s.rot1[1], e),
                    lerp(s.rot0[2], s.rot1[2], e) + s.spin * (1 - e),
                    lerp(s.rot0[3], s.rot1[3], e))
        if u >= 1 then
            sound(DROP_SOUND)
            table.remove(_tosses, i)
        end
    end
end

-- --- HUD: the prompt, the messages, the weapon in hand ------------------------------

local function drawPrompt(self, W, a)
    if not _offer or a <= 0.01 then return end
    local x, y, z = game.getPos(_offer)
    if not x then return end
    local mx, my = game.worldToHud(x, y, z)
    local px, py = game.worldToHud(x, y + 0.42, z)
    if not mx or not px then return end
    local full = #_items >= SLOTS
    local pulse = 0.5 + 0.5 * math.sin(_t * 3.4)
    diamond(mx, my, 6 + 2 * pulse, A.r, A.g, A.b, (0.55 + 0.4 * pulse) * a)
    game.hudCircle(mx, my, 13 + 5 * pulse, A.r, A.g, A.b, 0.35 * (1 - pulse) * a, 1.5)
    local label = full and TEXT.full or string.format(TEXT.take, _offerInfo.item)
    local fs = 25
    local tw, th = textSize(label, fs, true)
    local h = 48
    local w = (full and 0 or h - 12 + 14) + tw + 30
    local bx = clamp(px - w * 0.5, 16, W - w - 16)
    local by = clamp(py - h - 8 + (1 - a) * 10, 16, 1080 - h - 16)
    game.hudLine(mx, my - 12, clamp(px, bx + 12, bx + w - 12), by + h, 1, 1, 1, 0.30 * a, 1.5)
    game.hudRect(bx, by, w, h, 0.05, 0.055, 0.07, 0.86 * a, 11)
    game.hudFrame(bx, by, w, h, 1, 1, 1, 0.13 * a, 1.2, 11)
    local tx = bx + 15
    if not full then
        keycap(bx + 6, by + 6, h - 12, PICKUP_KEY:upper(), a, true)
        tx = bx + 6 + (h - 12) + 14
    end
    local c = full and {0.95, 0.55, 0.5} or {0.96, 0.96, 0.98}
    game.hudText(tx, by + (h - th) * 0.5, label, fs, c[1], c[2], c[3], a, 0, true)
end

local function drawToasts(W, dt)
    local y = 140
    for i = #_toasts, 1, -1 do
        _toasts[i].t = _toasts[i].t + dt
        if _toasts[i].t > TOAST_LIFE then table.remove(_toasts, i) end
    end
    for _, m in ipairs(_toasts) do
        local inU = ease(m.t / 0.3)
        local outU = 1 - ease((m.t - (TOAST_LIFE - 0.45)) / 0.45)
        local a = inU * outU
        local w, h = 440, 94
        local x = W - w - 40 + (1 - inU) * 70
        game.hudRect(x, y, w, h, 0.05, 0.055, 0.07, 0.86 * a, 12)
        game.hudFrame(x, y, w, h, 1, 1, 1, 0.10 * a, 1.2, 12)
        game.hudRect(x, y + 14, 4, h - 28, A.r, A.g, A.b, a, 2)
        if not icon(m.icon, x + 76, y + h * 0.5, 104, 62, a, true) then
            noIcon(m.title, x + 76, y + h * 0.5, 54, a)
        end
        game.hudText(x + 146, y + 18, m.title, 28, 0.97, 0.97, 0.98, a, 0, true)
        game.hudText(x + 146, y + 54, m.sub, 20, 0.72, 0.74, 0.78, a)
        y = y + h + 12
    end
end

local function drawEquipped(W, a)
    if a <= 0.01 then return end
    local it
    for _, x in ipairs(_items) do if x.id == _equipped then it = x end end
    if not it then return end
    local w, h = 330, 96
    local x, y = W - w - 40, 1080 - h - 40 + (1 - a) * 16
    game.hudRect(x, y, w, h, 0.05, 0.055, 0.07, 0.78 * a, 12)
    game.hudFrame(x, y, w, h, 1, 1, 1, 0.10 * a, 1.2, 12)
    if not icon(it.icon, x + 72, y + h * 0.5, 112, 66, a, true) then
        noIcon(it.name, x + 72, y + h * 0.5, 54, a)
    end
    game.hudText(x + 146, y + 20, it.name, 26, 0.97, 0.97, 0.98, a, 0, true)
    game.hudText(x + 146, y + 56, TEXT.equipped, 16, A.r, A.g, A.b, a, 0, true)
end

local function drawHint(a)
    if a <= 0.01 then return end
    local x, y, h = 40, 1080 - 40 - 44, 44
    local w = keyLabel(x, y, h, OPEN_KEY:upper(), a)
    game.hudText(x + w + 12, y + 10, TEXT.bag, 22, 0.92, 0.93, 0.95, a, 0, true)
end

-- --- HUD: the bag itself --------------------------------------------------------------

local function button(x, y, w, h, label, keyName, primary, a, mx, my, danger)
    local hover = mx and mx >= x and mx < x + w and my >= y and my < y + h
    if primary then
        local k = hover and 1.08 or 1.0
        game.hudRect(x, y, w, h, math.min(1, A.r * k), math.min(1, A.g * k), math.min(1, A.b * k),
                     0.96 * a, 10)
    else
        game.hudRect(x, y, w, h, 1, 1, 1, (hover and 0.11 or 0.05) * a, 10)
        if hover and danger then
            game.hudFrame(x, y, w, h, 0.96, 0.46, 0.40, 0.85 * a, 1.5, 10)
        else
            game.hudFrame(x, y, w, h, 1, 1, 1, (hover and 0.40 or 0.22) * a, 1.5, 10)
        end
    end
    local s = h - 18
    local fs = 21
    local tw, th = textSize(label, fs, true)
    local total = s + 12 + tw
    local kx = x + (w - total) * 0.5
    keycap(kx, y + 9, s, keyName, a, not primary, primary)
    local c = primary and 0.08 or 0.95
    game.hudText(kx + s + 12, y + (h - th) * 0.5, label, fs, c, c, c, a, 0, true)
    return hover
end

local function drawSlot(it, x, y, s, hovered, selected, a)
    game.hudRect(x, y, s, s, 1, 1, 1, (hovered and 0.085 or 0.045) * a, 12)
    game.hudGradient(x + 10, y + 2, s - 20, s * 0.45, 1, 1, 1, 0.035 * a, 1, 1, 1, 0)
    if selected then
        game.hudRect(x, y, s, s, A.r, A.g, A.b, 0.10 * a, 12)
        game.hudFrame(x - 4, y - 4, s + 8, s + 8, A.r, A.g, A.b, 0.22 * a, 3, 15)
        game.hudFrame(x, y, s, s, A.r, A.g, A.b, 0.95 * a, 2, 12)
    else
        game.hudFrame(x, y, s, s, 1, 1, 1, (hovered and 0.26 or 0.10) * a, 1.5, 12)
    end
    if not it then return end
    local lift = selected and math.sin(_t * 2.2) * 2 or 0
    if not icon(it.icon, x + s * 0.5, y + s * 0.5 + lift, s - 30, s - 40, a, true) then
        noIcon(it.name, x + s * 0.5, y + s * 0.5, s * 0.45, a)
    end
    if it.count and it.count > 1 then
        game.hudText(x + s - 10, y + s - 32, "×" .. it.count, 20, 0.95, 0.95, 0.95, a, 1, true)
    end
    if it.id == _equipped then
        -- A corner of accent: this one is in the hand.
        game.hudTri(x + s - 34, y + 1, x + s - 1, y + 1, x + s - 1, y + 34, A.r, A.g, A.b, 0.95 * a)
    end
end

local function drawBag(self, W, a, mx, my, click)
    -- The world behind steps back.
    game.hudRect(0, 0, W, 1080, 0.012, 0.016, 0.024, 0.72 * a)
    game.hudGradient(0, 0, W, 300, 0, 0, 0, 0.35 * a, 0, 0, 0, 0)
    game.hudGradient(0, 780, W, 300, 0, 0, 0, 0, 0, 0, 0, 0.45 * a)

    local S, gap = 160, 14
    local gw = COLS * S + (COLS - 1) * gap
    local rows = math.max(1, math.ceil(SLOTS / COLS))
    local gh = rows * S + (rows - 1) * gap
    local dw = 600
    local PW = gw + 40 + dw
    local px = math.floor((W - PW) * 0.5)
    local py = math.floor((1080 - gh) * 0.5) + 10 + (1 - a) * 26

    -- Title, a short stroke of accent under it, and how full the bag is.
    spaced(px, py - 92, TEXT.title, 50, 0.97, 0.97, 0.98, a, 7, true)
    game.hudRect(px, py - 28, 64, 4, A.r, A.g, A.b, a, 2)
    local count = string.format(TEXT.places, #_items, SLOTS)
    game.hudText(px + PW, py - 58, count, 20, 0.70, 0.72, 0.76, a, 1, true)
    game.hudLine(px + 80, py - 26, px + PW, py - 26, 1, 1, 1, 0.08 * a, 1)

    -- The slots.
    local hoverSlot
    for i = 1, SLOTS do
        local c, r = (i - 1) % COLS, math.floor((i - 1) / COLS)
        local x, y = px + c * (S + gap), py + r * (S + gap)
        local hov = mx and mx >= x and mx < x + S and my >= y and my < y + S
        if hov then hoverSlot = i end
        drawSlot(_items[i], x, y, S, hov, i == _sel, a)
    end
    if click and hoverSlot and hoverSlot ~= _sel then
        _sel = hoverSlot
        sound(CLICK_SOUND)
    end

    -- The card: the chosen thing, large, with what can be done with it.
    local dx, dy, dh = px + gw + 40, py, gh
    game.hudRect(dx, dy, dw, dh, 0.065, 0.07, 0.085, 0.94 * a, 16)
    game.hudFrame(dx, dy, dw, dh, 1, 1, 1, 0.08 * a, 1.2, 16)
    local it = _items[_sel]
    if not it then
        local _, th = textSize(TEXT.empty, 30, true)
        game.hudText(dx + dw * 0.5, dy + dh * 0.5 - 40, TEXT.empty, 30, 0.80, 0.82, 0.86, a, 0.5, true)
        game.hudText(dx + dw * 0.5, dy + dh * 0.5 + 4, string.format(TEXT.emptyHint, PICKUP_KEY:upper()),
                     21, 0.58, 0.60, 0.65, a, 0.5)
        return
    end
    -- A warm light behind the thing, as if it lay on a lit table.
    local cx, cy = dx + dw * 0.5, dy + 124
    for k = 1, 9 do
        game.hudCircle(cx, cy, 230 - k * 22, A.r, A.g, A.b, 0.016 * a)
    end
    if not icon(it.icon, cx, cy + math.sin(_t * 1.6) * 3, dw - 150, 176, a, true) then
        noIcon(it.name, cx, cy, 120, a)
    end
    game.hudLine(dx + 30, dy + 252, dx + dw - 30, dy + 252, 1, 1, 1, 0.08 * a, 1)
    game.hudText(dx + 32, dy + 266, it.name, 38, 0.98, 0.98, 0.99, a, 0, true)
    if it.count and it.count > 1 then
        game.hudText(dx + dw - 32, dy + 274, "×" .. it.count, 28, 0.85, 0.86, 0.9, a, 1, true)
    end
    local cat = TEXT.category[it.category] or TEXT.category.misc
    local pw = pill(dx + 32, dy + 316, cat, a, A.r, A.g, A.b)
    if it.id == _equipped then pill(dx + 32 + pw + 10, dy + 316, TEXT.equipped, a, 0.45, 0.86, 0.58) end
    local ly = dy + 358
    for i, line in ipairs(wrap(it.description or "", 22, dw - 64)) do
        if i > 3 then break end
        game.hudText(dx + 32, ly, line, 22, 0.78, 0.80, 0.84, a)
        ly = ly + 28
    end

    -- What can be done with it.
    local bh, by = 50, dy + dh - 50 - 22
    local bx, bw = dx + 32, dw - 64
    local acted
    if isWeapon(it) then
        local half = (bw - 14) * 0.5
        local label = it.id == _equipped and TEXT.unequip or TEXT.equip
        if button(bx, by, half, bh, label, PICKUP_KEY:upper(), true, a, mx, my) and click then
            acted = "equip"
        end
        if button(bx + half + 14, by, half, bh, TEXT.drop, DROP_KEY:upper(), false, a, mx, my, true)
           and click then
            acted = "drop"
        end
    else
        if button(bx, by, bw, bh, TEXT.drop, DROP_KEY:upper(), false, a, mx, my, true) and click then
            acted = "drop"
        end
    end
    return acted
end

local function drawFooter(W, a)
    local h, y = 36, 1080 - 76 + (1 - a) * 10
    local parts = {
        {OPEN_KEY:upper(), TEXT.close}, {PICKUP_KEY:upper(), TEXT.equip},
        {DROP_KEY:upper(), TEXT.drop}, {TEXT.arrows, TEXT.choose},
    }
    local total = 0
    for _, p in ipairs(parts) do
        local kw = math.max(h, textSize(p[1], h * 0.46, true) + h * 0.6)
        total = total + kw + 10 + textSize(p[2], 18, true) + 36
    end
    local x = (W - total + 36) * 0.5
    for _, p in ipairs(parts) do
        x = x + keyLabel(x, y, h, p[1], a) + 10
        local tw = textSize(p[2], 18, true)
        game.hudText(x, y + 8, p[2], 18, 0.72, 0.74, 0.78, a, 0, true)
        x = x + tw + 36
    end
end

function update(self, dt, t)
    _t = _t + dt
    poll()
    local fig = figureApi(self)
    local W = game.hudSize()

    -- --- Open and close -------------------------------------------------------
    if _open then
        if pressed(key(OPEN_KEY)) or pressed(game.KEY_TAB) or pressed(game.KEY_ESCAPE) then
            setOpen(false)
        end
    elseif pressed(key(OPEN_KEY)) or pressed(game.KEY_TAB) then
        setOpen(true)
    end
    if _open then game.captureInput() end
    _openT = clamp(_openT + (_open and dt or -dt) / 0.18, 0, 1)

    -- --- Offer and pick up -------------------------------------------------------
    local offer, info = nil, nil
    if not _open and not _pick and not _stow and not (fig and fig.busy()) then
        offer, info = nearest(self)
    end
    if offer then _offer, _offerInfo = offer, info end
    _offerA = clamp(_offerA + (offer and dt or -dt) / 0.15, 0, 1)
    if _offerA <= 0 then _offer = nil end
    if offer and pressed(key(PICKUP_KEY)) and #_items < SLOTS then
        local x, y, z = game.getPos(offer)
        local rx, ry, rz = game.getRot(offer)
        info.rest = {rx, ry, rz}
        info.restX, info.restY, info.restZ = x, y, z
        local job = fig and fig.pickup(x, y, z)
        if job then
            _pick = {id = offer, info = info, job = job, attached = false}
        else
            -- No figure controller to bend down: straight into the bag.
            local hx, hy, hz = game.getScale(offer)
            _stow = {id = offer, info = info, t = 0, half = {hx, hy, hz}}
            sound(STOW_SOUND)
        end
        _offerA = 0
    end
    tickPickup(self, dt)
    tickTosses(dt)

    -- --- In the bag -------------------------------------------------------------
    local mx, my, over = game.mousePos()
    if not over then mx, my = nil, nil end
    local mouse = game.mouseDown(game.MOUSE_LEFT)
    local click = _open and mouse and not _mouseWas
    _mouseWas = mouse
    if _open then
        local moved = 0
        if pressed(game.KEY_LEFT) or pressed(game.KEY_A) then moved = -1 end
        if pressed(game.KEY_RIGHT) or pressed(game.KEY_D) then moved = 1 end
        if pressed(game.KEY_UP) or pressed(game.KEY_W) then moved = -COLS end
        if pressed(game.KEY_DOWN) or pressed(game.KEY_S) then moved = COLS end
        if moved ~= 0 then
            local n = _sel + moved
            if n >= 1 and n <= SLOTS then
                _sel = n
                sound(CLICK_SOUND)
            end
        end
    end

    -- --- Draw --------------------------------------------------------------------
    local openA = ease(_openT)
    drawPrompt(self, W, _offerA * (1 - openA))
    _equipA = clamp(_equipA + ((_equipped and not _open) and dt or -dt) / 0.2, 0, 1)
    drawEquipped(W, ease(_equipA))
    _hintT = _hintT + dt
    local hintA = (HINT_TIME > 0 and _hintT < HINT_TIME and not _open)
                  and (1 - ease((_hintT - HINT_TIME + 0.6) / 0.6)) or 0
    drawHint(hintA * (1 - openA))
    local acted
    if openA > 0 then
        acted = drawBag(self, W, openA, mx, my, click)
        drawFooter(W, openA)
    end
    drawToasts(W, dt)

    -- --- What was asked for in the bag ------------------------------------------
    if _open then
        local it = _items[_sel]
        if it and (acted == "equip" or pressed(key(PICKUP_KEY)) or pressed(game.KEY_ENTER))
           and isWeapon(it) then
            if _equipped == it.id then unequip() else equip(self, it) end
        elseif it and (acted == "drop" or pressed(key(DROP_KEY)) or pressed(game.KEY_DELETE)) then
            drop(self, _sel)
        end
    end
end
