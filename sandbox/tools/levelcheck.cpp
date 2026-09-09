// levelcheck -- one seed, one whole race scene.
//
// Everything that can go wrong in a level generator is invisible from a
// screenshot and expensive to find by driving:
//
//   1. A corner tighter than the craft can hold. From the cockpit it looks like
//      one plausible corner you happen to keep crashing in, not like a bug.
//   2. A checkpoint the opponents never count. RaceSim drops a gate whose centre
//      is off the road, and misses one its racing line cannot reach: the first
//      hands the player a lap that cannot be completed, the second hands them a
//      field that never completes one. Both look like a race.
//   3. A banking sign flipped. The corridor is graded the mirror image of the
//      road above it -- one edge buried, the other hanging -- and the ribbon
//      still draws perfectly.
//   4. A seed that is plumbed through and then ignored. It takes someone
//      rerolling twenty times to notice.
//   5. A span naming a control point that does not exist, or straddling the
//      seam. RoadSystem::layout skips it in silence, so the bridge is simply
//      not there.
//   6. A profile the road's own smoothing cannot flatten, so the finished track
//      is steeper than the limit that was asked for.
//   7. Works for seed 1, undrivable for seed 137. There is no other way to find
//      that than to try a few hundred.
//
// No GL, no window, no assets: levelgen takes plain data and returns plain data,
// which is the whole point of the split (see LevelGen.hpp, and RaceGrid.hpp one
// level down for the same bargain).
//
//   levelcheck
// Exits non-zero if any measurement fails.

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/world/Terrain.hpp>

#include "../src/LevelGen.hpp"
#include "../src/RoadLoop.hpp"
#include "../src/SandboxMath.hpp"

