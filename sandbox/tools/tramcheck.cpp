// The tram check: a street tramway's track and the trams on it.
//
// The track (SplineGen with the Tram track preset, TramGen.cpp) on a straight
// path that is a street for its first half and its own ground for the second:
//   * in the street the rails are flush -- heads a hair above the paving band,
//     the band a hair above the road -- and there are no sleepers there;
//   * off it the track stands on ties;
//   * two tracks, apart in the middle, one in the stub at each end;
//   * the contact wire hangs where a pantograph on the tram reaches it.
// The trams (TramSim):
//   * an open double-track line: every tram serves stops and changes ends at
//     both termini -- its new front exactly where its tail stood -- and no two
//     trams ever touch, nor go faster than the line allows;
//   * something standing on the track stops a tram short of it, and it goes on
//     once the way is clear;
//   * a bend is taken at a speed a standing passenger keeps their feet at;
//   * one track, open: one tram, however many were asked for;
//   * a double-track loop: both directions run, nobody runs into anybody.
// The doors (TramSim): open only while the tram stands at a stop, on the side
// the stop is on; someone in the doorway holds it there; a car's frame never
// jumps, not even when the tram changes ends.
// Getting on (TramCar + the physics world, as in Play): a figure walks off the
// road up into an open doorway, not through a shut one nor into the cab, down
// the aisle past the seats and through the gangway into the next car -- and
// standing in a car that drives through a bend it rides along where it stood.
//
//   build/release/bin/tramcheck.exe
// Exits non-zero on any failure.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <fitzel/physics/Physics.hpp>

#include "SplineGen.hpp"
#include "TramCar.hpp"
#include "TramSim.hpp"
#include "TramTrack.hpp"

