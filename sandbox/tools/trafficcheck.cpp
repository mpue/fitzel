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
//   - people stay off the carriageways.
//   build/release/bin/trafficcheck.exe

#include "../src/TrafficSim.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

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

int main() {
    drivers();
    run(cityplan::Preset::Village);
    run(cityplan::Preset::SmallTown);
    run(cityplan::Preset::City);
    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "all good", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
