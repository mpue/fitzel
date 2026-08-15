#include "RaceSim.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <unordered_map>
#include <unordered_set>

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

// --- Collisions -------------------------------------------------------------
// What the glider can crash into. The scene already answers this, and it answers
// it in one place: a Physics component means "this has a collider in Play".
// Guessing from the geometry instead -- every box and model is a wall unless
// something on it says otherwise -- gets the arch of a checkpoint gate wrong,
// and a gate that destroys the craft flying through it is worse than a hundred
// props it flies past. So the rule is the author's, not this module's: no
// Physics, no collision. A scene that wants a wall to hurt gives the wall a
// collider, which it needs anyway for everything else that drives into it.
bool isSolidObstacle(const Entity& e) {
    if (!e.activeInHierarchy) return false;
    const ComponentList& c = e.components;
    // The other racers are craft, not scenery: they are solid on their own terms
    // (kinematic, so they carry no Physics component and never will).
    if (c.get<OpponentComponent>()) return true;
    if (!c.get<PhysicsComponent>()) return false;
    // ...and a few things stay non-solid even with a collider on them: a ramp IS
    // a slope to be climbed (the hover model carries the craft up it), an Empty
    // has no geometry, and the terrain is the ground the craft flies over.
    if (e.type == EntityType::Ramp || e.type == EntityType::Empty) return false;
    return !(c.get<TerrainComponent>() || c.get<GliderComponent>());
}

// Does this entity carry something that says "fly through me"? Same list the
// blacklist above rejects, minus the scenery-ish entries -- it is used to find
// the GATE PREFABS below, not to judge a single object.
bool isPassMarker(const Entity& e) {
    const ComponentList& c = e.components;
    return c.get<CheckpointComponent>()  || c.get<FinishLineComponent>() ||
           c.get<BoostPadComponent>()    || c.get<TriggerComponent>()    ||
           c.get<SceneTriggerComponent>()|| c.get<CollectibleComponent>() ||
           c.get<TriggerSoundComponent>()|| c.get<PlayerStartComponent>() ||
           c.get<CraftEntryComponent>()  || c.get<TrackEntryComponent>() ||
           c.get<ShowroomComponent>();
}

// Every entity that belongs to a gate, arch or pad prefab -- hierarchy included.
//
// A checkpoint is not one object. It is a prefab: an Empty called "Trigger"
// carrying the CheckpointComponent, next to a Model called "Door" carrying the
// arch, side by side under one root. The component sits on the trigger, so
// skipping only the entity that carries it leaves the arch itself standing
// across the road as a solid wall -- and then flying through a checkpoint, which
// is the one thing a lap requires, is what destroys the craft.
//
// The family is taken as the marker's PARENT and everything under it, not the
// topmost ancestor: a checkpoint parented high up in a scene would otherwise
// switch off collisions for the whole scene.
void collectPassThrough(const std::vector<Entity>& ents,
                        std::unordered_set<int>& out) {
    std::unordered_map<int, int> parentOf;  // id -> parent id (no O(n) find per hop)
    parentOf.reserve(ents.size() * 2);
    for (const Entity& e : ents) parentOf[e.id] = e.parent;

    std::unordered_set<int> groups;         // the gate prefabs' root ids
    for (const Entity& e : ents)
        if (isPassMarker(e))
            groups.insert(e.parent >= 0 && parentOf.count(e.parent) ? e.parent : e.id);
    if (groups.empty()) return;

    for (const Entity& e : ents) {
        int id = e.id;
        for (int guard = 0; id >= 0 && guard < 64; ++guard) {
            if (groups.count(id)) { out.insert(e.id); break; }
            const auto it = parentOf.find(id);
            if (it == parentOf.end()) break;
            id = it->second;
        }
    }
}

// Is `e` the driven craft itself, or one of the things hanging off it (its
// thrusters, its wings, its exhaust emitters)? A craft cannot crash into its
// own children, and they follow it exactly, so they would collide every frame.
bool partOfCraft(Document& doc, const Entity& e, int craftId) {
    if (craftId < 0) return false;
    int p = e.id;
    for (int guard = 0; p >= 0 && guard < 64; ++guard) {
        if (p == craftId) return true;
        const Entity* pe = doc.find(p);
        if (!pe) break;
        p = pe->parent;
    }
    return false;
}

