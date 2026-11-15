// swingcheck -- things that hang, and swing (Swing.hpp), measured.
//
// A pendulum is arithmetic, so the swing is checked against it: hanging still
// it stays still; pulled aside it swings with the period of a pendulum of its
// length; the damping takes the swing out at the rate it says; a push at the
// middle starts the swing a pendulum would; it never leans past its limit
// however hard it is hit; struck off-centre it twists, and the twist settles;
// the arm keeps its length; and the turn about the pivot leaves the pivot
// where it is. Held to one axis it swings in its plane only; pulled aside at
// the start it swings from there; things that knock trade their swing as two
// balls on strings do, and a Newton's cradle built of balls and strings sends
// out as many balls as were let go, and keeps its swing. No GL; a physics
// world only for the shots.
//
//   build/release/bin/swingcheck.exe

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
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

// A Newton's cradle as a scene builds it: five steel balls in a row, each a
// Sphere with a Swing held to its Z and two strings up to the two bars --
// the first `lifted` of them pulled out to -30 degrees.
struct Cradle {
    std::vector<Entity> ents;
    swing::System       sys;
    fitzel::PhysicsWorld world;
    std::map<int, PhysicsBodyId> bodies;
    static constexpr float r = 0.025f, len = 0.30f, top = 1.0f;
    static constexpr int   n = 5;
    explicit Cradle(int lifted, float bounce = 1.0f, float damping = 0.0f, bool knocks = true) {
        ents.reserve(n * 3);
        for (int i = 0; i < n; ++i) {
            Entity b;
            b.id = 100 + i * 10; b.type = EntityType::Sphere;
            b.localCenter = glm::vec3((i - 2) * (2.0f * r + 0.0002f), top - len, 0.0f);
            b.half = glm::vec3(r);
            auto sc = std::make_unique<SwingComponent>();
            sc->pivot = glm::vec3(0.0f, len, 0.0f);
            sc->mass = 0.5f;
            sc->damping = damping;
            sc->swingAxis = 2;
            sc->startAngle = i < lifted ? -30.0f : 0.0f;
            sc->knocks = knocks;
            sc->bounce = bounce;
            sc->knockSound = "click.wav";
            b.components.items.push_back(std::move(sc));
            ents.push_back(std::move(b));
            // Two strings from the top of the ball to the bars at z = +-0.12.
            for (int k = 0; k < 2; ++k) {
                Entity st;
                st.id = 100 + i * 10 + 1 + k; st.type = EntityType::Cylinder; st.parent = 100 + i * 10;
                const glm::vec3 from(0.0f, r, 0.0f), to(0.0f, len, k ? 0.12f : -0.12f);
                st.localCenter = 0.5f * (from + to);
                st.half = glm::vec3(0.0006f, 0.5f * glm::length(to - from), 0.0006f);
                st.localRotation = glm::vec3(glm::degrees(std::atan2(to.z - from.z, to.y - from.y)), 0.0f, 0.0f);
                ents.push_back(std::move(st));
            }
        }
        scenegraph::resolve(ents);
        sys.begin(ents, world, bodies);
    }
    const Entity& ball(int i) const {
        for (const Entity& e : ents) if (e.id == 100 + i * 10) return e;
        return ents[0];
    }
    // Degrees from hanging straight, + to +X.
    float angle(int i) const {
        const glm::vec3 d = ball(i).center - glm::vec3((i - 2) * (2.0f * r + 0.0002f), top, 0.0f);
        return glm::degrees(std::atan2(d.x, -d.y));
    }
    void run(float seconds, const std::function<void(float)>& each = {}) {
        for (int f = 0; f < static_cast<int>(seconds * 60.0f); ++f) {
            sys.update(ents, 1.0f / 60.0f, &world);
            scenegraph::resolve(ents);
            if (each) each(f / 60.0f);
        }
    }
};
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


    // --- One way only, a start angle, and knocks -----------------------------------------------
    {
        // Held to Z, a push with some of it along Z swings it about Z alone.
        const glm::vec3 z(0.0f, 0.0f, 1.0f);
        swing::State s;
        swing::kick(s, arm, 5.0f, arm, glm::vec3(2.0f, 0.0f, 1.5f), z);
        float off = 0.0f;
        for (int i = 0; i < 240; ++i) {
            swing::step(s, arm, 0.0f, 75.0f, dt, z);
            off = std::max(off, std::abs((s.q * arm).z));
        }
        check(off < 1e-5f && glm::length(glm::cross(s.w, z)) < 1e-5f,
              "held to one axis, it swings in its plane and nowhere else", std::to_string(off) + " m out of it");
    }
    {
        // Two balls of 0.5 kg on 1 m strings, side by side: one swings into the
        // other at the bottom -- it stops dead and the other leaves at its speed.
        const glm::vec3 z(0.0f, 0.0f, 1.0f);
        swing::Pendulum a, b;
        a.pivot = glm::vec3(0.0f, 1.0f, 0.0f);
        b.pivot = glm::vec3(0.1f, 1.0f, 0.0f);
        a.arm0 = b.arm0 = glm::vec3(0.0f, -1.0f, 0.0f);
        a.ball = a.pivot + a.arm0;
        b.ball = b.pivot + b.arm0;
        a.radius = b.radius = 0.05f;
        a.mass = b.mass = 0.5f;
        a.bounce = b.bounce = 1.0f;
        a.axis = b.axis = z;
        swing::State sa, sb;
        sa.w = z * 1.0f;   // moving to +X at 1 m/s
        check(swing::knock(sa, a, sb, b), "two balls touching and closing in knock");
        check(std::abs(sa.w.z) < 1e-5f && std::abs(sb.w.z - 1.0f) < 1e-5f,
              "...equal and of steel, the one stops and the other takes its whole speed",
              std::to_string(sa.w.z) + " / " + std::to_string(sb.w.z) + " rad/s");
        check(!swing::knock(sa, a, sb, b), "...and once parting, they knock no more");
        swing::State ca, cb;
        ca.w = z * 1.0f;
        a.bounce = b.bounce = 0.0f;
        swing::knock(ca, a, cb, b);
        check(std::abs(ca.w.z - 0.5f) < 1e-5f && std::abs(cb.w.z - 0.5f) < 1e-5f,
              "two of clay go on together at half the speed");
        swing::Pendulum heavy = b;
        heavy.mass = 1.5f;
        heavy.bounce = a.bounce = 1.0f;
        swing::State ha, hb;
        ha.w = z * 1.0f;
        swing::knock(ha, a, hb, heavy);
        check(std::abs(ha.w.z + 0.5f) < 1e-4f && std::abs(hb.w.z - 0.5f) < 1e-4f,
              "a light ball bounces back off a heavier one, as momentum and energy say",
              std::to_string(ha.w.z) + " / " + std::to_string(hb.w.z) + " rad/s (want -0.5 / 0.5)");
    }

    {
        Cradle c(1);
        c.run(1.0f / 60.0f);
        check(std::abs(c.angle(0) + 30.0f) < 0.5f, "pulled aside to start, the first ball swings from its start angle", std::to_string(c.angle(0)) + " deg");
        float out = 0.0f, middle = 0.0f;
        c.run(0.9f, [&](float) {
            out = std::max(out, c.angle(4));
            for (int i = 1; i <= 3; ++i) middle = std::max(middle, std::abs(c.angle(i)));
        });
        check(std::abs(out - 30.0f) < 0.6f, "one ball let go: the last one swings out as far as the first came in",
              std::to_string(out) + " deg");
        check(middle < 0.5f, "...and the three between stay where they hang", std::to_string(middle) + " deg");
        // Ten seconds on, steel and no damping: still the same swing, still only the ends.
        float ends = 0.0f;
        middle = 0.0f;
        c.run(10.0f, [&](float t) {
            if (t < 9.0f) return;
            ends = std::max({ends, -c.angle(0), c.angle(4)});
            for (int i = 1; i <= 3; ++i) middle = std::max(middle, std::abs(c.angle(i)));
        });
        check(std::abs(ends - 30.0f) < 1.0f && middle < 1.0f, "ten seconds on, it still swings at the two ends alone",
              std::to_string(ends) + " deg at the ends, " + std::to_string(middle) + " between");
        check(std::abs(glm::distance(c.ball(4).center, glm::vec3(2 * (2.0f * Cradle::r + 0.0002f), Cradle::top, 0.0f)) -
                       Cradle::len) < 1e-4f,
              "...each ball on its strings' length");
    }
    {
        // Let go from 30 degrees on 0.3 m, a ball meets the row at
        // sqrt(2 g L (1 - cos 30)) = 0.887 m/s. In 0.9 s it strikes once and
        // the last ball comes back once: two blows, each passed down four
        // pairs in the same instant -- and each heard as ONE click.
        Cradle c(1);
        int clicks = 0, frames = 0;
        float hardest = 0.0f;
        std::string sound;
        c.run(0.9f, [&](float) {
            for (const swing::System::Click& k : c.sys.clicks()) {
                if (k.speed < 0.02f) continue;   // what main plays: not the settling ticks
                ++clicks;
                hardest = std::max(hardest, k.speed);
                sound = k.sound;
            }
            for (const swing::System::Click& k : c.sys.clicks())
                if (k.speed >= 0.02f) { ++frames; break; }
        });
        check(clicks == 2 && frames == 2, "a blow down the row is heard as one click, not one per pair",
              std::to_string(clicks) + " clicks in " + std::to_string(frames) + " frames (want 2)");
        check(std::abs(hardest - 0.887f) < 0.03f && sound == "click.wav",
              "...as hard as the ball came in, with the sound the balls name",
              std::to_string(hardest) + " m/s (want 0.887)");
    }
    {
        Cradle c(2);
        float out3 = 0.0f, out4 = 0.0f, middle = 0.0f;
        c.run(0.9f, [&](float) {
            out3 = std::max(out3, c.angle(3));
            out4 = std::max(out4, c.angle(4));
            middle = std::max(middle, std::abs(c.angle(2)));
        });
        check(out3 > 28.5f && out4 > 28.5f && middle < 1.0f, "two let go: two swing out the other side",
              std::to_string(out3) + " / " + std::to_string(out4) + " deg, the middle one " + std::to_string(middle));
    }
    {
        // Steel that is not quite perfect, a little air: each blow leaves a little in
        // the balls it goes through, and after a while the row swings together.
        Cradle c(1, 0.97f, 0.02f);
        float early = 0.0f, late = 0.0f;
        c.run(30.0f, [&](float t) {
            if (t < 2.0f) early = std::max(early, std::abs(c.angle(2)));
            if (t > 28.0f) late = std::max(late, std::abs(c.angle(2)));
        });
        check(early < 2.0f && late > 3.0f, "with a bounce under one the row comes to swing together, in time",
              std::to_string(early) + " deg in the middle at first, " + std::to_string(late) + " at 30 s");
    }
    {
        Cradle c(1, 1.0f, 0.0f, /*knocks=*/false);
        float in = 0.0f;
        c.run(0.9f, [&](float) { in = std::min(in, c.angle(1)); in = std::max(in, std::abs(c.angle(4))); });
        check(in < 0.01f, "without Knocks they pass through one another, as before");
    }
    {
        // A shot at the middle ball of a cradle at rest: it swings in the cradle's
        // plane, and the blow goes on down the row.
        Cradle c(0);
        const glm::vec3 j(0.4f, 0.0f, 0.3f);
        check(c.sys.kick(c.ents, 120, j, nullptr), "a shot at a cradle's ball pushes it");
        float sideways = 0.0f, last = 0.0f;
        c.run(0.5f, [&](float) {
            sideways = std::max(sideways, std::abs(c.ball(2).center.z));
            last = std::max(last, c.angle(4));
        });
        check(sideways < 1e-5f && last > 1.0f, "...in the cradle's plane only, and the last ball takes the blow",
              std::to_string(sideways) + " m sideways, last ball " + std::to_string(last) + " deg");
    }

    std::printf("\nswingcheck: %s (%d failed)\n", failures ? "FAILED" : "all passed", failures);
    return failures ? 1 : 0;
}
