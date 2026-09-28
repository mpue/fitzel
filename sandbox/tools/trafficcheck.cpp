// trafficcheck: the towns' traffic (sandbox/src/TrafficSim.hpp), without a window.
//
// A traffic simulation fails quietly: a car that stops for ever at a corner, a
// bus that drives through the one in front, a queue that runs a red light --
// each of them still "moves", and a glance at the editor sees traffic. So the
// rules are measured over a few simulated minutes in real derived towns:
//   - vehicles are there and keep moving,
//   - none drives into the one ahead on its lane,
//   - none enters a signalled crossing on red,
//   - buses call at their stops,
//   - nobody goes faster than their kind allows,
//   - people stay off the carriageways,
//   - a wreck in the street is queued behind, not driven through,
//   - and the crash itself: a CPU car's driven body follows its lane exactly,
//     a hard knock crashes it into a wreck that skids to a stop, a nudge does
//     not, and a bus shrugs off a car.
//   build/release/bin/trafficcheck.exe

#include "../src/TrafficSim.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <glm/gtc/quaternion.hpp>
#include <fitzel/physics/Physics.hpp>

#include "../src/CivicGen.hpp"

namespace {

int g_fail = 0;
void check(bool ok, const std::string& what, const std::string& detail = {}) {
    std::printf("  [%s] %s%s%s\n", ok ? " ok " : "FAIL", what.c_str(), detail.empty() ? "" : "  -- ",
                detail.c_str());
    if (!ok) ++g_fail;
}

void run(cityplan::Preset preset) {
    using namespace cityplan;
    std::printf("\n== %s ==\n", presetName(preset));
    Rule r;
    applyPreset(r, preset);
    r.grid.center = {100.0f, -60.0f};
    r.grid.rotation = 12.0f;
    r.busShare = 0.15f;
    const Layout L = layout(r);
    std::vector<MaterialDef> mats;
    const Palettes pal = ensurePalettes(mats, r);
    Context ctx;
    auto ground = [](float x, float z) { return 0.015f * x + 0.4f * std::sin(z * 0.02f); };
    ctx.groundAt = ground;
    for (const Street& s : L.streets) ctx.roads.push_back({s.pts, s.width * 0.5f, s.name});
    const Town T = derive(r, pal, ctx);

    traffic::Sim sim;
    // A road wherever a street runs (the check's streets are all laid).
    sim.surfaceAt = [&](glm::vec2 p, float& y) {
        for (const Street& s : L.streets)
            for (std::size_t i = 0; i + 1 < s.pts.size(); ++i) {
                const glm::vec2 a = s.pts[i], ab = s.pts[i + 1] - a;
                const float t = glm::clamp(glm::dot(p - a, ab) / glm::dot(ab, ab), 0.0f, 1.0f);
                if (glm::length(p - (a + ab * t)) <= 0.5f * s.width + 0.5f) {
                    y = ground(p.x, p.y);
                    return true;
                }
            }
        return false;
    };
    sim.build({r}, {&T});
    const auto& V = sim.vehicles();
    int buses = 0, signals = 0, stops = 0;
    for (const traffic::Vehicle& v : V) buses += v.kind == traffic::Kind::Bus;
    for (const traffic::Node& n : sim.nodes()) signals += n.signal;
    for (const traffic::Lane& l : sim.lanes()) stops += static_cast<int>(l.stops.size());
    check(!V.empty(), "vehicles on the streets",
          std::to_string(V.size()) + " (" + std::to_string(buses) + " buses) on " +
              std::to_string(sim.lanes().size()) + " lanes, " + std::to_string(signals) +
              " signalled crossings, " + std::to_string(stops) + " stops on lanes, " +
              std::to_string(sim.walkers().size()) + " people");
    if (V.empty()) return;

    // --- Five simulated minutes -------------------------------------------------
    const float dt = 1.0f / 30.0f;
    std::vector<glm::vec3> start;
    for (const traffic::Vehicle& v : V) start.push_back(sim.pose(v).pos);
    std::vector<float> travelled(V.size(), 0.0f);
    std::vector<glm::vec3> last = start;
    float worstOverlap = 0.0f, fastest = 0.0f;
    float worstJump = 0.0f, worstTurn = 0.0f;   // pose: metres beyond the distance driven, radians
    int redRuns = 0, dwells = 0;
    double stepMs = 0.0;
    const int steps = static_cast<int>(300.0f / dt);
    for (int k = 0; k < steps; ++k) {
        const double clock = k * dt;
        std::vector<traffic::Vehicle> before = V;
        const auto t0 = std::chrono::steady_clock::now();
        sim.step(dt, clock);
        stepMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        for (std::size_t i = 0; i < V.size(); ++i) {
            const traffic::Vehicle& a = V[i];
            const traffic::Vehicle& b = before[i];
            // Into a signalled crossing: from its lane into the turn.
            if (!b.turning && a.turning) {
                const traffic::Lane& l = sim.lanes()[static_cast<std::size_t>(b.lane)];
                if (sim.nodes()[static_cast<std::size_t>(l.to)].signal) {
                    civic::SignalLamps lamps[2];
                    civic::signalPhase(clock, lamps[0], lamps[1]);
                    if (lamps[l.axis].red) ++redRuns;
                }
            }
            if (a.dwell > 0.0f && b.dwell <= 0.0f) ++dwells;
            fastest = std::max(fastest, a.v / a.vmax);
            // The pose moves as far as the vehicle drove, and turns gradually:
            // a camera shooting it sees every jump (lane to turn to lane).
            {
                const traffic::Pose pa = sim.pose(a), pb = sim.pose(b);
                const float moved = glm::length(glm::vec2(pa.pos.x - pb.pos.x, pa.pos.z - pb.pos.z));
                worstJump = std::max(worstJump, moved - (a.odo - b.odo));
                const float c = glm::clamp(glm::dot(pa.heading, pb.heading), -1.0f, 1.0f);
                worstTurn = std::max(worstTurn, std::acos(c));
            }
            const glm::vec3 p = sim.pose(a).pos;
            travelled[i] += glm::length(p - last[i]);
            last[i] = p;
        }
        // Nobody inside the one ahead on the same lane.
        for (std::size_t i = 0; i < V.size(); ++i)
            for (std::size_t j = 0; j < V.size(); ++j) {
                if (i == j || V[i].turning || V[j].turning || V[i].lane != V[j].lane) continue;
                if (V[j].s <= V[i].s) continue;   // j ahead of i
                const float gap = (V[j].s - V[j].length) - V[i].s;
                worstOverlap = std::min(worstOverlap, gap);
            }
    }
    int moved = 0;
    for (float d : travelled) moved += d > 50.0f;
    check(moved >= static_cast<int>(0.9 * V.size()), "the traffic keeps moving",
          std::to_string(moved) + " of " + std::to_string(V.size()) + " went more than 50 m in 5 min");
    check(worstOverlap > -0.05f, "nobody drives into the one ahead",
          "tightest gap " + std::to_string(worstOverlap).substr(0, 5) + " m");
    check(redRuns == 0, "nobody enters a signalled crossing on red", std::to_string(redRuns));
    check(stops == 0 || buses == 0 || dwells > 0, "buses call at their stops",
          std::to_string(dwells) + " calls");
    check(fastest <= 1.001f, "nobody faster than their kind allows",
          std::to_string(fastest).substr(0, 5) + " of top speed");
    // Measured before the fix: 6.6 m and 108 degrees in one step, where a turn
    // handed the vehicle on to its next lane. Now a tight village corner at its
    // turning speed turns a car ~3.7 degrees a step (30 a second).
    check(worstJump < 0.02f && worstTurn < glm::radians(5.0f), "poses move smoothly, no jumps",
          "worst " + std::to_string(worstJump).substr(0, 5) + " m beyond the distance driven, " +
              std::to_string(glm::degrees(worstTurn)).substr(0, 5) + " deg in one step");

    // People: never on a carriageway.
    float worstIn = -1e9f;
    for (const traffic::Walker& w : sim.walkers()) {
        const glm::vec3 p = sim.pose(w).pos;
        for (const Street& s : L.streets)
            for (std::size_t i = 0; i + 1 < s.pts.size(); ++i) {
                const glm::vec2 a = s.pts[i], ab = s.pts[i + 1] - a;
                const glm::vec2 q(p.x, p.z);
                const float t = glm::clamp(glm::dot(q - a, ab) / glm::dot(ab, ab), 0.0f, 1.0f);
                worstIn = std::max(worstIn, 0.5f * s.width - glm::length(q - (a + ab * t)));
            }
    }
    check(sim.walkers().empty() || worstIn < 0.0f, "people stay on the pavement",
          std::to_string(sim.walkers().size()) + " people, nearest " +
              std::to_string(-worstIn).substr(0, 4) + " m from a carriageway");
    std::printf("  %.3f ms per step for %zu vehicles and %zu people\n", stepMs / steps, V.size(),
                sim.walkers().size());
}

} // namespace

