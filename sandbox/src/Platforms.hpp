#pragma once

#include <map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include "SceneTypes.hpp"   // Entity

namespace fitzel { class PhysicsWorld; }
class MoverComponent;
class SpinnerComponent;
using PhysicsBodyId = unsigned int;

// --- Things that move on their own: platforms, lifts, pistons, turntables ----
// A Mover takes an object back and forth along its offset, a Spinner turns it
// about its axis or rocks it; an object may have both. Each frame, before the
// physics step, the object is put where its clock says, and its children go
// with it through the hierarchy.
//
// What collides goes along: the object itself and every part below it with a
// STATIC Physics component get a moving (kinematic) collider instead of the
// static body they would otherwise have -- a box the size of the part, or a
// cylinder for a cylinder -- led there for the step. That is what lets a lift
// carry a ball up, a piston shove it off the edge and a turning arm sweep it
// away; a body standing on one is carried by its friction. Each collider is
// registered as its part's body, so a ray that strikes it names the part (a
// rolling ball can ask what it is standing on). A part with a dynamic Physics
// component keeps its own body and falls off when it is not held.
//
// Something that moves below something that moves (a turntable on a moving
// floor) is carried by its parent and does its own moving on top.
namespace platforms {

// The ids of everything a Mover or Spinner takes along: the objects with one,
// and every part below them. Their static colliders are the moving ones
// System::begin makes, so the static world must leave them out.
std::unordered_set<int> carriedIds(const std::vector<Entity>& entities);

// How far along its offset a Mover is, `t` seconds into Play: 0 at the start,
// 1 at the far end, eased at both, resting `pause` at each.
float moverAt(const MoverComponent& mv, double t);
// How far a Spinner has turned, `t` seconds into Play, in degrees.
float spinnerAt(const SpinnerComponent& sp, double t);

class System {
public:
    // At Play start, once the world transforms are resolved: every moving
    // object's start pose, and a moving collider for each of its solid parts
    // (into `bodies`, as each part's own body).
    void begin(const std::vector<Entity>& entities, fitzel::PhysicsWorld& physics,
               std::map<int, PhysicsBodyId>& bodies);
    // Once a frame, before the physics step: every clock moved on by `dt`, the
    // objects put where it took them, their colliders led after them. An object
    // that gained a Mover or Spinner during Play (a spawned one) starts moving
    // from where it is, without colliders.
    void update(std::vector<Entity>& entities, float dt, fitzel::PhysicsWorld* physics);
    void clear() { m_items.clear(); m_known.clear(); }

private:
    struct Part {
        int           id = -1;
        glm::mat4     rel{1.0f};   // its world at rest, in the object's frame at rest
        PhysicsBodyId body = 0;
    };
    struct Item {
        int               id = -1;
        int               depth = 0;    // how many moving objects it sits under
        glm::vec3         homePos{0.0f}, homeRot{0.0f};   // local, at Play start
        double            time = 0.0;
        std::vector<Part> parts;
    };
    std::vector<Item>       m_items;   // outer before inner
    std::unordered_set<int> m_known;
};

} // namespace platforms
