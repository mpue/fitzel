#pragma once

#include <map>
#include <string>
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
// Two strings in a V (a swing seat, a bell on its yoke, a ball of a Newton's
// cradle) let it swing one way only: the component can hold it to turning
// about its own X or Z axis. It can start pulled aside (`startAngle`), so a
// pendulum set up in the editor is already swinging when Play starts, with no
// script to let it go.
//
// Swinging things that `knock` strike one another: each with a ball about its
// heaviest part -- a cradle's steel ball, a punch bag's bag -- and the blow
// between two of them is that of two bodies on their pivots, with their
// `bounce` (how much of the speed they close in with they part with). Blows
// down a row of them are taken one pair at a time, in the same instant, which
// is what hands a cradle's swing through the balls at rest to the last one --
// instead of sharing it out among all of them, as a solver that sees every
// contact at once does (Jolt does: measured, a fifth of the swing was left).
// A bounce under 1 leaves a little behind in every ball the blow goes through,
// and after a while the whole row swings as one -- as a real cradle does.
// They knock only into each other: the static world they hang in is what the
// angle limit stands for.
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
// A non-zero `axis` (world, unit) is the only one it may turn about.
void step(State& s, const glm::vec3& arm0, float damping, float maxAngleDeg, float dt,
          const glm::vec3& axis = glm::vec3(0.0f));
// An impulse `j` (N s) at `r` from the pivot (world), on a body of `mass` kg
// whose middle is at `arm0` from the pivot at rest.
void kick(State& s, const glm::vec3& arm0, float mass, const glm::vec3& r, const glm::vec3& j,
          const glm::vec3& axis = glm::vec3(0.0f));

// One swinging thing as the knocks between them see it.
struct Pendulum {
    glm::vec3 pivot{0.0f};            // world
    glm::vec3 arm0{0.0f, -1.0f, 0.0f}; // pivot -> middle at rest (world): what its weight hangs on
    glm::vec3 axis{0.0f};             // the one it turns about (world, unit); zero = any way
    float     mass = 5.0f, damping = 0.3f, maxAngle = 75.0f;
    // What it strikes with: a ball, its centre at rest (world), and how much
    // of the closing speed it parts with (0 = clay, 1 = steel on steel).
    glm::vec3 ball{0.0f, -1.0f, 0.0f};
    float     radius = 0.1f, bounce = 0.95f;
};
// Where pendulum `p`'s ball is now.
glm::vec3 ballAt(const State& s, const Pendulum& p);
// If the balls of `a` and `b` touch and close in on each other, the blow
// between them: both turned on by it, as their masses and levers say, with the
// mean of their bounces. True when there was one; `speed` (if given) is how
// fast they closed in (m/s) and `at` where they met -- what a click is made of.
bool knock(State& sa, const Pendulum& a, State& sb, const Pendulum& b,
           float* speed = nullptr, glm::vec3* at = nullptr);
// A blow between pendulums `a` and `b` (indices into stepTogether's lists).
struct Knock {
    int       a = -1, b = -1;
    glm::vec3 at{0.0f};
    float     speed = 0.0f;   // m/s they closed in at
};
// Advance pendulums that knock by `dt`, together: each swings as step() swings
// it, and after every short step all blows between them are taken, pair after
// pair, until none closes in any more -- a row of them hands one swing down.
// Each blow goes into `knocks` when given -- a blow handed down a row is one
// per pair, all in the same instant.
void stepTogether(const std::vector<State*>& s, const std::vector<const Pendulum*>& p, float dt,
                  std::vector<Knock>* knocks = nullptr);
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
    // What struck what in the last update(), for the sound of it: the blows of
    // one instant down a row together, as one click -- where they were, the
    // hardest of them, and the sound the striking things name (Knock sound).
    struct Click {
        glm::vec3   at{0.0f};
        float       speed = 0.0f;   // m/s, the hardest blow in it
        std::string sound;
    };
    const std::vector<Click>& clicks() const { return m_clicks; }
    void clear() { m_items.clear(); m_clicks.clear(); }
    bool swings(int rootId) const { return m_items.count(rootId) != 0; }

private:
    struct Part {
        int           id = -1;
        glm::mat4     rest{1.0f};   // its world transform at rest
        PhysicsBodyId body = 0;
    };
    struct Item {
        Pendulum          p;
        bool              knocks = false;
        std::string       sound;         // what a knock of it sounds like
        glm::mat4         rest{1.0f};    // the root's world transform at rest
        State             state;
        std::vector<Part> parts;         // root included
    };
    std::unordered_map<int, Item> m_items;   // by the root's id
    std::vector<Click>            m_clicks;  // the last update()'s
};

} // namespace swing
