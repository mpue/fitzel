#include "RaceSim.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <fitzel/core/Input.hpp>
#include <fitzel/scene/Camera.hpp>
#include <fitzel/world/Terrain.hpp>

#include "SceneTypes.hpp"
#include "Document.hpp"
#include "Component.hpp"
#include "RoadSystem.hpp"

namespace racesim {

namespace {
// Steering aim for a free-flying craft on autopilot: the yaw (radians --
// atan2(dx, dz)) from a world point TOWARD the centreline `lookahead` metres
// further along the track (wrapping on a closed loop). Aiming at the point
// rather than copying the road's tangent is what pulls a craft that is off to
// one side back to the middle -- with the tangent alone it would fly the whole
// lap parallel to the track, permanently offset. Returns false if there's no road.
bool roadAimAhead(RoadSystem& road, const glm::vec3& p, float lookahead, float& yawOut) {
    if (!road.built()) return false;
    const std::vector<glm::vec2>& cl = road.centerline();
    if (cl.size() < 2) return false;
    const std::size_t n = cl.size();
    const std::size_t segs = road.closed ? n : n - 1;
    float total = 0.0f;
    for (std::size_t i = 0; i < segs; ++i) total += glm::length(cl[(i + 1) % n] - cl[i]);
    if (total < 1e-3f) return false;

    // Nearest arc-length to the craft.
    const glm::vec2 q(p.x, p.z);
    float bestD = 1e30f, bestS = 0.0f, walk = 0.0f;
    for (std::size_t i = 0; i < segs; ++i) {
        const glm::vec2 a = cl[i], b = cl[(i + 1) % n], ab = b - a;
        const float L2 = glm::dot(ab, ab);
        const float t = L2 > 1e-8f ? glm::clamp(glm::dot(q - a, ab) / L2, 0.0f, 1.0f) : 0.0f;
        const float d = glm::length(q - (a + ab * t));
        if (d < bestD) { bestD = d; bestS = walk + glm::length(ab) * t; }
        walk += glm::length(ab);
    }

    // The centreline point a little further along, and the tangent there.
    float s = bestS + lookahead;
    if (road.closed) { s = std::fmod(s, total); if (s < 0.0f) s += total; }
    else             s = glm::clamp(s, 0.0f, total);
    glm::vec2 aim = cl[0], dir(0.0f, 1.0f);
    float acc = 0.0f;
    for (std::size_t i = 0; i < segs; ++i) {
        const glm::vec2 a = cl[i], b = cl[(i + 1) % n];
        const float seg = glm::length(b - a);
        if (seg < 1e-5f) continue;
        if (acc + seg >= s || i == segs - 1) {
            dir = (b - a) / seg;
            aim = a + dir * glm::clamp(s - acc, 0.0f, seg);
            break;
        }
        acc += seg;
    }
    // Aim at that point; fall back to the tangent if we are sitting right on it.
    const glm::vec2 to = aim - q;
    yawOut = glm::dot(to, to) > 1.0f ? std::atan2(to.x, to.y)
                                     : std::atan2(dir.x, dir.y);
    return true;
}

// Nearest point on the road centreline to a world point: its XZ, the craft's
// lateral distance from it, and the road heading there. Used to detect a craft
// that has left the track and to snap it back on. Returns false if no road.
bool roadSnap(RoadSystem& road, const glm::vec3& p,
              glm::vec2& snapXZ, float& lateral, float& yawOut) {
    if (!road.built()) return false;
    const std::vector<glm::vec2>& cl = road.centerline();
    if (cl.size() < 2) return false;
    const std::size_t n = cl.size();
    const std::size_t segs = road.closed ? n : n - 1;
    const glm::vec2 q(p.x, p.z);
    float bestD2 = 1e30f;
    glm::vec2 bestP = cl[0], bestDir(0.0f, 1.0f);
    for (std::size_t i = 0; i < segs; ++i) {
        const glm::vec2 a = cl[i], b = cl[(i + 1) % n], ab = b - a;
        const float L2 = glm::dot(ab, ab);
        const float t = L2 > 1e-8f ? glm::clamp(glm::dot(q - a, ab) / L2, 0.0f, 1.0f) : 0.0f;
        const glm::vec2 proj = a + ab * t;
        const float d2 = glm::dot(q - proj, q - proj);
        if (d2 < bestD2) {
            bestD2 = d2; bestP = proj;
            if (L2 > 1e-8f) bestDir = ab / std::sqrt(L2);
        }
    }
    snapXZ  = bestP;
    lateral = std::sqrt(bestD2);
    yawOut  = std::atan2(bestDir.x, bestDir.y);
    return true;
}
} // namespace

void updateArcadeCar(RaceState& st, const RaceEnv& env) {
    // Arcade car: throttle + steering, drag, bicycle-model heading. When a scene
    // vehicle is being test-driven, its component supplies the geometry and the
    // sim glues the model along. Integrated on the fixed kSimH clock; the drawn
    // pose is the interpolation of the pre-/post-step state (see simAlpha), so
    // the follow stays smooth under jittery frame times.
    Entity* dv  = (env.driveVehicleId >= 0) ? env.document.find(env.driveVehicleId)
                                            : nullptr;
    auto*   dvc = dv ? dv->components.get<VehicleComponent>() : nullptr;
    const float wb = dvc ? glm::max(dvc->frontZ - dvc->rearZ, 0.5f) : 2.7f;
    const float wr = dvc ? glm::max(dvc->wheelRadius, 0.05f) : 0.42f;

    // Controls sampled once per frame, applied to every substep.
    const bool kW = env.input.isKeyDown(GLFW_KEY_W);
    const bool kS = env.input.isKeyDown(GLFW_KEY_S);
    const bool kA = env.input.isKeyDown(GLFW_KEY_A);
    const bool kD = env.input.isKeyDown(GLFW_KEY_D);
    bool  kBrake   = env.input.isKeyDown(GLFW_KEY_SPACE);
    float throttle = (kW ? 1.0f : 0.0f) - (kS ? 1.0f : 0.0f);
    float steerIn  = (kA ? 1.0f : 0.0f) - (kD ? 1.0f : 0.0f);
    // Gamepad: RT accelerate / LT reverse; left stick steers (note this model's
    // steerIn is left-positive, so subtract the stick); B brakes.
    if (env.input.hasGamepad()) {
        throttle = glm::clamp(throttle
            + env.input.gamepadTrigger(GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER)
            - env.input.gamepadTrigger(GLFW_GAMEPAD_AXIS_LEFT_TRIGGER), -1.0f, 1.0f);
        steerIn = glm::clamp(
            steerIn - env.input.gamepadStick(GLFW_GAMEPAD_AXIS_LEFT_X), -1.0f, 1.0f);
        if (env.input.gamepadButton(GLFW_GAMEPAD_BUTTON_B)) kBrake = true;
    }

    const float maxSteer = glm::radians(dvc ? dvc->maxSteerDeg : 32.0f);
    const float steerSpd = dvc ? dvc->steerSpeed   : 7.0f;
    const float camDist  = dvc ? dvc->camDistance  : 7.0f;
    const float camH     = dvc ? dvc->camHeight    : 3.2f;
    const float camSide  = dvc ? dvc->camSide      : 0.0f;
    const float camLook  = dvc ? dvc->camLookHeight: 1.2f;
    const float camStiff = dvc ? dvc->camStiffness : 4.0f;

    // *0 hold the pose just BEFORE the final substep (== the current pose when
    // no substep runs), so the render interpolates across the last step by
    // simAlpha -- the standard fixed-timestep interpolation. Snapshotting once
    // before the loop would blend across the whole (often 2-step) frame and snap
    // the craft backward.
    glm::vec3 carPos0    = st.carPos;
    float     carYaw0    = st.carYaw;
    float     steerAng0  = st.steerAngle;
    float     wheelSpin0 = st.wheelSpin;
    glm::vec3 camChase0  = st.camChase;
    for (int s = 0; s < env.simSteps; ++s) {
        carPos0 = st.carPos; carYaw0 = st.carYaw; steerAng0 = st.steerAngle;
        wheelSpin0 = st.wheelSpin; camChase0 = st.camChase;
        st.steerAngle += (steerIn * maxSteer - st.steerAngle) * std::min(1.0f, env.kSimH * steerSpd);
        st.carSpeed += throttle * 14.0f * env.kSimH;                 // accelerate
        if (kBrake) st.carSpeed -= glm::sign(st.carSpeed) * 26.0f * env.kSimH;
        st.carSpeed *= (1.0f - 0.6f * env.kSimH);                    // drag
        if (throttle == 0.0f && !kBrake) st.carSpeed *= (1.0f - 1.2f * env.kSimH);
        st.carSpeed = glm::clamp(st.carSpeed, -8.0f, 26.0f);
        if (std::abs(st.carSpeed) < 0.02f) st.carSpeed = 0.0f;
        st.carYaw += (st.carSpeed / wb) * std::tan(st.steerAngle) * env.kSimH;
        const glm::vec3 fwdS(std::sin(st.carYaw), 0.0f, std::cos(st.carYaw));
        st.carPos   += fwdS * st.carSpeed * env.kSimH;
        st.carPos.y  = env.streamer.heightAt(st.carPos.x, st.carPos.z);
        st.wheelSpin += (st.carSpeed / wr) * env.kSimH;
        // Chase camera eased in the same fixed step so its follow is as smooth
        // as the craft it interpolates alongside.
        const glm::vec3 rightS  = glm::normalize(glm::cross(glm::vec3(0, 1, 0), fwdS));
        const glm::vec3 wantedS = st.carPos - fwdS * camDist + rightS * camSide +
                                  glm::vec3(0.0f, camH, 0.0f);
        st.camChase += (wantedS - st.camChase) * std::min(1.0f, env.kSimH * camStiff);
    }

    // Render pose: blend pre-/post-step state. All of these are continuous
    // accumulators (no angle wrap within a frame), so a plain lerp is exact.
    const glm::vec3 rPos   = glm::mix(carPos0,    st.carPos,    env.simAlpha);
    const float     rYaw   = glm::mix(carYaw0,    st.carYaw,    env.simAlpha);
    const float     rSteer = glm::mix(steerAng0,  st.steerAngle,env.simAlpha);
    const float     rSpin  = glm::mix(wheelSpin0, st.wheelSpin, env.simAlpha);
    const glm::vec3 rCam   = glm::mix(camChase0,  st.camChase,  env.simAlpha);

    // Feed the engine sound from the arcade sim's speed/throttle.
    st.engineDriving  = true;
    st.engineSpeedMps = std::abs(st.carSpeed);
    st.engineThrottle = std::abs(throttle);
    st.engineWheelR   = wr;

    // Glue the driven model onto the interpolated pose: the root follows the
    // heading at its rest ride height, wheel children spin/steer (restored from
    // the snapshot when drive mode ends).
    if (dv && dvc) {
        const float restY  = wr - dvc->wheelY; // ground -> body centre
        const float yawDeg = glm::degrees(rYaw) -
                             (dvc->forward == 1 ? 180.0f : 0.0f);
        const glm::mat4 pw = env.parentWorldMat(*dv);
        env.setWorld(*dv, rPos + glm::vec3(0.0f, restY, 0.0f),
                     glm::vec3(0.0f, yawDeg, 0.0f),
                     dv->parent >= 0 ? &pw : nullptr);
        const float spinSign = (dvc->forward == 1) ? -1.0f : 1.0f;
        auto restOf = [&](int id) -> const Entity* {
            for (const Entity& b : env.driveBackup)
                if (b.id == id) return &b;
            return nullptr;
        };
        for (int i = 0; i < 4; ++i) {
            Entity*       w    = env.document.find(dvc->wheelId[i]);
            const Entity* rest = restOf(dvc->wheelId[i]);
            if (!w || !rest) continue;
            glm::vec3 rot = rest->localRotation;
            rot.x += glm::degrees(rSpin) * spinSign;
            if (i < 2) rot.y += glm::degrees(rSteer); // fronts steer
            w->localRotation = rot;
        }
    }

    // Chase camera: aim from the interpolated cam position at the craft (behind
    // and above, looking ahead).
    st.blurAnchorWorld = rPos; st.blurAnchorValid = true; // keep the car sharp
    st.blurSpeed01 = glm::clamp(std::abs(st.carSpeed) / 28.0f, 0.0f, 1.2f);
    env.camera.setPosition(rCam);
    const glm::vec3 d = glm::normalize(
        (rPos + glm::vec3(0.0f, camLook, 0.0f)) - rCam);
    env.camera.setYaw(glm::degrees(std::atan2(d.z, d.x)));
    env.camera.setPitch(glm::degrees(std::asin(glm::clamp(d.y, -1.0f, 1.0f))));
}

void updateGlider(RaceState& st, const RaceEnv& env) {
    // Wipeout-style hover racer: an arcade flight sim (no Jolt). The craft
    // thrusts along its heading, floats a ride height above the ground under it,
    // kills sideways drift by `grip`, banks into turns, and a chase camera trails
    // it -- same controls/feel as the car. The driven model is glued to the sim
    // and restored from the snapshot when flight ends.
    Entity* dg = env.document.find(env.driveGliderId);
    auto*   gc = dg ? dg->components.get<GliderComponent>() : nullptr;
    if (!dg || !gc) return;

    const bool kW = env.input.isKeyDown(GLFW_KEY_W);
    const bool kS = env.input.isKeyDown(GLFW_KEY_S);
    const bool kA = env.input.isKeyDown(GLFW_KEY_A);
    const bool kD = env.input.isKeyDown(GLFW_KEY_D);
    bool  kBrake  = env.input.isKeyDown(GLFW_KEY_SPACE);
    float throttle = (kW ? 1.0f : 0.0f) - (kS ? 1.0f : 0.0f);
    float steerIn  = (kD ? 1.0f : 0.0f) - (kA ? 1.0f : 0.0f); // right +
    // Gamepad: RT accelerate / LT reverse, left stick steers, B brakes.
    if (env.input.hasGamepad()) {
        throttle = glm::clamp(throttle
            + env.input.gamepadTrigger(GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER)
            - env.input.gamepadTrigger(GLFW_GAMEPAD_AXIS_LEFT_TRIGGER), -1.0f, 1.0f);
        steerIn = glm::clamp(
            steerIn + env.input.gamepadStick(GLFW_GAMEPAD_AXIS_LEFT_X), -1.0f, 1.0f);
        if (env.input.gamepadButton(GLFW_GAMEPAD_BUTTON_B)) kBrake = true;
    }
    if (gc->invertSteer) steerIn = -steerIn; // flip left/right

    // Manual boost: held, not tapped. Read here with the rest of the input; what
    // it costs and what it does is settled inside the fixed step below, because
    // both are rates and a rate integrated on the render clock would make the
    // tank last longer on a fast machine than on a slow one.
    bool boostHeld = env.input.isKeyDown(GLFW_KEY_LEFT_SHIFT);
    if (env.input.hasGamepad() && env.input.gamepadButton(GLFW_GAMEPAD_BUTTON_A))
        boostHeld = true;
    // The HUD's end-of-race prompt also answers to A, and the victory lap flies
    // itself -- so a finished race must not read the button as thrust.
    if (st.raceFinished) boostHeld = false;
    // Keep the tank's shape in step with the craft that is being flown, so
    // editing the component in the inspector shows up immediately.
    st.boostCapacity = glm::max(gc->boostCapacity, 1.0f);
    st.boostCharge   = glm::min(st.boostCharge, st.boostCapacity);
    // Segments are derived, not authored: one per ten units of tank, so a bigger
    // tank reads as a longer bar rather than the same bar with coarser steps.
    st.boostSegments = glm::clamp(
        static_cast<int>(std::lround(st.boostCapacity / 10.0f)), 4, 20);

    // Race over: the craft flies itself away on a victory lap. Take the controls
    // off the player and cruise at full throttle, but STEER ALONG THE ROAD (the
    // craft is a free flyer, so without this it would just fly dead straight off
    // the track). Pure pursuit: aim at the centreline a bit ahead, so wherever
    // the craft took the flag it eases back to the middle instead of flying the
    // lap offset to one side. The aim point runs further ahead the faster it
    // goes, so the correction stays smooth rather than weaving.
    if (st.raceFinished) {
        throttle = 1.0f;
        kBrake   = false;
        steerIn  = 0.0f;
        const float speed = glm::length(glm::vec2(st.gliderVel.x, st.gliderVel.z));
        const float look  = glm::clamp(speed * 0.6f, 14.0f, 45.0f);
        float roadYaw = 0.0f;
        if (roadAimAhead(env.road, st.gliderPos, look, roadYaw)) {
            float diff = roadYaw - st.gliderYaw;
            while (diff >  3.14159265f) diff -= 6.28318531f;
            while (diff < -3.14159265f) diff += 6.28318531f;
            steerIn = glm::clamp(diff * 1.5f, -1.0f, 1.0f); // turn toward the track
        }
    }

    // Ready/Set/Go: hold the craft still until GO, then start the race clock (so
    // the timer and the opponents begin exactly at GO -- no one jumps the start).
    // The countdown ticks once per frame; sub-ms precision here is irrelevant.
    const bool frozen = st.raceCountdown > 0.0f;
    if (frozen) {
        st.raceCountdown = glm::max(0.0f, st.raceCountdown - env.dt);
        throttle = 0.0f; steerIn = 0.0f; kBrake = false;
        st.gliderVel = glm::vec3(0.0f);
        if (st.raceCountdown <= 0.0f) {
            st.goFlash = 1.3f;               // "GO!" flash
            // The race starts now; the finish line then only completes laps.
            // Ignore the immediate first crossing as the craft drives off the
            // start line.
            st.raceActive = true; st.raceFinished = false;
            // Everyone leaves the grid with a full tank.
            st.boostCharge = st.boostCapacity;
            st.boostIdle   = 0.0f;
            st.raceClock = st.lapClock = 0.0f; st.raceLap = 0;
            st.lastLap = st.bestLap = 0.0f; st.cpPassed.clear();
            st.raceLaps = 0;
            for (const Entity& fe : env.entities)
                if (const auto* fl = fe.components.get<FinishLineComponent>())
                    { st.raceLaps = static_cast<int>(std::lround(fl->laps)); break; }
            st.finishWasOver = true; st.finishArm = 1.0f;
        }
    } else if (st.goFlash > 0.0f) {
        st.goFlash = glm::max(0.0f, st.goFlash - env.dt);
    }

    // Announce each step of the start sequence once, from the samples on the
    // start/finish line. Phase boundaries are the ones the HUD draws, so word
    // and sound always land together; cdPhase re-arms after "GO!" has faded.
    {
        int phase = -1;
        if (st.raceCountdown > 1.5f)      phase = 0;   // READY
        else if (st.raceCountdown > 0.0f) phase = 1;   // SET
        else if (st.goFlash > 0.0f)       phase = 2;   // GO!
        if (phase < 0) st.cdPhase = -1;
        else if (phase != st.cdPhase) {
            st.cdPhase = phase;
            if (env.playCue)
                for (const Entity& fe : env.entities) {
                    const auto* fl = fe.components.get<FinishLineComponent>();
                    if (!fl) continue;
                    const std::string& s = phase == 0 ? fl->soundReady
                                         : phase == 1 ? fl->soundSet
                                                      : fl->soundGo;
                    if (!s.empty()) env.playCue(s, fl->soundGain, 1.0f);
                    break;
                }
        }
    }

    // Off-track rescue: drop a breadcrumb whenever the craft is safely on the
    // road, and if it later falls well off (far to the side, or well below the
    // road/bridge surface next to it -- i.e. off a bridge) pop it back onto the
    // last breadcrumb, facing along the track and stopped. Only meaningful with a
    // road to reference; skipped during the countdown.
    if (env.road.built() && !frozen) {
        glm::vec2 snapXZ; float lateral = 0.0f, roadYaw = 0.0f;
        if (roadSnap(env.road, st.gliderPos, snapXZ, lateral, roadYaw)) {
            const float halfW = env.road.width * 0.5f;
            float surfY = st.gliderPos.y;
            env.road.surfaceHeightAt(glm::vec2(st.gliderPos.x, st.gliderPos.z), 1.0e6f, surfY);
            if (lateral < halfW + 4.0f && st.gliderPos.y > surfY - 4.0f) {
                // safely on the road here: remember it as the rescue point
                st.respawnPos = glm::vec3(snapXZ.x, surfY + gc->rideHeight, snapXZ.y);
                st.respawnYaw = roadYaw;
                st.haveRespawn = true;
            } else if (st.haveRespawn &&
                       (lateral > halfW + 25.0f || st.gliderPos.y < surfY - 12.0f)) {
                // fallen off the track -> back to the last safe road point
                st.gliderPos = st.respawnPos;
                st.gliderYaw = st.respawnYaw;
                st.gliderVel = glm::vec3(0.0f);
                st.gliderOverspeed = 0.0f;
            }
        }
    }

    // Advance simSteps fixed ticks. Integration and every step-bound event
    // (heading, velocity, boost pads, gate/checkpoint/lap logic, hover,
    // attitude, chase-cam easing) run on the fixed clock; fixed steps also stop
    // a fast craft tunnelling a gate. The *0 snapshots hold the pose just BEFORE
    // the final substep (== current pose if no step runs) so the render
    // interpolates across the last step by simAlpha.
    glm::vec3 gliderPos0   = st.gliderPos;
    float     gliderYaw0   = st.gliderYaw;
    float     gliderBank0  = st.gliderBank;
    float     gliderPitch0 = st.gliderPitch;
    glm::vec3 camChase0    = st.camChase;
    for (int s = 0; s < env.simSteps; ++s) {
        gliderPos0 = st.gliderPos; gliderYaw0 = st.gliderYaw;
        gliderBank0 = st.gliderBank; gliderPitch0 = st.gliderPitch;
        camChase0 = st.camChase;
        // Heading: steer right increases yaw (fwd rotates +Z -> +X).
        st.gliderYaw += glm::radians(gc->turnRate) * steerIn * env.kSimH;
        const glm::vec3 fwd(std::sin(st.gliderYaw), 0.0f, std::cos(st.gliderYaw));
        const glm::vec3 right = glm::normalize(glm::cross(glm::vec3(0, 1, 0), fwd));

        // Horizontal velocity: thrust along heading, brake, kill drift, drag,
        // then clamp to the top speed.
        glm::vec3 velH(st.gliderVel.x, 0.0f, st.gliderVel.z);
        const float accel = (throttle >= 0.0f) ? throttle * gc->thrust
                                               : throttle * gc->thrust * 0.6f;
        velH += fwd * accel * env.kSimH;
        if (kBrake) {
            const float sp = glm::length(velH);
            if (sp > 1e-4f) velH -= glm::normalize(velH) * glm::min(sp, gc->brakeForce * env.kSimH);
        }
        const float lat = glm::dot(velH, right);          // sideways slip
        velH -= right * lat * glm::clamp(gc->grip * env.kSimH, 0.0f, 1.0f);
        velH *= glm::max(0.0f, 1.0f - gc->drag * env.kSimH);

        // Boost pads: the instant the craft's footprint touches an active
        // BoostPad, its forward speed snaps up to that pad's boostSpeed (a punchy
        // kick that doesn't depend on how long it sits on the strip), with an
        // extra push while it stays on. The raised speed cap (gliderOverspeed)
        // then bleeds off over the pad's `hold` seconds after leaving.
        st.gliderOverspeed -= st.gliderOverspeed *
            glm::min(1.0f, env.kSimH / glm::max(st.gliderBoostHold, 0.1f));
        if (st.gliderOverspeed < 0.05f) st.gliderOverspeed = 0.0f;
        bool onBoostPad = false;
        const BoostPadComponent* hitPad = nullptr; // the pad just mounted
        for (const Entity& pe : env.entities) {
            if (!pe.activeInHierarchy) continue;
            const auto* bp = pe.components.get<BoostPadComponent>();
            if (!bp) continue;
            if (st.gliderPos.x < pe.center.x - pe.half.x ||
                st.gliderPos.x > pe.center.x + pe.half.x) continue;
            if (st.gliderPos.z < pe.center.z - pe.half.z ||
                st.gliderPos.z > pe.center.z + pe.half.z) continue;
            // Generous vertical window: the craft hovers rideHeight above the
            // pad, and may still be rising onto it.
            const float padTop = pe.center.y + pe.half.y;
            if (st.gliderPos.y < padTop - 2.0f ||
                st.gliderPos.y > padTop + gc->rideHeight + 5.0f) continue;
            glm::vec3 dir = fwd; // default: along the craft's heading
            if (bp->usePadDir) {
                glm::vec3 pd = glm::quat(glm::radians(pe.rotation)) *
                               glm::vec3(0.0f, 0.0f, 1.0f);
                pd.y = 0.0f;
                if (glm::length(pd) > 1e-4f) dir = glm::normalize(pd);
            }
            if (bp->reverse) dir = -dir; // flip the boost direction
            // Instant kick: bring the forward speed up to boostSpeed (never slows
            // a craft already going faster), plus a small sustained shove while
            // still on the strip.
            const float along = glm::dot(velH, dir);
            if (along < bp->boostSpeed) velH += dir * (bp->boostSpeed - along);
            velH += dir * bp->accel * env.kSimH;
            st.gliderBoostHold = glm::max(bp->hold, 0.1f);
            st.gliderOverspeed = glm::max(st.gliderOverspeed,
                                          bp->boostSpeed - gc->maxSpeed);
            onBoostPad = true;
            hitPad     = bp;
        }
        // --- Manual boost ------------------------------------------------
        // Spent and refilled on the FIXED clock: both are rates, and a rate
        // integrated on the render clock would empty the tank faster on a slow
        // machine than on a fast one. Sits after the pads so it shares their
        // decay and their speed-cap path rather than fighting over it.
        //
        // Frozen on the grid counts as not spending: a pilot leaning on the
        // button through "Ready, Set..." would otherwise reach GO with nothing
        // left, which punishes exactly the wrong instinct.
        const bool spending = boostHeld && !frozen && st.boostCharge > 0.0f;
        if (spending) {
            st.boostCharge = glm::max(0.0f,
                                      st.boostCharge - gc->boostDrain * env.kSimH);
            st.boostIdle   = 0.0f;
            velH += fwd * gc->boostThrust * env.kSimH;
            st.gliderOverspeed = glm::max(st.gliderOverspeed, gc->boostTopSpeed);
            // Long enough that the cap does not collapse between two steps, short
            // enough that letting go slows the craft promptly -- a manual boost
            // should end when the pilot ends it, unlike a pad's afterglow.
            st.gliderBoostHold = glm::max(st.gliderBoostHold, 0.55f);
            if (st.boostCharge <= 0.0f) st.boostDryFlash = 0.9f; // HUD: ran dry
        } else {
            st.boostIdle += env.kSimH;
            if (st.boostIdle >= glm::max(gc->boostDelay, 0.0f))
                st.boostCharge = glm::min(st.boostCapacity,
                                          st.boostCharge + gc->boostRegen * env.kSimH);
        }
        st.boostActive = spending;
        if (st.boostDryFlash > 0.0f)
            st.boostDryFlash = glm::max(0.0f, st.boostDryFlash - env.kSimH);

        st.gliderBoosting = onBoostPad || spending ||
                            st.gliderOverspeed > 1.0f; // HUD
        // Deep punch the instant the craft mounts a pad (rising edge), so the
        // boost is *felt*, not just seen. Retriggers only on a fresh entry.
        if (onBoostPad && !st.gliderWasOnPad && hitPad) env.playBoostPunch(*hitPad);
        st.gliderWasOnPad = onBoostPad;

        // Gate trigger shared by checkpoints and the finish line. The gate has
        // its OWN size (w x h x d) and orientation (the entity rotation plus a
        // `yaw` offset), independent of the visual object. Horizontal test is in
        // the gate's turned frame; vertical is world up.
        auto overGate = [&](const Entity& fe, float w, float h, float d, float yawOff) {
            const glm::quat q = glm::quat(glm::radians(fe.rotation)) *
                                glm::angleAxis(glm::radians(yawOff), glm::vec3(0, 1, 0));
            const glm::vec3 l = glm::conjugate(q) * (st.gliderPos - fe.center);
            if (std::abs(l.x) > w * 0.5f || std::abs(l.z) > d * 0.5f) return false;
            const float dy = st.gliderPos.y - fe.center.y;
            return dy > -2.0f && dy < h + gc->rideHeight + 2.0f;
        };

        // Checkpoints: every one must be flown through before a lap counts.
        // Passing one records it for the current lap (order doesn't matter).
        st.cpTotal = 0;
        for (const Entity& ce : env.entities) {
            if (!ce.activeInHierarchy) continue;
            const auto* cp = ce.components.get<CheckpointComponent>();
            if (!cp) continue;
            ++st.cpTotal;
            if (st.raceActive && overGate(ce, cp->width, cp->height, cp->depth, cp->yaw)) {
                // insert() reports whether this is the first pass this lap, which
                // is exactly when the gate should sound.
                if (st.cpPassed.insert(ce.id).second && !cp->sound.empty() && env.playCue)
                    env.playCue(cp->sound, cp->soundGain, cp->soundPitch);
            }
        }

        // Start/Finish line: first crossing starts the clock; each later crossing
        // completes a lap -- but only if all checkpoints were passed this lap. A
        // re-arm guard stops one pass counting twice.
        if (st.finishArm > 0.0f) st.finishArm = glm::max(0.0f, st.finishArm - env.kSimH);
        if (st.raceMissedFlash > 0.0f) st.raceMissedFlash = glm::max(0.0f, st.raceMissedFlash - env.kSimH);
        st.raceHasLine = false;
        bool overFinish = false; int lineLaps = 0;
        for (const Entity& fe : env.entities) {
            if (!fe.activeInHierarchy) continue;
            const auto* fl = fe.components.get<FinishLineComponent>();
            if (!fl) continue;
            st.raceHasLine = true;
            lineLaps = static_cast<int>(std::lround(fl->laps));
            if (overGate(fe, fl->width, fl->height, fl->depth, fl->yaw))
                overFinish = true;
        }
        if (st.raceActive && !st.raceFinished) { st.raceClock += env.kSimH; st.lapClock += env.kSimH; }
        if (st.raceCountdown <= 0.0f && overFinish && !st.finishWasOver &&
            st.finishArm <= 0.0f && !st.raceFinished) {
            st.finishArm = 2.0f; // no legit re-cross within 2 s
            if (!st.raceActive) {
                st.raceActive = true; st.raceClock = st.lapClock = 0.0f;
                st.raceLap = 0; st.raceLaps = lineLaps;
                st.lastLap = st.bestLap = 0.0f; st.cpPassed.clear();
            } else if (static_cast<int>(st.cpPassed.size()) >= st.cpTotal) {
                st.lastLap = st.lapClock;
                if (st.bestLap <= 0.0f || st.lastLap < st.bestLap) st.bestLap = st.lastLap;
                st.lapClock = 0.0f;
                ++st.raceLap;
                st.cpPassed.clear(); // fresh set for the next lap
                if (st.raceLaps > 0 && st.raceLap >= st.raceLaps) st.raceFinished = true;
            } else {
                st.raceMissedFlash = 2.5f; // crossed the line a checkpoint short
            }
        }
        st.finishWasOver = overFinish;

        const float effMax = gc->maxSpeed + glm::max(0.0f, st.gliderOverspeed);
        const float hs = glm::length(velH);
        if (hs > effMax) velH *= effMax / hs;
        st.gliderVel.x = velH.x; st.gliderVel.z = velH.z;
        st.gliderPos.x += st.gliderVel.x * env.kSimH;
        st.gliderPos.z += st.gliderVel.z * env.kSimH;

        // Hover: a spring-damper holds the body centre a ride height above the
        // ground under it; gravity takes over when launched well above the band
        // (flying off a ledge), and it never sinks through the surface.
        const float ground = env.gliderGround(st.gliderPos.x, st.gliderPos.z,
                                               st.gliderPos.y + gc->rideHeight);
        const float restY = ground + gc->rideHeight;
        const float gap   = restY - st.gliderPos.y; // >0: below rest
        st.gliderVel.y += (gap * gc->hoverStiffness - st.gliderVel.y * gc->hoverDamp) * env.kSimH;
        if (gap < -0.5f) st.gliderVel.y -= gc->gravity * env.kSimH; // airborne above band
        st.gliderPos.y += st.gliderVel.y * env.kSimH;
        const float floorY = ground + gc->rideHeight * 0.3f;
        if (st.gliderPos.y < floorY) { st.gliderPos.y = floorY; if (st.gliderVel.y < 0) st.gliderVel.y = 0; }

        // Attitude (visual): bank into the turn, tip the nose with climb/descent,
        // both eased toward their target.
        const float targetBank  = -steerIn * gc->bankAngle;
        const float targetPitch = glm::clamp(-st.gliderVel.y * gc->pitchFollow * 2.0f,
                                             -25.0f, 25.0f);
        const float k = std::min(1.0f, env.kSimH * gc->levelRate);
        st.gliderBank  += (targetBank  - st.gliderBank)  * k;
        st.gliderPitch += (targetPitch - st.gliderPitch) * k;

        // Chase camera eased on the fixed clock (same knobs as car).
        const glm::vec3 wantedC = st.gliderPos - fwd * gc->camDistance +
                                  right * gc->camSide + glm::vec3(0.0f, gc->camHeight, 0.0f);
        st.camChase += (wantedC - st.camChase) * std::min(1.0f, env.kSimH * gc->camStiffness);
    }

    // Render pose: blend pre-/post-step state (continuous values -> plain lerp).
    const glm::vec3 rPos   = glm::mix(gliderPos0,   st.gliderPos,   env.simAlpha);
    const float     rYaw   = glm::mix(gliderYaw0,   st.gliderYaw,   env.simAlpha);
    const float     rBank  = glm::mix(gliderBank0,  st.gliderBank,  env.simAlpha);
    const float     rPitch = glm::mix(gliderPitch0, st.gliderPitch, env.simAlpha);
    const glm::vec3 rCam   = glm::mix(camChase0,    st.camChase,    env.simAlpha);

    // Feed the jet-thruster sound: airspeed + throttle load.
    const float airspeed = glm::length(glm::vec2(st.gliderVel.x, st.gliderVel.z));
    st.gliderAudioActive = true;
    st.gliderSpeedMps    = airspeed;
    st.gliderThrottle    = std::abs(throttle);
    st.gliderTopSpeed    = gc->maxSpeed;

    // Glue the model onto the interpolated pose (children ride along via the
    // graph).
    const float yawDeg = glm::degrees(rYaw) -
                         (gc->forward == 1 ? 180.0f : 0.0f);
    const glm::mat4 pw = env.parentWorldMat(*dg);
    env.setWorld(*dg, rPos, glm::vec3(rPitch, yawDeg, rBank),
                 dg->parent >= 0 ? &pw : nullptr);

    // The craft is what the camera follows: anchor the radial speed blur to it
    // (stays sharp) and drive its length by airspeed.
    st.blurAnchorWorld = rPos; st.blurAnchorValid = true;
    st.blurSpeed01 = glm::clamp(airspeed / glm::max(gc->maxSpeed, 1.0f),
                                0.0f, 1.4f);

    // Chase camera aims from the interpolated cam position at the craft.
    env.camera.setPosition(rCam);
    const glm::vec3 dc = glm::normalize(
        (rPos + glm::vec3(0.0f, gc->camLookHeight, 0.0f)) - rCam);
    env.camera.setYaw(glm::degrees(std::atan2(dc.z, dc.x)));
    env.camera.setPitch(glm::degrees(std::asin(glm::clamp(dc.y, -1.0f, 1.0f))));
}

void updateOpponents(RaceState& st, const RaceEnv& env) {
    // Opponents: AI racers that travel along the built road centreline (world XZ
    // polyline + terrain height), facing along the road and banking into corners.
    // Kinematic; a closed track loops, an open road stops at the end. Snaps onto
    // the road on the first tick, so the marker can be placed anywhere.
    // Without a road there is nothing to measure progress along, so the field
    // (and with it the HUD's participant list) stays empty.
    if (!env.road.built()) { st.standings.clear(); return; }
    const std::vector<glm::vec2>& cl = env.road.centerline();
    if (cl.size() < 2) { st.standings.clear(); return; }
    const std::size_t n = cl.size();
    // The road surface per sample: over a bridged stretch this is the deck, so a
    // racer that follows it drives ACROSS the gap instead of diving into the
    // ravine with the terrain. Missing only on a road that was never built with
    // heights cached -- then we fall back to the ground, i.e. the old behaviour.
    const std::vector<float>& cy = env.road.centerlineY();
    const bool haveY = cy.size() == n;
    const std::size_t segs = env.road.closed ? n : n - 1;
    float total = 0.0f;
    for (std::size_t i = 0; i < segs; ++i)
        total += glm::length(cl[(i + 1) % n] - cl[i]);
    // Position + tangent at arc-length `s` (wrapped/clamped), and -- when asked
    // for -- the road surface height there.
    auto sampleAt = [&](float s, glm::vec2& pos, glm::vec2& dir, float* outY = nullptr) {
        if (total < 1e-3f) {
            pos = cl[0]; dir = glm::vec2(0.0f, 1.0f);
            if (outY && haveY) *outY = cy[0];
            return;
        }
        if (env.road.closed) { s = std::fmod(s, total); if (s < 0.0f) s += total; }
        else s = glm::clamp(s, 0.0f, total);
        float acc = 0.0f;
        for (std::size_t i = 0; i < segs; ++i) {
            const glm::vec2 a = cl[i], b = cl[(i + 1) % n];
            const float seg = glm::length(b - a);
            if (seg < 1e-5f) continue;
            if (acc + seg >= s || i == segs - 1) {
                const float t = glm::clamp((s - acc) / seg, 0.0f, 1.0f);
                pos = a + (b - a) * t;
                dir = (b - a) / seg;
                if (outY && haveY) *outY = glm::mix(cy[i], cy[(i + 1) % n], t);
                return;
            }
            acc += seg;
        }
    };
    // Project a world XZ point onto the centreline -> arc length along it.
    auto projDist = [&](glm::vec2 q) -> float {
        float bestD = 1e30f, bestS = 0.0f, walk = 0.0f;
        for (std::size_t i = 0; i < segs; ++i) {
            const glm::vec2 a = cl[i], b = cl[(i + 1) % n];
            const glm::vec2 ab = b - a;
            const float L2 = glm::dot(ab, ab);
            const float t = L2 > 1e-8f ? glm::clamp(glm::dot(q - a, ab) / L2, 0.0f, 1.0f) : 0.0f;
            const float d = glm::length(q - (a + ab * t));
            if (d < bestD) { bestD = d; bestS = walk + glm::length(ab) * t; }
            walk += glm::length(ab);
        }
        return bestS;
    };
    // Shortest signed along-track delta a-b (+ = a is ahead of b). On a closed
    // loop it wraps to [-total/2, total/2] so "ahead" is the short way round.
    auto signedDelta = [&](float a, float b) -> float {
        float d = a - b;
        if (env.road.closed) {
            d = std::fmod(d, total);
            if (d >  total * 0.5f) d -= total;
            if (d < -total * 0.5f) d += total;
        }
        return d;
    };
    // Stable per-racer hash in [0,1): the seed for each one's "personality".
    auto hash01 = [](int id, int salt) {
        const float v = std::sin(float(id) * 127.13f + float(salt) * 71.7f) * 43758.5453f;
        return v - std::floor(v);
    };

    const float halfW   = glm::max(env.road.width * 0.5f, 0.5f);
    const float laneMax = glm::max(halfW - 1.2f, 0.0f); // keep clear of the road edge
    const bool  frozen  = st.raceCountdown > 0.0f;

    // The human racer, projected onto the road, as one more racer for the
    // rubber-band and overtaking logic (only when a craft is actually driven).
    const bool haveP = env.playerActive;
    float playerDist = 0.0f, playerLane = 0.0f;
    if (haveP) {
        const glm::vec2 pxz(env.playerPos.x, env.playerPos.z);
        playerDist = projDist(pxz);
        glm::vec2 pp, pd; sampleAt(playerDist, pp, pd);
        playerLane = glm::dot(pxz - pp, glm::vec2(pd.y, -pd.x));
    }

    // Snapshot every racer's along-track position / lane / speed BEFORE anyone
    // moves, so avoidance sees one consistent frame. Seed new opponents here.
    struct Racer { float dist, lane, speed; };
    std::vector<Racer> racers;
    std::vector<Entity*> oppEnts;
    std::vector<OpponentComponent*> oppComps;
    for (Entity& e : env.entities) {
        auto* op = e.components.get<OpponentComponent>();
        if (!op) continue;
        if (!op->started) {
            // Seed dist from where the marker was placed (projected onto the road);
            // startDistance then staggers the grid.
            op->dist = projDist(glm::vec2(e.center.x, e.center.z)) + op->startDistance;
            op->curSpeed = 0.0f;   // standing start: accelerate off the line at GO
            op->started = true;
        }
        if (!op->laneSeeded) { op->laneCur = op->laneOffset; op->laneSeeded = true; }
        racers.push_back({op->dist, op->laneCur, op->curSpeed});
        oppEnts.push_back(&e);
        oppComps.push_back(op);
    }
    if (haveP) racers.push_back({playerDist, playerLane, env.playerSpeed});

    // Boost pads on the track, gathered once: opponents grab them like the player.
    // Each is also projected onto the centreline (along-track distance + lane), so
    // a racer can spot one ahead and steer onto it instead of only taking the ones
    // that happen to sit under its line.
    struct Pad { glm::vec2 c, h; float boostSpeed, hold; float dist, lane, radius; };
    std::vector<Pad> pads;
    for (const Entity& pe : env.entities) {
        if (!pe.activeInHierarchy) continue;
        const auto* bp = pe.components.get<BoostPadComponent>();
        if (!bp) continue;
        Pad pad{ glm::vec2(pe.center.x, pe.center.z),
                 glm::vec2(pe.half.x, pe.half.z), bp->boostSpeed, bp->hold,
                 0.0f, 0.0f, 0.0f };
        pad.dist = projDist(pad.c);
        glm::vec2 pp, pd; sampleAt(pad.dist, pp, pd);
        pad.lane   = glm::dot(pad.c - pp, glm::vec2(pd.y, -pd.x));
        // The AABB is axis-aligned, so its worst-case reach sideways is the
        // smaller half-extent -- a conservative "how close must I be to touch it".
        pad.radius = glm::max(glm::min(pad.h.x, pad.h.y), 0.1f);
        pads.push_back(pad);
    }

    // Race gates, projected onto the centreline the same way. Opponents lap by
    // the player's rules -- every checkpoint crossed, then the start/finish line
    // -- but tested in arc length instead of box overlap: a kinematic racer can
    // never tunnel a gate that way, however fast it runs. The lane test keeps a
    // gate that only spans part of the track meaningful (drive around it and it
    // does not count).
    struct Gate { int id; float dist, lane, halfW; };
    std::vector<Gate> cps;
    bool  haveLine = false;
    float lineDist = 0.0f;
    int   lineLaps = 0;
    for (const Entity& ge : env.entities) {
        if (!ge.activeInHierarchy) continue;
        const auto* cp = ge.components.get<CheckpointComponent>();
        const auto* fl = ge.components.get<FinishLineComponent>();
        if (!cp && !fl) continue;
        const glm::vec2 gxz(ge.center.x, ge.center.z);
        const float gs = projDist(gxz);
        glm::vec2 gp, gd; sampleAt(gs, gp, gd);
        const float lane = glm::dot(gxz - gp, glm::vec2(gd.y, -gd.x));
        if (cp) {
            // A gate placed so far off the track that nobody on the road could
            // fly through it would make a lap impossible -- ignore it rather
            // than stall the whole field on lap 1.
            if (std::abs(lane) > cp->width * 0.5f + halfW) continue;
            cps.push_back({ ge.id, gs, lane, cp->width * 0.5f });
        }
        else if (!haveLine) {
            haveLine = true; lineDist = gs;
            lineLaps = static_cast<int>(std::lround(fl->laps));
        }
    }
    // The race restarting (GO, or the player crossing the line to begin) wipes
    // the whole field's progress, so a second run in one Play session is clean.
    const bool startEdge = st.raceActive && !st.oppWasActive;
    st.oppWasActive = st.raceActive;
    if (startEdge) {
        st.winnerName.clear(); st.winnerIsPlayer = false; st.winnerTime = 0.0f;
        st.raceOver = false; st.playerPlace = 0;
    }

    for (std::size_t oi = 0; oi < oppComps.size(); ++oi) {
        OpponentComponent* op = oppComps[oi];
        Entity& e = *oppEnts[oi];

        // --- Personality: a stable per-racer skew so they aren't clones -------
        const float h1 = hash01(e.id, 1), h2 = hash01(e.id, 2), h3 = hash01(e.id, 3);
        const float skillSpeed = 0.92f + 0.12f * h1;  // some run faster than others
        const float skillGrip  = 0.90f + 0.18f * h2;  // ...and corner better/worse

        // Seed this racer's lap/timing state on the first tick it may move, and
        // again whenever the race (re)starts.
        if (startEdge || !op->raceSeeded) {
            op->raceSeeded = true;
            op->lap = 0; op->place = 0;
            op->raceT = op->lapT = op->lapDist = 0.0f;
            op->lastLapT = op->bestLapT = op->finishT = 0.0f;
            op->finishedRace = false;
            op->cpDone.clear();
        }

        bool slipping = false;
        if (!frozen) {
            // --- Occasional slip-up: a brief lift + wide wobble on a per-racer
            // timer, so nobody is metronome-perfect ------------------------------
            if (op->mistakeT > 0.0f) op->mistakeT -= env.dt;
            else {
                op->mistakeCd -= env.dt;
                if (op->mistakeCd <= 0.0f) {
                    op->mistakeT  = 0.4f + 0.7f * h3;   // slip lasts ~0.4-1.1 s
                    op->mistakeCd = 8.0f + 14.0f * hash01(e.id, static_cast<int>(op->dist));
                }
            }
            slipping = op->mistakeT > 0.0f;

            const float grip  = glm::max(op->grip, 0.5f) * skillGrip;
            const float brake = glm::max(op->brake, 1.0f);
            const float accel = glm::max(op->accel, 0.5f);
            float vmax = glm::max(op->speed * skillSpeed, 2.0f);

            // --- Corner speed: look-ahead braking (as before) -----------------
            const float horizon = glm::clamp(
                op->curSpeed * op->curSpeed / (2.0f * brake) + 8.0f, 12.0f, 80.0f);
            const float step = 5.0f;
            glm::vec2 tmp, prevDir; sampleAt(op->dist, tmp, prevDir);
            float vtarget = vmax;
            for (float ahead = step; ahead <= horizon; ahead += step) {
                glm::vec2 da; sampleAt(op->dist + ahead, tmp, da);
                const float dth = std::acos(glm::clamp(glm::dot(prevDir, da), -1.0f, 1.0f));
                prevDir = da;
                const float kappa = dth / step;
                if (kappa > 1e-4f) {
                    const float vCorner = std::sqrt(grip / kappa);
                    vtarget = glm::min(vtarget, std::sqrt(vCorner * vCorner + 2.0f * brake * ahead));
                }
            }

            // --- Rubber-band: ease off when leading the player, push when behind -
            if (haveP && op->catchup > 0.0f) {
                const float gap = signedDelta(op->dist, playerDist); // + = ahead of player
                const float f   = glm::clamp(gap / 55.0f, -1.0f, 1.0f);
                vtarget *= (1.0f - op->catchup * f);
                vmax    *= (1.0f + op->catchup);   // a trailing racer may exceed base top
            }
            if (slipping) vtarget *= 0.72f;        // lift off mid-slip
            vtarget = glm::clamp(vtarget, 1.5f, vmax);

            // --- Racing line: cut toward the inside of the bend just ahead -----
            glm::vec2 posNow, dC, dN; sampleAt(op->dist, posNow, dC); sampleAt(op->dist + 12.0f, tmp, dN);
            const float turn = std::acos(glm::clamp(glm::dot(dC, dN), -1.0f, 1.0f)); // 0..pi
            const glm::vec2 perpR(dC.y, -dC.x);                              // right of travel
            const float inside = (glm::dot(perpR, dN - dC) >= 0.0f) ? 1.0f : -1.0f;
            const float apex = inside * glm::clamp(turn * 2.0f, 0.0f, 1.0f) * laneMax * op->racingLine;
            float laneTarget = glm::clamp(op->laneOffset + apex, -laneMax, laneMax);

            // --- Overtake / avoid: dodge the nearest racer just ahead in our lane.
            // The dodge SIDE comes from the blocker's side of the road CENTRE (a
            // stable snapshot) and the target lane is ABSOLUTE -- never relative to
            // our own eased lane -- so laneCur can't oscillate around the blocker
            // (that ping-pong is what made them shiver and weave).
            bool dodging = false;
            if (op->awareness > 0.0f) {
                const float look = glm::clamp(op->curSpeed * 0.9f, 6.0f, 45.0f);
                float bestGap = 1e9f; const Racer* block = nullptr;
                for (std::size_t ri = 0; ri < racers.size(); ++ri) {
                    if (ri == oi) continue;                              // skip self
                    const Racer& r = racers[ri];
                    const float g = signedDelta(r.dist, op->dist);       // + = r ahead of us
                    if (g <= 0.5f || g > look) continue;
                    if (std::abs(r.lane - op->laneCur) > 2.4f) continue; // not in our lane
                    if (g < bestGap) { bestGap = g; block = &r; }
                }
                if (block && (block->speed < op->curSpeed - 1.0f || bestGap < 5.0f)) {
                    const float dodge = (block->lane >= 0.0f) ? -1.0f : 1.0f; // roomier side
                    laneTarget = glm::clamp(dodge * glm::min(laneMax, halfW * 0.6f) * op->awareness,
                                            -laneMax, laneMax);
                    if (bestGap < 4.0f) vtarget = glm::min(vtarget, block->speed); // no rear-end
                    dodging = true;
                }
            }

            // --- Pad hunting: steer onto the nearest boost pad ahead. Skipped
            // while dodging (a blocker outranks a boost) and while slipping, and
            // only for pads we can still cross to in the distance left -- so they
            // commit to a line early instead of lurching sideways at the last
            // metre. Chasing one also quickens the lane easing a little.
            float laneEase = 1.8f;
            if (!dodging && !slipping && op->padSeek > 0.0f) {
                const float look = glm::clamp(op->curSpeed * 1.3f, 12.0f, 70.0f);
                const Pad* best = nullptr; float bestGap = 1e9f;
                for (const Pad& pad : pads) {
                    const float gap = signedDelta(pad.dist, op->dist);
                    if (gap <= 1.0f || gap > look) continue;
                    // Off to the side of the road (or beyond our lane limits): not
                    // worth leaving the track for.
                    if (std::abs(pad.lane) - pad.radius > laneMax) continue;
                    const float need = std::abs(pad.lane - op->laneCur) - pad.radius;
                    if (need > 0.0f) {
                        // Sideways speed we can realistically carry ~= 6 m/s.
                        const float reach = 6.0f * gap / glm::max(op->curSpeed, 4.0f);
                        if (need > reach) continue;   // can't get across in time
                    }
                    if (gap < bestGap) { bestGap = gap; best = &pad; }
                }
                if (best) {
                    const float aim = glm::clamp(best->lane, -laneMax, laneMax);
                    laneTarget = glm::mix(laneTarget, aim, op->padSeek);
                    laneEase   = 2.8f;
                }
            }
            if (slipping) laneTarget = glm::clamp(laneTarget + (h3 - 0.5f) * 2.0f, -laneMax, laneMax);

            // --- Boost pads: grab them like the player. Bleed off any leftover
            // boost, then if we're over a pad snap our speed up to it and refresh
            // the overspeed cap (which then decays over the pad's `hold`).
            op->overspeed -= op->overspeed * glm::min(1.0f, env.dt / glm::max(op->boostHold, 0.1f));
            if (op->overspeed < 0.05f) op->overspeed = 0.0f;
            const glm::vec2 curXZ = posNow + perpR * op->laneCur;
            for (const Pad& pad : pads) {
                if (curXZ.x < pad.c.x - pad.h.x || curXZ.x > pad.c.x + pad.h.x) continue;
                if (curXZ.y < pad.c.y - pad.h.y || curXZ.y > pad.c.y + pad.h.y) continue;
                op->overspeed = glm::max(op->overspeed, pad.boostSpeed - op->speed);
                op->boostHold = glm::max(pad.hold, 0.1f);
                op->curSpeed  = glm::max(op->curSpeed, pad.boostSpeed); // instant kick
            }
            const float effTop = vmax + glm::max(0.0f, op->overspeed);
            // While boosted, ride the raised speed so the kick doesn't brake off.
            if (op->overspeed > 0.0f)
                vtarget = glm::max(vtarget, glm::min(op->speed + op->overspeed, effTop));
            vtarget = glm::clamp(vtarget, 1.5f, effTop);

            // Ease speed and lane toward their targets, then advance along the road.
            // The lane easing is deliberately gentle so line changes read as smooth
            // steering, not twitches.
            if (vtarget > op->curSpeed) op->curSpeed = glm::min(vtarget, op->curSpeed + accel * env.dt);
            else                        op->curSpeed = glm::max(vtarget, op->curSpeed - brake * env.dt);
            op->curSpeed = glm::min(op->curSpeed, effTop);
            op->laneCur += (laneTarget - op->laneCur) * glm::min(1.0f, env.dt * laneEase);
            const float prevDist = op->dist;
            op->dist += op->curSpeed * env.dt;

            // --- Laps: which gates did this step carry us across? --------------
            if (!op->finishedRace) {
                op->raceT   += env.dt;
                op->lapT    += env.dt;
                op->lapDist += op->dist - prevDist;
                // `s` was ahead of us before the step and is behind us after it.
                auto crossed = [&](float s) {
                    return signedDelta(s, prevDist) > 0.0f && signedDelta(s, op->dist) <= 0.0f;
                };
                for (const Gate& g : cps)
                    if (crossed(g.dist) && std::abs(op->laneCur - g.lane) <= g.halfW + 1.0f)
                        op->cpDone.insert(g.id);
                // A lap needs the line AND every checkpoint. The half-lap distance
                // guard keeps the pass right after the start (and a racer parked on
                // the line) from ticking laps over.
                if (haveLine && crossed(lineDist) && op->lapDist > total * 0.5f &&
                    op->cpDone.size() >= cps.size()) {
                    op->lastLapT = op->lapT;
                    if (op->bestLapT <= 0.0f || op->lastLapT < op->bestLapT)
                        op->bestLapT = op->lastLapT;
                    op->lapT = 0.0f; op->lapDist = 0.0f; op->cpDone.clear();
                    ++op->lap;
                    if (lineLaps > 0 && op->lap >= lineLaps) {
                        op->finishedRace = true;
                        op->finishT = op->raceT;
                    }
                }
            }
        }
        if (!op->loop) op->dist = glm::min(op->dist, total);

        // Place: centreline point offset by the live lane, on the ROAD surface --
        // the ribbon is lofted flat across its width, so the centreline height is
        // the right one whatever lane we sit in, and over a bridge it is the deck.
        glm::vec2 pos = cl[0], dir(0.0f, 1.0f);
        float surfY = 0.0f;
        sampleAt(op->dist, pos, dir, &surfY);
        const glm::vec2 perp(dir.y, -dir.x);          // right of travel
        const glm::vec2 p = pos + perp * op->laneCur;
        if (!haveY) surfY = env.streamer.heightAt(p.x, p.y);
        const glm::vec3 wpos(p.x, surfY + op->rideHeight, p.y);
        const float yaw0 = std::atan2(dir.x, dir.y);
        const float yawDeg = glm::degrees(yaw0) - (op->forward == 1 ? 180.0f : 0.0f);
        // Bank into the corner: heading change a little ahead.
        glm::vec2 pa = pos, da = dir; sampleAt(op->dist + 5.0f, pa, da);
        float dYaw = glm::degrees(std::atan2(da.x, da.y) - yaw0);
        while (dYaw > 180.0f) dYaw -= 360.0f;
        while (dYaw < -180.0f) dYaw += 360.0f;
        const float targetBank =
            glm::clamp(-dYaw * 0.6f, -op->bankAngle, op->bankAngle);
        op->bankCur += (targetBank - op->bankCur) * glm::min(1.0f, env.dt * 5.0f);
        // Pitch onto the slope, measured over a short chord around us: a bridge
        // ramp (or any hill) is climbed nose-up instead of level-through-the-deck.
        // Positive pitch is nose-down for a +Z model, so a -Z nose flips the sign.
        float targetPitch = 0.0f;
        if (haveY) {
            const float ds = 4.0f;
            float yB = surfY, yA = surfY; glm::vec2 tp, td;
            sampleAt(op->dist - ds, tp, td, &yB);
            sampleAt(op->dist + ds, tp, td, &yA);
            targetPitch = -glm::degrees(std::atan2(yA - yB, 2.0f * ds)) *
                          (op->forward == 1 ? -1.0f : 1.0f);
        }
        op->pitchCur += (targetPitch - op->pitchCur) * glm::min(1.0f, env.dt * 5.0f);
        const glm::mat4 pw = env.parentWorldMat(e);
        env.setWorld(e, wpos, glm::vec3(op->pitchCur, yawDeg, op->bankCur),
                     e.parent >= 0 ? &pw : nullptr);
    }

    // --- Standings: the field in running order ------------------------------
    // Progress is measured from the start/finish line so the order is the racing
    // order, not "distance from the road's first point": completed laps times the
    // lap length, plus how far into the current lap each racer is.
    auto lapPos = [&](float d) {
        if (total < 1e-3f) return 0.0f;
        float v = std::fmod(d - (haveLine ? lineDist : 0.0f), total);
        if (v < 0.0f) v += total;
        return v;
    };
    st.standings.clear();
    st.standings.reserve(oppComps.size() + 1);
    for (std::size_t oi = 0; oi < oppComps.size(); ++oi) {
        const OpponentComponent* op = oppComps[oi];
        const Entity& e = *oppEnts[oi];
        RaceState::Standing s;
        s.id       = e.id;
        s.name     = !e.name.empty() ? e.name : ("Racer " + std::to_string(e.id));
        s.lap      = op->lap;
        s.progress = static_cast<float>(op->lap) * total + lapPos(op->dist);
        s.bestLap  = op->bestLapT;
        s.lastLap  = op->lastLapT;
        s.finished = op->finishedRace;
        s.totalTime = op->finishT;
        st.standings.push_back(std::move(s));
    }
    if (haveP) {
        RaceState::Standing s;
        s.id       = -1;
        s.name     = "YOU";
        s.isPlayer = true;
        s.lap      = st.raceLap;
        s.progress = static_cast<float>(st.raceLap) * total + lapPos(playerDist);
        s.bestLap  = st.bestLap;
        s.lastLap  = st.lastLap;
        s.finished = st.raceFinished;
        s.totalTime = st.raceClock;
        st.standings.push_back(std::move(s));
    }
    // Finishers rank ahead of anyone still running, by the time they took.
    std::sort(st.standings.begin(), st.standings.end(),
              [](const RaceState::Standing& a, const RaceState::Standing& b) {
                  if (a.finished != b.finished) return a.finished;
                  if (a.finished) return a.totalTime < b.totalTime;
                  return a.progress > b.progress;
              });
    st.playerPlace = 0;
    st.raceOver = !st.standings.empty();
    for (std::size_t i = 0; i < st.standings.size(); ++i) {
        RaceState::Standing& s = st.standings[i];
        s.gap = glm::max(0.0f, st.standings[0].progress - s.progress);
        if (s.isPlayer) st.playerPlace = static_cast<int>(i) + 1;
        if (!s.finished) st.raceOver = false;
        // Mirror the live position back onto the racer (handy for scripts/debug).
        if (!s.isPlayer)
            for (std::size_t oi = 0; oi < oppComps.size(); ++oi)
                if (oppEnts[oi]->id == s.id) { oppComps[oi]->place = static_cast<int>(i) + 1; break; }
    }

    // Winner: the first racer over the last lap's line. The player's finish is
    // resolved in updateGlider earlier this frame, so check it first.
    if (st.winnerName.empty()) {
        if (haveP && st.raceFinished) {
            st.winnerName = "YOU"; st.winnerIsPlayer = true; st.winnerTime = st.raceClock;
        } else {
            const OpponentComponent* first = nullptr; const Entity* firstE = nullptr;
            for (std::size_t oi = 0; oi < oppComps.size(); ++oi)
                if (oppComps[oi]->finishedRace &&
                    (!first || oppComps[oi]->finishT < first->finishT))
                    { first = oppComps[oi]; firstE = oppEnts[oi]; }
            if (first) {
                st.winnerName = !firstE->name.empty() ? firstE->name
                                                      : ("Racer " + std::to_string(firstE->id));
                st.winnerIsPlayer = false;
                st.winnerTime = first->finishT;
            }
        }
    }
}

} // namespace racesim
