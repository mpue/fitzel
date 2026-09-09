-- Sokoban -- a complete puzzle game, entirely in Lua.
--
-- Put this script on ONE object and press Play. An Empty is the right carrier:
-- the board is built around wherever that object stands, so anything visible
-- would end up standing in the middle of the level. Nothing else has to be in
-- the scene -- floor, walls, crates, goals and the player are all spawned here
-- and removed again when Play stops.
--
--   Arrows / WASD    walk; walking into a crate pushes it
--   Z / Backspace    undo one move (all the way back to the start of the level)
--   R                restart the level
--   Q / E            turn the view by 90 degrees (the controls turn with it)
--   N / P            skip to the next / previous level
--   Enter            next level, once this one is solved
--
-- The camera is driven every frame (scripts tick after the player movement, so
-- they win). Make sure no Camera entity in the scene is "active on start" --
-- that one would take the view back at the end of the frame.
--
-- All twelve levels were machine-checked for solvability before they went in.

-- --- Inspector parameters (globals = editable fields on the Script component) --
startLevel   = 1      -- which level Play starts on
cellSize     = 1.0    -- edge length of one grid cell, in metres
stepTime     = 0.11   -- seconds one step takes (the whole move animates)
repeatDelay  = 0.32   -- how long a held direction waits before it repeats
repeatRate   = 0.13   -- seconds between repeats while a direction is held
camPitch     = 52.0   -- camera angle above the board (90 = straight down)
camZoom      = 1.0    -- >1 pulls the camera back, <1 pushes it in
sound        = true   -- play the little cues (silently skipped if absent)
snapToGround = false  -- true: sit the board on the terrain instead of on the
                      -- carrier's own height

-- --- The levels ---------------------------------------------------------------
-- XSB notation:  # wall   . goal   $ crate   * crate on goal   @ player
--                + player on goal  (space) floor
local LEVELS = {
{ name = "First Push", map = {
    "#######",
    "#     #",
    "#  @  #",
    "#  $  #",
    "#     #",
    "#  .  #",
    "#     #",
    "#######" }},

{ name = "Around the Corner", map = {
    "#########",
    "#       #",
    "#  @$   #",
    "#       #",
    "#     . #",
    "#       #",
    "#########" }},

{ name = "The Vault", map = {
    "##########",
    "#        #",
    "#  ####  #",
    "#  #  #  #",
    "#  $..$  #",
    "#  #  #  #",
    "#  ####  #",
    "#   @    #",
    "##########" }},

{ name = "Two of a Kind", map = {
    "#########",
    "#       #",
    "#   @   #",
    "#  $ $  #",
    "#       #",
    "#  . .  #",
    "#       #",
    "#########" }},

{ name = "Sluice", map = {
    "##########",
    "#        #",
    "# $#$#$  #",
    "# . . .  #",
    "#        #",
    "#   @    #",
    "##########" }},

{ name = "Two Chambers", map = {
    "##########",
    "#   ##   #",
    "# $ ## $ #",
    "#   ##   #",
    "#        #",
    "#  .  .  #",
    "#   @    #",
    "##########" }},

{ name = "Carousel", map = {
    "##########",
    "#        #",
    "#  $  $  #",
    "#  .##.  #",
    "#  .##.  #",
    "#  $  $  #",
    "#   @    #",
    "##########" }},

{ name = "The Chamber", map = {
    "##########",
    "#        #",
    "#  $ $   #",
    "#  .#.   #",
    "#  $ $   #",
    "#  .#.   #",
    "#   @    #",
    "##########" }},

{ name = "Squeeze", map = {
    "##########",
    "#  #     #",
    "#  # $ $ #",
    "#  #  #  #",
    "#. .  #  #",
    "#     #  #",
    "#  @     #",
    "##########" }},

{ name = "The Hook", map = {
    "##########",
    "#    #   #",
    "# $  #   #",
    "#  ###   #",
    "#  .   $ #",
    "#  .###  #",
    "# $ .   @#",
    "##########" }},

{ name = "The Depot", map = {
    "###########",
    "#         #",
    "#  $ #  $ #",
    "#  . #  . #",
    "#  ### ## #",
    "#  $   $  #",
    "#  . @ .  #",
    "###########" }},

{ name = "The Cross", map = {
    "##########",
    "#        #",
    "#  $  $  #",
    "#   ..   #",
    "#   ..   #",
    "#  $  $  #",
    "#    @   #",
    "##########" }},
}

