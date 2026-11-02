#pragma once

#include <functional>
#include <map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include "HullRide.hpp"
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
//
// A HULL is the third kind: an object with a Buoyancy component and no dynamic
// Physics -- a ship. It is not thrown about by the solver (see Buoyancy.hpp for
// the bodies that are) but led over the sea (HullRide.hpp): it heaves, pitches
// and rolls with the waves under its waterline, and everything on it -- the
// player on its deck included -- goes along. With static Physics on an imported
// model's group root, every part of the model collides as its own triangles,
// moving with it, so the deck is walked on where it is drawn. Sails (Sail
// component) are cloth and stay out.
namespace platforms {

// What the system cannot know by itself; main fills it in. Every member may be
// left unset: then model parts collide as their boxes and a hull rests.
struct World {
    // A moving collider for a solid model part at world `w`: its own triangles
    // (addKinematicModelBody). 0 where it has none.
    std::function<PhysicsBodyId(const Entity& part, const glm::mat4& w)> modelCollider;
    // A part's surface as world points (a model's vertices), for where a hull's
    // waterline is. Unset or left empty: the corners of its box.
    std::function<void(const Entity& part, std::vector<glm::vec3>& out)> surfacePoints;
    // The open water: its height at (x, z) now, and its still level. A level at
    // or below -500 is no sea at all.
    std::function<float(float x, float z)> seaAt;
    float seaLevel = -1000.0f;
};

// Does this object ride the sea as a hull (see above)?
bool isHull(const Entity& e);

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
               std::map<int, PhysicsBodyId>& bodies, const World* world = nullptr);
    // Once a frame, before the physics step: every clock moved on by `dt`, the
    // objects put where it took them, their colliders led after them. An object
    // that gained a Mover or Spinner during Play (a spawned one) starts moving
    // from where it is, without colliders.
    void update(std::vector<Entity>& entities, float dt, fitzel::PhysicsWorld* physics,
                const World* world = nullptr);
    void clear() { m_items.clear(); m_known.clear(); }

    // A hull's ride (its waterplane and how it is moving), or null: for
    // whoever wants to know how far a ship is rolling (oceancheck, a script).
    const ocean::HullRide* hullRide(int id) const;

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
        // A hull (isHull): its world at rest and how it rides.
        bool              hull = false;
        glm::mat4         home{1.0f};
        ocean::HullRide   ride;
    };
    // A hull's waterplane, from what it carries (Platforms.cpp).
    static void shapeHull(Item& it, const std::vector<Entity>& entities, const World* world);
    std::vector<Item>       m_items;   // outer before inner
    std::unordered_set<int> m_known;
};

} // namespace platforms
