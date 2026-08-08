#pragma once

#include <glm/glm.hpp>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace fitzel { class Input; class Camera; class TerrainStreamer; }
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

    // --- Chase camera + fixed-timestep clock ----------------------------
    glm::vec3 camChase{0.0f};  // smoothed chase-camera position
    float simAccum = 0.0f;     // fixed-timestep accumulator

    // --- Glider (hover racer) -------------------------------------------
    glm::vec3 gliderPos{0.0f}; // body-centre world position
    float gliderYaw   = 0.0f;  // heading (radians)
    glm::vec3 gliderVel{0.0f}; // world-space velocity (m/s)
    float gliderBank  = 0.0f;  // smoothed roll (deg)
    float gliderPitch = 0.0f;  // smoothed pitch (deg)
    float gliderOverspeed = 0.0f; // speed cap above maxSpeed from a boost pad
    float gliderBoostHold = 1.5f; // linger time of the last pad's boost (s)
    bool  gliderBoosting  = false; // on/just-left a boost pad (HUD flash)
    bool  gliderWasOnPad  = false; // last frame's pad contact (entry punch edge)
    // Off-track rescue: the last spot the glider was safely on the road, to pop
    // it back onto after it falls off (e.g. off a bridge).
    glm::vec3 respawnPos{0.0f};
    float     respawnYaw  = 0.0f;
    bool      haveRespawn = false;

    // --- Race / lap timing ----------------------------------------------
    bool  raceActive = false, raceFinished = false, raceHasLine = false;
    float raceClock = 0.0f, lapClock = 0.0f, lastLap = 0.0f, bestLap = 0.0f;
    int   raceLap = 0, raceLaps = 0;   // completed laps / target
    bool  finishWasOver = false;       // edge-detect the line crossing
    float finishArm = 0.0f;            // re-arm guard so one pass counts once (s)
    std::unordered_set<int> cpPassed;  // checkpoint entity ids passed this lap
    int   cpTotal = 0;                 // checkpoints in the scene (for the HUD)
    float raceMissedFlash = 0.0f;      // HUD flash after finishing a lap short
    float raceCountdown = 0.0f;        // Ready/Set/Go: > 0 holds everyone still
    float goFlash       = 0.0f;        // "GO!" flash once the countdown hits 0
    int   cdPhase       = -1;          // countdown step already announced (0/1/2, -1 = none)

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
struct RaceEnv {
    fitzel::Input&    input;
    fitzel::Camera&   camera;
    Document&         document;
    std::vector<Entity>& entities;
    fitzel::TerrainStreamer& streamer;
    RoadSystem&              road;
    int   driveVehicleId;
    int   driveGliderId;
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
    std::function<float(float, float, float)>      gliderGround;
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

// AI opponents: kinematic racers that lap the road centreline, slowing for
// corners (corner speed = sqrt(grip / curvature)) and banking into them.
void updateOpponents(RaceState& st, const RaceEnv& env);

} // namespace racesim