// A scene object joining the traffic (TrafficDriverComponent's path) and
// vehicles dressed as prefabs of another length (TownTraffic's): both are
// placed, drive, and keep their distance like everybody else.
void drivers() {
    using namespace cityplan;
    std::printf("\n== Drivers and prefab-dressed vehicles ==\n");
    Rule r;
    applyPreset(r, Preset::SmallTown);
    const Layout L = layout(r);
    std::vector<MaterialDef> mats;
    const Palettes pal = ensurePalettes(mats, r);
    Context ctx;
    ctx.groundAt = [](float, float) { return 0.0f; };
    for (const Street& s : L.streets) ctx.roads.push_back({s.pts, s.width * 0.5f, s.name, {}});
    const Town T = derive(r, pal, ctx);
    traffic::Sim sim;
    sim.surfaceAt = [&](glm::vec2 p, float& y) {
        for (const Street& s : L.streets)
            for (std::size_t i = 0; i + 1 < s.pts.size(); ++i) {
                const glm::vec2 a = s.pts[i], ab = s.pts[i + 1] - a;
                const float t = glm::clamp(glm::dot(p - a, ab) / glm::dot(ab, ab), 0.0f, 1.0f);
                if (glm::length(p - (a + ab * t)) <= 0.5f * s.width + 0.5f) { y = 0.0f; return true; }
            }
        return false;
    };
    // Every other car a long prefab (7.5 m).
    int dressed = 0;
    sim.build({r}, {&T}, 1, [&](traffic::Vehicle& v) {
        if (v.kind == traffic::Kind::Car && (v.rng & 1U)) { v.prefab = 0; v.length = 7.5f; ++dressed; }
    });
    int longOnes = 0;
    for (const traffic::Vehicle& v : sim.vehicles()) longOnes += v.prefab == 0 && v.length == 7.5f;
    check(dressed > 0 && longOnes > 0, "vehicles dressed as a prefab carry its length",
          std::to_string(longOnes) + " of " + std::to_string(sim.vehicles().size()));

    // Who is who (TownTraffic keys the bodies it lends by it): every vehicle
    // its own uid, and a crashed one leaves the traffic alone.
    {
        std::vector<std::uint32_t> uids;
        for (const traffic::Vehicle& v : sim.vehicles()) uids.push_back(v.uid);
        std::sort(uids.begin(), uids.end());
        const bool unique = std::adjacent_find(uids.begin(), uids.end()) == uids.end() && uids.front() != 0;
        const std::size_t n = sim.vehicles().size();
        const std::uint32_t gone = sim.vehicles()[n / 2].uid;
        const bool removed = sim.removeVehicle(gone) && sim.vehicles().size() + 1 == n &&
                             !sim.removeVehicle(gone);
        check(unique && removed, "every vehicle has its own id, and a crashed one leaves alone");
    }

    // A driver dropped next to a street, facing along it.
    const Street& st = L.streets[L.streets.size() / 2];
    const glm::vec2 a = st.pts[1], b = st.pts[2];
    const glm::vec2 dir = glm::normalize(b - a);
    const glm::vec2 at = 0.5f * (a + b) + glm::vec2(-dir.y, dir.x) * 2.0f;
    const bool placed = sim.addDriver(4242, at, dir, traffic::Kind::Bus, 11.0f, 10.0f);
    check(placed, "a scene object is put on the lane beside it");
    if (!placed) return;
    auto find = [&]() -> const traffic::Vehicle* {
        for (const traffic::Vehicle& v : sim.vehicles()) if (v.entity == 4242) return &v;
        return nullptr;
    };
    const glm::vec3 start = sim.pose(*find()).pos;
    float travelled = 0.0f, worstGap = 1e9f;
    glm::vec3 last = start;
    for (int k = 0; k < 90 * 30; ++k) {
        sim.step(1.0f / 30.0f, k / 30.0);
        const traffic::Vehicle* me = find();
        const glm::vec3 p = sim.pose(*me).pos;
        travelled += glm::length(p - last);
        last = p;
        for (const traffic::Vehicle& o : sim.vehicles())
            if (&o != me && !o.turning && !me->turning && o.lane == me->lane && o.s > me->s)
                worstGap = std::min(worstGap, (o.s - o.length) - me->s);
    }
    check(travelled > 100.0f, "the driven object drives", std::to_string(static_cast<int>(travelled)) + " m in 90 s");
    check(worstGap > -0.05f, "and keeps its distance",
          worstGap > 1e8f ? std::string("never caught anyone up")
                          : "tightest " + std::to_string(worstGap).substr(0, 5) + " m");
    const std::size_t before = sim.vehicles().size();
    sim.removeDrivers();
    check(sim.vehicles().size() + 1 == before && !find(), "Stop takes it out of the traffic again");
}

