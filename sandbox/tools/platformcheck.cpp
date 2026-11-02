// platformcheck -- things that move on their own (Platforms.hpp), measured.
//
// The clocks first: a Mover is at its start, eased out to its far end, rests
// there as long as its pause, comes back and rests again, and its start phase
// shifts all of that; a Spinner turns at its speed, or rocks to its swing and
// back. Then with a real physics world: a crate on a moving platform with a
// static collider is lifted with it; a crate on a turntable goes round with
// it; a piston shoves a crate along; the collider of a part below a turning
// Empty goes round where the part goes; what a Mover takes along gets no
// static body; and a Mover without a collider moves only the picture. Last a
// hull (Buoyancy, no dynamic Physics) on a rolling swell: it heaves, pitches
// and rolls about where it was placed, gently, and the walking player on its
// deck goes with it and stays on it. No GL.
//
//   build/release/bin/platformcheck.exe

#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <fitzel/physics/Physics.hpp>

#include "../src/Component.hpp"
#include "../src/Platforms.hpp"
#include "../src/SceneGraph.hpp"

namespace {

int failures = 0;
void check(bool ok, const std::string& what, const std::string& detail = {}) {
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what.c_str(), detail.empty() ? "" : "  -- ",
                detail.c_str());
    if (!ok) ++failures;
}

std::string num(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.3f", v);
    return b;
}

Entity solid(int id, EntityType type, glm::vec3 at, glm::vec3 half, int parent = -1) {
    Entity e;
    e.id = id;
    e.type = type;
    e.half = half;
    e.parent = parent;
    e.localCenter = at;
    auto pc = std::make_unique<PhysicsComponent>();
    pc->dynamic = false;
    e.components.items.push_back(std::move(pc));
    return e;
}

MoverComponent* addMover(Entity& e, glm::vec3 offset, float duration, float pause = 0.0f) {
    auto m = std::make_unique<MoverComponent>();
    m->offset = offset;
    m->duration = duration;
    m->pause = pause;
    MoverComponent* p = m.get();
    e.components.items.push_back(std::move(m));
    return p;
}

SpinnerComponent* addSpinner(Entity& e, float speed) {
    auto s = std::make_unique<SpinnerComponent>();
    s->speed = speed;
    SpinnerComponent* p = s.get();
    e.components.items.push_back(std::move(s));
    return p;
}

// A play session in miniature: the static world as Play builds it (everything
// with a static Physics component that nothing takes along), the moving
// colliders, a crate dropped somewhere, and frames stepped the way main does.
struct World {
    fitzel::PhysicsWorld physics;
    std::vector<Entity> entities;
    std::map<int, PhysicsBodyId> bodies;
    platforms::System sys;
    PhysicsBodyId crate = 0;
    platforms::World* sea = nullptr;   // a hull's sea, when there is one
    bool walker = false;               // the player capsule is on board
    glm::vec3 foot{0.0f};              // ...and where its feet are
    std::function<void(float)> onTick; // after every frame

