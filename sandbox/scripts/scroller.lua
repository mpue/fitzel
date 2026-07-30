-- Vertical-scroller camera (top-down shoot'em-up style).
-- Put this on ANY single entity (an Empty works well), then press Play: the view
-- snaps to a high, steep top-down angle and scrolls forward at a constant speed,
-- like Raiden / 1942 / Xevious. The world appears to slide down the screen.
--
-- It drives the free camera every frame, so make sure NO Camera entity in the
-- scene is marked "active on start" -- that would take over the view instead.
--
-- Scroll runs along +Z by default (+Z reads as "up/into the screen"). To scroll
-- along X instead, swap the axes in update() (see the note there).

-- --- Tunables ---------------------------------------------------------------
local SPEED  = 12.0   -- scroll speed (world units / second)
local HEIGHT = 45.0   -- camera height above the scroll line
local TILT   = 6.0    -- how far the camera trails behind its focus point.
                      -- Smaller = closer to straight-down; keep it > 0, because
                      -- an exactly vertical view gimbal-locks a yaw/pitch camera.
local FOV    = 35.0   -- narrower FOV looks flatter / more "2D" from up high
local START  = 0.0    -- where the scroll begins along the axis

-- --- State ------------------------------------------------------------------
local d = 0.0         -- distance scrolled so far

function start(e)
    d = START
    game.setCameraFov(FOV)
end

function update(e, dt, t)
    d = d + SPEED * dt
    -- Camera sits HEIGHT above and TILT behind the focus point (0, 0, d),
    -- looking forward-and-down at it. Direction = focus - camPos (the host
    -- normalises it).
    game.setCameraPos(0.0, HEIGHT, d - TILT)
    game.setCameraDir(0.0, -HEIGHT, TILT)

    -- To scroll along X instead, use these two lines instead of the pair above:
    --   game.setCameraPos(d - TILT, HEIGHT, 0.0)
    --   game.setCameraDir(TILT, -HEIGHT, 0.0)
end