// A wreck in the street (Sim::setObstacles): whoever comes up behind it stops
// short and waits -- nobody drives into it -- and once it is cleared away the
// queue drives on.
void obstacles() {
    using namespace cityplan;
    std::printf("\n== A wreck in the street ==\n");
    Rule r;
    applyPreset(r, Preset::SmallTown);
    r.traffic *= 2.0f;   // enough cars that a queue forms in a few minutes
    const Layout L = layout(r);
    std::vector<MaterialDef> mats;
    const Palettes pal = ensurePalettes(mats, r);
    Context ctx;
    ctx.groundAt = [](float, float) { return 0.0f; };
    for (const Street& s : L.streets) ctx.roads.push_back({s.pts, s.width * 0.5f, s.name, {}});
    const Town T = derive(r, pal, ctx);
    traffic::Sim sim;
    sim.surfaceAt = [&](glm::vec2 p, float& y) {
        for (const Street& s : L.streets)
            for (std::size_t i = 0; i + 1 < s.pts.size(); ++i) {
                const glm::vec2 a = s.pts[i], ab = s.pts[i + 1] - a;
                const float t = glm::clamp(glm::dot(p - a, ab) / glm::dot(ab, ab), 0.0f, 1.0f);
                if (glm::length(p - (a + ab * t)) <= 0.5f * s.width + 0.5f) { y = 0.0f; return true; }
            }
        return false;
    };
    sim.build({r}, {&T});

    // A car-sized wreck in the middle of the three longest lanes nobody is
    // about to reach yet.
    struct Wreck { int lane; float back, front; };
    std::vector<Wreck> wrecks;
    std::vector<traffic::Obstacle> obs;
    std::vector<int> order(sim.lanes().size());
    for (int i = 0; i < static_cast<int>(order.size()); ++i) order[static_cast<std::size_t>(i)] = i;
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return sim.lanes()[static_cast<std::size_t>(a)].len > sim.lanes()[static_cast<std::size_t>(b)].len;
    });
    for (int l : order) {
        const traffic::Lane& ln = sim.lanes()[static_cast<std::size_t>(l)];
        if (ln.len < 40.0f || wrecks.size() == 3) break;
        const float mid = 0.5f * ln.len;
        bool clear = true;
        for (const traffic::Vehicle& v : sim.vehicles())
            if ((v.turning && v.next == l) ||
                (!v.turning && v.lane == l && v.s > mid - 30.0f && v.s - v.length < mid + 2.2f))
                clear = false;
        if (!clear) continue;
        traffic::Obstacle o;
        const glm::vec2 c = ln.p0 + ln.dir * mid;
        o.center  = {c.x, ln.heightAt(mid) + 0.75f, c.y};
        o.axes[0] = glm::vec3(ln.dir.x, 0.0f, ln.dir.y) * 2.2f;
        o.axes[1] = glm::vec3(0.0f, 0.75f, 0.0f);
        o.axes[2] = glm::vec3(-ln.dir.y, 0.0f, ln.dir.x) * 0.9f;
        obs.push_back(o);
        wrecks.push_back({l, mid - 2.2f, mid + 2.2f});
    }
    check(!wrecks.empty(), "wrecks laid in the street", std::to_string(wrecks.size()) + " lanes blocked");
    if (wrecks.empty()) return;
    sim.setObstacles(obs);

    float worst = 1e9f;
    for (int k = 0; k < 240 * 30; ++k) {
        sim.step(1.0f / 30.0f, k / 30.0);
        for (const traffic::Vehicle& v : sim.vehicles())
            for (const Wreck& w : wrecks)
                if (!v.turning && v.lane == w.lane && v.s - v.length < w.front)
                    worst = std::min(worst, w.back - v.s);
    }
    std::vector<std::size_t> waiting;
    for (std::size_t i = 0; i < sim.vehicles().size(); ++i) {
        const traffic::Vehicle& v = sim.vehicles()[i];
        for (const Wreck& w : wrecks)
            if (!v.turning && v.lane == w.lane && v.v < 0.1f && w.back - v.s >= 0.0f && w.back - v.s < 15.0f)
                waiting.push_back(i);
    }
    check(worst > -0.05f, "nobody drives into a wreck",
          worst > 1e8f ? std::string("nobody came") : "tightest " + std::to_string(worst).substr(0, 5) + " m");
    check(!waiting.empty(), "the traffic queues behind it",
          std::to_string(waiting.size()) + " waiting at a wreck after 4 min");

    // Cleared away: the queue drives on.
    std::vector<float> odo;
    for (std::size_t i : waiting) odo.push_back(sim.vehicles()[i].odo);
    sim.setObstacles({});
    for (int k = 0; k < 60 * 30; ++k) sim.step(1.0f / 30.0f, 240.0 + k / 30.0);
    int moved = 0;
    for (std::size_t n = 0; n < waiting.size(); ++n) moved += sim.vehicles()[waiting[n]].odo - odo[n] > 30.0f;
    check(!waiting.empty() && moved == static_cast<int>(waiting.size()), "cleared away, the queue drives on",
          std::to_string(moved) + " of " + std::to_string(waiting.size()) + " went on 30 m in a minute");

    // A wreck in a crossing, 4 m past the end of a lane on its line: who comes
    // along that lane waits at the stop line instead of driving into it.
    std::vector<int> ends;
    std::vector<traffic::Obstacle> cross;
    for (int l : order) {
        const traffic::Lane& ln = sim.lanes()[static_cast<std::size_t>(l)];
        if (ln.len < 30.0f || ends.size() == 3) break;
        bool clear = true;
        for (const traffic::Vehicle& v : sim.vehicles())
            if (v.lane == l && (v.turning || v.s > ln.len - 30.0f)) clear = false;
        if (!clear) continue;
        traffic::Obstacle o;
        const glm::vec2 c = ln.p1 + ln.dir * 4.0f;
        o.center  = {c.x, ln.heightAt(ln.len) + 0.75f, c.y};
        o.axes[0] = glm::vec3(ln.dir.x, 0.0f, ln.dir.y) * 2.2f;
        o.axes[1] = glm::vec3(0.0f, 0.75f, 0.0f);
        o.axes[2] = glm::vec3(-ln.dir.y, 0.0f, ln.dir.x) * 0.9f;
        cross.push_back(o);
        ends.push_back(l);
    }
    sim.setObstacles(cross);
    int through = 0;
    for (int k = 0; k < 180 * 30; ++k) {
        const std::vector<traffic::Vehicle> before = sim.vehicles();
        sim.step(1.0f / 30.0f, 300.0 + k / 30.0);
        for (std::size_t i = 0; i < before.size(); ++i)
            if (!before[i].turning && sim.vehicles()[i].turning &&
                std::find(ends.begin(), ends.end(), before[i].lane) != ends.end())
                ++through;
    }
    int atLine = 0;
    for (const traffic::Vehicle& v : sim.vehicles())
        if (!v.turning && v.v < 0.1f && std::find(ends.begin(), ends.end(), v.lane) != ends.end() &&
            sim.lanes()[static_cast<std::size_t>(v.lane)].len - v.s < 10.0f)
            ++atLine;
    check(!ends.empty() && through == 0 && atLine > 0,
          "a wreck in a crossing: who comes straight at it waits at the stop line",
          std::to_string(ends.size()) + " crossings, " + std::to_string(through) + " drove in, " +
              std::to_string(atLine) + " waiting at the line after 3 min");
}