    void begin() {
        physics.setGravity(glm::vec3(0.0f, -9.81f, 0.0f));
        scenegraph::resolve(entities);
        const auto carried = platforms::carriedIds(entities);
        for (const Entity& e : entities)
            if (e.components.get<PhysicsComponent>() && !carried.count(e.id))
                bodies[e.id] = physics.addBox(e.half, e.center, glm::quat(glm::radians(e.rotation)), 0.0f);
        sys.begin(entities, physics, bodies, sea);
    }
    void dropCrate(glm::vec3 at) {
        crate = physics.addBox(glm::vec3(0.3f), at, glm::quat(1, 0, 0, 0), 5.0f);
    }
    void run(float seconds) {
        const float dt = 1.0f / 60.0f;
        for (int i = 0; i < static_cast<int>(std::lround(seconds / dt)); ++i) {
            // As main has it: the player walks, the platforms move, the world
            // steps, and what the player stands on takes it along.
            bool onGround = false;
            if (walker) foot = physics.moveCharacter(glm::vec3(0.0f), false, dt, onGround);
            sys.update(entities, dt, &physics, sea);
            physics.step(dt);
            if (walker) { float turn = 0.0f; foot += physics.carryCharacter(turn); }
            scenegraph::resolve(entities);
            if (onTick) onTick(dt);
        }
    }
    glm::vec3 cratePos() const {
        glm::vec3 p;
        glm::quat q;
        physics.getTransform(crate, p, q);
        return p;
    }
    const Entity& find(int id) const {
        for (const Entity& e : entities)
            if (e.id == id) return e;
        return entities.front();
    }
};

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // --- The clocks --------------------------------------------------------
    {
        MoverComponent m;
        m.duration = 4.0f;   // 2 s out, 2 s back
        m.pause = 1.0f;      // a cycle of 6 s
        check(std::abs(platforms::moverAt(m, 0.0)) < 1e-6f, "a mover starts at its start");
        check(std::abs(platforms::moverAt(m, 1.0) - 0.5f) < 1e-4f, "...is halfway at a quarter of its way",
              num(platforms::moverAt(m, 1.0)));
        check(std::abs(platforms::moverAt(m, 2.0) - 1.0f) < 1e-4f &&
                  std::abs(platforms::moverAt(m, 2.9) - 1.0f) < 1e-6f,
              "...rests at its far end for its pause");
        check(std::abs(platforms::moverAt(m, 4.0) - 0.5f) < 1e-4f, "...comes back",
              num(platforms::moverAt(m, 4.0)));
        check(platforms::moverAt(m, 5.5) < 1e-6f && std::abs(platforms::moverAt(m, 7.0) - 0.5f) < 1e-4f,
              "...rests at its start, and goes again");
        m.startPhase = 0.5f;
        check(std::abs(platforms::moverAt(m, 0.0) - 1.0f) < 1e-4f, "a start phase of 0.5 begins at the far end");
        MoverComponent old;   // what a scene from before pause/startPhase gets
        old.duration = 3.0f;
        bool same = true;
        for (float t = 0.0f; t < 7.0f; t += 0.1f)
            same = same && std::abs(platforms::moverAt(old, t) -
                                    (0.5f - 0.5f * std::cos(6.2831853f * t / 3.0f))) < 1e-4f;
        check(same, "with no pause it moves as Movers always did (one cosine)");

        SpinnerComponent s;
        s.speed = 45.0f;
        check(std::abs(platforms::spinnerAt(s, 2.0) - 90.0f) < 1e-3f, "a spinner turns at its speed");
        s.startPhase = 0.25f;
        check(std::abs(platforms::spinnerAt(s, 2.0) - 180.0f) < 1e-3f, "...from where its phase says");
        SpinnerComponent r;
        r.swing = 10.0f;
        r.period = 4.0f;
        check(std::abs(platforms::spinnerAt(r, 1.0) - 10.0f) < 1e-3f &&
                  std::abs(platforms::spinnerAt(r, 3.0) + 10.0f) < 1e-3f &&
                  std::abs(platforms::spinnerAt(r, 4.0)) < 1e-3f,
              "a rocking one goes to its swing each way and back in its period");
    }

    // --- A lift --------------------------------------------------------------
    {
        World w;
        w.entities.push_back(solid(1, EntityType::Box, {0, -0.5f, 0}, {10, 0.5f, 10}));   // the ground
        Entity lift = solid(2, EntityType::Box, {0, 0.25f, 0}, {1.5f, 0.25f, 1.5f});
        // 2 s each way, 2 s at each end; it begins with its rest at the bottom.
        addMover(lift, {0, 5, 0}, 4.0f, 2.0f)->startPhase = 0.75f;
        w.entities.push_back(std::move(lift));
        w.begin();
        check(w.bodies.count(2) == 1, "a mover with a static collider gets a moving one");
        w.dropCrate({0, 0.82f, 0});
        w.run(1.0f);
        const glm::vec3 low = w.cratePos();
        w.run(3.5f);   // the rest of its rest, 2 s up, half its pause at the top
        const glm::vec3 top = w.cratePos();
        check(std::abs(top.y - 5.8f) < 0.08f, "a crate on a lift is lifted with it", num(top.y) + " m (want 5.8)");
        check(glm::length(glm::vec2(top.x, top.z)) < 0.05f, "...and stays where it stood on it");
        check(std::abs(w.find(2).center.y - 5.25f) < 1e-3f, "...and the lift is drawn where its collider is");
        w.run(5.0f);   // down and resting
        check(std::abs(w.cratePos().y - low.y) < 0.08f, "...and brought down again", num(w.cratePos().y));
    }

    // --- A turntable ------------------------------------------------------------
    {
        World w;
        Entity disc = solid(1, EntityType::Cylinder, {0, -0.25f, 0}, {5, 0.25f, 5});
        addSpinner(disc, 30.0f);
        w.entities.push_back(std::move(disc));
        w.begin();
        w.dropCrate({3, 0.31f, 0});
        w.run(0.5f);
        const glm::vec3 a = w.cratePos();
        w.run(3.0f);   // 90 degrees more
        const glm::vec3 b = w.cratePos();
        const float turned = glm::degrees(std::atan2(a.x * b.z - a.z * b.x, a.x * b.x + a.z * b.z));
        const float r = glm::length(glm::vec2(b.x, b.z));
        check(std::abs(std::abs(turned) - 90.0f) < 6.0f, "a crate on a turntable goes round with it",
              num(turned) + " degrees in 3 s (want 90)");
        check(std::abs(r - 3.0f) < 0.3f, "...at the same distance from the middle", num(r) + " m");
    }

    // --- A piston ----------------------------------------------------------------
    {
        World w;
        w.entities.push_back(solid(1, EntityType::Box, {0, -0.5f, 0}, {10, 0.5f, 10}));
        Entity piston = solid(2, EntityType::Box, {3, 0.5f, 0}, {0.5f, 0.45f, 0.9f});
        // Half a second out and back, a second at each end: it begins resting home.
        addMover(piston, {-3, 0, 0}, 1.0f, 1.0f)->startPhase = 2.0f / 3.0f;
        w.entities.push_back(std::move(piston));
        w.begin();
        w.dropCrate({1.6f, 0.31f, 0});
        w.run(0.2f);
        const float before = w.cratePos().x;
        w.run(1.2f);
        check(w.cratePos().x < before - 1.5f, "a piston shoves a crate along",
              "from x " + num(before) + " to " + num(w.cratePos().x));
    }

    // --- A part below a turning Empty, and what gets no static body --------------
    {
        World w;
        Entity hub;
        hub.id = 1;
        hub.type = EntityType::Empty;
        hub.localCenter = {0, 0, 0};
        addSpinner(hub, 90.0f);
        w.entities.push_back(std::move(hub));
        w.entities.push_back(solid(2, EntityType::Box, {3, 0.5f, 0}, {0.5f, 0.5f, 0.5f}, 1));
        Entity picture;   // a mover with nothing to collide with
        picture.id = 3;
        picture.type = EntityType::Box;
        picture.half = glm::vec3(0.5f);
        picture.localCenter = {-6, 0, 0};
        addMover(picture, {0, 2, 0}, 2.0f);
        w.entities.push_back(std::move(picture));
        w.entities.push_back(solid(4, EntityType::Box, {0, -5, 0}, {1, 1, 1}));   // nothing to do with it
        const auto carried = platforms::carriedIds(w.entities);
        check(carried.count(1) && carried.count(2) && carried.count(3) && !carried.count(4),
              "what a Mover or Spinner takes along is known, the rest is not");
        w.begin();
        check(w.bodies.count(2) == 1 && w.bodies.count(3) == 0,
              "the part with a collider gets a moving one, the bare picture none");
        w.run(1.0f);   // a quarter turn
        glm::vec3 hit, n;
        PhysicsBodyId body = 0;
        const glm::vec3 at = glm::vec3(w.find(2).center);
        const bool there = w.physics.castRay(at + glm::vec3(0, 5, 0), {0, -1, 0}, 10.0f, hit, n, body) &&
                           body == w.bodies[2];
        const bool left = w.physics.castRay({3, 5, 0}, {0, -1, 0}, 10.0f, hit, n, body) && body == w.bodies[2];
        check(glm::length(glm::vec2(at.x, at.z) - glm::vec2(0, -3)) < 0.1f || glm::length(glm::vec2(at.x, at.z) - glm::vec2(0, 3)) < 0.1f,
              "the part goes round its Empty", num(at.x) + ", " + num(at.z));
        check(there && !left, "...and its collider with it, not left where it was");
        check(std::abs(w.find(3).center.y - 2.0f) < 1e-3f, "a mover with no collider still moves");
    }

    // --- A hull on a swell ---------------------------------------------------
    {
        World w;
        // A barge twenty metres long and six wide, sitting a metre and a half
        // deep in water whose still level is half a metre up.
        Entity hull = solid(1, EntityType::Box, {0, 0, 0}, {3, 1, 10});
        hull.components.items.push_back(std::make_unique<BuoyancyComponent>());
        w.entities.push_back(std::move(hull));
        check(platforms::isHull(w.entities.back()), "Buoyancy without dynamic Physics makes a hull");
        // A swell from ahead and a smaller one from the side: forty-metre
        // waves along the hull, thirty across.
        float t = 0.0f;
        platforms::World sea;
        sea.seaLevel = 0.5f;
        sea.seaAt = [&](float x, float z) {
            return 0.5f + 1.2f * std::sin(6.2831853f * (z / 40.0f - t / 6.0f)) +
                          0.5f * std::sin(6.2831853f * (x / 30.0f - t / 5.0f));
        };
        w.sea = &sea;
        w.begin();
        const ocean::HullRide* ride = w.sys.hullRide(1);
        check(ride && std::abs(ride->half.x - 3.0f) < 0.01f && std::abs(ride->half.y - 10.0f) < 0.01f,
              "its waterplane is the hull's footprint",
              ride ? num(ride->half.x) + " x " + num(ride->half.y) : "none");
        check(ride && std::abs(ride->waterline - 0.5f) < 0.01f && std::abs(ride->draft - 1.5f) < 0.01f,
              "...at the still water, as deep as it sits",
              ride ? num(ride->waterline) + ", draft " + num(ride->draft) : "");
        check(w.bodies.count(1) == 1, "the hull gets a moving collider");
        if (!ride) {
            std::printf("\nFAILED (no ride)\n");
            return 1;
        }
        // The player on deck, two metres aft of the middle.
        w.physics.spawnCharacter(0.3f, 0.6f, glm::vec3(0.0f, 1.02f, -2.0f));
        w.walker = true;
        w.foot = glm::vec3(0.0f, 1.02f, -2.0f);
        const glm::vec3 onDeck(0.0f, 1.0f, -2.0f);   // the hull's frame is the world's at rest
        float heaveLo = 1e9f, heaveHi = -1e9f, pitchMax = 0.0f, rollMax = 0.0f, drift = 0.0f;
        bool finite = true;
        w.onTick = [&](float dt) {
            t += dt;
            if (t < 6.0f) return;   // let it settle into the swell
            heaveLo  = std::min(heaveLo, ride->heave);
            heaveHi  = std::max(heaveHi, ride->heave);
            pitchMax = std::max(pitchMax, std::abs(ride->pitch));
            rollMax  = std::max(rollMax, std::abs(ride->roll));
            finite = finite && std::isfinite(ride->heave + ride->pitch + ride->roll);
            // Where the player stands, in the hull's frame now.
            const glm::mat4 now = scenegraph::worldOf(w.find(1));
            const glm::vec3 local = glm::vec3(glm::inverse(now) * glm::vec4(w.foot, 1.0f));
            drift = std::max(drift, glm::length(glm::vec2(local.x - onDeck.x, local.z - onDeck.z)));
        };
        w.run(30.0f);
        const float deg = 57.29578f;
        check(finite, "the ride stays finite");
        check(heaveHi - heaveLo > 0.6f && heaveHi - heaveLo < 4.0f, "the hull heaves with the swell",
              num(heaveLo) + " .. " + num(heaveHi) + " m");
        check(pitchMax * deg > 1.0f && pitchMax * deg < 15.0f, "...pitches to the waves along it",
              num(pitchMax * deg) + " deg");
        check(rollMax * deg > 0.5f && rollMax * deg < 15.0f, "...and rolls to the ones across it",
              num(rollMax * deg) + " deg");
        const glm::mat4 now = scenegraph::worldOf(w.find(1));
        const glm::vec3 local = glm::vec3(glm::inverse(now) * glm::vec4(w.foot, 1.0f));
        check(drift < 0.35f, "the player on deck rides along where they stood",
              num(drift) + " m off the spot at most");
        check(std::abs(local.y - 1.0f) < 0.15f, "...standing on the deck, not in the sea",
              "feet at " + num(local.y) + " in the hull's frame (deck 1.0)");
        // And something on the sea with a dynamic Physics is no hull.
        Entity box = solid(9, EntityType::Box, {0, 0, 0}, {1, 1, 1});
        box.components.get<PhysicsComponent>()->dynamic = true;
        box.components.items.push_back(std::make_unique<BuoyancyComponent>());
        check(!platforms::isHull(box), "a floating crate (dynamic Physics) is not led as a hull");
    }

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all good", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