-- --- Colours ------------------------------------------------------------------
local COL_FLOOR  = {0.30, 0.32, 0.36}
local COL_GOAL   = {0.90, 0.66, 0.18}
local COL_WALL   = {0.52, 0.55, 0.60}
local COL_CRATE  = {0.72, 0.46, 0.20}
local COL_DONE   = {0.28, 0.74, 0.36}
local COL_BODY   = {0.24, 0.54, 0.95}
local COL_HEAD   = {0.95, 0.82, 0.60}

-- --- State --------------------------------------------------------------------
local board   = nil    -- { w, h, name, index, wall[r][c], goal[r][c], inside[r][c] }
local boxes   = {}     -- { c, r, id, x0, z0, x1, z1, onGoal }
local player  = { c = 0, r = 0, parts = {}, x0 = 0, z0 = 0, x1 = 0, z1 = 0 }
local spawned = {}     -- every entity this script created, for the next teardown

local cell, baseY, originX, originZ = 1.0, 0.0, 0.0, 0.0
local moving, animT, movingBox = false, 0.0, nil
local moves, pushes, totalMoves = 0, 0, 0
local undo    = {}
local solved, solvedAt, finished = false, 0.0, false
local viewK, viewYaw, viewYawWant = 0, 0.0, 0.0   -- view rotation in 90 deg steps
local held, holdTimer = nil, 0.0
local colorDirty, settleFrames = false, 0
local now, lastBump = 0.0, -1.0   -- clock, and when the last bump was heard
local cues = {}
local FOV = 42.0

-- --- Small helpers ------------------------------------------------------------

local function cue(name)
    if sound and cues[name] then game.playSound(name) end
end

local function gridToWorld(c, r)
    return originX + (c - (board.w - 1) * 0.5) * cell,
           originZ + (r - (board.h - 1) * 0.5) * cell
end

local function isWall(c, r)
    if not board then return true end
    if c < 0 or r < 0 or c >= board.w or r >= board.h then return true end
    return board.wall[r][c] == true
end

local function boxAt(c, r)
    for i, b in ipairs(boxes) do
        if b.c == c and b.r == r then return i, b end
    end
    return nil
end

local function isGoal(c, r)
    return board and board.goal[r] and board.goal[r][c] == true
end

local function boxesOnGoal()
    local n = 0
    for _, b in ipairs(boxes) do if isGoal(b.c, b.r) then n = n + 1 end end
    return n
end

-- --- Building a level ---------------------------------------------------------

local function clearLevel()
    for _, id in ipairs(spawned) do game.destroy(id) end
    spawned      = {}
    boxes        = {}
    player.parts = {}
    moving       = false
    movingBox    = nil
    undo         = {}
    moves, pushes = 0, 0
    solved       = false
end