// Does the craft's hull touch this object's box? The answer is horizontal only:
// the craft hovers, so a contact can only push it sideways -- a vertical push
// would just wrestle the hover spring for the same metre.
//
// The height test is what decides whether an object is an obstacle at all.
// Anything whose top sits below the craft is something it flies OVER (a kerb, a
// ramp, the blocks a track is built from -- the hover model climbs those, and
// turning them into walls would break every block-built track); anything whose
// bottom is above it is flown UNDER (a bridge deck, a gantry). Only what spans
// the craft's own height can be crashed into. `anyHeight` skips that test for
// another RACER, whose hull is as flat as the player's and would otherwise never
// register.
bool hullHitsBox(const Entity& e, const glm::vec3& p, float r, float headY,
                 bool anyHeight, glm::vec3& normal, float& depth) {
    const glm::quat q = glm::quat(glm::radians(e.rotation));
    const glm::mat3 m = glm::mat3_cast(q);
    // The turned box's vertical half-extent in world space.
    const float hy = std::abs(m[0].y) * e.half.x + std::abs(m[1].y) * e.half.y +
                     std::abs(m[2].y) * e.half.z;
    if (anyHeight) {
        if (std::abs(e.center.y - p.y) > hy + 2.5f) return false;
    } else if (e.center.y + hy < headY || e.center.y - hy > headY) {
        return false;
    }
    const glm::vec3 l = glm::conjugate(q) * (p - e.center);
    const float px = (e.half.x + r) - std::abs(l.x);
    const float pz = (e.half.z + r) - std::abs(l.z);
    if (px <= 0.0f || pz <= 0.0f) return false;
    // Out through the face the craft came in by: the axis it is least deep into.
    glm::vec3 nl(0.0f);
    if (px < pz) { nl.x = l.x >= 0.0f ? 1.0f : -1.0f; depth = px; }
    else         { nl.z = l.z >= 0.0f ? 1.0f : -1.0f; depth = pz; }
    normal   = q * nl;
    normal.y = 0.0f;
    const float len = glm::length(normal);
    if (len < 1e-4f) return false;
    normal /= len;
    return true;
}

// Resolve one contact: bounce the velocity off the surface and charge the hull
// for it. The cost is the CLOSING speed along the contact normal, not the
// craft's own speed -- brushing a wall at a shallow angle flat out has to cost
// less than driving into it head-on, or the only safe way to fly is slowly,
// which is the wrong thing for a racer to teach. Sliding along a wall therefore
// costs nothing after the first touch, which is exactly right.
void applyImpact(RaceState& st, const GliderComponent& gc, glm::vec3& velH,
                 const glm::vec3& n, const RaceEnv& env) {
    const float into = -glm::dot(velH, n);   // closing speed along the normal
    if (into <= 0.0f) return;                // already leaving: not a fresh hit
    velH += n * into * (1.0f + glm::clamp(gc.crashBounce, 0.0f, 1.0f));
    st.gliderVel.x = velH.x;
    st.gliderVel.z = velH.z;
    st.gliderOverspeed = 0.0f;               // a crash ends the boost run
    if (into < gc.crashMinSpeed) return;     // a nudge, not a crash
    const float dmg = (into - gc.crashMinSpeed) * glm::max(gc.crashDamage, 0.0f);
    if (dmg <= 0.0f) return;
    st.energy        = glm::max(0.0f, st.energy - dmg);
    st.energyIdle    = 0.0f;                 // ...and the recharge waits again
    st.energyLastHit = dmg;
    st.energyHitFlash = 0.7f;
    // Harder hits sound deeper: one sample, pitched by what it cost.
    if (!gc.soundHit.empty() && env.playCue) {
        const float sev = glm::clamp(dmg / glm::max(st.energyCapacity * 0.3f, 1.0f),
                                     0.0f, 1.0f);
        env.playCue(gc.soundHit, gc.soundHitGain, glm::mix(1.15f, 0.75f, sev));
    }
}
} // namespace

