#pragma once

#include <glm/glm.hpp>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace fitzel { class Input; class TerrainStreamer; }
class Document;
class RoadSystem;
class BoostPadComponent;
struct Entity;

// The arcade racing simulation: the fixed-timestep drive tick for the arcade
// car and the Wipeout-style glider, plus the AI opponents that lap the road.
// Extracted from main's game loop so the sim lives in one place instead of
// bloating main.cpp. The Jolt physics-car/boat path stays in main (it is wound
// through the water/spray FX there); this module is the pure-arcade sim.
namespace racesim {

// Persistent per-craft/race state. main owns one instance and aliases its
// fields as references (so the rest of the loop reads them by their old names),
// and passes it to the update entry points below. Fields default to the same
// values the loop-local declarations used.
struct RaceState {
    // --- Arcade car (bicycle model) -------------------------------------
    glm::vec3 carPos{0.0f};
    float carYaw     = 0.0f;   // heading (radians)
    float carSpeed   = 0.0f;   // m/s (negative = reverse)
    float wheelSpin  = 0.0f;   // rolling angle (radians)
    float steerAngle = 0.0f;   // front-wheel steer (radians)

    // --- Fixed-timestep clock -------------------------------------------
    float simAccum = 0.0f;     // fixed-timestep accumulator

    // The pose one fixed step ago, for the render interpolation -- and it has to
    // PERSIST ACROSS FRAMES, which is the whole point of keeping it here rather
    // than in a local.
    //
    // At 120 Hz there are plenty of frames in which no step falls at all. Taken
    // fresh each frame, the "previous" pose is then the current one, the craft
    // renders at the far end of its last step and stands still for that frame --
    // while everything drawn on the render clock (the chase camera) keeps
    // moving. Relative to the camera the craft slides backwards, once per
    // skipped step, which is exactly what it looks like.
    glm::vec3 prevPos{0.0f};
    float     prevYaw = 0.0f, prevBank = 0.0f, prevPitch = 0.0f;
    float     prevSteer = 0.0f, prevSpin = 0.0f;   // car only
    bool      prevValid = false;   // false = nothing to interpolate from yet

    // --- Glider (hover racer) -------------------------------------------
    glm::vec3 gliderPos{0.0f}; // body-centre world position
    float gliderYaw   = 0.0f;  // heading (radians)
    // How fast the heading is CHANGING (rad/s). State, not a derived value: the
    // stick asks for a yaw rate and this chases it (see GliderComponent::
    // steerResponse), which is where the craft's rotational inertia lives -- and
    // what the bank angle is read off, so the craft leans because it is turning
    // rather than because a button is held.
    float gliderYawRate = 0.0f;
    glm::vec3 gliderVel{0.0f}; // world-space velocity (m/s)
    float gliderBank  = 0.0f;  // smoothed roll (deg)
    float gliderPitch = 0.0f;  // smoothed pitch (deg)
    float gliderOverspeed = 0.0f; // speed cap above maxSpeed from a boost pad
    float gliderBoostHold = 1.5f; // linger time of the last pad's boost (s)
    bool  gliderBoosting  = false; // on/just-left a boost pad (HUD flash)
    bool  gliderWasOnPad  = false; // last frame's pad contact (entry punch edge)

    // --- Riding a vertical loop ------------------------------------------
    // While on a loop the craft leaves the hover model entirely: it is carried
    // ALONG the loop's frames with gravity resolved against the surface, because
    // hovering over gliderGround(x,z) has no meaning on a surface that is
    // vertical (and then inverted). `loopIndex` < 0 means the normal flight sim
    // is in charge.
    int   loopIndex = -1;   // which loop of road.loopGeometry(), -1 = none
    float loopArc   = 0.0f; // metres travelled around it
    float loopSide  = 0.0f; // offset across the carriageway (m)
    float loopSpeed = 0.0f; // speed along the loop (m/s)
    // The craft's orientation while on a loop, for the render and the camera --
    // a Euler yaw cannot describe being upside down at the top of a turn.
    glm::vec3 loopFwd{0.0f, 0.0f, 1.0f}, loopUp{0.0f, 1.0f, 0.0f};
    float loopExitFlash = 0.0f; // counts down after being thrown off (HUD/FX)

