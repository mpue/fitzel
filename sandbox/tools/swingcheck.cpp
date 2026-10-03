// swingcheck -- things that hang, and swing (Swing.hpp), measured.
//
// A pendulum is arithmetic, so the swing is checked against it: hanging still
// it stays still; pulled aside it swings with the period of a pendulum of its
// length; the damping takes the swing out at the rate it says; a push at the
// middle starts the swing a pendulum would; it never leans past its limit
// however hard it is hit; struck off-centre it twists, and the twist settles;
// the arm keeps its length; and the turn about the pivot leaves the pivot
// where it is. No GL, no physics world.
//
//   build/release/bin/swingcheck.exe

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <fitzel/physics/Physics.hpp>

#include "../src/Component.hpp"
#include "../src/SceneGraph.hpp"
#include "../src/Swing.hpp"

namespace {

int failures = 0;
void check(bool ok, const std::string& what, const std::string& detail = {}) {
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what.c_str(), detail.empty() ? "" : "  -- ",
                detail.c_str());
    if (!ok) ++failures;
}

// How far the arm leans from straight down, in degrees.
float lean(const swing::State& s, const glm::vec3& arm0) {
    const glm::vec3 d = glm::normalize(s.q * arm0);
    return glm::degrees(std::acos(std::clamp(glm::dot(d, glm::normalize(arm0)), -1.0f, 1.0f)));
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const glm::vec3 arm(0.0f, -1.0f, 0.0f);   // a metre below its pivot
    const float dt = 1.0f / 60.0f;

    {
        swing::State s;
        for (int i = 0; i < 600; ++i) swing::step(s, arm, 0.3f, 75.0f, dt);
        check(lean(s, arm) < 1e-3f && glm::length(s.w) < 1e-6f, "hanging still, it stays still");
    }
    {
        // Pulled 5 degrees aside, no damping: the period of a 1 m pendulum is 2.006 s.
        swing::State s;
        s.q = glm::angleAxis(glm::radians(5.0f), glm::vec3(0.0f, 0.0f, 1.0f));
        float t = 0.0f, prevX = (s.q * arm).x, first = -1.0f, third = -1.0f;
        int crossings = 0;
        const float h = 1.0f / 600.0f;
        while (t < 10.0f && third < 0.0f) {
            swing::step(s, arm, 0.0f, 75.0f, h);
            t += h;
            const float x = (s.q * arm).x;
            if ((x < 0.0f) != (prevX < 0.0f)) {
                ++crossings;
                if (crossings == 1) first = t;
                if (crossings == 3) third = t;
            }
            prevX = x;
        }
        const float period = third - first;
        check(std::abs(period - 2.006f) < 0.01f, "it swings with the period of a pendulum of its length",
              std::to_string(period) + " s (want 2.006)");
        check(std::abs(glm::length(s.q * arm) - 1.0f) < 1e-4f, "...and the arm keeps its length");
    }
    {
        // Damping 0.5/s: the swing's peaks fall as exp(-0.5 t / 2) -- the
        // amplitude at half the rate the velocity is damped.
        swing::State s;
        s.q = glm::angleAxis(glm::radians(10.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        float prev2 = lean(s, arm), prev1 = prev2, t = 0.0f, t1 = -1.0f, p1 = 0.0f, t2 = -1.0f, p2 = 0.0f;
        for (int i = 0; i < 600; ++i) {
            swing::step(s, arm, 0.5f, 75.0f, dt);
            t += dt;
            const float now = lean(s, arm);
            if (prev1 > prev2 && prev1 >= now && prev1 > 0.5f) {   // a peak, one step ago
                if (t1 < 0.0f && t > 0.5f) { t1 = t - dt; p1 = prev1; }
                else if (t1 >= 0.0f && t - dt - t1 > 3.0f && t2 < 0.0f) { t2 = t - dt; p2 = prev1; }
            }
            prev2 = prev1;
            prev1 = now;
        }
        const float want = std::exp(-0.5f * (t2 - t1) / 2.0f), got = t2 > 0.0f ? p2 / p1 : 0.0f;
        check(t2 > 0.0f && std::abs(got - want) < 0.03f, "the damping takes the swing out at the rate it says",
              std::to_string(got) + " of the swing left after " + std::to_string(t2 - t1) + " s (want " + std::to_string(want) + ")");
    }
    {
        // A push of 2 N s, square to the arm at its middle, on 5 kg: w = 0.4 rad/s,
        // and the swing reaches w / sqrt(g/L) = 7.3 degrees.
        swing::State s;
        swing::kick(s, arm, 5.0f, arm, glm::vec3(2.0f, 0.0f, 0.0f));
        check(std::abs(glm::length(s.w) - 0.4f) < 1e-4f, "a push starts the turn a pendulum would", std::to_string(glm::length(s.w)) + " rad/s");
        float peak = 0.0f;
        for (int i = 0; i < 120; ++i) {
            swing::step(s, arm, 0.0f, 75.0f, dt);
            peak = std::max(peak, lean(s, arm));
        }
        const float want = glm::degrees(0.4f / std::sqrt(9.81f));
        check(std::abs(peak - want) < 0.15f, "...and swings out as far as one",
              std::to_string(peak) + " deg (want " + std::to_string(want) + ")");
        swing::State d;
        swing::kick(d, arm, 5.0f, arm, glm::vec3(2.0f, 0.0f, 0.0f));
        for (int i = 0; i < 20; ++i) swing::step(d, arm, 0.0f, 75.0f, dt);
        check((d.q * arm).x > 0.05f, "...in the direction it was pushed");
    }
    {
        swing::State s;
        swing::kick(s, arm, 1.0f, arm, glm::vec3(500.0f, 0.0f, 0.0f));
        float peak = 0.0f;
        for (int i = 0; i < 300; ++i) {
            swing::step(s, arm, 0.3f, 40.0f, dt);
            peak = std::max(peak, lean(s, arm));
        }
        check(peak <= 40.01f, "however hard it is hit, it never leans past its limit", std::to_string(peak) + " deg");
    }
    {
        // Struck at the side of a 0.4 m wide thing, across it: it twists about its
        // cable -- and the twist settles.
        swing::State s;
        swing::kick(s, arm, 5.0f, arm + glm::vec3(0.0f, 0.0f, 0.2f), glm::vec3(2.0f, 0.0f, 0.0f));
        check(std::abs(s.w.y) > 0.05f, "struck off-centre it twists about its cable", std::to_string(s.w.y) + " rad/s");
        for (int i = 0; i < 1800; ++i) swing::step(s, arm, 0.5f, 75.0f, dt);
        check(glm::length(s.w) < 1e-3f, "...and the twist settles with the swing");
    }
    {
        const glm::vec3 pivot(3.0f, 5.0f, -2.0f);
        const glm::mat4 m = swing::turnAbout(pivot, glm::angleAxis(0.7f, glm::normalize(glm::vec3(1.0f, 0.2f, 0.3f))));
        const glm::vec3 p(m * glm::vec4(pivot, 1.0f));
        check(glm::length(p - pivot) < 1e-5f, "the turn leaves the pivot where it is");
    }

    // --- In a scene, with a physics world: a hook on a chain ---------------------------------
    {
        // The chain (with the Swing) hangs 1 m from a ceiling at 3 m, the hook on it.
        std::vector<Entity> ents(2);
        ents[0].id = 10; ents[0].name = "Chain"; ents[0].type = EntityType::Box;
        ents[0].localCenter = glm::vec3(0.0f, 2.5f, 0.0f);
        ents[0].half = glm::vec3(0.05f, 0.5f, 0.05f);
        ents[0].components.items.push_back(std::make_unique<SwingComponent>());
        ents[1].id = 11; ents[1].name = "Hook"; ents[1].type = EntityType::Box; ents[1].parent = 10;
        ents[1].localCenter = glm::vec3(0.0f, -0.65f, 0.0f);
        ents[1].half = glm::vec3(0.15f, 0.15f, 0.15f);
        scenegraph::resolve(ents);
        fitzel::PhysicsWorld world;
        std::map<int, PhysicsBodyId> bodies;
        swing::System sys;
        sys.begin(ents, world, bodies);
        check(sys.swings(10) && bodies.count(10) && bodies.count(11), "the chain and the hook each get a moving box");
        // A shot from the side at the hook's height names the hook.
        glm::vec3 hp, hn;
        PhysicsBodyId hitBody = 0;
        const bool hit = world.castRay(glm::vec3(-4.0f, 1.85f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 10.0f, hp, hn, hitBody);
        check(hit && hitBody == bodies[11], "a shot at the hook hits the hook's box");
        // The shot pushes it (on the hook, it swings the whole chain).
        const glm::vec3 j(4.0f, 0.0f, 0.0f);
        check(sys.kick(ents, 11, j, &hp), "the hook passes the push up to what swings");
        const glm::vec3 hook0 = ents[1].center;
        float most = 0.0f;
        for (int i = 0; i < 40; ++i) {
            sys.update(ents, 1.0f / 60.0f, &world);
            world.step(1.0f / 60.0f);
            scenegraph::resolve(ents);
            most = std::max(most, ents[1].center.x - hook0.x);
        }
        check(most > 0.1f, "the hook swings away from the shot, on its chain", std::to_string(most) + " m");
        glm::vec3 bp;
        glm::quat bq;
        world.getTransform(bodies[11], bp, bq);
        check(glm::length(bp - ents[1].center) < 0.05f, "...and its box goes with it, for the next shot",
              std::to_string(glm::length(bp - ents[1].center)) + " m apart");
        check(std::abs(glm::distance(ents[0].center, glm::vec3(0.0f, 3.0f, 0.0f)) - 0.5f) < 1e-3f,
              "the chain still hangs from the ceiling");
        // The same push, swinging chain and hook together: no wild throw from a
        // pistol -- the hook goes out a few dozen centimetres, not most of a metre.
        check(most < 0.5f, "a shot moves the chain and the hook together, as their weight says",
              std::to_string(most) + " m");
    }
    {
        // A group with no box of its own (an empty), the chain and hook below it:
        // it hangs from the top of the chain, wherever the empty itself stands.
        std::vector<Entity> ents(3);
        ents[0].id = 20; ents[0].type = EntityType::Empty; ents[0].localCenter = glm::vec3(5.0f, 0.0f, 0.0f);
        ents[0].components.items.push_back(std::make_unique<SwingComponent>());
        ents[1].id = 21; ents[1].type = EntityType::Box; ents[1].parent = 20;
        ents[1].localCenter = glm::vec3(0.0f, 2.5f, 0.0f); ents[1].half = glm::vec3(0.05f, 0.5f, 0.05f);
        ents[2].id = 22; ents[2].type = EntityType::Box; ents[2].parent = 20;
        ents[2].localCenter = glm::vec3(0.0f, 1.85f, 0.0f); ents[2].half = glm::vec3(0.15f);
        scenegraph::resolve(ents);
        fitzel::PhysicsWorld world;
        std::map<int, PhysicsBodyId> bodies;
        swing::System sys;
        sys.begin(ents, world, bodies);
        sys.kick(ents, 22, glm::vec3(0.0f, 0.0f, 4.0f), nullptr);
        for (int i = 0; i < 30; ++i) {
            sys.update(ents, 1.0f / 60.0f, &world);
            scenegraph::resolve(ents);
        }
        const float top = glm::distance(ents[1].center + glm::vec3(0.0f), glm::vec3(5.0f, 3.0f, 0.0f));
        check(std::abs(top - 0.5f) < 1e-3f && std::abs(ents[2].center.z) > 0.05f && !bodies.count(20),
              "a group swings from the top of the chain below it, and gets no box of its own");
    }

    std::printf("\nswingcheck: %s (%d failed)\n", failures ? "FAILED" : "all passed", failures);
    return failures ? 1 : 0;
}