void applyDamage(RaceState& st, float dmg) {
    if (dmg <= 0.0f) return;
    st.energy         = glm::max(0.0f, st.energy - dmg);
    st.energyIdle     = 0.0f;      // the recharge waits again
    st.energyLastHit  = dmg;
    st.energyHitFlash = 0.7f;
}

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

    // *0 hold the pose just BEFORE the final substep (== the current pose when
    // no substep runs), so the render interpolates across the last step by
    // simAlpha -- the standard fixed-timestep interpolation. Snapshotting once
    // before the loop would blend across the whole (often 2-step) frame and snap
    // the craft backward.
    // Persistent, not per-frame: see RaceState::prevPos -- a frame with no step
    // must still interpolate across the last one.
    if (!st.prevValid) {
        st.prevPos = st.carPos; st.prevYaw = st.carYaw;
        st.prevSteer = st.steerAngle; st.prevSpin = st.wheelSpin;
        st.prevValid = true;
    }
    for (int s = 0; s < env.simSteps; ++s) {
        st.prevPos = st.carPos; st.prevYaw = st.carYaw;
        st.prevSteer = st.steerAngle; st.prevSpin = st.wheelSpin;
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
    }

    // Render pose: blend pre-/post-step state. All of these are continuous
    // accumulators (no angle wrap within a frame), so a plain lerp is exact.
    const glm::vec3 rPos   = glm::mix(st.prevPos,   st.carPos,    env.simAlpha);
    const float     rYaw   = glm::mix(st.prevYaw,   st.carYaw,    env.simAlpha);
    const float     rSteer = glm::mix(st.prevSteer, st.steerAngle,env.simAlpha);
    const float     rSpin  = glm::mix(st.prevSpin,  st.wheelSpin, env.simAlpha);

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

    // The camera is not this function's business any more: the car's own camera
    // entity follows it (see CameraSystem). What stays is what the RENDERER
    // needs about the car itself -- the anchor the radial speed blur is centred
    // on, which is a property of the craft, not of the eye.
    st.blurAnchorWorld = rPos; st.blurAnchorValid = true; // keep the car sharp
    st.blurSpeed01 = glm::clamp(std::abs(st.carSpeed) / 28.0f, 0.0f, 1.2f);
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

    // Controls arrive resolved (see RaceControls): which device produced them is
    // the caller's business, which is what lets two players fly at once.
    float throttle = env.controls.throttle;
    float steerIn  = env.controls.steer; // right +
    bool  kBrake   = env.controls.brake;
    if (gc->invertSteer) steerIn = -steerIn; // flip left/right

    // Manual boost: held, not tapped. What it costs and what it does is settled
    // inside the fixed step below, because both are rates and a rate integrated
    // on the render clock would make the tank last longer on a fast machine than
    // on a slow one.
    bool boostHeld = env.controls.boost;
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
    // Same for the hull's energy, so the inspector's tank size shows up live.
    st.energyCapacity = glm::max(gc->energyCapacity, 1.0f);
    st.energy         = glm::min(st.energy, st.energyCapacity);

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

    // Hull gone: the run is lost. The controls go dead and the craft coasts to a
    // stop (see the extra drag in the step below) while the HUD puts up the board.
    // After the victory-lap block, so a craft that has already taken the flag
    // cannot be flown by a wreck.
    if (st.energyOut) {
        throttle = 0.0f; steerIn = 0.0f; kBrake = false; boostHeld = false;
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
            // Everyone leaves the grid with a full tank -- and a full hull.
            st.boostCharge = st.boostCapacity;
            st.boostIdle   = 0.0f;
            st.energy      = st.energyCapacity;
            st.energyOut   = false; st.energyLow = false;
            st.energyIdle  = 0.0f;  st.energyHitFlash = 0.0f;
            st.raceClock = st.lapClock = 0.0f; st.raceLap = 0;
            st.lastLap = st.bestLap = 0.0f; st.cpPassed.clear();
            st.raceLaps = 0;
            for (const Entity& fe : env.entities)
                if (const auto* fl = fe.components.get<FinishLineComponent>())
                    { st.raceLaps = static_cast<int>(std::lround(fl->laps)); break; }
            st.finishWasOver = true; st.finishArm = 1.0f;
            // The grid stands behind the line, so the run up to it is not a lap:
            // the next crossing opens lap 1 (see lapBegun).
            st.lapBegun = false;
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
            // The raised edges are road too: a craft parked up the lip has not
            // left the track, so the rescue must measure against the whole
            // section, not just the carriageway between the walls.
            const float halfW = env.road.surfaceHalf();
            // Ask for the storey the craft is ON, not the nearest branch in plan
            // view: under a flyover the deck overhead would otherwise read as the
            // road surface, and the craft -- twenty metres below it -- would count
            // as having fallen off and be teleported back.
            //
            // Whether an answer came back at all is half the information. Under a
            // bridge that spans open ground there IS no carriageway below the
            // craft's head: every branch sits overhead and the query finds
            // nothing. Reading that as "no news, carry on" is what broke the
            // rescue there -- surfY kept the craft's own height, "am I above the
            // surface" compared the craft against itself and always said yes, so
            // a craft that had dropped off a bridge counted as safely on the road,
            // was never fetched back, and quietly overwrote its own rescue point
            // with the spot under the bridge. No surface below = not on the road.
            float surfY = 0.0f;
            const bool onRoadDeck = env.road.surfaceHeightAt(
                glm::vec2(st.gliderPos.x, st.gliderPos.z), 1.0e6f, surfY,
                st.gliderPos.y + gc->rideHeight + 3.0f);
            // ...unless the road carries no height data at all (an older road that
            // was never built with one). Then "nothing found" says nothing about
            // where the craft is, and pinning it to its breadcrumb every frame
            // would be worse than not rescuing it.
            const bool haveRoadY =
                env.road.centerlineY().size() == env.road.centerline().size();

            // --- The track's own wall ---------------------------------------
            // A road with raised edges is a road with barriers: riding up the lip
            // is still flying (that is what the lip is for), but running out over
            // the top of it is a crash, and the hull pays for it. Only where there
            // IS an edge -- a road without one has no wall to hit, and the rescue
            // below owns that case. Resolved once a frame rather than per fixed
            // step because it is a standing condition, not an event, and the
            // nearest-centreline query behind it is not cheap.
            //
            // The two guards keep this to craft that are actually against the
            // barrier: on the road's own deck (not thirty metres below it in an
            // underpass), and only just past the line (a craft already far out has
            // left the track, which is the rescue's business, not the wall's).
            if (st.loopIndex < 0 && !st.energyOut && env.road.edgeWidth > 0.01f &&
                onRoadDeck && st.gliderPos.y > surfY - 4.0f &&
                st.gliderPos.y < surfY + 10.0f) {
                const float lim = glm::max(halfW - gc->hullRadius, 1.0f);
                glm::vec2 inward = snapXZ - glm::vec2(st.gliderPos.x, st.gliderPos.z);
                if (lateral > lim && lateral < lim + 6.0f &&
                    glm::length(inward) > 1e-4f) {
                    inward = glm::normalize(inward);
                    const glm::vec3 n(inward.x, 0.0f, inward.y);
                    st.gliderPos += n * (lateral - lim);
                    glm::vec3 velW(st.gliderVel.x, 0.0f, st.gliderVel.z);
                    applyImpact(st, *gc, velW, n, env);
                    lateral = lim;   // it is back inside: the rescue sees that
                }
            }

            if (onRoadDeck && lateral < halfW + 4.0f && st.gliderPos.y > surfY - 4.0f) {
                // safely on the road here: remember it as the rescue point
                st.respawnPos = glm::vec3(snapXZ.x, surfY + gc->rideHeight, snapXZ.y);
                st.respawnYaw = roadYaw;
                st.haveRespawn = true;
            } else if (st.haveRespawn &&
                       (lateral > halfW + 25.0f ||
                        (onRoadDeck && st.gliderPos.y < surfY - 12.0f) ||
                        (!onRoadDeck && haveRoadY))) {
                // fallen off the track -> back to the last safe road point
                st.gliderPos = st.respawnPos;
                st.gliderYaw = st.respawnYaw;
                st.gliderVel = glm::vec3(0.0f);
                st.gliderOverspeed = 0.0f;
            }
        }
    }

    // The gate prefabs, resolved once a frame: a checkpoint's arch is a sibling
    // of the trigger that carries the component, so the collision sweep below
    // cannot tell it from a wall without being told. Built here rather than in
    // the step loop -- the hierarchy does not change between two fixed steps.
    std::unordered_set<int> passThrough;
    collectPassThrough(env.entities, passThrough);

    // Advance simSteps fixed ticks. Integration and every step-bound event
    // (heading, velocity, boost pads, gate/checkpoint/lap logic, hover,
    // attitude) run on the fixed clock; fixed steps also stop a fast craft
    // tunnelling a gate.
    //
    // The snapshot of the pose before the last step lives in the STATE, not in a
    // local: on a frame where no step falls it must still hold the previous
    // step's pose, so the render keeps interpolating across that step as
    // simAlpha grows. Reset per frame instead, the craft freezes for that frame
    // while the camera keeps moving -- see RaceState::prevPos.
    if (!st.prevValid) {
        st.prevPos = st.gliderPos; st.prevYaw = st.gliderYaw;
        st.prevBank = st.gliderBank; st.prevPitch = st.gliderPitch;
        st.prevValid = true;
    }
    for (int s = 0; s < env.simSteps; ++s) {
        st.prevPos = st.gliderPos; st.prevYaw = st.gliderYaw;
        st.prevBank = st.gliderBank; st.prevPitch = st.gliderPitch;
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
        const bool spending = boostHeld && !frozen && !st.energyOut &&
                              st.boostCharge > 0.0f;
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

        // --- Energy: recharge, alarm, and running out ------------------------
        // Rates on the fixed clock, for the same reason the boost tank is: a
        // hull that recharged on the render clock would heal faster on a slow
        // machine. The pause before it starts (energyDelay) is what makes a
        // clean line worth flying -- without it, a crash is free as long as the
        // next corner is far enough away.
        if (st.energyHitFlash > 0.0f)
            st.energyHitFlash = glm::max(0.0f, st.energyHitFlash - env.kSimH);
        if (!st.energyOut) {
            st.energyIdle += env.kSimH;
            if (st.energyIdle >= glm::max(gc->energyDelay, 0.0f))
                st.energy = glm::min(st.energyCapacity,
                                     st.energy + gc->energyRegen * env.kSimH);
        }
        // The low-energy alarm: once on crossing the threshold, then repeating
        // while it stays there. An alarm that sounds once is a notification; a
        // pilot who cannot look at the bar mid-corner needs it to keep nagging.
        const float frac = st.energy / glm::max(st.energyCapacity, 1.0f);
        const bool  low  = !st.energyOut &&
                           frac <= glm::clamp(gc->energyWarnAt, 0.0f, 1.0f);
        if (low && !st.energyLow) st.energyWarnT = 0.0f;   // sound it immediately
        st.energyLow = low;
        if (low) {
            st.energyWarnT -= env.kSimH;
            if (st.energyWarnT <= 0.0f) {
                st.energyWarnT = 2.5f;
                if (!gc->soundWarn.empty() && env.playCue)
                    env.playCue(gc->soundWarn, gc->soundWarnGain, 1.0f);
            }
        } else {
            st.energyWarnT = 0.0f;
        }
        // Out of energy: the run is lost from here. One last low thud off the
        // hit sample marks it, and the HUD takes over.
        if (!st.energyOut && st.energy <= 0.0f) {
            st.energyOut  = true;
            st.energyLow  = false;
            st.boostActive = false;
            if (!gc->soundHit.empty() && env.playCue)
                env.playCue(gc->soundHit, gc->soundHitGain, 0.6f);
        }

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
                st.lapBegun = true;
            } else if (!st.lapBegun) {
                // First pass after GO: the craft has just driven up from the grid
                // BEHIND the line, so this opens lap 1 rather than completing one.
                // Nothing was missed here -- flashing "missed a checkpoint" at a
                // pilot who has not had the chance to fly through one yet is the
                // bug this branch exists for. The lap clock restarts at the line so
                // lap 1 is measured line-to-line like every other lap (the race
                // clock keeps running from GO, so the run-up still costs time).
                st.lapBegun = true;
                st.lapClock = 0.0f;
                st.cpPassed.clear();
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
        const float halfW  = env.road.width * 0.5f;
        const std::vector<roadloop::Loop>& lps = env.road.loopGeometry();
        if (st.loopExitFlash > 0.0f)
            st.loopExitFlash = glm::max(0.0f, st.loopExitFlash - env.kSimH);

        if (st.loopIndex >= 0 && st.loopIndex < static_cast<int>(lps.size())) {
            // --- Riding a vertical loop -----------------------------------
            // The hover model is switched off here, and it has to be: it holds
            // the craft a ride height above gliderGround(x,z) with gravity
            // pointing at world down, and neither of those means anything on a
            // surface that is vertical and then inverted. Instead the craft is
            // integrated ALONG the turn, with gravity resolved against the
            // track's own frame.
            const roadloop::Loop& lp = lps[st.loopIndex];
            roadloop::Frame fr = roadloop::at(lp, st.loopArc);

            float acc = (throttle >= 0.0f) ? throttle * gc->thrust
                                           : throttle * gc->thrust * 0.6f;
            if (spending) acc += gc->boostThrust;      // the boost works in here too
            if (kBrake)   acc -= gc->brakeForce;
            // Gravity along the track: climbing the front of the turn costs
            // speed, coming down the back gives it back. This is what makes a
            // loop something you have to arrive at fast enough for.
            acc -= gc->gravity * fr.tangent.y;
            st.loopSpeed += acc * env.kSimH;
            st.loopSpeed *= glm::max(0.0f, 1.0f - gc->drag * env.kSimH);
            st.loopSpeed  = glm::min(st.loopSpeed, effMax);

            // Steering moves the craft ACROSS the carriageway rather than
            // turning it -- on a loop there is nowhere to turn to.
            st.loopSide = glm::clamp(
                st.loopSide + steerIn * gc->turnRate * 0.05f * env.kSimH,
                -halfW + 0.4f, halfW - 0.4f);
            st.loopArc += st.loopSpeed * env.kSimH;

            // Does the track still hold it? The surface can only PUSH, so the
            // normal force per unit mass is v^2/R + g*n.y, and once that goes
            // negative the craft is asking to be pulled through the road. At the
            // bottom n.y is +1 and it can never happen; at the top n.y is -1 and
            // it needs v^2 > g*R -- the classic condition, and exactly the
            // "arrive fast enough or fall out of it" the loop exists for.
            const float hold = st.loopSpeed * st.loopSpeed /
                                   glm::max(lp.radius, 1.0f) +
                               gc->gravity * fr.normal.y;

            fr = roadloop::at(lp, glm::clamp(st.loopArc, 0.0f, lp.length));
            st.gliderPos = fr.pos + fr.side * st.loopSide + fr.normal * gc->rideHeight;
            st.gliderVel = fr.tangent * st.loopSpeed;
            st.loopFwd   = fr.tangent;
            st.loopUp    = fr.normal;
            // Keep the flat sim's heading in step, so leaving the loop -- at
            // either end, or halfway up -- carries on the way the craft was
            // actually pointing.
            st.gliderYaw   = std::atan2(fr.tangent.x, fr.tangent.z);
            st.gliderBank  = 0.0f;
            // Nose-up is negative pitch here (see targetPitch below), and the
            // frame's angle runs 0..2pi without wrapping, so the model rolls right
            // through the inversion instead of flipping at the top.
            st.gliderPitch = -glm::degrees(fr.pitch);

            const bool ranOut = st.loopArc >= lp.length || st.loopArc <= 0.0f;
            if (ranOut || hold < 0.0f || st.loopSpeed < 1.0f) {
                st.loopIndex = -1;
                if (!ranOut) st.loopExitFlash = 1.2f;  // thrown off mid-turn
                // Hand the craft back to the flight sim carrying the velocity it
                // had, so being thrown off the top is a real fall and finishing
                // the turn is a clean exit at speed.
                velH = glm::vec3(st.gliderVel.x, 0.0f, st.gliderVel.z);
                st.loopFwd = glm::vec3(std::sin(st.gliderYaw), 0.0f,
                                       std::cos(st.gliderYaw));
                st.loopUp  = glm::vec3(0.0f, 1.0f, 0.0f);
                // Fold the accumulated turn back into (-180, 180]: a full loop
                // leaves pitch at -360, and easing THAT back to level would spin
                // the craft a whole extra revolution. The pre-step snapshot moves
                // with it, or the render would lerp across the fold for a frame.
                st.gliderPitch = std::remainder(st.gliderPitch, 360.0f);
                st.prevPitch   = st.gliderPitch;
            }
            continue;   // the flat sim does not get a say while on a loop
        }

        // A wreck coasts: no thrust reaches it, so the only thing left is drag
        // heavy enough that it comes to rest instead of gliding off into the
        // scenery while the board is up.
        if (st.energyOut) velH *= glm::max(0.0f, 1.0f - 2.2f * env.kSimH);

        const float hs = glm::length(velH);
        if (hs > effMax) velH *= effMax / hs;
        st.gliderVel.x = velH.x; st.gliderVel.z = velH.z;
        st.gliderPos.x += st.gliderVel.x * env.kSimH;
        st.gliderPos.z += st.gliderVel.z * env.kSimH;

        // --- Crashing into the scenery ---------------------------------------
        // Tested against the pose just integrated, so what is resolved here is
        // the overlap the craft actually flew into -- and on the fixed clock,
        // which is also what stops a fast craft tunnelling through a thin wall
        // between two frames. Other racers count too (a nudge is free, a
        // T-bone is not); everything else is judged by isSolidObstacle.
        if (!frozen && !st.energyOut) {
            const float headY = st.gliderPos.y + gc->rideHeight * 0.5f;
            for (const Entity& oe : env.entities) {
                // Bounding-sphere rejection first, and deliberately: it costs a
                // few multiplies, while everything behind it (a blacklist of
                // twenty dynamic_casts, a turned-box test, a walk up the parent
                // chain) is expensive per entity -- and this runs over the whole
                // scene on every fixed step.
                const glm::vec3 d = oe.center - st.gliderPos;
                const float reach = glm::length(oe.half) + gc->hullRadius + 2.5f;
                if (glm::dot(d, d) > reach * reach) continue;
                if (passThrough.count(oe.id)) continue;   // part of a gate/pad prefab
                if (!isSolidObstacle(oe)) continue;
                if (partOfCraft(env.document, oe, env.driveGliderId)) continue;
                const bool racer = oe.components.get<OpponentComponent>() != nullptr;
                glm::vec3 n(0.0f);
                float     depth = 0.0f;
                if (!hullHitsBox(oe, st.gliderPos, gc->hullRadius, headY, racer,
                                 n, depth))
                    continue;
                st.gliderPos += n * depth;      // out of the object it is inside
                applyImpact(st, *gc, velH, n, env);
                if (st.energy <= 0.0f) break;   // the loss is settled above
            }
        }

        // Hover: a spring-damper holds the body centre a ride height above the
        // ground under it; gravity takes over when launched well above the band
        // (flying off a ledge), and it never sinks through the surface.
        const float ground = env.gliderGround(st.gliderPos.x, st.gliderPos.z,
                                              st.gliderPos.y + gc->rideHeight,
                                              env.driveGliderId);
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

        // --- Mounting a loop -------------------------------------------------
        // Only near a loop's ENTRY, and only heading into it. A loop leaves the
        // ground tangentially, so a craft on the flat road arrives on its first
        // metres with no lip to climb -- but the turn also passes back over its
        // own start, and without the arc test the craft would be snatched onto
        // the top of it while driving underneath.
        st.loopUp  = glm::vec3(0.0f, 1.0f, 0.0f);
        st.loopFwd = glm::vec3(std::sin(st.gliderYaw), 0.0f, std::cos(st.gliderYaw));
        if (!frozen && !st.raceFinished && !st.energyOut && st.loopExitFlash <= 0.0f)
            for (std::size_t li = 0; li < lps.size(); ++li) {
                float arc = 0.0f, lat = 0.0f, hgt = 0.0f;
                if (!roadloop::locate(lps[li], st.gliderPos, halfW, arc, lat, hgt))
                    continue;
                if (arc > 8.0f && arc < lps[li].length - 8.0f) continue; // not at an end
                const roadloop::Frame lf = roadloop::at(lps[li], arc);
                const glm::vec3 v = st.gliderVel;
                if (glm::dot(glm::vec3(v.x, 0.0f, v.z), lf.tangent) <= 0.0f) continue;
                st.loopIndex = static_cast<int>(li);
                st.loopArc   = (arc <= 8.0f) ? arc : 0.0f;
                st.loopSide  = glm::clamp(lat, -halfW + 0.4f, halfW - 0.4f);
                st.loopSpeed = glm::length(glm::vec2(v.x, v.z));
                break;
            }
    }

    // Render pose: blend pre-/post-step state (continuous values -> plain lerp).
    const glm::vec3 rPos   = glm::mix(st.prevPos,   st.gliderPos,   env.simAlpha);
    const float     rYaw   = glm::mix(st.prevYaw,   st.gliderYaw,   env.simAlpha);
    const float     rBank  = glm::mix(st.prevBank,  st.gliderBank,  env.simAlpha);
    const float     rPitch = glm::mix(st.prevPitch, st.gliderPitch, env.simAlpha);

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
    // (stays sharp) and drive its length by airspeed. This is about the CRAFT,
    // which is why it stays here now that the eye has moved out -- the camera
    // itself is the craft's own camera entity's business (see CameraSystem).
    st.blurAnchorWorld = rPos; st.blurAnchorValid = true;
    st.blurSpeed01 = glm::clamp(airspeed / glm::max(gc->maxSpeed, 1.0f),
                                0.0f, 1.4f);
}