    // --- Manual boost (gamepad A / Left Shift) ---------------------------
    // A tank the pilot spends, as opposed to the boost PADS above, which the
    // track hands out. Refilled to full at every race start (the countdown), so
    // nobody lines up on the grid with an empty one.
    float boostCharge   = 100.0f;  // what is left in the tank
    float boostCapacity = 100.0f;  // what a full one holds (mirrored for the HUD)
    int   boostSegments = 10;      // HUD bar divisions, derived from the capacity
    bool  boostActive   = false;   // held AND actually spending (HUD + speed FX)
    // The button's own state last step, which is NOT the same thing as
    // boostActive: the ignition sound hangs off the pilot pressing, so that a
    // tank trickling back over zero under a held button re-ignites in silence
    // instead of thudding once every boostDelay.
    bool  boostWasHeld  = false;
    float boostIdle     = 0.0f;    // seconds since it last spent anything
    float boostDryFlash = 0.0f;    // counts down after running the tank dry (HUD)
    // --- Energy (the hull) -----------------------------------------------
    // Collisions cost energy; it recharges on its own after a pause; at zero the
    // run is over (energyOut) and the HUD asks the end-of-race question. Refilled
    // at every race start, exactly like the boost tank.
    float energy         = 100.0f; // what is left of the hull
    float energyCapacity = 100.0f; // what a full one holds (mirrored for the HUD)
    float energyIdle     = 0.0f;   // seconds since the last hit (arms the regen)
    float energyHitFlash = 0.0f;   // counts down after a hit (HUD flash)
    float energyLastHit  = 0.0f;   // energy the last hit cost (HUD)
    float energyWarnT    = 0.0f;   // counts down to the alarm's next repeat
    bool  energyLow      = false;  // under the warning threshold (HUD pulse)
    bool  energyOut      = false;  // hull gone: the run is lost

    // --- The player's margin for error (see Difficulty.hpp) ------------------
    // Set once as a race starts, per seat, and 1 everywhere means the craft
    // exactly as its author tuned it. This is the half of the difficulty ladder
    // that is about the player rather than the field: how hard a hit bites, how
    // fast the hull comes back, how much boost is handed out. It lives on the
    // state and not in RaceEnv because applyDamage() is called from outside the
    // sim (a warhead landing), and a hit has to cost the same wherever it came
    // from.
    float damageScale = 1.0f;
    float healScale   = 1.0f;
    float boostScale  = 1.0f;

    // Off-track rescue: the last spot the glider was safely on the road, to pop
    // it back onto after it falls off (e.g. off a bridge).
    // How long the craft has been judged OFF the track, in seconds. The rescue
    // waits for this rather than firing on the first frame that says so, because
    // "am I on the road" is a query that can come back no for a single frame
    // while the craft is flying perfectly normally -- over a seam between two
    // ribbon sections, a bridge joint, the mouth of a tunnel -- and a rescue
    // fired on one such frame is a teleport backwards to the last breadcrumb in
    // the middle of a clean lap. Falling off a track takes far longer than the
    // grace below, so nothing that should be rescued stops being rescued.
    float     offTrackT = 0.0f;
    glm::vec3 respawnPos{0.0f};
    float     respawnYaw  = 0.0f;
    bool      haveRespawn = false;

