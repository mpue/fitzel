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

    // Race over: the craft flies itself away. Take the controls off the player
    // and hold a steady forward cruise (no steering, no brake) so it keeps
    // gliding on past the finish instead of coasting to a stop.
    if (st.raceFinished) {
        throttle = 1.0f;
        steerIn  = 0.0f;
        kBrake   = false;
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
        st.gliderBoosting = onBoostPad || st.gliderOverspeed > 1.0f; // HUD
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
            if (st.raceActive && overGate(ce, cp->width, cp->height, cp->depth, cp->yaw))
                st.cpPassed.insert(ce.id);
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
    if (!env.road.built()) return;
    const std::vector<glm::vec2>& cl = env.road.centerline();
    if (cl.size() < 2) return;
    const std::size_t n = cl.size();
    const std::size_t segs = env.road.closed ? n : n - 1;
    float total = 0.0f;
    for (std::size_t i = 0; i < segs; ++i)
        total += glm::length(cl[(i + 1) % n] - cl[i]);
    // Position + tangent at arc-length `s` (wrapped/clamped).
    auto sampleAt = [&](float s, glm::vec2& pos, glm::vec2& dir) {
        if (total < 1e-3f) { pos = cl[0]; dir = glm::vec2(0.0f, 1.0f); return; }
        if (env.road.closed) { s = std::fmod(s, total); if (s < 0.0f) s += total; }
        else s = glm::clamp(s, 0.0f, total);
        float acc = 0.0f;
        for (std::size_t i = 0; i < segs; ++i) {
            const glm::vec2 a = cl[i], b = cl[(i + 1) % n];
            const float seg = glm::length(b - a);
            if (seg < 1e-5f) continue;
            if (acc + seg >= s || i == segs - 1) {
                pos = a + (b - a) * glm::clamp((s - acc) / seg, 0.0f, 1.0f);
                dir = (b - a) / seg;
                return;
            }
            acc += seg;
        }
    };
    const bool frozen = st.raceCountdown > 0.0f;
    for (Entity& e : env.entities) {
        auto* op = e.components.get<OpponentComponent>();
        if (!op) continue;
        if (!op->started) {
            // Seed travel distance from WHERE THE MARKER WAS PLACED: project its
            // XZ onto the centreline so it starts there, not at spline point 0.
            // `startDistance` is then a forward offset (stagger a starting grid).
            const glm::vec2 q(e.center.x, e.center.z);
            float bestD = 1e30f, bestS = 0.0f, walk = 0.0f;
            for (std::size_t i = 0; i < segs; ++i) {
                const glm::vec2 a = cl[i], b = cl[(i + 1) % n];
                const glm::vec2 ab = b - a;
                const float L2 = glm::dot(ab, ab);
                const float t = L2 > 1e-8f
                    ? glm::clamp(glm::dot(q - a, ab) / L2, 0.0f, 1.0f) : 0.0f;
                const float d = glm::length(q - (a + ab * t));
                if (d < bestD) { bestD = d; bestS = walk + glm::length(ab) * t; }
                walk += glm::length(ab);
            }
            op->dist = bestS + op->startDistance;
            op->curSpeed = op->speed;   // roll onto the grid at pace
            op->started = true;
        }
        // Corner speed: scan the road ahead over a braking horizon and take the
        // slowest the sharpest upcoming bend allows (corner speed =
        // sqrt(grip / curvature)), requiring we can still brake down to it in
        // time. The racer eases toward that target -- braking into the corner,
        // back on the throttle out of it -- instead of railing every bend at full
        // speed. Held until GO.
        if (!frozen) {
            const float grip  = glm::max(op->grip, 0.5f);
            const float brake = glm::max(op->brake, 1.0f);
            const float accel = glm::max(op->accel, 0.5f);
            // Look further ahead the faster we go (braking distance).
            const float horizon = glm::clamp(
                op->curSpeed * op->curSpeed / (2.0f * brake) + 8.0f,
                12.0f, 80.0f);
            const float step = 5.0f;
            glm::vec2 sp, prevDir; sampleAt(op->dist, sp, prevDir);
            float vtarget = op->speed;
            for (float ahead = step; ahead <= horizon; ahead += step) {
                glm::vec2 pa, da; sampleAt(op->dist + ahead, pa, da);
                const float c   = glm::clamp(glm::dot(prevDir, da), -1.0f, 1.0f);
                const float dth = std::acos(c);   // heading change over `step`
                prevDir = da;
                const float kappa = dth / step;   // curvature (1/m)
                if (kappa > 1e-4f) {
                    const float vCorner = std::sqrt(grip / kappa);
                    const float vAllow  = std::sqrt(
                        vCorner * vCorner + 2.0f * brake * ahead);
                    vtarget = glm::min(vtarget, vAllow);
                }
            }
            vtarget = glm::clamp(vtarget, 2.0f, op->speed);
            // Ease toward it: accelerate up, brake down.
            if (vtarget > op->curSpeed)
                op->curSpeed = glm::min(vtarget, op->curSpeed + accel * env.dt);
            else
                op->curSpeed = glm::max(vtarget, op->curSpeed - brake * env.dt);
            op->dist += op->curSpeed * env.dt;
        }
        if (!op->loop) op->dist = glm::min(op->dist, total);
        glm::vec2 pos = cl[0], dir(0.0f, 1.0f);
        sampleAt(op->dist, pos, dir);
        const glm::vec2 perp(dir.y, -dir.x);          // right of travel
        const glm::vec2 p = pos + perp * op->laneOffset;
        const glm::vec3 wpos(p.x, env.streamer.heightAt(p.x, p.y) + op->rideHeight, p.y);
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
        const glm::mat4 pw = env.parentWorldMat(e);
        env.setWorld(e, wpos, glm::vec3(0.0f, yawDeg, op->bankCur),
                     e.parent >= 0 ? &pw : nullptr);
    }
}

} // namespace racesim