// The physics side of a CPU car (TownTraffic::playTick): its driven body is
// steered onto its target every tick, and what the step just taken knocked it
// off what it was asked is the jolt that crashes it. `hitSpeed` sends the
// player's car -- a plain dynamic box, 1200 kg -- square into its side.
struct CrashRun {
    float     maxJolt = 0.0f;      // m/s, over the run while it was driven
    float     worstError = 0.0f;   // how far off its target it ever was, metres
    float     endError = 0.0f;     // ...and at the end
    bool      crashed = false;
    glm::vec3 crashAt{0.0f}, end{0.0f};
    float     endSpeed = 0.0f;
};

CrashRun crashRun(float mass, float hitSpeed, float driveSpeed, float seconds) {
    const float kCrash = 10.0f / 3.6f;   // TrafficDriverComponent's default
    fitzel::PhysicsWorld w;
    w.setGravity({0.0f, -9.81f, 0.0f});
    const glm::quat q(1.0f, 0.0f, 0.0f, 0.0f);
    w.addBox({300.0f, 0.5f, 300.0f}, {0.0f, -0.5f, 0.0f}, q, 0.0f);   // the road
    const glm::vec3 half(0.9f, 0.75f, 2.2f);
    glm::vec3 target(0.0f, 0.75f, 0.0f);
    const std::uint32_t car = w.addDrivenBox(half, target, q, mass);
    if (hitSpeed > 0.0f) {
        // 30 cm off its side: near enough that the road's friction cannot stop
        // even a slow push before it lands.
        const std::uint32_t other = w.addBox({0.9f, 0.5f, 2.0f}, {-(half.x + 0.9f + 0.3f), 0.5f, 0.0f},
                                             q, 1200.0f);
        w.setLinearVelocity(other, {hitSpeed, 0.0f, 0.0f});
    }
    CrashRun run;
    glm::vec3 asked(0.0f), askedSpin(0.0f);
    const float dt = 1.0f / 60.0f;
    for (int k = 0; k < static_cast<int>(seconds / dt); ++k) {
        w.step(dt);
        if (run.crashed) continue;
        glm::vec3 v(0.0f), s(0.0f), p(0.0f);
        glm::quat r;
        w.getLinearVelocity(car, v);
        w.getAngularVelocity(car, s);
        w.getTransform(car, p, r);
        const float j = traffic::jolt(v, asked, s, askedSpin, glm::length(half));
        run.maxJolt = std::max(run.maxJolt, j);
        if (j >= kCrash) {
            run.crashed = true;
            run.crashAt = p;
            w.releaseBody(car);
            continue;
        }
        run.worstError = std::max(run.worstError, glm::distance(p, target));
        run.endError   = glm::distance(p, target);
        target.z += driveSpeed * dt;
        w.setKinematicTarget(car, target, q, dt);
        w.getLinearVelocity(car, asked);
        w.getAngularVelocity(car, askedSpin);
    }
    glm::quat r;
    glm::vec3 v(0.0f);
    w.getTransform(car, run.end, r);
    w.getLinearVelocity(car, v);
    run.endSpeed = glm::length(v);
    return run;
}

