#pragma once

#include <map>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "SceneTypes.hpp"   // Entity

namespace fitzel { class PhysicsWorld; }
using PhysicsBodyId = unsigned int;

// --- Things that hang, and swing ---------------------------------------------
// A hook on a chain from the ceiling, a lamp on its cable, a sign on two rings,
// a punch bag: give the object -- or the group the chain and the hook are in --
// a Swing component, and in Play it hangs from a point and swings round it when
// it is hit. The point is the top of all that hangs (where a chain meets the
// ceiling) unless the component names one; the weight sits at the middle of
// all of it, chain and hook together, weighted by their size. A shot (game.applyImpulse, with the point it struck) pushes it;
// gravity brings it back; damping lets it settle. What hangs below the object
// in the hierarchy swings with it.
//
// It is a rigid pendulum, not a rope: the object turns about its pivot as one
// piece, which is what a hook on a short chain, a lamp or a sign does -- and
// what can be worked out (the period of a pendulum is arithmetic, see
// swingcheck). It swings in any direction and may twist about its own cable
// when struck off-centre; the twist has no gravity to bring it back and only
// the damping settles it, like the real thing.
//
// To be hit at all it needs a collider that goes where it goes. Each part of
// the swinging object gets a moving (kinematic) box the size of its own box,
// led along every frame -- registered as that part's body, so a ray that
// strikes it names the part and the impulse finds its way up to the Swing.
// A swinging part is therefore never part of a model's static collider (see
// addGroupBodies), and never receives a decal (one would stay in the air).
namespace swing {

// One pendulum's motion: how it is turned from where it hung at rest, and how
// fast it is turning (world space, rad/s).
struct State {
    glm::quat q{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 w{0.0f};
};

// Advance by `dt`: `arm0` is the vector from the pivot to the object's middle at
// rest (world), gravity pulls the middle down, `damping` (1/s) takes the motion
// out, and the arm never leans further than `maxAngleDeg` from where it hung.
void step(State& s, const glm::vec3& arm0, float damping, float maxAngleDeg, float dt);
// An impulse `j` (N s) at `r` from the pivot (world), on a body of `mass` kg
// whose middle is at `arm0` from the pivot at rest.
void kick(State& s, const glm::vec3& arm0, float mass, const glm::vec3& r, const glm::vec3& j);
// The whole turn about `pivot` as one matrix: what the rest transform of every
// swinging part is multiplied by.
glm::mat4 turnAbout(const glm::vec3& pivot, const glm::quat& q);

class System {
public:
    // At Play start, once the physics world has its bodies: every Swing object's
    // rest pose, and a moving box for each of its parts (into `bodies`, as each
    // part's own body).
    void begin(const std::vector<Entity>& entities, fitzel::PhysicsWorld& physics,
               std::map<int, PhysicsBodyId>& bodies);
    // Push what object `id` belongs to, if it swings: impulse `j`, struck at
    // `at` (world; null = its middle). False when nothing there swings.
    bool kick(const std::vector<Entity>& entities, int id, const glm::vec3& j, const glm::vec3* at);
    // Once a frame, before the physics step: the swing moved on, the objects put
    // where it took them, their boxes led after them.
    void update(std::vector<Entity>& entities, float dt, fitzel::PhysicsWorld* physics);
    void clear() { m_items.clear(); }
    bool swings(int rootId) const { return m_items.count(rootId) != 0; }

private:
    struct Part {
        int           id = -1;
        glm::mat4     rest{1.0f};   // its world transform at rest
        PhysicsBodyId body = 0;
    };
    struct Item {
        glm::vec3         pivot{0.0f};   // world
        glm::vec3         arm0{0.0f};    // pivot -> middle at rest
        glm::mat4         rest{1.0f};    // the root's world transform at rest
        float             mass = 5.0f, damping = 0.3f, maxAngle = 75.0f;
        State             state;
        std::vector<Part> parts;         // root included
    };
    std::unordered_map<int, Item> m_items;   // by the root's id
};

} // namespace swing
