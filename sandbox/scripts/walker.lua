-- A figure you walk with the keyboard, switching an Animation Graph between Idle
-- and Walk.
--
--   W / Up      walk forward          A / Left    turn left
--   S / Down    walk backward         D / Right   turn right
--
-- or, with CAMERA_RELATIVE on (third person, for a camera orbited with the
-- mouse): W walks away from the camera, S towards it, A / D across it, and the
-- figure turns to face the way it walks.
--
-- WHAT TO SET UP FIRST. The script only moves the figure and tells the graph
-- whether it is walking; which animation that means lives in the graph.
--
--   1. The figure: a Model entity whose model brings its animations along (a
--      rigged .glb). No dynamic Physics on it -- this script places it every
--      frame and a physics body would fight it. A parent is fine as long as it
--      is only moved, not turned or scaled.
--   2. On the figure: Add Component -> Animation Graph, pick the graph. Then
--      Add Component -> Script and choose this file (copy it into the
--      project's scripts/ folder first).
--   3. Animation graph panel, with the figure selected (that is where the
--      "Model animation" list gets the model's clips from): two states and
--      two arrows --
--         Idle   Model animation = the model's idle   -- entry state
--         Walk   Model animation = the model's walk
--         Idle -> Walk   when `walking` is true,  Fade 0.25 s
--         Walk -> Idle   when `walking` is false, Fade 0.25 s
--      Parameter: `walking`, Bool. A model that only brings a walk: give Idle
--      the walk as well, at Speed 0 -- it holds the walk's first frame.
--      Backing up (S) looks like a moonwalk unless the walk plays backward:
--      add a Number parameter `walkSpeed` ("Start it at 1") and pick it as the
--      Walk state's "Speed times". The script then sets it to the pace the
--      figure really goes -- 1 forward, -0.5 backing up at half speed.
--      An Animation component on the figure may stay; in a state that names a
--      model animation, the graph poses the figure instead of it.
--
-- Then press Play. The camera follows behind the figure; while it does, the
-- walking player camera stands aside, so WASD moves only the figure.
--
-- THIRD PERSON. Hang a Camera on the figure as its child: Mode "Follow parent",
-- "Orbit with mouse" and "Active on start" on, and place it where the view
-- should open (a few metres behind, a little up). Here, switch FOLLOW_CAMERA off
-- and CAMERA_RELATIVE on. In Play the mouse swings the camera round the figure
-- and the keys walk it relative to the view. Esc leaves Play.

-- Everything below without a leading underscore shows up in the Script
-- component's inspector and can be tuned there per figure.
PARAM          = "walking"   -- the graph's Bool parameter
SPEED_PARAM    = "walkSpeed" -- the graph's Number the walk's pace is multiplied by
WALK_SPEED     = 1.4         -- m/s; match it to the clip or the feet slide
BACK_FACTOR    = 0.5         -- walking backward is slower
TURN_SPEED     = 120         -- degrees per second
TURN_ANIMATES  = true        -- turning on the spot counts as walking
-- Third person: walk relative to the camera, facing the way you walk. Pair it
-- with an orbiting Camera and FOLLOW_CAMERA off (see above).
CAMERA_RELATIVE = false
FACE_SPEED     = 540         -- degrees per second the figure turns to face its way
-- The model's own facing, in degrees. 0 = the figure looks along +Z; if it walks
-- backward (or sideways), try 180 (or 90 / -90).
FACE_OFFSET    = 0
FOLLOW_GROUND  = true        -- keep the figure's height above the terrain
-- Walk through the physics world (game.moveCharacter): stand on roads and
-- bridges, stop at walls, step up kerbs, fall off edges. Off = follow the
-- terrain only, through everything else.
COLLIDE        = true
FOLLOW_CAMERA  = true
CAM_DIST       = 4.0         -- metres behind the figure
CAM_HEIGHT     = 1.2         -- metres above the figure's centre
LOOK_HEIGHT    = 0.5         -- the point it looks at, above the centre
CAM_SMOOTH     = 6.0         -- higher = the camera sticks closer
SHOW_STATE     = true        -- graph state in the corner of the screen

local _yaw          -- where the figure is heading, without FACE_OFFSET
local _groundOffset -- the figure's height above the terrain where it started
local _cx, _cy, _cz -- the camera, smoothed
local _pace = 1     -- the walk's pace while it last walked (see update)
local _footOffset   -- the figure's origin above its feet, as placed (COLLIDE)

local function held(a, b)
    return game.keyDown(a) or game.keyDown(b)
end

-- self.x/y/z are the figure's LOCAL position -- relative to its parent -- while
-- the terrain and the camera live in the world. Last frame's world position
-- minus this frame's local one (neither has moved yet) is where the parent puts
-- it: nothing for a root figure, the parent's position for a parented one.
local function parentOffset(self)
    local wx, wy, wz = game.getPos(self.id)
    if not wx then return 0, 0, 0 end
    return wx - self.x, wy - self.y, wz - self.z
end

function start(self, dt, t)
    _yaw = self.ry - FACE_OFFSET
    -- Whatever height the figure was placed at in the editor is the one it
    -- keeps, whether its origin is at the feet or in the middle of the model.
    local ox, _, oz = parentOffset(self)
    _groundOffset = self.y - game.terrainHeight(self.x + ox, self.z + oz)
    _cx = nil
    _pace = 1
    _footOffset = nil
end

-- The keys as a walk: along the heading and turning (tank), or relative to the
-- camera and facing the way it goes (third person). Returns the step on the
-- ground as x, z (a unit direction times how fast, 0..1), whether it counts as
-- walking, and the pace the walk should play at (negative = backward).
local function tankWalk(dt)
    local move, turn = 0, 0
    if held(game.KEY_W, game.KEY_UP)    then move = move + 1 end
    if held(game.KEY_S, game.KEY_DOWN)  then move = move - BACK_FACTOR end
    if held(game.KEY_A, game.KEY_LEFT)  then turn = turn + 1 end
    if held(game.KEY_D, game.KEY_RIGHT) then turn = turn - 1 end
    -- A yaw of 0 looks along +Z and a positive one turns toward +X, which is
    -- the figure's left -- so A adds and D subtracts.
    _yaw = _yaw + turn * TURN_SPEED * dt
    local r = math.rad(_yaw)
    -- The legs go the way and the pace the figure does; turning on the spot
    -- steps forward.
    return math.sin(r) * move, math.cos(r) * move,
           move ~= 0 or (TURN_ANIMATES and turn ~= 0),
           move ~= 0 and move or 1
end

local function cameraWalk(dt)
    local ahead, side = 0, 0
    if held(game.KEY_W, game.KEY_UP)    then ahead = ahead + 1 end
    if held(game.KEY_S, game.KEY_DOWN)  then ahead = ahead - 1 end
    if held(game.KEY_A, game.KEY_LEFT)  then side = side - 1 end
    if held(game.KEY_D, game.KEY_RIGHT) then side = side + 1 end
    -- The camera's forward, flattened onto the ground. Looking straight down
    -- leaves no forward, so the figure's own heading stands in.
    local fx, _, fz = game.cameraDir()
    local len = math.sqrt(fx * fx + fz * fz)
    if len > 1e-4 then
        fx, fz = fx / len, fz / len
    else
        fx, fz = math.sin(math.rad(_yaw)), math.cos(math.rad(_yaw))
    end
    -- Its right on the ground is forward x up: (-fz, fx).
    local mx = fx * ahead - fz * side
    local mz = fz * ahead + fx * side
    local m = math.sqrt(mx * mx + mz * mz)
    if m < 1e-4 then return 0, 0, false, 1 end
    mx, mz = mx / m, mz / m
    -- Face the way it walks, turning the short way round and no faster than
    -- FACE_SPEED -- a snap would read as a glitch, a slow turn as sliding.
    local want = math.deg(math.atan(mx, mz))
    local diff = (want - _yaw + 180) % 360 - 180
    local step = FACE_SPEED * dt
    _yaw = _yaw + math.max(-step, math.min(step, diff))
    return mx, mz, true, 1      -- it always faces its way: always forward
end

function update(self, dt, t)
    local ox, oy, oz = parentOffset(self)   -- before the figure moves

    -- --- Walk ----------------------------------------------------------------
    local dx, dz, walking, pace
    if CAMERA_RELATIVE then
        dx, dz, walking, pace = cameraWalk(dt)
    else
        dx, dz, walking, pace = tankWalk(dt)
    end
    -- Through the physics world when it can: a capsule that stands on roads and
    -- bridges, stops at walls, steps up kerbs and falls off edges. Standing on
    -- the terrain, the feet go on the terrain as it is DRAWN (the physics only
    -- samples it every 4 m); on anything else, on what the capsule stands on.
    local vx, vz = dx * WALK_SPEED, dz * WALK_SPEED
    local fx, fy, fz, ground, terrain
    if COLLIDE and game.moveCharacter then
        fx, fy, fz, ground, terrain = game.moveCharacter(self.id, vx, vz, dt)
    end
    if fx then
        if terrain then fy = game.terrainHeight(fx, fz) end
        if not _footOffset then _footOffset = self.y + oy - fy end
        -- Held up by a wall, the legs stop too: walking on the spot reads as a
        -- glitch, standing at the wall as the wall.
        local moved = math.sqrt((fx - ox - self.x) ^ 2 + (fz - oz - self.z) ^ 2)
        if (vx ~= 0 or vz ~= 0) and moved < 0.2 * WALK_SPEED * dt then walking = false end
        self.x, self.y, self.z = fx - ox, fy + _footOffset - oy, fz - oz
    else
        self.x = self.x + vx * dt
        self.z = self.z + vz * dt
        if FOLLOW_GROUND then
            self.y = game.terrainHeight(self.x + ox, self.z + oz) + _groundOffset
        end
    end
    self.ry = _yaw + FACE_OFFSET

    -- --- Tell the graph -----------------------------------------------------
    -- Every frame rather than only on a change: the machine resets its
    -- parameters to their defaults when it starts, and a value sent once before
    -- that would be lost.
    game.animBool(self.id, PARAM, walking)
    -- Standing still keeps the last pace: the walk fading out after backing up
    -- must go on backing up, not flick forward for its last quarter second.
    -- (A graph without this parameter simply ignores it.)
    if walking then _pace = pace end
    if SPEED_PARAM ~= "" then game.animNumber(self.id, SPEED_PARAM, _pace) end

    -- --- Camera --------------------------------------------------------------
    if FOLLOW_CAMERA then
        -- In the world, where the camera lives: the figure plus its parent.
        local wx, wy, wz = self.x + ox, self.y + oy, self.z + oz
        local r = math.rad(_yaw)
        local fx, fz = math.sin(r), math.cos(r)
        local tx = wx - fx * CAM_DIST
        local ty = wy + CAM_HEIGHT
        local tz = wz - fz * CAM_DIST
        if not _cx then
            _cx, _cy, _cz = tx, ty, tz            -- first frame: no swoop in
        else
            -- Frame-rate independent easing toward the spot behind the figure.
            local k = 1 - math.exp(-CAM_SMOOTH * dt)
            _cx = _cx + (tx - _cx) * k
            _cy = _cy + (ty - _cy) * k
            _cz = _cz + (tz - _cz) * k
        end
        game.setCameraPos(_cx, _cy, _cz)
        game.setCameraDir(wx - _cx, wy + LOOK_HEIGHT - _cy, wz - _cz)
    end

    -- A graph that is not switching looks exactly like a graph that is not wired
    -- up; the state name tells the two apart.
    if SHOW_STATE then
        local state = game.animState(self.id)
        game.setHud(string.format("figure: %s   %s = %s", state or "no graph",
                                  PARAM, tostring(walking)))
    end
end