local function tile(x, y, z, hx, hy, hz, col, name, kind)
    local id = game.spawn{
        type = kind or game.BOX,
        x = x, y = y, z = z,
        sx = hx, sy = hy, sz = hz,
        r = col[1], g = col[2], b = col[3],
        physics = game.PHYSICS_NONE,
        name = name,
    }
    spawned[#spawned + 1] = id
    return id
end

-- Everything the player can stand on, found by flooding out from the start.
-- Cells outside the level's walls stay empty, so a ragged map does not grow a
-- floor around its edges.
local function floodInside(startC, startR)
    local inside = {}
    for r = 0, board.h - 1 do inside[r] = {} end
    local stack = {{startC, startR}}
    inside[startR][startC] = true
    while #stack > 0 do
        local p = table.remove(stack)
        local c, r = p[1], p[2]
        local n = {{c + 1, r}, {c - 1, r}, {c, r + 1}, {c, r - 1}}
        for _, q in ipairs(n) do
            local qc, qr = q[1], q[2]
            if qc >= 0 and qr >= 0 and qc < board.w and qr < board.h
               and not board.wall[qr][qc] and not inside[qr][qc] then
                inside[qr][qc] = true
                stack[#stack + 1] = {qc, qr}
            end
        end
    end
    return inside
end

local function buildLevel(n)
    clearLevel()
    local L = LEVELS[n]
    board = { index = n, name = L.name, h = #L.map, w = 0,
              wall = {}, goal = {}, inside = {} }
    for _, row in ipairs(L.map) do
        if #row > board.w then board.w = #row end
    end

    local startC, startR = 0, 0
    local crates = {}
    for r = 0, board.h - 1 do
        board.wall[r], board.goal[r] = {}, {}
        local row = L.map[r + 1]
        for c = 0, board.w - 1 do
            local ch = c < #row and row:sub(c + 1, c + 1) or " "
            if ch == "#" then board.wall[r][c] = true end
            if ch == "." or ch == "*" or ch == "+" then board.goal[r][c] = true end
            if ch == "$" or ch == "*" then crates[#crates + 1] = {c, r} end
            if ch == "@" or ch == "+" then startC, startR = c, r end
        end
    end
    board.inside = floodInside(startC, startR)

    -- Floor, and a flat amber disc on the goal cells.
    local hy = 0.06
    for r = 0, board.h - 1 do
        for c = 0, board.w - 1 do
            if board.inside[r][c] then
                local x, z = gridToWorld(c, r)
                tile(x, baseY - hy, z, cell * 0.48, hy, cell * 0.48,
                     COL_FLOOR, "floor")
                if isGoal(c, r) then
                    tile(x, baseY + 0.015, z, cell * 0.30, 0.015, cell * 0.30,
                         COL_GOAL, "goal", game.CYLINDER)
                end
            end
        end
    end

    -- Walls.
    local wh = cell * 0.45
    for r = 0, board.h - 1 do
        for c = 0, board.w - 1 do
            if board.wall[r][c] then
                local x, z = gridToWorld(c, r)
                tile(x, baseY + wh, z, cell * 0.49, wh, cell * 0.49,
                     COL_WALL, "wall")
            end
        end
    end

    -- Crates.
    local bh = cell * 0.38
    for _, p in ipairs(crates) do
        local c, r = p[1], p[2]
        local x, z = gridToWorld(c, r)
        local id = tile(x, baseY + bh, z, bh, bh, bh, COL_CRATE, "crate")
        boxes[#boxes + 1] = { c = c, r = r, id = id,
                              x0 = x, z0 = z, x1 = x, z1 = z, onGoal = nil }
    end

    -- The player: a body and a head, moved together.
    local px, pz = gridToWorld(startC, startR)
    player.c, player.r = startC, startR
    player.x0, player.z0, player.x1, player.z1 = px, pz, px, pz
    player.parts = {
        { id = tile(px, baseY + cell * 0.28, pz, cell * 0.26, cell * 0.28,
                    cell * 0.26, COL_BODY, "player", game.CYLINDER),
          oy = cell * 0.28 },
        { id = tile(px, baseY + cell * 0.72, pz, cell * 0.20, cell * 0.20,
                    cell * 0.20, COL_HEAD, "player head", game.SPHERE),
          oy = cell * 0.72 },
    }

    -- Crate colours are applied one frame later: game.spawn defers creation
    -- to the end of this frame, so setColor on those ids would be a no-op now.
    colorDirty, settleFrames = true, 1
end

-- --- Moving -------------------------------------------------------------------

-- Screen directions in grid terms, for the current 90-degree view rotation.
-- At viewK 0 the camera sits on the +Z side, so "up" on screen is row - 1.
local function screenDirs()
    local a   = math.rad(viewK * 90.0)
    local sa  = math.floor(math.sin(a) + 0.5)
    local ca  = math.floor(math.cos(a) + 0.5)
    return { up    = { -sa, -ca },   -- {dc, dr}
             down  = {  sa,  ca },
             right = {  ca, -sa },
             left  = { -ca,  sa } }
end

local function startMove(dc, dr, pushIndex)
    local nx, nz = gridToWorld(player.c + dc, player.r + dr)
    player.x0, player.z0 = player.x1, player.z1
    player.x1, player.z1 = nx, nz
    player.c, player.r   = player.c + dc, player.r + dr

    movingBox = nil
    if pushIndex then
        local b = boxes[pushIndex]
        local bx, bz = gridToWorld(b.c + dc, b.r + dr)
        b.x0, b.z0 = b.x1, b.z1
        b.x1, b.z1 = bx, bz
        b.c, b.r   = b.c + dc, b.r + dr
        movingBox  = pushIndex
    end
    moving, animT = true, 0.0
end

-- A refused step says so once, not once per repeat while the key is held.
local function bump()
    if now - lastBump > 0.25 then
        lastBump = now
        cue("impact.wav")
    end
end

local function tryMove(dc, dr)
    local nc, nr = player.c + dc, player.r + dr
    if isWall(nc, nr) then bump(); return false end

    local bi = boxAt(nc, nr)
    if bi then
        local tc, tr = nc + dc, nr + dr
        if isWall(tc, tr) or boxAt(tc, tr) then bump(); return false end
        pushes = pushes + 1
    end

    undo[#undo + 1] = { dc = dc, dr = dr, box = bi }
    moves, totalMoves = moves + 1, totalMoves + 1
    startMove(dc, dr, bi)
    return true
end

local function undoMove()
    if #undo == 0 then return end
    local u = table.remove(undo)
    -- Walk the player back first, then pull the crate into the cell the player
    -- just left: that is exactly the push, run backwards.
    local b = u.box and boxes[u.box] or nil
    if b then
        local bx, bz = gridToWorld(b.c - u.dc, b.r - u.dr)
        b.x0, b.z0 = b.x1, b.z1
        b.x1, b.z1 = bx, bz
        b.c, b.r   = b.c - u.dc, b.r - u.dr
        pushes = math.max(0, pushes - 1)
    end
    local px, pz = gridToWorld(player.c - u.dc, player.r - u.dr)
    player.x0, player.z0 = player.x1, player.z1
    player.x1, player.z1 = px, pz
    player.c, player.r   = player.c - u.dc, player.r - u.dr
    movingBox = u.box
    moving, animT = true, 0.0
    moves  = math.max(0, moves - 1)
    solved = false
    cue("swoosh.wav")
end

-- Crates turn green the moment they sit on a goal. setColor gives the entity its
-- own material, so one crate never tints another.
local function recolour()
    for _, b in ipairs(boxes) do
        local on = isGoal(b.c, b.r)
        if b.onGoal ~= on then
            local col = on and COL_DONE or COL_CRATE
            game.setColor(b.id, col[1], col[2], col[3])
            if b.onGoal ~= nil and on then cue("plopp.wav") end
            b.onGoal = on
        end
    end
end

-- --- Level flow ---------------------------------------------------------------

local function goToLevel(n)
    if n < 1 then n = 1 end
    if n > #LEVELS then
        finished = true
        clearLevel()
        return
    end
    finished = false
    buildLevel(n)
end

local function checkSolved(t)
    if solved or moving then return end
    if #boxes > 0 and boxesOnGoal() == #boxes then
        solved, solvedAt = true, t
        game.addScore(1)
        cue("plopp.wav")
    end
end

-- --- Camera -------------------------------------------------------------------

local function driveCamera(dt)
    -- Swing to the wanted rotation instead of snapping, so a turn stays readable.
    local d = viewYawWant - viewYaw
    viewYaw = math.abs(d) < 0.5 and viewYawWant or viewYaw + d * math.min(1.0, dt * 9.0)

    local w = board and board.w or 8
    local h = board and board.h or 8
    local radius = 0.5 * math.max(w, h) * cell + cell
    local dist   = radius / math.tan(math.rad(FOV * 0.5)) * math.max(0.2, camZoom)
    local pitch  = math.rad(math.min(88.0, math.max(15.0, camPitch)))
    local a      = math.rad(viewYaw)

    local flat = dist * math.cos(pitch)
    local cx   = originX + math.sin(a) * flat
    local cy   = baseY   + dist * math.sin(pitch)
    local cz   = originZ + math.cos(a) * flat
    game.setCameraPos(cx, cy, cz)
    game.setCameraDir(originX - cx, baseY - cy, originZ - cz)
    game.setCameraFov(FOV)
end

-- --- Lifecycle ----------------------------------------------------------------

function start(e)
    cell     = math.max(0.2, cellSize)
    originX, originZ = e.x, e.z
    baseY    = snapToGround and (game.terrainHeight(e.x, e.z) + 0.02) or e.y
    viewK, viewYaw, viewYawWant = 0, 0.0, 0.0
    totalMoves = 0

    -- Only play cues whose file the asset database actually knows.
    for _, name in ipairs({"plopp.wav", "swoosh.wav", "impact.wav"}) do
        cues[name] = game.findAsset(name, "Sound") ~= nil
    end

    game.setCamera(-1)   -- take the view back from any active Camera component
    goToLevel(math.max(1, math.floor(startLevel)))
end

local DIRKEYS = {
    up    = { game.KEY_UP,    game.KEY_W },
    down  = { game.KEY_DOWN,  game.KEY_S },
    left  = { game.KEY_LEFT,  game.KEY_A },
    right = { game.KEY_RIGHT, game.KEY_D },
}
local DIRORDER = { "up", "down", "left", "right" }

local function anyPressed(keys)
    for _, k in ipairs(keys) do if game.keyPressed(k) then return true end end
    return false
end
local function anyDown(keys)
    for _, k in ipairs(keys) do if game.keyDown(k) then return true end end
    return false
end

function update(e, dt, t)
    now = t
    -- Level chrome first: these work in every state, including the end screen.
    if game.keyPressed(game.KEY_R) then
        if finished then
            totalMoves = 0
            goToLevel(1)
        elseif board then
            goToLevel(board.index)
        end
    end
    if game.keyPressed(game.KEY_N) and board then goToLevel(board.index + 1) end
    if game.keyPressed(game.KEY_P) and board then goToLevel(board.index - 1) end
    if game.keyPressed(game.KEY_Q) then viewK = (viewK + 3) % 4; viewYawWant = viewYawWant - 90.0 end
    if game.keyPressed(game.KEY_E) then viewK = (viewK + 1) % 4; viewYawWant = viewYawWant + 90.0 end

    driveCamera(dt)

    if finished then
        game.setHud(string.format(
            "SOKOBAN   all %d levels solved, %d moves in total.\n" ..
            "R: start over   Q/E: turn the view", #LEVELS, totalMoves))
        return
    end
    if not board then return end

    -- Walking. A fresh press steps at once; holding repeats after a short wait,
    -- so nudging a crate three cells does not need three separate presses.
    if not moving and not solved then
        local dirs = screenDirs()
        local want, fresh = nil, false
        for _, name in ipairs(DIRORDER) do
            if anyPressed(DIRKEYS[name]) then want, fresh = name, true; break end
        end
        if not want then
            for _, name in ipairs(DIRORDER) do
                if anyDown(DIRKEYS[name]) then want = name; break end
            end
        end

        if want == nil then
            held, holdTimer = nil, 0.0
        elseif fresh or held ~= want then
            held, holdTimer = want, repeatDelay
            tryMove(dirs[want][1], dirs[want][2])
        else
            holdTimer = holdTimer - dt
            if holdTimer <= 0.0 then
                holdTimer = repeatRate
                tryMove(dirs[want][1], dirs[want][2])
            end
        end

        if game.keyPressed(game.KEY_Z) or game.keyPressed(game.KEY_BACKSPACE) then
            undoMove()
        end
    end

    -- The step animation. Positions are written every frame; the grid state is
    -- already the state after the move, so nothing depends on the tween.
    if moving then
        animT = animT + dt / math.max(0.02, stepTime)
        local u = animT >= 1.0 and 1.0 or animT
        local s = u * u * (3.0 - 2.0 * u)             -- smoothstep
        local px = player.x0 + (player.x1 - player.x0) * s
        local pz = player.z0 + (player.z1 - player.z0) * s
        local hop = math.sin(u * math.pi) * cell * 0.06
        for _, p in ipairs(player.parts) do
            game.setPos(p.id, px, baseY + p.oy + hop, pz)
        end
        if movingBox then
            local b = boxes[movingBox]
            local bx = b.x0 + (b.x1 - b.x0) * s
            local bz = b.z0 + (b.z1 - b.z0) * s
            game.setPos(b.id, bx, baseY + cell * 0.38, bz)
        end
        if u >= 1.0 then
            moving, movingBox = false, nil
            colorDirty = true
        end
    end

    if settleFrames > 0 then
        settleFrames = settleFrames - 1
    elseif colorDirty then
        recolour()
        colorDirty = false
    end
    checkSolved(t)

    if solved then
        if game.keyPressed(game.KEY_ENTER) or (t - solvedAt) > 1.6 then
            goToLevel(board.index + 1)
            return
        end
    end

    local on = boxesOnGoal()
    local line1 = string.format("SOKOBAN   Level %d/%d   %s",
                                board.index, #LEVELS, board.name)
    local line2 = string.format("Crates %d/%d   Moves %d   Pushes %d",
                                on, #boxes, moves, pushes)
    local line3 = solved
        and "Solved!   Enter: on to the next one"
        or  "Arrows walk   Z undo   R restart   Q/E turn   N/P level"
    game.setHud(line1 .. "\n" .. line2 .. "\n" .. line3)
end