void crashes() {
    std::printf("\n== Crashes ==\n");
    auto kmh = [](float mps) { return std::to_string(static_cast<int>(std::lround(mps * 3.6f))); };

    const CrashRun drive = crashRun(1300.0f, 0.0f, 13.9f, 3.0f);
    check(drive.worstError < 0.01f && drive.maxJolt < 0.2f,
          "a CPU car's body follows its lane, and the road does not drag on it",
          "off by at most " + std::to_string(drive.worstError).substr(0, 5) + " m, jolt " +
              std::to_string(drive.maxJolt).substr(0, 4) + " m/s");

    const CrashRun hit = crashRun(1300.0f, 8.0f, 0.0f, 8.0f);
    check(hit.crashed && hit.maxJolt < 8.0f, "hit at 29 km/h, a waiting car crashes",
          "a " + kmh(hit.maxJolt) + " km/h jolt");
    const float slid = glm::length(glm::vec2(hit.end.x - hit.crashAt.x, hit.end.z - hit.crashAt.z));
    check(hit.crashed && hit.endSpeed < 0.3f && hit.end.y > 0.4f && hit.end.y < 1.3f && slid < 12.0f,
          "the wreck lands on the road and skids to a stop",
          "slid " + std::to_string(slid).substr(0, 4) + " m, at " + std::to_string(hit.endSpeed).substr(0, 4) +
              " m/s after 8 s, centre " + std::to_string(hit.end.y).substr(0, 4) + " m up");

    const CrashRun nudge = crashRun(1300.0f, 1.5f, 0.0f, 4.0f);
    check(!nudge.crashed && nudge.maxJolt > 0.2f && nudge.endError < 0.02f,
          "a nudge does not crash it, and it keeps its place",
          "a " + std::to_string(nudge.maxJolt).substr(0, 4) + " m/s jolt, " +
              std::to_string(nudge.endError).substr(0, 5) + " m off at the end");

    const CrashRun bus = crashRun(12000.0f, 13.9f, 0.0f, 4.0f);
    check(!bus.crashed, "a bus shrugs off a car at 50 km/h", "a " + kmh(bus.maxJolt) + " km/h jolt");

    // Put on its lane from far off: a jump, at rest, nothing hit on the way.
    fitzel::PhysicsWorld w;
    const glm::quat q(1.0f, 0.0f, 0.0f, 0.0f);
    const std::uint32_t car = w.addDrivenBox({0.9f, 0.75f, 2.2f}, {0.0f, 0.75f, 0.0f}, q, 1300.0f);
    w.setTransform(car, {50.0f, 0.75f, -40.0f}, q);
    w.step(1.0f / 60.0f);
    glm::vec3 p(0.0f), v(1.0f);
    glm::quat r;
    w.getTransform(car, p, r);
    w.getLinearVelocity(car, v);
    check(glm::distance(p, glm::vec3(50.0f, 0.75f, -40.0f)) < 1e-3f && glm::length(v) < 1e-3f,
          "a jump to a far lane arrives at once and at rest");
}

int main() {
    crashes();
    obstacles();
    drivers();
    run(cityplan::Preset::Village);
    run(cityplan::Preset::SmallTown);
    run(cityplan::Preset::City);
    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "all good", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