void updateOpponents(RaceState& st, const RaceEnv& env, RaceState* st2) {
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
    // Cross-fall per sample. A racer runs OFF the centreline, so on a banked
    // stretch the height the centreline reports is not the height under it.
    const std::vector<float>& cb = env.road.centerlineBank();
    const bool haveBank = cb.size() == n;
    const std::size_t segs = env.road.closed ? n : n - 1;
    float total = 0.0f;
    for (std::size_t i = 0; i < segs; ++i)
        total += glm::length(cl[(i + 1) % n] - cl[i]);
    // Position + tangent at arc-length `s` (wrapped/clamped), and -- when asked
    // for -- the road surface height there.
    auto sampleAt = [&](float s, glm::vec2& pos, glm::vec2& dir, float* outY = nullptr,
                        float* outBank = nullptr) {
        if (outBank) *outBank = 0.0f;
        if (total < 1e-3f) {
            pos = cl[0]; dir = glm::vec2(0.0f, 1.0f);
            if (outY && haveY) *outY = cy[0];
            if (outBank && haveBank) *outBank = cb[0];
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
                if (outBank && haveBank) *outBank = glm::mix(cb[i], cb[(i + 1) % n], t);
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
        // Sat out of this session (unticked, or a time trial): not on the grid,
        // not in the standings, not in anyone's mirrors. Checked here rather than
        // by deactivating the entity so the sim reads the same in the editor,
        // where nothing has been deactivated.
        if (!op->entered || !e.activeInHierarchy) continue;
        // Flown by player two: a human has the controls, so the spline lets go.
        // Checked here rather than by clearing the opponent's `entered` tick --
        // that is authored state, it is what the start grid reads, and turning
        // it off would delete the craft from the field the next time the race
        // lines up.
        if (env.playerGliderId2 >= 0 && e.id == env.playerGliderId2) continue;
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
        float lineBank = 0.0f;
        sampleAt(op->dist, pos, dir, &surfY, &lineBank);
        const glm::vec2 perp(dir.y, -dir.x);          // right of travel
        const glm::vec2 p = pos + perp * op->laneCur;
        if (!haveY) surfY = env.streamer.heightAt(p.x, p.y);
        // On a banked stretch the carriageway under this racer is `lane *
        // tan(bank)` off the centreline the sample reported. Without it a whole
        // field floats over the outside of a banked corner and cuts into the
        // inside. Positive bank drops the right edge and laneCur is positive to
        // the right, hence the minus -- the same sign the road's own height query
        // uses.
        if (haveY && lineBank != 0.0f)
            surfY -= op->laneCur * std::tan(glm::radians(lineBank));
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
    // The two humans go in the same way, told apart by entity id: player one
    // carries -1 (it has no craft of its own as far as the field is concerned),
    // player two the craft it flies. That id is what lets the list be handed to
    // the other pane below with the roles swapped.
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
    if (st2) {
        const Entity* e2 = env.document.find(env.playerGliderId2);
        RaceState::Standing s;
        s.id       = env.playerGliderId2;
        s.name     = (e2 && !e2->name.empty()) ? e2->name : "Player 2";
        s.lap      = st2->raceLap;
        s.progress = static_cast<float>(st2->raceLap) * total +
                     lapPos(projDist(glm::vec2(st2->gliderPos.x, st2->gliderPos.z)));
        s.bestLap  = st2->bestLap;
        s.lastLap  = st2->lastLap;
        s.finished = st2->raceFinished;
        s.totalTime = st2->raceClock;
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

    // The same order, seen from the other seat. Both panes show one field, but
    // each player is "YOU" in their own and a named rival in the other's --
    // which is a property of the READER, not of the race, so it is done by
    // copying the finished list and swapping the two humans' roles rather than
    // by ranking anything twice.
    if (st2) {
        st2->standings = st.standings;
        st2->playerPlace = 0;
        const Entity* e1 = env.document.find(env.driveGliderId);
        for (std::size_t i = 0; i < st2->standings.size(); ++i) {
            RaceState::Standing& s = st2->standings[i];
            if (s.id == env.playerGliderId2) {          // player two reads itself
                s.isPlayer = true;
                s.name     = "YOU";
                st2->playerPlace = static_cast<int>(i) + 1;
            } else if (s.id == -1) {                    // ...and player one by name
                s.isPlayer = false;
                s.name = (e1 && !e1->name.empty()) ? e1->name : "Player 1";
            }
        }
        st2->raceOver = st.raceOver;
    }

    // Winner: the first racer over the last lap's line. Both players' finishes
    // are resolved in updateGlider earlier this frame, so they are checked
    // before the field. If the two cross on the SAME frame player one takes it;
    // splitting a tie finer than the fixed step would be pretending to a
    // precision the race does not have.
    if (st.winnerName.empty()) {
        const Entity* e2 = st2 ? env.document.find(env.playerGliderId2) : nullptr;
        if (haveP && st.raceFinished) {
            st.winnerName = "YOU"; st.winnerIsPlayer = true; st.winnerTime = st.raceClock;
            st.winnerId = -1;
        } else if (st2 && st2->raceFinished) {
            st.winnerName = (e2 && !e2->name.empty()) ? e2->name : "Player 2";
            st.winnerIsPlayer = false;         // ...from player one's seat
            st.winnerTime = st2->raceClock;
            st.winnerId = env.playerGliderId2;
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
                st.winnerId = firstE->id;
            }
        }
    }
    // The result, read from the other seat: same winner, but "did I win" is a
    // different question over there -- and one the id answers outright.
    if (st2) {
        st2->winnerId       = st.winnerId;
        st2->winnerTime     = st.winnerTime;
        st2->winnerIsPlayer = (st.winnerId == env.playerGliderId2);
        st2->winnerName     = st2->winnerIsPlayer ? "YOU" : st.winnerName;
    }
}

} // namespace racesim