    // --- Race / lap timing ----------------------------------------------
    bool  raceActive = false, raceFinished = false, raceHasLine = false;
    float raceClock = 0.0f, lapClock = 0.0f, lastLap = 0.0f, bestLap = 0.0f;
    int   raceLap = 0, raceLaps = 0;   // completed laps / target
    bool  finishWasOver = false;       // edge-detect the line crossing
    float finishArm = 0.0f;            // re-arm guard so one pass counts once (s)
    // Has the line been crossed to BEGIN lap 1? A countdown start puts the craft
    // on the grid, which stands behind the line, so the first pass opens the lap
    // instead of closing one -- without this it reads as a lap flown without a
    // single checkpoint. False only between GO and that first pass.
    bool  lapBegun = true;
    std::unordered_set<int> cpPassed;  // checkpoint entity ids passed this lap
    int   cpTotal = 0;                 // checkpoints in the scene (for the HUD)
    float raceMissedFlash = 0.0f;      // HUD flash after finishing a lap short
    float raceCountdown = 0.0f;        // Ready/Set/Go: > 0 holds everyone still
    float goFlash       = 0.0f;        // "GO!" flash once the countdown hits 0
    int   cdPhase       = -1;          // countdown step already announced (0/1/2, -1 = none)
    // The held moment on the grid, before the countdown. The field stands exactly
    // as the countdown holds it, but nothing is counting: the camera circles the
    // player's craft and the race waits to be asked for. Only a launch from the
    // start screen opens it -- a restart is a second run of something already
    // introduced, and a time trial has no field to look at.
    bool  onGrid   = false;
    float gridTime = 0.0f;             // seconds held there, which is the orbit angle

    // --- Standings (the HUD's participant list) --------------------------
    // Rebuilt every frame by updateOpponents from the opponents' race state plus
    // the human racer, ordered leader-first. Empty when there is no road (nothing
    // to measure progress along), in which case the HUD falls back to plain lap
    // times.
    struct Standing {
        int         id = -1;          // entity id (-1 = the human racer)
        std::string name;
        int   lap       = 0;          // completed laps
        float progress  = 0.0f;       // laps * lapLength + metres into this lap
        float gap       = 0.0f;       // metres behind the leader (0 = leading)
        float bestLap   = 0.0f, lastLap = 0.0f;
        float totalTime = 0.0f;       // final time once finished
        bool  finished  = false;
        bool  isPlayer  = false;
    };
    std::vector<Standing> standings;   // leader first; [i].place == i + 1
    int   playerPlace = 0;             // 1-based, 0 = not in the field
    bool  raceOver    = false;         // every racer has taken the flag
    std::string winnerName;            // first over the last lap's line
    // Who that was, as an identity rather than a name: -2 = nobody yet, -1 =
    // player one, otherwise the winning craft's entity id. Split screen needs it
    // -- "did I win" has a different answer per seat, and answering it by
    // comparing finish times or names would be guessing.
    int   winnerId       = -2;
    bool  winnerIsPlayer = false;
    float winnerTime     = 0.0f;
    bool  oppWasActive   = false;      // edge-detect the race start (re-seeds the field)

    // --- Engine-sound feed (car), consumed in the audio mix -------------
    bool  engineDriving  = false;
    float engineSpeedMps = 0.0f;
    float engineThrottle = 0.0f;
    float engineWheelR   = 0.42f;

    // --- Glider jet-sound feed ------------------------------------------
    bool  gliderAudioActive = false;
    float gliderSpeedMps    = 0.0f;
    float gliderThrottle    = 0.0f;
    float gliderTopSpeed    = 60.0f;