namespace {

int g_fail = 0;
void check(bool ok, const std::string& what, const std::string& detail = "") {
    if (!ok) ++g_fail;
    std::printf("  [%s] %s%s%s\n", ok ? " ok " : "FAIL", what.c_str(),
                detail.empty() ? "" : "  -- ", detail.c_str());
}
std::string f2(float v) { char b[32]; std::snprintf(b, sizeof(b), "%.3f", v); return b; }

// --- The track -------------------------------------------------------------------

void checkTrack() {
    std::printf("\n== Track in a street ==\n");
    const float y0 = 10.0f;
    std::vector<glm::vec3> path;
    std::vector<char> road;
    for (float z = 0.0f; z <= 400.0f; z += 2.0f) {
        path.push_back({0.0f, y0, z});
        road.push_back(z <= 200.0f ? 1 : 0);
    }
    const splinegen::Style s = splinegen::preset(splinegen::Preset::Tram);
    splinegen::Palette pal;
    pal.primary   = fitzel::AssetId::generate();
    pal.secondary = fitzel::AssetId::generate();
    pal.tertiary  = fitzel::AssetId::generate();
    pal.extra     = fitzel::AssetId::generate();
    const splinegen::Result r =
        splinegen::generate(splinegen::Kind::Rail, s, path, false, pal, 50000, &road);

    // The parts by material, near the ground (wire and masts are steel too).
    float steelRoadMax = -1e9f, steelRoadMin = 1e9f, bandMax = -1e9f, darkRoadMax = -1e9f;
    float darkOffMax = -1e9f, wireTop = -1e9f, wireOff = -1e9f, mastTop = -1e9f;
    float midInner = 1e9f, midOuter = 0.0f, endOuter = 0.0f;
    bool sign = false;
    for (const splinegen::Batch& b : r.batches)
        for (const fitzel::Vertex& v : b.data.vertices) {
            const glm::vec3 p = v.position;
            const bool inRoad = p.z > 40.0f && p.z < 190.0f;
            const bool offRoad = p.z > 260.0f && p.z < 360.0f;
            // The rails' height band: above the feet of masts and sign poles
            // (which stand at or below the street), below the wire.
            const bool low = p.y > y0 + 0.005f && p.y < y0 + 1.0f;
            if (b.material == pal.primary) {
                if (inRoad && low) {
                    steelRoadMax = std::max(steelRoadMax, p.y);
                    steelRoadMin = std::min(steelRoadMin, p.y);
                }
                if (low && p.z > 95.0f && p.z < 105.0f) {
                    midInner = std::min(midInner, std::abs(p.x));
                    midOuter = std::max(midOuter, std::abs(p.x));
                }
                if (low && p.z < 8.0f) endOuter = std::max(endOuter, std::abs(p.x));
                // The wire, between the masts (every 30 m) and their arms.
                const float between = std::fmod(p.z, 30.0f);
                if (p.y > y0 + 1.0f && std::abs(p.x) < 3.0f && between > 5.0f && between < 25.0f) {
                    if (inRoad)  wireTop = std::max(wireTop, p.y);
                    if (offRoad) wireOff = std::max(wireOff, p.y);
                }
                if (!low) mastTop = std::max(mastTop, p.y);
            } else if (b.material == pal.tertiary) {
                if (inRoad) bandMax = std::max(bandMax, p.y);
            } else if (b.material == pal.secondary) {
                if (inRoad) darkRoadMax = std::max(darkRoadMax, p.y);
                if (offRoad) darkOffMax = std::max(darkOffMax, p.y);
            } else if (b.material == pal.extra) {
                sign = true;
            }
        }
    std::printf("  %d pieces, %d verts in %zu batches\n", r.pieces, r.verts, r.batches.size());
    check(std::abs(bandMax - (y0 + tramtrack::kBandTop)) < 1e-3f,
          "the paving band lies just above the road", f2(bandMax - y0) + " m");
    check(std::abs(steelRoadMax - (y0 + tramtrack::kRailTop)) < 1e-3f && steelRoadMin > y0,
          "the rails in the street are flush: heads a hair above the band",
          f2(steelRoadMin - y0) + " .. " + f2(steelRoadMax - y0) + " m");
    check(darkRoadMax < y0 + tramtrack::kBandTop + 0.01f,
          "no sleepers in the street (only the grooves)", f2(darkRoadMax - y0) + " m");
    check(std::abs(darkOffMax - (y0 + s.sleeperHeight)) < 1e-3f,
          "off the street the track stands on ties", f2(darkOffMax - y0) + " m");
    const float g = s.gauge * 0.5f, half = s.trackSpacing * 0.5f;
    check(std::abs(midInner - (half - g - s.railWidth * 0.5f)) < 0.05f &&
          std::abs(midOuter - (half + g + s.railWidth * 0.5f)) < 0.05f,
          "two tracks, one each side of the middle",
          "rails from " + f2(midInner) + " to " + f2(midOuter) + " m out");
    check(endOuter < g + 0.2f, "at the end the two are one track (the stub)",
          f2(endOuter) + " m out");
    // 5.62 m over the rail head wherever the rail is: the pantograph's bow is
    // at 5.58-5.605 over it (TramSystem), the wire's underside at 5.608.
    const float wantWire = y0 + tramtrack::kRailTop + 5.62f + 0.012f;
    const float wantOff  = y0 + tramtrack::railTopOffRoad(s.sleeperHeight, s.railHeight) + 5.632f;
    check(std::abs(wireTop - wantWire) < 0.02f && std::abs(wireOff - wantOff) < 0.02f,
          "the contact wire hangs where the pantograph reaches, in the street and off it",
          f2(wireTop - y0) + " / " + f2(wireOff - y0) + " m");
    check(mastTop > y0 + 5.9f && mastTop < y0 + 6.3f, "masts carry it", f2(mastTop - y0) + " m");
    check(sign, "the stops are signed");
}

// --- The trams -------------------------------------------------------------------

tramsim::Line straight(float len, int tracks, int trams) {
    tramsim::Line l;
    for (float z = 0.0f; z <= len; z += 2.0f) l.center.push_back({0.0f, 0.0f, z});
    l.tracks = tracks;
    l.trams = trams;
    l.stopEvery = 200.0f;
    l.dwell = 5.0f;
    l.vmax = 11.1f;
    return l;
}

float minSeparation(const tramsim::Sim& sim) {
    float best = 1e9f;
    for (int a = 0; a < sim.tramCount(); ++a)
        for (int b = a + 1; b < sim.tramCount(); ++b) {
            tramsim::Section sa[tramsim::kSections], sb[tramsim::kSections];
            sim.sections(a, sa);
            sim.sections(b, sb);
            for (const auto& x : sa)
                for (const auto& y : sb)
                    for (float ea : {-4.0f, 0.0f, 4.0f})
                        for (float eb : {-4.0f, 0.0f, 4.0f})
                            best = std::min(best, glm::length((x.center + x.dir * ea) -
                                                              (y.center + y.dir * eb)));
        }
    return best;
}

void checkOpenLine() {
    std::printf("\n== An open double-track line ==\n");
    tramsim::Sim sim;
    sim.setLines({straight(800.0f, 2, 3)});
    check(sim.tramCount() == 3, "three trams on it", std::to_string(sim.tramCount()));
    const float dt = 0.05f;
    float vmax = 0.0f, closest = 1e9f, worstTurn = 0.0f;
    std::vector<int> flips(sim.tramCount(), 0);
    std::vector<glm::vec3> lastTail(sim.tramCount());
    for (int i = 0; i < sim.tramCount(); ++i) {
        tramsim::Section s[tramsim::kSections];
        sim.sections(i, s);
        lastTail[i] = s[tramsim::kSections - 1].center - s[tramsim::kSections - 1].dir * (tramsim::kSectionLen * 0.5f);
    }
    for (int k = 0; k < static_cast<int>(1200.0f / dt); ++k) {
        sim.step(dt, {});
        closest = std::min(closest, minSeparation(sim));
        for (int i = 0; i < sim.tramCount(); ++i) {
            vmax = std::max(vmax, sim.speed(i));
            tramsim::Section s[tramsim::kSections];
            sim.sections(i, s);
            const tramsim::Sim::Info in = sim.info(i);
            if (in.flips != flips[i]) {
                // Changed ends: its new front is where its tail was.
                const glm::vec3 front = s[0].center + s[0].dir * (tramsim::kSectionLen * 0.5f);
                worstTurn = std::max(worstTurn, glm::length(front - lastTail[i]));
                flips[i] = in.flips;
            }
            lastTail[i] = s[tramsim::kSections - 1].center -
                          s[tramsim::kSections - 1].dir * (tramsim::kSectionLen * 0.5f);
        }
    }
    int minServed = 1 << 30, minFlips = 1 << 30;
    for (int i = 0; i < sim.tramCount(); ++i) {
        minServed = std::min(minServed, sim.info(i).served);
        minFlips  = std::min(minFlips, sim.info(i).flips);
    }
    check(minServed >= 6, "every tram serves its stops", "fewest: " + std::to_string(minServed));
    check(minFlips >= 2, "every tram changes ends at both termini",
          "fewest: " + std::to_string(minFlips));
    check(worstTurn < 0.6f, "changing ends moves nothing sideways (new front = old tail)",
          f2(worstTurn) + " m");
    check(closest > 2.5f, "no two trams ever touch", "closest " + f2(closest) + " m");
    check(vmax <= 11.1f + 1e-3f, "never faster than the line allows", f2(vmax * 3.6f) + " km/h");
}

void checkBlocker() {
    std::printf("\n== Something on the track ==\n");
    tramsim::Sim sim;
    tramsim::Line l = straight(600.0f, 1, 1);
    l.stopEvery = 0.0f;
    sim.setLines({l});
    check(sim.tramCount() == 1, "one tram");
    const float dt = 0.05f;
    // Ahead of it, on its track, wherever it is.
    const float startX = sim.info(0).x;
    const glm::vec3 at = sim.pointOn(0, sim.info(0).half, startX + 150.0f);
    std::vector<tramsim::Blocker> b = {{at, 0.5f}};
    for (int k = 0; k < static_cast<int>(60.0f / dt); ++k) sim.step(dt, b);
    tramsim::Section s[tramsim::kSections];
    sim.sections(0, s);
    const glm::vec3 front = s[0].center + s[0].dir * (tramsim::kSectionLen * 0.5f);
    const float gap = glm::length(glm::vec2(front.x - at.x, front.z - at.z));
    check(sim.speed(0) < 0.01f && gap > 1.0f && gap < 8.0f, "it stops short of it",
          f2(gap) + " m off, at " + f2(sim.speed(0)) + " m/s");
    const float stoppedAt = sim.info(0).x;
    for (int k = 0; k < static_cast<int>(20.0f / dt); ++k) sim.step(dt, {});
    check(sim.info(0).x > stoppedAt + 20.0f, "and goes on once the way is clear");
}

void checkCurve() {
    std::printf("\n== A bend ==\n");
    tramsim::Line l;
    // 200 m straight, a quarter circle of 20 m radius, 200 m straight.
    for (float z = 0.0f; z <= 200.0f; z += 2.0f) l.center.push_back({0.0f, 0.0f, z});
    for (int i = 1; i <= 16; ++i) {
        const float a = 1.5707963f * i / 16.0f;
        l.center.push_back({20.0f - 20.0f * std::cos(a), 0.0f, 200.0f + 20.0f * std::sin(a)});
    }
    for (float x = 22.0f; x <= 220.0f; x += 2.0f) l.center.push_back({x, 0.0f, 220.0f});
    l.tracks = 1; l.trams = 1; l.stopEvery = 0.0f; l.vmax = 13.9f;
    tramsim::Sim sim;
    sim.setLines({l});
    float inBend = 0.0f;
    for (int k = 0; k < 4000; ++k) {
        sim.step(0.05f, {});
        tramsim::Section s[tramsim::kSections];
        sim.sections(0, s);
        if (s[0].center.x > 4.0f && s[0].center.x < 16.0f && s[0].center.z > 203.0f)
            inBend = std::max(inBend, sim.speed(0));
    }
    check(inBend > 1.0f && inBend <= std::sqrt(20.0f) + 0.6f,
          "it slows for a 20 m bend", f2(inBend * 3.6f) + " km/h");
}

void checkSingleAndLoop() {
    std::printf("\n== One track, and a loop ==\n");
    tramsim::Sim one;
    one.setLines({straight(600.0f, 1, 3)});
    check(one.tramCount() == 1, "one track, open: one tram, however many asked",
          std::to_string(one.tramCount()));

    tramsim::Line ring;
    for (int i = 0; i < 300; ++i) {
        const float a = 6.2831853f * i / 300.0f;
        ring.center.push_back({150.0f * std::cos(a), 0.0f, 150.0f * std::sin(a)});
    }
    ring.closed = true; ring.tracks = 2; ring.trams = 4; ring.stopEvery = 200.0f; ring.dwell = 4.0f;
    tramsim::Sim sim;
    sim.setLines({ring});
    check(sim.tramCount() == 4 && sim.halfCount(0) == 2, "a double loop runs both ways",
          std::to_string(sim.tramCount()) + " trams on " + std::to_string(sim.halfCount(0)) + " tracks");
    float closest = 1e9f;
    for (int k = 0; k < 12000; ++k) {
        sim.step(0.05f, {});
        if (k % 4 == 0) closest = std::min(closest, minSeparation(sim));
    }
    int served = 1 << 30;
    for (int i = 0; i < sim.tramCount(); ++i) served = std::min(served, sim.info(i).served);
    check(served >= 2, "every tram on the loop serves stops", "fewest " + std::to_string(served));
    check(closest > 2.5f, "and none runs into another", "closest " + f2(closest) + " m");
}

// --- The doors -------------------------------------------------------------------

glm::vec3 xform(const glm::mat4& m, const glm::vec3& p) { return glm::vec3(m * glm::vec4(p, 1.0f)); }

void checkDoors() {
    std::printf("\n== Doors ==\n");
    tramsim::Line line;
    for (float z = 0.0f; z <= 900.0f; z += 2.0f)
        line.center.push_back({60.0f * std::sin(z / 200.0f), 0.0f, z});
    line.tracks = 2; line.trams = 2; line.stopEvery = 200.0f; line.dwell = 8.0f;
    tramsim::Sim sim;
    sim.setLines({line});
    const float dt = 0.05f;
    bool openWhileMoving = false, opened = false, wrongSide = false;
    float jump = 0.0f, turnJump = 0.0f;
    std::vector<glm::mat4> last(static_cast<std::size_t>(sim.tramCount() * tramsim::kSections));
    for (int k = 0; k < 24000; ++k) {
        sim.step(dt, {});
        for (int i = 0; i < sim.tramCount(); ++i) {
            const float door = sim.doors(i);
            if (door > 0.0f && sim.speed(i) > 0.0f) openWhileMoving = true;
            if (door >= 1.0f) opened = true;
            tramsim::Section sec[tramsim::kSections];
            sim.sections(i, sec);
            for (int c = 0; c < tramsim::kSections; ++c) {
                const glm::mat4 f = sim.carFrame(i, c);
                // The doors open to the right of the way it drives.
                const glm::vec3 side = xform(f, {static_cast<float>(sim.doorSide(i, c)), 0.0f, 0.0f}) -
                                       glm::vec3(f[3]);
                const glm::vec3 right(-sec[0].dir.z, 0.0f, sec[0].dir.x);
                if (glm::dot(glm::normalize(glm::vec3(side.x, 0.0f, side.z)),
                             glm::normalize(right)) < 0.8f)
                    wrongSide = true;
                glm::mat4& l = last[static_cast<std::size_t>(i * tramsim::kSections + c)];
                if (k > 0) {
                    jump = std::max(jump, glm::length(glm::vec3(f[3]) - glm::vec3(l[3])));
                    turnJump = std::max(turnJump, std::acos(std::clamp(glm::dot(glm::vec3(f[2]), glm::vec3(l[2])),
                                                                       -1.0f, 1.0f)));
                }
                l = f;
            }
        }
    }
    int flips = 0;
    for (int i = 0; i < sim.tramCount(); ++i) flips = std::max(flips, sim.info(i).flips);
    check(opened, "the doors open at the stops");
    check(!openWhileMoving, "and are always shut while a tram moves");
    check(!wrongSide, "on the side the stop is on (the right of the way it drives)");
    check(flips >= 2 && jump < 0.8f && turnJump < 0.1f,
          "a car's frame never jumps, not when the tram changes ends either",
          std::to_string(flips) + " changes of ends, largest step " + f2(jump) + " m, turn " +
              f2(turnJump) + " rad");

    // Someone in the doorway: the tram waits for them.
    tramsim::Sim one;
    tramsim::Line straight;
    for (float z = 0.0f; z <= 600.0f; z += 2.0f) straight.center.push_back({0.0f, 0.0f, z});
    straight.tracks = 2; straight.trams = 1; straight.stopEvery = 150.0f; straight.dwell = 6.0f;
    one.setLines({straight});
    float t = 0.0f;
    while (one.info(0).dwell <= 0.0f && t < 400.0f) { one.step(dt, {}); t += dt; }
    float held = 0.0f;
    while (held < 30.0f) { one.holdDoors(0); one.step(dt, {}); held += dt; }
    const bool stayed = one.info(0).dwell > 0.0f && one.doors(0) >= 1.0f;
    float gone = 0.0f;
    while (one.speed(0) <= 0.0f && gone < 20.0f) { one.step(dt, {}); gone += dt; }
    check(stayed, "someone in a doorway holds the tram at its stop", "held 30 s");
    check(gone < tramsim::kDoorTime + 6.0f, "and once they are through it goes on",
          "left after " + f2(gone) + " s");
}

// --- Getting on -------------------------------------------------------------------

struct Car {
    std::vector<glm::vec3> hc, hh;
    fitzel::PhysicsBodyId shell = 0;
    fitzel::PhysicsBodyId leaf[8]{};
    bool cab = true;
};

Car makeCar(fitzel::PhysicsWorld& w, bool cab, const glm::mat4& f, int openSide, float open) {
    Car c;
    c.cab = cab;
    splinegen::detail::Slot slot[tramcar::MatCount];
    tramcar::build(cab, slot, c.hc, c.hh);
    const glm::quat q = glm::quat_cast(glm::mat3(f));
    c.shell = w.addPlatform(c.hc.data(), c.hh.data(), static_cast<int>(c.hc.size()), glm::vec3(f[3]), q);
    for (int n = 0; n < 8; ++n) {
        int j;
        float sx, k;
        tramcar::leafOf(n, j, sx, k);
        const glm::vec3 zero(0.0f);
        c.leaf[n] = w.addPlatform(&zero, &tramcar::kLeafHalf, 1,
                                  xform(f, tramcar::leafAt(cab, j, sx, k,
                                                           sx == static_cast<float>(openSide) ? open : 0.0f)),
                                  q);
    }
    return c;
}

void moveCar(fitzel::PhysicsWorld& w, const Car& c, const glm::mat4& f, float dt) {
    const glm::quat q = glm::quat_cast(glm::mat3(f));
    w.setKinematicTarget(c.shell, glm::vec3(f[3]), q, dt);
    for (int n = 0; n < 8; ++n) {
        int j;
        float sx, k;
        tramcar::leafOf(n, j, sx, k);
        w.setKinematicTarget(c.leaf[n], xform(f, tramcar::leafAt(c.cab, j, sx, k, 0.0f)), q, dt);
    }
}

// Walk figure `fig` at `vel` for `secs`; where its feet end up.
glm::vec3 walkFor(fitzel::PhysicsWorld& w, int fig, glm::vec3 vel, float secs) {
    fitzel::PhysicsWorld::FigureStep st;
    const float dt = 1.0f / 60.0f;
    for (float t = 0.0f; t < secs; t += dt) {
        w.step(dt);
        w.moveFigure(fig, vel, dt, st);
    }
    return st.foot;
}

void checkBoarding() {
    std::printf("\n== Getting on ==\n");
    const float rail = tramtrack::kRailTop;
    const float floorY = rail + tramsim::kFloor;
    float dz[2];
    tramsim::doorsOf(true, dz);
    const glm::mat4 at = glm::translate(glm::mat4(1.0f), {0.0f, rail, 0.0f});

    {
        fitzel::PhysicsWorld w;
        w.setGravity({0.0f, -9.81f, 0.0f});
        w.addBox({60.0f, 0.5f, 60.0f}, {0.0f, -0.5f, 0.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), 0.0f);
        const Car car = makeCar(w, true, at, +1, 1.0f);
        // In at an open doorway, off the road and up the step.
        const int in = w.addFigure(0.3f, 0.6f, {3.0f, 0.0f, dz[0]});
        const glm::vec3 a = walkFor(w, in, {-1.4f, 0.0f, 0.0f}, 4.0f);
        check(std::abs(a.y - floorY) < 0.05f && a.x < 0.3f && a.x > -tramsim::kWallIn,
              "a figure walks off the road up into an open doorway",
              "feet at (" + f2(a.x) + ", " + f2(a.y) + "), the floor at " + f2(floorY));
        // Not through a shut one.
        const int out = w.addFigure(0.3f, 0.6f, {-3.0f, 0.0f, dz[0]});
        const glm::vec3 b = walkFor(w, out, {1.4f, 0.0f, 0.0f}, 3.0f);
        check(b.x < -tramsim::kHalfWidth - 0.2f && b.y < 0.1f, "but not through a shut one",
              "stopped at x " + f2(b.x));
        // Down the aisle to the front -- but not into the cab.
        const int fwd = w.addFigure(0.3f, 0.6f, {0.0f, floorY, dz[0]});
        const glm::vec3 c = walkFor(w, fwd, {0.0f, 0.0f, 1.4f}, 6.0f);
        check(c.z < tramsim::kCabWall - 0.25f && c.z > dz[1] + 1.0f && std::abs(c.y - floorY) < 0.05f,
              "down the aisle to the front, but not into the driver's cab",
              "stopped at z " + f2(c.z) + ", the cab wall at " + f2(tramsim::kCabWall));
        // Riding: the car drives off through a bend; the figure stays put in it.
        const int ride = w.addFigure(0.3f, 0.6f, {0.0f, floorY, dz[0]});
        walkFor(w, ride, glm::vec3(0.0f), 0.5f);
        fitzel::PhysicsWorld::FigureStep st;
        w.moveFigure(ride, glm::vec3(0.0f), 1.0f / 60.0f, st);
        const glm::vec3 local0 = st.foot - glm::vec3(0.0f, rail, 0.0f);
        // As in the editor: the car is led to where the tram has just driven
        // AFTER the frame's physics step, and the next frame -- a different
        // length, frame times are never even -- steps it there; the figure
        // moves after that step and is drawn with the car as it then stands.
        // Over a crest and through a dip, at tram speed.
        const float v = 11.0f, yawRate = 0.15f;
        glm::vec3 pos(0.0f, rail, 0.0f);
        float yaw = 0.0f, turned = 0.0f, shake = 0.0f, off = 0.0f, lift = 0.0f, dist = 0.0f;
        glm::vec3 lastLocal = local0;
        int offGround = 0, frames = 0;
        glm::mat4 f = at;
        std::uint32_t rng = 12345u;
        float lastDt = 1.0f / 60.0f;
        for (float t = 0.0f; t < 6.0f;) {
            rng = rng * 1664525u + 1013904223u;
            const float dt = (1.0f / 144.0f) + (1.0f / 40.0f - 1.0f / 144.0f) *
                             static_cast<float>(rng >> 8) / 16777216.0f;
            t += dt;
            w.step(dt);
            w.moveFigure(ride, glm::vec3(0.0f), dt, st);
            turned += st.turn;
            ++frames;
            offGround += st.onGround ? 0 : 1;
            // Where it stands in the car as the car is drawn this frame.
            // (It stands upright while the car pitches over the crest, so its
            // feet move a few centimetres along the floor and back, smoothly;
            // what must not happen is a jump from one frame to the next.)
            const glm::vec3 local = xform(glm::inverse(f), st.foot);
            off   = std::max(off, glm::length(glm::vec2(local.x - local0.x, local.z - local0.z)));
            shake = std::max(shake, glm::length(local - lastLocal));
            lift  = std::max(lift, std::abs(local.y - (floorY - rail)));
            lastLocal = local;
            // The tram drives on.
            yaw  += yawRate * dt;
            dist += v * dt;
            pos  += glm::vec3(std::sin(yaw), 0.0f, std::cos(yaw)) * (v * dt);
            pos.y = rail + 1.5f * (1.0f - std::cos(dist / 25.0f));
            const float pitch = std::atan(1.5f / 25.0f * std::sin(dist / 25.0f));
            f = glm::rotate(glm::rotate(glm::translate(glm::mat4(1.0f), pos), yaw,
                                        glm::vec3(0.0f, 1.0f, 0.0f)),
                            -pitch, glm::vec3(1.0f, 0.0f, 0.0f));
            moveCar(w, car, f, lastDt = dt);
        }
        (void)lastDt;
        check(shake < 0.005f && off < 0.1f && lift < 0.03f && offGround == 0,
              "riding a car through a bend, over a crest and a dip at 40 km/h, with frame "
              "times all over the place, a figure stays where it stood in it -- no shaking",
              "largest jump between frames " + f2(shake) + " m, furthest off its spot " + f2(off) +
                  " m, off the floor " + f2(lift) + " m, " + std::to_string(offGround) + " of " +
                  std::to_string(frames) + " frames not on it");
        check(std::abs(turned - yaw) < 0.02f, "and is turned with it",
              "turned " + f2(turned) + " rad, the car " + f2(yaw));
    }
    {
        // The player on foot (the first-person walker): walked BEFORE the
        // frame's physics step, carried after it (carryCharacter) -- the eye
        // must stand still in the car as the car is drawn, every frame.
        fitzel::PhysicsWorld w;
        w.setGravity({0.0f, -9.81f, 0.0f});
        w.addBox({60.0f, 0.5f, 60.0f}, {0.0f, -0.5f, 0.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), 0.0f);
        const Car car = makeCar(w, true, at, +1, 0.0f);
        w.spawnCharacter(0.3f, 0.6f, {0.0f, floorY + 0.05f, dz[0]});
        bool ground = false;
        glm::vec3 foot(0.0f);
        for (int k = 0; k < 60; ++k) {
            foot = w.moveCharacter(glm::vec3(0.0f), false, 1.0f / 60.0f, ground);
            w.step(1.0f / 60.0f);
        }
        const glm::vec3 local0 = foot - glm::vec3(0.0f, rail, 0.0f);
        glm::vec3 last = local0;
        const float v = 11.0f, yawRate = 0.15f;
        glm::vec3 pos(0.0f, rail, 0.0f);
        float yaw = 0.0f, turned = 0.0f, shake = 0.0f, dist = 0.0f;
        glm::mat4 f = at;
        std::uint32_t rng = 777u;
        for (float t = 0.0f; t < 6.0f;) {
            rng = rng * 1664525u + 1013904223u;
            const float dt = (1.0f / 144.0f) + (1.0f / 40.0f - 1.0f / 144.0f) *
                             static_cast<float>(rng >> 8) / 16777216.0f;
            t += dt;
            foot = w.moveCharacter(glm::vec3(0.0f), false, dt, ground);
            w.step(dt);
            float turn = 0.0f;
            foot += w.carryCharacter(turn);
            turned += turn;
            const glm::vec3 local = xform(glm::inverse(f), foot);   // in the car as drawn
            shake = std::max(shake, glm::length(local - last));
            last = local;
            yaw  += yawRate * dt;
            dist += v * dt;
            pos  += glm::vec3(std::sin(yaw), 0.0f, std::cos(yaw)) * (v * dt);
            pos.y = rail + 1.5f * (1.0f - std::cos(dist / 25.0f));
            const float pitch = std::atan(1.5f / 25.0f * std::sin(dist / 25.0f));
            f = glm::rotate(glm::rotate(glm::translate(glm::mat4(1.0f), pos), yaw,
                                        glm::vec3(0.0f, 1.0f, 0.0f)),
                            -pitch, glm::vec3(1.0f, 0.0f, 0.0f));
            moveCar(w, car, f, dt);
        }
        check(shake < 0.005f && std::abs(turned - yaw) < 0.02f,
              "the player on foot rides along too, the eye steady in the car and turned with it",
              "largest jump between frames " + f2(shake) + " m, turned " + f2(turned) +
                  " rad, the car " + f2(yaw));
    }
    {
        // Down the aisle past the seats, through the gangway into the next car.
        fitzel::PhysicsWorld w;
        w.setGravity({0.0f, -9.81f, 0.0f});
        w.addBox({60.0f, 0.5f, 60.0f}, {0.0f, -0.5f, 0.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), 0.0f);
        makeCar(w, true, at, +1, 0.0f);
        const float back = tramsim::kSectionLen + tramsim::kGap;
        makeCar(w, false, glm::translate(glm::mat4(1.0f), {0.0f, rail, -back}), +1, 0.0f);
        const int fig = w.addFigure(0.3f, 0.6f, {0.0f, floorY, dz[0]});
        const glm::vec3 e = walkFor(w, fig, {0.0f, 0.0f, -1.4f}, 7.0f);
        check(e.z < -tramsim::kSectionLen * 0.5f - tramsim::kGap - 1.5f && std::abs(e.y - floorY) < 0.05f,
              "down the aisle and through the gangway into the next car",
              "from z " + f2(dz[0]) + " to " + f2(e.z));
    }
}

} // namespace

int main() {
    checkTrack();
    checkOpenLine();
    checkBlocker();
    checkCurve();
    checkSingleAndLoop();
    checkDoors();
    checkBoarding();
    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "all good", g_fail,
                g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
