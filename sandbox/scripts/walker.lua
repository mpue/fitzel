-- A figure you walk with the keyboard, switching an Animation Graph between Idle
-- and Walk.
--
--   W / Up      walk forward          A / Left    turn left
--   S / Down    walk backward         D / Right   turn right
--
-- WHAT TO SET UP FIRST. The script only moves the figure and tells the graph
-- whether it is walking; which animation that means lives in the graph.
--
--   1. The figure: a Model entity whose model brings its animations along (a
--      rigged .glb). No dynamic Physics on it -- this script places it every
--      frame and a physics body would fight it.
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
--      An Animation component on the figure may stay; in a state that names a
--      model animation, the graph poses the figure instead of it.
--
-- Then press Play. The camera follows behind the figure; while it does, the
-- walking player camera stands aside, so WASD moves only the figure.

-- Everything below without a leading underscore shows up in the Script
-- component's inspector and can be tuned there per figure.
PARAM          = "walking"   -- the graph's Bool parameter
WALK_SPEED     = 1.4         -- m/s; match it to the clip or the feet slide
BACK_FACTOR    = 0.5         -- walking backward is slower
TURN_SPEED     = 120         -- degrees per second
TURN_ANIMATES  = true        -- turning on the spot counts as walking
-- The model's own facing, in degrees. 0 = the figure looks along +Z; if it walks
-- backward (or sideways), try 180 (or 90 / -90).
FACE_OFFSET    = 0
FOLLOW_GROUND  = true        -- keep the figure's height above the terrain
FOLLOW_CAMERA  = true
CAM_DIST       = 4.0         -- metres behind the figure
CAM_HEIGHT     = 1.2         -- metres above the figure's centre
LOOK_HEIGHT    = 0.5         -- the point it looks at, above the centre
CAM_SMOOTH     = 6.0         -- higher = the camera sticks closer
SHOW_STATE     = true        -- graph state in the corner of the screen

local _yaw          -- where the figure is heading, without FACE_OFFSET
local _groundOffset -- the figure's height above the terrain where it started
local _cx, _cy, _cz -- the camera, smoothed

local function held(a, b)
    return game.keyDown(a) or game.keyDown(b)
end

function start(self, dt, t)
    _yaw = self.ry - FACE_OFFSET
    -- Whatever height the figure was placed at in the editor is the one it
    -- keeps, whether its origin is at the feet or in the middle of the model.
    _groundOffset = self.y - game.terrainHeight(self.x, self.z)
    _cx = nil
end

function update(self, dt, t)
    -- --- Input ---------------------------------------------------------------
    local move, turn = 0, 0
    if held(game.KEY_W, game.KEY_UP)    then move = move + 1 end
    if held(game.KEY_S, game.KEY_DOWN)  then move = move - BACK_FACTOR end
    if held(game.KEY_A, game.KEY_LEFT)  then turn = turn + 1 end
    if held(game.KEY_D, game.KEY_RIGHT) then turn = turn - 1 end

    -- --- Move ----------------------------------------------------------------
    _yaw = _yaw + turn * TURN_SPEED * dt
    local r = math.rad(_yaw)
    -- A yaw of 0 looks along +Z and a positive one turns toward +X, which is
    -- the figure's left -- so A adds and D subtracts.
    local fx, fz = math.sin(r), math.cos(r)
    self.x = self.x + fx * move * WALK_SPEED * dt
    self.z = self.z + fz * move * WALK_SPEED * dt
    self.ry = _yaw + FACE_OFFSET
    if FOLLOW_GROUND then
        self.y = game.terrainHeight(self.x, self.z) + _groundOffset
    end

    -- --- Tell the graph -----------------------------------------------------
    -- Every frame rather than only on a change: the machine resets its
    -- parameters to their defaults when it starts, and a value sent once before
    -- that would be lost.
    local walking = move ~= 0 or (TURN_ANIMATES and turn ~= 0)
    game.animBool(self.id, PARAM, walking)

    -- --- Camera --------------------------------------------------------------
    if FOLLOW_CAMERA then
        local tx = self.x - fx * CAM_DIST
        local ty = self.y + CAM_HEIGHT
        local tz = self.z - fz * CAM_DIST
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
        game.setCameraDir(self.x - _cx, self.y + LOOK_HEIGHT - _cy, self.z - _cz)
    end

    -- A graph that is not switching looks exactly like a graph that is not wired
    -- up; the state name tells the two apart.
    if SHOW_STATE then
        local state = game.animState(self.id)
        game.setHud(string.format("figure: %s   %s = %s", state or "no graph",
                                  PARAM, tostring(walking)))
    end
end