    // --- Radial speed-blur anchor (read by the render) ------------------
    glm::vec3 blurAnchorWorld{0.0f};
    bool  blurAnchorValid = false;
    float blurSpeed01     = 0.0f; // craft speed 0..~1.4 -> radial streak length
};

// Everything the sim reads from the surrounding loop: engine subsystems by
// reference, the frame's timing, the driven-entity ids, and the four scene-graph
// / FX helpers main owns as lambdas (passed as callbacks so this module needs no
// access to main's captured state).
// One player's controls for this frame, already resolved from whatever device
// drives them.
//
// The sim used to read the keyboard and the pad itself, which quietly assumed
// there was exactly one player: two craft would have answered to the same W and
// the same stick. Resolving the device OUTSIDE means split screen is a matter of
// filling this twice -- pad for one, keys for the other -- and the flight model
// never learns that more than one player exists.
struct RaceControls {
    float throttle = 0.0f;  // -1 = reverse .. +1 = full ahead
    float steer    = 0.0f;  // -1 = left .. +1 = right (before invertSteer)
    bool  brake    = false;
    bool  boost    = false; // held, not tapped
};

struct RaceEnv {
    fitzel::Input&    input;
    // No camera. The sim moves the CRAFT and nothing else; where anyone watches
    // it from is the business of that craft's own camera entity (CameraSystem).
    // It used to take a fitzel::Camera& and set its position and angles from
    // inside the flight model, which quietly meant there could only ever be one
    // eye in the world -- the same shape of mistake as reading the keyboard in
    // here, and the reason a second view had to be threaded through everything.
    // What this player is asking the craft to do. Filled by the caller; the sim
    // does not touch the keyboard for flight control.
    RaceControls      controls;
    Document&         document;
    std::vector<Entity>& entities;
    fitzel::TerrainStreamer& streamer;
    RoadSystem&              road;
    int   driveVehicleId;
    int   driveGliderId;
    // Split screen's second craft, or -1. Not a thing the flight model reads:
    // it is here so the AI knows which craft is spoken for. A craft flown by
    // player two may well carry an Opponent component (that is how a two-craft
    // track is usually authored), and one steered by a player AND by the spline
    // at the same time fights itself.
    int   playerGliderId2 = -1;
    const std::vector<Entity>& driveBackup;
    float dt;        // real (clamped) frame time
    float kSimH;     // fixed sim step (s)
    float simAlpha;  // render interpolation blend in [0,1)
    int   simSteps;  // fixed steps taken this frame

    // The human racer, for opponent rubber-band + overtaking. playerActive is
    // false when no craft is being driven (e.g. plain editing) -> opponents then
    // only race each other.
    glm::vec3 playerPos{0.0f};
    float     playerSpeed = 0.0f;
    bool      playerActive = false;

    std::function<void(Entity&, const glm::vec3&, const glm::vec3&, const glm::mat4*)> setWorld;
    std::function<glm::mat4(const Entity&)>       parentWorldMat;
    // Ground under (x, z) below `yMax`, IGNORING entity `ignoreId` and its
    // children. The ignore is a parameter rather than something the callback
    // knows by itself: a craft must not stand on its own model, and with two
    // players there are two different craft that must not -- a query that knew
    // only about player one's had player two hovering on top of itself, climbing
    // by its own height every frame, and taking the other craft with it once its
    // roof passed overhead.
    std::function<float(float, float, float, int)>  gliderGround;
    std::function<void(const BoostPadComponent&)>  playBoostPunch;
    // One-shot race cue by Sound-asset filename, with gain and pitch: the
    // Ready/Set/Go samples off the start/finish line and a checkpoint's gate
    // sound. Generic on purpose -- the sim shouldn't grow a callback per SFX.
    std::function<void(const std::string&, float, float)> playCue;
};

// Arcade car: fixed-step bicycle-model integration + interpolated chase cam.
// Runs when the driven craft is a car (not the Jolt physics vehicle).
void updateArcadeCar(RaceState& st, const RaceEnv& env);

// Glider hover racer: fixed-step flight sim, boost pads, gate/checkpoint/lap
// logic, hover spring, banked attitude, interpolated chase cam.
void updateGlider(RaceState& st, const RaceEnv& env);

// Charge a hull for damage that did not come from flying into something -- a
// missile. Same bookkeeping a crash does: the recharge pause starts over and the
// HUD flashes, so where the damage came from does not change how it reads on the
// bar. The run ending at zero is left to the sim's own step, which is the single
// place that decides it.
void applyDamage(RaceState& st, float dmg);

// AI opponents: kinematic racers that lap the road centreline, slowing for
// corners (corner speed = sqrt(grip / curvature)) and banking into them.
//
// This also builds the STANDINGS, which is why split screen reaches in here:
// `st2`, when given, is the second player's state. It joins the field as a
// racer, and gets the finished order written back into it -- with the two
// players' roles swapped, so each one reads "YOU" in its own pane and sees the
// other by name. Null for a single-player race, and then nothing changes.
void updateOpponents(RaceState& st, const RaceEnv& env, RaceState* st2 = nullptr);

} // namespace racesim