namespace {

int g_fails = 0;

void check(bool ok, const char* what, const std::string& detail) {
    if (ok) std::printf("  ok   %s -- %s\n", what, detail.c_str());
    else  { std::printf("[FAIL] %s: %s\n", what, detail.c_str()); ++g_fails; }
}

std::string fmt(const char* f, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

float cross2(const glm::vec2& a, const glm::vec2& b) { return a.x * b.y - a.y * b.x; }

// The sampled centreline, measured the way the road will be.
struct Path {
    std::vector<glm::vec2> line;
    std::vector<int>       pt;
    std::vector<float>     arc;
    float total = 0.0f;
    int   ring  = 0;
    const glm::vec2& at(int k) const { return line[((k % ring) + ring) % ring]; }
};

Path pathOf(const std::vector<glm::vec2>& pts) {
    Path P;
    P.line = sampleSpline(pts, true, &P.pt);
    if (P.line.size() < 4) return P;
    P.ring = static_cast<int>(P.line.size()) - 1;
    P.arc.assign(P.line.size(), 0.0f);
    for (std::size_t i = 1; i < P.line.size(); ++i)
        P.arc[i] = P.arc[i - 1] + glm::length(P.line[i] - P.line[i - 1]);
    P.total = P.arc.back();
    return P;
}

float radiusAt(const Path& P, int j) {
    const glm::vec2 a = P.at(j - 1), b = P.at(j), c = P.at(j + 1);
    const float area2 = std::fabs(cross2(b - a, c - a));
    if (area2 < 1e-6f) return 1.0e9f;
    return glm::length(b - a) * glm::length(c - b) * glm::length(a - c) / (2.0f * area2);
}

float turnSign(const Path& P, int j) {
    const float x = cross2(P.at(j) - P.at(j - 1), P.at(j + 1) - P.at(j));
    return (x < 0.0f) ? 1.0f : (x > 0.0f ? -1.0f : 0.0f);
}

float arcGap(float a, float b, float total) {
    const float d = std::fabs(a - b);
    return std::min(d, total - d);
}

// The distance from a point to the centreline, in plan. What "on the road"
// means for a gate and for a grid slot alike.
float laneOf(const Path& P, const glm::vec2& q) {
    float best = 1.0e9f;
    for (int j = 0; j < P.ring; ++j) best = std::min(best, glm::length(P.at(j) - q));
    return best;
}

// The profile the road will settle on: the same low-pass RoadSystem runs, plus
// the ramped lift. Shared with the generator through LevelGen.hpp precisely so
// this can measure the road that will EXIST rather than the points that were
// asked for.
std::vector<float> profileOf(const Path& P, const levelgen::Track& t,
                             const std::function<float(float, float)>& ground) {
    std::vector<float> prof(P.line.size());
    for (std::size_t i = 0; i < P.line.size(); ++i)
        prof[i] = ground(P.line[i].x, P.line[i].y);
    levelgen::lowPass(prof, levelgen::passesForSigma(3.0f + t.grade * 15.0f));

    std::vector<float> ramp(P.line.size(), 0.0f);
    const int n = static_cast<int>(t.lift.size());
    if (n > 0) {
        for (std::size_t k = 0; k + 1 < P.pt.size(); ++k) {
            const int a = P.pt[k], b = P.pt[k + 1];
            if (b <= a) continue;
            const float va = t.lift[k % n], vb = t.lift[(k + 1) % n];
            for (int i = a; i <= b && i < static_cast<int>(ramp.size()); ++i)
                ramp[i] = glm::mix(va, vb, float(i - a) / float(b - a));
        }
        levelgen::lowPass(ramp, 5);
    }
    for (std::size_t i = 0; i < prof.size(); ++i) prof[i] += ramp[i];
    return prof;
}

} // namespace

int main() {
    // Without this every height in the engine answers 0, and a generator laid
    // out on a flat void passes every gradient, bridge and tunnel test for the
    // wrong reason.
    fitzel::setTerrainPresent(true);

    // The generator asks for a height on SETTINGS it hands over, so that it can
    // soften the land and ask again (see LevelGen.hpp). Level::terrain is what
    // it settled on, and what a caller must then apply.
    auto samplerFor = [](int* calls = nullptr) {
        return [calls](const fitzel::TerrainSettings& ts, float x, float z) {
            if (calls) ++*calls;
            return terrainBaseHeight(ts, x, z);
        };
    };
    auto build = [&](levelgen::Params p, int* calls = nullptr) {
        return levelgen::generate(p, samplerFor(calls));
    };

    // --- 1) The same seed gives the same circuit -----------------------------
    {
        levelgen::Params p;
        p.seed = 7;
        const levelgen::Level a = build(p), b = build(p);
        float worst = 0.0f;
        bool same = a.track.points.size() == b.track.points.size() &&
                    a.track.lift.size() == b.track.lift.size() &&
                    a.track.bank.size() == b.track.bank.size() &&
                    a.track.bridges.size() == b.track.bridges.size() &&
                    a.track.tunnels.size() == b.track.tunnels.size() &&
                    a.markers.size() == b.markers.size();
        if (same)
            for (std::size_t i = 0; i < a.track.points.size(); ++i) {
                worst = std::max(worst, glm::length(a.track.points[i] - b.track.points[i]));
                worst = std::max(worst, std::fabs(a.track.lift[i] - b.track.lift[i]));
                worst = std::max(worst, std::fabs(a.track.bank[i] - b.track.bank[i]));
            }
        check(same && worst == 0.0f, "the same seed gives the same circuit",
              fmt("%zu points, worst drift %.9f", a.track.points.size(), worst));
    }

    // --- 2) The seed actually reaches the world ------------------------------
    {
        int distinct = 0;
        float worstTerrainStep = 1.0e9f;
        std::vector<glm::vec2> prev;
        for (unsigned s = 1; s <= 60; ++s) {
            levelgen::Params p; p.seed = s;
            const levelgen::Level l = build(p);
            if (!prev.empty() && prev.size() == l.track.points.size()) {
                float d = 0.0f;
                for (std::size_t i = 0; i < prev.size(); ++i)
                    d = std::max(d, glm::length(prev[i] - l.track.points[i]));
                if (d > 1.0f) ++distinct;
            } else if (!prev.empty()) ++distinct;
            prev = l.track.points;
            if (s > 1)
                worstTerrainStep = std::min(worstTerrainStep,
                    std::fabs(levelgen::terrainFor(p).seed -
                              levelgen::terrainFor({s - 1}).seed));
        }
        check(distinct == 59 && worstTerrainStep > 500.0f,
              "the seed reaches both the land and the layout",
              fmt("%d/59 circuits differ, smallest terrain-seed step %.0f",
                  distinct, worstTerrainStep));
    }

    // --- 3) No hidden state --------------------------------------------------
    {
        levelgen::Params p; p.seed = 21;
        int c1 = 0, c2 = 0;
        build(p, &c1);
        build(p, &c2);
        check(c1 == c2 && c1 > 0, "the generator reads nothing but its inputs",
              fmt("%d ground samples both times", c1));
    }

    // --- 4..7, 9, 11, 12, 14) One circuit, measured all over -----------------
    auto measure = [&](levelgen::Params p, const char* label, bool report) {
        const levelgen::Level lv = levelgen::generate(p, samplerFor());
        const fitzel::TerrainSettings ts = lv.terrain;
        const auto ground = [&](float x, float z) { return terrainBaseHeight(ts, x, z); };
        const Path P = pathOf(lv.track.points);
        const float Rmin = levelgen::minRadius(p);
        const float half = levelgen::surfaceHalf(p);
        const int   n    = static_cast<int>(lv.track.points.size());
        bool allOk = true;

        // 4) the tightest corner
        float worstR = 1.0e9f; int worstJ = 0;
        for (int j = 0; j < P.ring; ++j) {
            const float R = radiusAt(P, j);
            if (R < worstR) { worstR = R; worstJ = j; }
        }
        if (report)
            check(worstR >= Rmin * 0.98f, "every corner is drivable",
                  fmt("tightest %.0f m at station %.0f, minimum %.0f m",
                      worstR, P.arc[worstJ], Rmin));
        allOk = allOk && worstR >= Rmin * 0.98f;

        // 5) the gradient of the road that will exist
        // The generator's OWN definition of a gradient, not a second one: a
        // harness that measures something slightly different from the thing
        // under test measures the difference between them.
        const std::vector<float> prof = profileOf(P, lv.track, ground);
        const float worstG = levelgen::worstGradient(P.arc, prof);
        int gj = 0;
        if (report) {
            // Against the BARE ground as well, so a pass means the lift did the
            // work -- not that the land happened to be flat where this seed put
            // the circuit.
            std::vector<float> bare(P.line.size());
            for (std::size_t i = 0; i < P.line.size(); ++i)
                bare[i] = ground(P.line[i].x, P.line[i].y);
            levelgen::lowPass(bare,
                              levelgen::passesForSigma(3.0f + lv.track.grade * 15.0f));
            check(worstG <= p.maxGradient * 1.15f, "the profile obeys the gradient limit",
                  fmt("steepest %.1f%% over %.0f m of a %.1f%% hillside, limit %.1f%%",
                      worstG * 100.0f, levelgen::kGradientWindow,
                      levelgen::worstGradient(P.arc, bare) * 100.0f,
                      p.maxGradient * 100.0f));
        }
        allOk = allOk && worstG <= p.maxGradient * 1.15f;

        // 6) banking: bound, and the right way round
        float worstB = 0.0f;
        bool signOk = true;
        for (int i = 0; i < n; ++i) {
            worstB = std::max(worstB, std::fabs(lv.track.bank[i]));
            const int j = P.pt[i];
            const float R = radiusAt(P, j);
            const float ts2 = turnSign(P, j);
            // Only judge a corner tight enough for the sign to be meaningful.
            if (R < 2.0f * Rmin && std::fabs(lv.track.bank[i]) > 1.0f &&
                lv.track.bank[i] * ts2 < 0.0f)
                signOk = false;
        }
        if (report)
            check(signOk && worstB <= p.maxBank + 0.01f,
                  "the road banks into its corners, not out of them",
                  fmt("worst %.1f deg of %.1f allowed, sign %s",
                      worstB, p.maxBank, signOk ? "right" : "MIRRORED"));
        allOk = allOk && signOk && worstB <= p.maxBank + 0.01f;

        // 7) the ring keeps off itself
        const float sepArc = 4.0f * (half + 8.0f);
        float worstD = 1.0e9f;
        for (int j = 0; j < P.ring; j += 2)
            for (int k = j + 2; k < P.ring; k += 2) {
                if (arcGap(P.arc[j], P.arc[k], P.total) < sepArc) continue;
                worstD = std::min(worstD, glm::length(P.at(j) - P.at(k)));
            }
        const bool sepOk = (p.shape == levelgen::Shape::Eight) || worstD >= 2.0f * half;
        if (report)
            check(sepOk, "the circuit does not brush itself",
                  fmt("closest distant approach %.1f m, needs %.1f", worstD, 2.0f * half));
        allOk = allOk && sepOk;

        // 9) every span is real, in range, and alone on its ground
        bool spansOk = true;
        std::vector<levelgen::Span> all = lv.track.bridges;
        all.insert(all.end(), lv.track.tunnels.begin(), lv.track.tunnels.end());
        for (const levelgen::Span& s : all) {
            if (s.a < 0 || s.b >= n || s.b <= s.a) spansOk = false;
            for (const levelgen::Span& o : all)
                if (&o != &s && s.a <= o.b && o.a <= s.b) spansOk = false;
        }
        for (const roadloop::Spec& s : lv.track.loops) {
            if (s.a < 0 || s.b >= n || s.b <= s.a) spansOk = false;
            for (const levelgen::Span& o : all)
                if (s.a <= o.b && o.a <= s.b) spansOk = false;
        }
        if (report)
            check(spansOk, "every structure names points that exist and stands alone",
                  fmt("%zu bridge(s), %zu tunnel(s), %zu loop(s)",
                      lv.track.bridges.size(), lv.track.tunnels.size(),
                      lv.track.loops.size()));
        allOk = allOk && spansOk;

        // 11) checkpoints: on the centreline, and wide enough for the AI's line
        float worstLane = 0.0f, worstMargin = 1.0e9f;
        int gates = 0;
        for (const levelgen::Marker& m : lv.markers) {
            if (m.kind != levelgen::Marker::Kind::Checkpoint) continue;
            ++gates;
            worstLane = std::max(worstLane, laneOf(P, glm::vec2(m.pos.x, m.pos.z)));
            worstMargin = std::min(worstMargin,
                                   m.gateW * 0.5f + 1.0f - (p.width * 0.5f - 1.2f));
        }
        const bool cpOk = gates >= 3 && worstLane < 0.5f && worstMargin >= 0.0f;
        if (report)
            check(cpOk, "every checkpoint can be flown AND is counted",
                  fmt("%d gates, worst lane %.2f m, worst AI margin %.1f m",
                      gates, worstLane, worstMargin));
        allOk = allOk && cpOk;

        // 12) the lap is possible
        std::vector<float> st;
        float finish = -1.0f;
        for (const levelgen::Marker& m : lv.markers) {
            if (m.kind == levelgen::Marker::Kind::Checkpoint) st.push_back(m.station);
            if (m.kind == levelgen::Marker::Kind::Finish) finish = m.station;
        }
        std::sort(st.begin(), st.end());
        bool lapOk = finish >= 0.0f && st.size() >= 3;
        for (std::size_t i = 0; i + 1 < st.size(); ++i)
            if (st[i + 1] - st[i] < 1.0f) lapOk = false;
        for (float s : st)
            if (arcGap(s, finish, P.total) < 12.0f) lapOk = false;
        if (report)
            check(lapOk, "the lap can be completed",
                  fmt("%zu gates, finish at %.0f of %.0f m", st.size(), finish, P.total));
        allOk = allOk && lapOk;

        // 13) the grid stands on the road, facing along it
        float worstGrid = 0.0f, worstYaw = 0.0f, closest = 1.0e9f;
        std::vector<glm::vec3> slots;
        int players = 0;
        for (const levelgen::Marker& m : lv.markers) {
            if (m.kind != levelgen::Marker::Kind::Grid) continue;
            if (m.player) ++players;
            worstGrid = std::max(worstGrid, laneOf(P, glm::vec2(m.pos.x, m.pos.z)));
            const int j = 0;
            (void)j;
            for (const glm::vec3& o : slots)
                closest = std::min(closest, glm::length(glm::vec2(o.x - m.pos.x,
                                                                 o.z - m.pos.z)));
            slots.push_back(m.pos);
            // the heading the marker claims, against the road under it
            float best = 1.0e9f; int bj = 0;
            for (int k = 0; k < P.ring; ++k) {
                const float d = glm::length(P.at(k) - glm::vec2(m.pos.x, m.pos.z));
                if (d < best) { best = d; bj = k; }
            }
            glm::vec2 dir = P.at(bj + 1) - P.at(bj - 1);
            if (glm::length(dir) > 1e-4f) {
                dir = glm::normalize(dir);
                const float want = glm::degrees(std::atan2(dir.x, dir.y));
                float diff = std::fabs(want - m.headingDeg);
                while (diff > 180.0f) diff = std::fabs(diff - 360.0f);
                worstYaw = std::max(worstYaw, diff);
            }
        }
        const bool gridOk = worstGrid < p.width * 0.5f && worstYaw < 5.0f &&
                            closest > 2.5f && players == (p.pinPlayerPole ? 1 : 0);
        if (report)
            check(gridOk, "the grid stands on the road, facing along it",
                  fmt("worst offset %.1f m, worst yaw %.1f deg, closest pair %.1f m, "
                      "%d player slot", worstGrid, worstYaw, closest, players));
        allOk = allOk && gridOk;

        // 14) every marker is self-sufficient
        bool selfOk = true;
        std::vector<int> usedSlots;
        for (const levelgen::Marker& m : lv.markers) {
            if (m.kind != levelgen::Marker::Kind::Grid &&
                (m.gateW <= 0.0f || m.gateH <= 0.0f || m.gateD <= 0.0f)) selfOk = false;
            if (m.kind == levelgen::Marker::Kind::Grid) {
                for (int s : usedSlots) if (s == m.slot) selfOk = false;
                usedSlots.push_back(m.slot);
            }
        }
        if (report)
            check(selfOk, "no marker leaves a number for the apply step to invent",
                  fmt("%zu markers, %zu grid slots", lv.markers.size(), usedSlots.size()));
        allOk = allOk && selfOk;

        if (report && !lv.report.ok)
            std::printf("       (report: %s)\n", lv.report.why.c_str());
        (void)label;
        return allOk && lv.report.ok;
    };

    measure(levelgen::Params{}, "defaults", true);

    // --- 8) The crossing is the one that was asked for -----------------------
    {
        levelgen::Params p;
        p.shape = levelgen::Shape::Eight;
        p.crossing = levelgen::Crossing::Flyover;
        p.seed = 3;
        const levelgen::Level lv = build(p);
        // The lifted branch must be carried by a span, or the corridor grading
        // builds an embankment across the branch underneath it.
        bool carried = false;
        for (const levelgen::Span& s : lv.track.bridges) {
            float peak = 0.0f;
            for (int i = s.a; i <= s.b && i < static_cast<int>(lv.track.lift.size()); ++i)
                peak = std::max(peak, lv.track.lift[i]);
            if (peak > p.flyover * 0.4f) carried = true;
        }
        check(lv.report.crossings == 1 && lv.report.crossingClearance >= 3.5f && carried,
              "a flyover clears its underpass and stands on a bridge",
              fmt("%d crossing(s), %.1f m clearance, %s",
                  lv.report.crossings, lv.report.crossingClearance,
                  carried ? "carried by a span" : "NO SPAN -- an embankment"));
    }
    {
        levelgen::Params p;
        p.shape = levelgen::Shape::Eight;
        p.crossing = levelgen::Crossing::Level;
        p.seed = 3;
        p.relief = 0.5f;
        const levelgen::Level lv = build(p);
        check(lv.report.crossings == 1 && lv.report.crossingClearance < 1.5f,
              "a level crossing brings both branches to one height",
              fmt("%d crossing(s), %.2f m apart", lv.report.crossings,
                  lv.report.crossingClearance));
    }

    // --- 9b) Each shape is the shape it says it is ---------------------------
    // A shape is worth having only for what makes it different, and for three of
    // the five that is whether -- and how often -- it crosses itself. Counting
    // that is also the one test with teeth on the crossing finder: a mirrored
    // pair counted twice lifts BOTH branches of a figure of eight over each
    // other, which leaves them level and looks like nothing at all.
    {
        struct Want { levelgen::Shape s; int crossings; bool straights; };
        const Want wants[] = {
            {levelgen::Shape::Ring,     0, false},
            {levelgen::Shape::Speedway, 0, true},
            {levelgen::Shape::Street,   0, true},
            {levelgen::Shape::Eight,    1, false},
            {levelgen::Shape::Knot,     3, false},
        };
        bool ok = true;
        std::string detail;
        for (const Want& w : wants) {
            int worstCross = -1;
            float longest = 0.0f;
            for (unsigned s = 1; s <= 8; ++s) {
                levelgen::Params p;
                p.seed = s;
                p.shape = w.s;
                p.length = 3500.0f;
                const levelgen::Level lv = build(p);
                if (worstCross < 0 || lv.report.crossings != w.crossings)
                    worstCross = lv.report.crossings;
                if (lv.report.crossings != w.crossings) ok = false;
                // How much of the lap is straight RELATIVE TO ITSELF. Against a
                // fixed radius a big smooth ring counts as all straight, which
                // is the opposite of the thing being measured -- what tells a
                // speedway from a ring is that its radius VARIES: nearly
                // infinite down the straights, tight in the turns.
                const Path pa = pathOf(lv.track.points);
                std::vector<float> radii;
                for (int j = 0; j < pa.ring; ++j) radii.push_back(radiusAt(pa, j));
                std::vector<float> sorted = radii;
                std::sort(sorted.begin(), sorted.end());
                const float median = sorted[sorted.size() / 2];
                float run = 0.0f, best = 0.0f;
                for (int j = 0; j < pa.ring; ++j) {
                    if (radii[j] >= 3.0f * median) {
                        run += pa.arc[j + 1] - pa.arc[j];
                        best = std::max(best, run);
                    } else run = 0.0f;
                }
                longest = std::max(longest, best / pa.total);
            }
            // A shape that claims straights must have a run of them worth the
            // name: a twentieth of the lap, unbroken. A ring measures zero here,
            // which is the point -- this is what separates them.
            if (w.straights && longest < 0.05f) ok = false;
            if (!w.straights && w.crossings == 0 && longest > 0.02f) ok = false;
            detail += fmt("%s %d\u00d7 %.0f%%str  ", levelgen::shapeName(w.s),
                          worstCross, longest * 100.0f);
        }
        check(ok, "every shape crosses itself as often as it claims to", detail);
    }

    // --- 10) Loops are real, and roadloop agrees -----------------------------
    {
        levelgen::Params p;
        p.seed = 11;
        p.loops = 2;
        p.length = 4000.0f;
        const levelgen::Level lv = build(p);
        std::string detail = fmt("%zu loop(s) planned", lv.track.loops.size());
        bool ok = true;
        if (!lv.track.loops.empty()) {
            const Path P = pathOf(lv.track.points);
            const fitzel::TerrainSettings ts = lv.terrain;
            const auto ground = [&](float x, float z) { return terrainBaseHeight(ts, x, z); };
            const std::vector<float> prof = profileOf(P, lv.track, ground);
            // The REAL planner: a spec it cannot honour is skipped in silence,
            // so a loop can simply not exist and nothing says so.
            const std::vector<roadloop::Loop> built =
                roadloop::plan(P.line, prof, P.pt, lv.track.loops, lv.track.width);
            ok = built.size() == lv.track.loops.size();
            for (const roadloop::Loop& l : built) if (!l.inverts) ok = false;
            detail += fmt(", %zu built by roadloop, all inverting: %s",
                          built.size(), ok ? "yes" : "NO");
        }
        check(ok, "every planned loop is one roadloop will actually build", detail);
    }

    // --- 15) The whole slider space ------------------------------------------
    {
        int bad = 0, tried = 0, softened = 0;
        std::string firstBad;
        float worstR = 1.0e9f, worstG = 0.0f;
        int dropped = 0;
        auto sweep = [&](levelgen::Params p, const char* what, unsigned seeds) {
            for (unsigned s = 1; s <= seeds; ++s) {
                p.seed = s;
                ++tried;
                const levelgen::Level lv = build(p);
                worstR = std::min(worstR, lv.report.minRadius / levelgen::minRadius(p));
                worstG = std::max(worstG, lv.report.maxGradient / p.maxGradient);
                dropped += lv.report.droppedFeatures;
                if (!lv.report.why.empty() && lv.report.ok) ++softened;
                if (lv.report.ok && lv.report.minRadius >= levelgen::minRadius(p) * 0.98f)
                    continue;
                if (bad == 0)
                    firstBad = fmt("%s seed %u: %s", what, s,
                                   lv.report.why.empty() ? "radius"
                                                         : lv.report.why.c_str());
                else if (bad < 6) firstBad += fmt(" | %s seed %u", what, s);
                ++bad;
            }
        };
        sweep(levelgen::Params{}, "defaults", 200);
        { levelgen::Params p; p.length = 800.0f;  p.corners = 6;  sweep(p, "short", 20); }
        { levelgen::Params p; p.length = 8000.0f; p.corners = 24; sweep(p, "long", 20); }
        { levelgen::Params p; p.irregular = 1.0f;               sweep(p, "wild", 20); }
        { levelgen::Params p; p.irregular = 0.0f;               sweep(p, "oval", 20); }
        { levelgen::Params p; p.relief = 2.0f;                  sweep(p, "alpine", 20); }
        { levelgen::Params p; p.relief = 0.2f;                  sweep(p, "plain", 20); }
        { levelgen::Params p; p.width = 40.0f;                  sweep(p, "wide", 20); }
        { levelgen::Params p; p.width = 8.0f;                   sweep(p, "narrow", 20); }
        { levelgen::Params p; p.cornerPace = 40.0f;             sweep(p, "fast", 20); }
        { levelgen::Params p; p.maxGradient = 0.04f;            sweep(p, "flat", 20); }
        { levelgen::Params p; p.maxBank = 18.0f;                sweep(p, "banked", 20); }
        { levelgen::Params p; p.bridges = 6; p.tunnels = 4;     sweep(p, "built-up", 20); }
        { levelgen::Params p; p.loops = 3;                      sweep(p, "looped", 20); }
        { levelgen::Params p; p.checkpoints = 20; p.gridSlots = 24;
                                                                sweep(p, "crowded", 20); }
        { levelgen::Params p; p.shape = levelgen::Shape::Speedway; sweep(p, "speedway", 20); }
        { levelgen::Params p; p.shape = levelgen::Shape::Street;   sweep(p, "street", 20); }
        { levelgen::Params p; p.shape = levelgen::Shape::Eight;    sweep(p, "eight", 20); }
        { levelgen::Params p; p.shape = levelgen::Shape::Knot;     sweep(p, "knot", 20); }
        { levelgen::Params p; p.shape = levelgen::Shape::Eight;
          p.crossing = levelgen::Crossing::Level;                  sweep(p, "level", 20); }
        { levelgen::Params p; p.shape = levelgen::Shape::Knot;
          p.crossing = levelgen::Crossing::Level;                  sweep(p, "knot level", 20); }
        check(bad == 0, "every corner of the slider space produces a drivable circuit",
              bad ? fmt("%d of %d failed, first: %s", bad, tried, firstBad.c_str())
                  : fmt("%d circuits; tightest corner %.0f%% of the minimum, steepest "
                        "%.0f%% of the limit, %d softened, %d features dropped",
                        tried, worstR * 100.0f, worstG * 100.0f, softened, dropped));
    }

    std::printf(g_fails ? "\nlevelcheck: %d FAILED\n" : "\nlevelcheck: all good\n",
                g_fails);
    return g_fails ? 1 : 0;
}
