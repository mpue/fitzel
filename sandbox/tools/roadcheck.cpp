// roadcheck -- are there really SEVERAL roads, or only one that is drawn twice?
//
// A scene holds as many roads as the author draws (see RoadSet.hpp), and every
// way that can go wrong is a way that looks fine in the viewport for a while:
//
//   1. Only the last road is graded. Every road cuts its corridor into the SAME
//      terrain edit field, absolute deltas over the base terrain, and a build
//      that stops at the selected one -- or one that erases the cells another
//      road wrote -- leaves a road hanging over the natural ground. The ribbon is
//      drawn either way. You find out by driving into the gap.
//   2. Building twice is not the same as building once. The corridors are cut
//      from the bare terrain every time on purpose; if any of it accumulates
//      instead, a road sinks a little further every Build.
//   3. Only one road is ground. The ground query is what a craft stands on, and
//      one that answers for the selected road alone drops a car through the
//      second one.
//   4. The scene file keeps one road. Save and load is where a list quietly
//      becomes a single object again -- and the road that is gone is gone.
//   5. Deleting a road destroys it. The undo history borrows RoadSystem
//      pointers, so a delete that frees one is a crash on Ctrl+Z, which is the
//      one moment the author is already unhappy.
//   6. The vegetation mows a strip between two roads. Their centrelines travel to
//      the vegetation as one polyline, and without a break marker the gap between
//      road A's end and road B's start is treated as road.
//   7. The ground query answers with the road OVERHEAD. Two roads exist so one
//      can cross another; a query that returns the highest of them hands the
//      craft driving underneath the deck of the flyover.
//
// ...and then the junctions, which are found rather than authored (see
// RoadJunction.hpp) and so have their own ways of being wrong:
//
//   8. One meeting reported as four. At two-metre sampling a crossing hits from
//      several neighbouring segment pairs, and stacked aprons pull the profile
//      twice.
//   9. The two branches do not actually meet. Each road smooths its profile out
//      of the base terrain on its own, so they miss by decimetres unless they are
//      pulled together -- and the pull has to stay local, or a junction would
//      flatten the road either side of it.
//  10. The escape hatch closes. Lifting one branch past the clearance is how an
//      author asks for an over/under, and it is the only way they have.
//  11. A ring reports a junction at its own start line, where the sampled
//      centreline lands back on sample 0 -- or a figure-of-eight reports none.
//  12. A road running alongside another and stopping there is read as a T.
//  13. Building twice stops being identical once a two-pass build is in front of
//      it. This is the one the whole design is most likely to break.
//  14. The apron is scenery: drawn, but not something a craft stands on.
//  14b. There is a GAP where the ribbon stops and the apron has not started.
//      The ribbon is cut by whole quads, so a hole decided one sample at a time
//      runs a sample further than it was meant to -- two metres of daylight, and
//      the first thing anyone sees.
//  15. Junctions exist only after a Build -- a scene load re-lofts through
//      rebuildMeshes and would come back with holes where the crossings were.
//
// None of that is visible by looking at one screenshot, so it is measured here.
// A GL context is opened because a RoadSystem owns materials and textures; no
// pixel is ever drawn.
//
//   roadcheck [shaderDir]
// Exits non-zero if any measurement fails.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

#include <nlohmann/json.hpp>

#include <fitzel/asset/AssetDatabase.hpp>
#include <fitzel/graphics/Shader.hpp>
#include <fitzel/world/Terrain.hpp>

#include "../src/LevelGen.hpp"
#include "../src/RoadSet.hpp"
#include "../src/SandboxMath.hpp"

namespace fs = std::filesystem;

namespace {

int g_fails = 0;

void fail(const char* what, const std::string& detail) {
    std::printf("[FAIL] %s: %s\n", what, detail.c_str());
    ++g_fails;
}
void pass(const char* what, const std::string& detail) {
    std::printf("  ok   %s -- %s\n", what, detail.c_str());
}
void check(bool ok, const char* what, const std::string& detail) {
    if (ok) pass(what, detail); else fail(what, detail);
}

// A straight run of control points down the world's Z axis at `x`.
std::vector<glm::vec2> straightRun(float x, float z0, float z1, int n) {
    std::vector<glm::vec2> pts;
    for (int i = 0; i < n; ++i)
        pts.push_back({x, glm::mix(z0, z1, static_cast<float>(i) / (n - 1))});
    return pts;
}

// The same run turned ninety degrees: along X at `z`.
std::vector<glm::vec2> acrossRun(float z, float x0, float x1, int n) {
    std::vector<glm::vec2> pts;
    for (int i = 0; i < n; ++i)
        pts.push_back({glm::mix(x0, x1, static_cast<float>(i) / (n - 1)), z});
    return pts;
}

void setRoad(RoadSystem& r, const std::string& name,
             const std::vector<glm::vec2>& pts, float width) {
    r.name = name;
    r.clearPoints();
    for (const glm::vec2& p : pts)
        r.insertPoint(static_cast<int>(r.roadPts.size()), p);
    r.width = width;
}

// Is (x,z) under any triangle of this mesh, seen from straight above? That is
// what a gap in the carriageway IS: a place you can look down and find terrain
// where there should be asphalt. Plan view rather than a raycast, because the
// apron and the ribbon overlap in height by a centimetre by design and a gap is
// a hole in their FOOTPRINT, not in their stacking.
bool coveredInPlan(const std::vector<glm::vec3>& v,
                   const std::vector<std::uint32_t>& idx, glm::vec2 p) {
    auto side = [](glm::vec2 a, glm::vec2 b, glm::vec2 c) {
        return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    };
    // In BARYCENTRIC coordinates, with a tolerance -- not in raw cross products.
    // A probe on a shared edge (and a road probed at its own samples is on one
    // every time) gets a signed area of nothing from both triangles either side,
    // and the sign of nothing is a coin toss: the first version of this reported
    // one hole in a road that had none.
    for (std::size_t t = 0; t + 2 < idx.size(); t += 3) {
        const glm::vec2 a(v[idx[t]].x,     v[idx[t]].z);
        const glm::vec2 b(v[idx[t + 1]].x, v[idx[t + 1]].z);
        const glm::vec2 c(v[idx[t + 2]].x, v[idx[t + 2]].z);
        const float area = side(a, b, c);
        if (std::fabs(area) < 1e-9f) continue;          // a sliver, not a surface
        const float u = side(a, b, p) / area;
        const float w = side(b, c, p) / area;
        const float z = side(c, a, p) / area;
        if (u >= -1e-4f && w >= -1e-4f && z >= -1e-4f) return true;
    }
    return false;
}

// ...across every road in the set, because the apron belongs to one of them and
// the ribbon running into it belongs to the other.
bool anyRoadCovers(const RoadSet& roads, glm::vec2 p) {
    for (int i = 0; i < roads.count(); ++i)
        if (coveredInPlan(roads.at(i).collVerts(), roads.at(i).collIndices(), p))
            return true;
    return false;
}

// Walk `n` metres of carriageway either side of the origin along `dir`, at three
// offsets across it, and report the first place nothing covers. Returns the
// number of uncovered probes and writes one of them to `firstGap`.
int probeForGaps(const RoadSet& roads, glm::vec2 dir, float lat, float reach,
                 glm::vec2& firstGap) {
    const glm::vec2 side(dir.y, -dir.x);
    int bad = 0;
    for (float t = -reach; t <= reach; t += 0.5f)
        for (int k = -1; k <= 1; ++k) {
            const glm::vec2 p = dir * t + side * (lat * static_cast<float>(k));
            if (anyRoadCovers(roads, p)) continue;
            if (bad == 0) firstGap = p;
            ++bad;
        }
    return bad;
}

// The ground as the terrain mesh sees it after a build: base noise plus the
// corridor's delta, which is exactly what a wheel would rest on.
float groundAfterCut(const fitzel::TerrainSettings& s,
                     const fitzel::TerrainEditField& edit, glm::vec2 p) {
    return terrainBaseHeight(s, p.x, p.y) + edit.sample(p.x, p.y);
}

} // namespace

int main(int argc, char** argv) {
    const fs::path shDir = (argc > 1) ? fs::path(argv[1])
                                      : fs::path("assets/shaders");

    // --- A context to own the materials in -----------------------------------
    if (!glfwInit()) { std::printf("[roadcheck] glfwInit failed\n"); return 2; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* win = glfwCreateWindow(64, 64, "roadcheck", nullptr, nullptr);
    if (!win) { std::printf("[roadcheck] no GL 3.3 core context\n"); glfwTerminate(); return 2; }
    glfwMakeContextCurrent(win);
    if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress))) {
        std::printf("[roadcheck] glad failed\n"); glfwTerminate(); return 2;
    }

    fitzel::Shader lit = fitzel::Shader::fromFiles(
        (shDir / "lit.vert").generic_string(), (shDir / "lit.frag").generic_string());
    fitzel::AssetDatabase assetDb{FITZEL_CONTENT_DIR};

    // Hilly ground, so "the corridor was graded" is a claim with something to
    // measure: over a flat plain a road that was never cut looks like one that was.
    fitzel::TerrainSettings ts;
    fitzel::TerrainStreamer streamer(ts, 1);
    fitzel::setTerrainPresent(true);

    RoadSet roads(lit, assetDb, streamer, FITZEL_TEXTURE_DIR);

    // Two roads, well apart, running the same way: a main road and a service road.
    const std::vector<glm::vec2> ptsA = straightRun(-40.0f, -80.0f, 80.0f, 5);
    const std::vector<glm::vec2> ptsB = straightRun( 40.0f, -80.0f, 80.0f, 5);
    setRoad(roads.active(), "Main road", ptsA, 8.0f);
    const int iB = roads.add("Service road");
    setRoad(roads.at(iB), "Service road", ptsB, 4.0f);
    check(roads.count() == 2 && roads.selected() == iB, "two roads",
          "added road is selected, count=" + std::to_string(roads.count()));

    // --- 1) Every road grades its own corridor -------------------------------
    fitzel::TerrainEditField edit;
    glm::vec2 mn(0.0f), mx(0.0f);
    const bool built = roads.buildAll(edit, mn, mx);
    fitzel::setTerrainEditSnapshot(
        std::make_shared<const fitzel::TerrainEditField>(edit));
    roads.rebuildMeshes();
    check(built && !edit.deltas.empty(), "buildAll cut something",
          std::to_string(edit.deltas.size()) + " cells");

    // What each road cuts on its own, as the reference. The two run 80 m apart, so
    // their corridors share no cell: whatever buildAll wrote has to be exactly the
    // two of them side by side -- every cell, to the bit. That is the whole claim,
    // and it needs no tolerance to state.
    {
        fitzel::TerrainEditField solo[2];
        for (int i = 0; i < 2; ++i) {
            glm::vec2 a(0.0f), b(0.0f);
            roads.at(i).build(solo[i], a, b);
        }
        int missing = 0, differs = 0;
        for (int i = 0; i < 2; ++i)
            for (const auto& kv : solo[i].deltas) {
                const auto it = edit.deltas.find(kv.first);
                if (it == edit.deltas.end())            ++missing;
                else if (it->second != kv.second)       ++differs;
            }
        char msg[200];
        std::snprintf(msg, sizeof(msg),
                      "%zu + %zu cells alone, %zu together; %d missing, %d changed",
                      solo[0].deltas.size(), solo[1].deltas.size(),
                      edit.deltas.size(), missing, differs);
        check(!solo[0].deltas.empty() && !solo[1].deltas.empty() &&
                  missing == 0 && differs == 0 &&
                  edit.deltas.size() ==
                      solo[0].deltas.size() + solo[1].deltas.size(),
              "every road's corridor is in the field", msg);
    }

    // ...and the ground under each road really is under it: the graded corridor
    // sits a little BELOW the asphalt (the clearance the ribbon is lofted by, plus
    // the sub-cell bulge of the base noise that a delta on a one-metre grid cannot
    // flatten away) and never, anywhere, above it. Terrain through the carriageway
    // is what a road that was not graded looks like.
    for (int i = 0; i < roads.count(); ++i) {
        const RoadSystem& r = roads.at(i);
        float above = 0.0f, below = 0.0f, natural = 0.0f;
        int   samples = 0, missing = 0;
        for (float z = -70.0f; z <= 70.0f; z += 5.0f) {
            const glm::vec2 p(r.roadPts.front().x, z);
            float surf = 0.0f;
            if (!r.surfaceHeightAt(p, r.surfaceHalf(), surf)) { ++missing; continue; }
            const float d = groundAfterCut(ts, edit, p) - surf;
            above   = std::max(above,   d);
            below   = std::max(below,  -d);
            natural = std::max(natural,
                               std::fabs(terrainBaseHeight(ts, p.x, p.y) - surf));
            ++samples;
        }
        char msg[200];
        std::snprintf(msg, sizeof(msg),
                      "%s: %d samples, %d off the ribbon, %.3f m above / %.3f m "
                      "below the asphalt (uncut ground is %.2f m out)",
                      r.name.c_str(), samples, missing, above, below, natural);
        check(samples >= 20 && missing == 0 && above <= 0.01f && below < 1.5f,
              "nothing pokes through the road", msg);
    }

    // --- 2) Building again changes nothing -----------------------------------
    {
        fitzel::TerrainEditField again = edit;
        glm::vec2 a(0.0f), b(0.0f);
        roads.buildAll(again, a, b);
        bool same = again.deltas.size() == edit.deltas.size();
        float worst = 0.0f;
        if (same)
            for (const auto& kv : again.deltas) {
                const auto it = edit.deltas.find(kv.first);
                if (it == edit.deltas.end()) { same = false; break; }
                worst = std::max(worst, std::fabs(it->second - kv.second));
            }
        char msg[128];
        std::snprintf(msg, sizeof(msg), "%zu cells, worst drift %.6f m",
                      again.deltas.size(), worst);
        check(same && worst == 0.0f, "second Build is identical", msg);
    }

    // --- 3) Both roads are ground --------------------------------------------
    for (int i = 0; i < roads.count(); ++i) {
        const RoadSystem& r = roads.at(i);
        const glm::vec2 p(r.roadPts.front().x, 0.0f);
        float own = 0.0f, set = 0.0f;
        const bool a = r.surfaceHeightAt(p, r.surfaceHalf(), own);
        const bool b = roads.surfaceHeightAt(p, set, 1.0e9f);
        char msg[160];
        std::snprintf(msg, sizeof(msg), "%s: road says %.3f, the set says %.3f",
                      r.name.c_str(), own, set);
        check(a && b && std::fabs(own - set) < 1e-3f, "the set answers for it", msg);
    }
    {
        // ...and a hidden road is not ground either: that is what the checkbox in
        // the road list means.
        RoadSystem& b = roads.at(1);
        b.enabled = false;
        float y = 0.0f;
        const bool hit = roads.surfaceHeightAt(glm::vec2(b.roadPts.front().x, 0.0f),
                                               y, 1.0e9f);
        b.enabled = true;
        check(!hit, "a hidden road is not ground", "query over it comes back empty");
    }

    // --- 4) Both roads survive the scene file --------------------------------
    {
        nlohmann::json j;
        roads.save(j);
        RoadSet other(lit, assetDb, streamer, FITZEL_TEXTURE_DIR);
        other.load(j);
        bool ok = other.count() == roads.count();
        std::string detail = std::to_string(other.count()) + " roads back";
        for (int i = 0; ok && i < other.count(); ++i) {
            ok = other.at(i).name == roads.at(i).name &&
                 other.at(i).width == roads.at(i).width &&
                 other.at(i).roadPts.size() == roads.at(i).roadPts.size();
            if (!ok) detail += ", road " + std::to_string(i) + " came back wrong";
        }
        check(ok, "save/load keeps every road", detail);

        // A scene from before roads were plural: one object under "road".
        nlohmann::json legacy;
        legacy["road"] = j["roads"][1];
        RoadSet old(lit, assetDb, streamer, FITZEL_TEXTURE_DIR);
        old.load(legacy);
        check(old.count() == 1 && old.at(0).name == roads.at(1).name,
              "an old single-road scene still loads",
              old.count() == 1 ? old.at(0).name : "count=" + std::to_string(old.count()));
    }

    // --- 5) Deleting a road does not destroy it ------------------------------
    {
        const int         id  = roads.idAt(1);
        const RoadSystem* was = &roads.at(1);
        const std::size_t pts = roads.at(1).roadPts.size();
        const bool gone = roads.remove(1);
        const bool one  = roads.count() == 1;
        roads.setAlive(id, true);          // ...what Ctrl+Z does
        const bool back = roads.count() == 2 && &roads.at(1) == was &&
                          roads.at(1).roadPts.size() == pts;
        check(gone && one && back, "delete + undo is the same object",
              back ? "same RoadSystem, points intact" : "the road came back changed");
    }
    {
        // The last road cannot go: everything downstream is written against "the
        // road being edited" and would have to ask whether there is one.
        RoadSet solo(lit, assetDb, streamer, FITZEL_TEXTURE_DIR);
        check(!solo.remove(0) && solo.count() == 1, "the last road stays",
              "remove refused, count=" + std::to_string(solo.count()));
    }

    // --- 6) Nothing is mowed between two roads -------------------------------
    {
        const std::vector<glm::vec2> cls = roads.centerlines();
        int breaks = 0;
        for (const glm::vec2& p : cls) if (isLineBreak(p)) ++breaks;
        // Halfway between the two carriageways, 40 m from either: a phantom
        // segment joining road A's end to road B's start would run right through
        // here and report a distance of nothing.
        const float d2 = roadDistanceSq(cls, 0.0f, 0.0f);
        char msg[160];
        std::snprintf(msg, sizeof(msg), "%d break(s), midpoint is %.1f m from any road",
                      breaks, std::sqrt(d2));
        check(breaks == 1 && d2 > 30.0f * 30.0f, "the gap between roads is not road",
              msg);
    }

    // --- 7) The ground query does not answer with the flyover ----------------
    // Its own case because it is its own claim, and one the junctions lean on:
    // the documented way to refuse a junction is to lift one branch, which makes
    // two-road over/unders something authors are actively told to build.
    {
        RoadSet rs(lit, assetDb, streamer, FITZEL_TEXTURE_DIR);
        setRoad(rs.active(), "Under", straightRun(0.0f, -60.0f, 60.0f, 5), 8.0f);
        const int ib = rs.add("Over");
        setRoad(rs.at(ib), "Over", acrossRun(0.0f, -60.0f, 60.0f, 5), 8.0f);
        rs.at(ib).setLift(2, 8.0f);                 // a deck two storeys up
        fitzel::TerrainEditField e;
        glm::vec2 a(0.0f), b(0.0f);
        rs.buildAll(e, a, b);
        // On the lower carriageway, three metres off the flyover's centreline:
        // both surfaces are in range, and the nearer one is the one being driven.
        float low = 0.0f, set = 0.0f;
        const bool l = rs.at(0).surfaceHeightAt(glm::vec2(0.0f, 3.0f),
                                                rs.at(0).surfaceHalf(), low);
        const bool s = rs.surfaceHeightAt(glm::vec2(0.0f, 3.0f), set, 1.0e9f);
        char msg[160];
        std::snprintf(msg, sizeof(msg), "under is %.2f, the set says %.2f", low, set);
        check(l && s && std::fabs(low - set) < 0.05f,
              "the set answers with the road you are on", msg);
    }

    // --- 8..15) Junctions -----------------------------------------------------
    // Each case builds its own RoadSet and its own edit field, so a junction
    // scene cannot disturb the checks above or the next one along.
    auto crossScene = [&](RoadSet& rs, float liftB) {
        setRoad(rs.active(), "North-south", straightRun(0.0f, -60.0f, 60.0f, 5), 8.0f);
        const int ib = rs.add("East-west");
        setRoad(rs.at(ib), "East-west", acrossRun(0.0f, -60.0f, 60.0f, 5), 8.0f);
        if (liftB != 0.0f) rs.at(ib).setLift(2, liftB);
    };

    {
        RoadSet rs(lit, assetDb, streamer, FITZEL_TEXTURE_DIR);
        crossScene(rs, 0.0f);
        fitzel::TerrainEditField e;
        glm::vec2 a(0.0f), b(0.0f);
        rs.buildAll(e, a, b);

        // 8) One meeting, one crossing.
        const std::vector<roadjunction::Crossing>& js = rs.junctions();
        char msg[192];
        std::snprintf(msg, sizeof(msg), "%zu crossing(s), %zu-sided apron", js.size(),
                      js.empty() ? std::size_t(0) : js[0].plate.size());
        check(js.size() == 1 && glm::length(js[0].at) < 1.0f &&
                  js[0].plate.size() >= 3 && !js[0].tee,
              "two roads meet once", msg);

        // 9) ...at one height, and only there. Each road is asked on its OWN
        //    carriageway a few metres off the other's centreline, so the answer is
        //    that road's ribbon rather than whichever happens to be nearer.
        float ya = 0.0f, yb = 0.0f;
        const bool ga = rs.at(0).surfaceHeightAt(glm::vec2(0.0f, 3.0f),
                                                 rs.at(0).surfaceHalf(), ya);
        const bool gb = rs.at(1).surfaceHeightAt(glm::vec2(3.0f, 0.0f),
                                                 rs.at(1).surfaceHalf(), yb);
        std::snprintf(msg, sizeof(msg), "%.3f vs %.3f, %.0f mm apart", ya, yb,
                      std::fabs(ya - yb) * 1000.0f);
        check(ga && gb && std::fabs(ya - yb) < 0.05f, "both branches meet in height",
              msg);

        // ...and the same road built alone is unchanged well outside the ramp.
        {
            RoadSet solo(lit, assetDb, streamer, FITZEL_TEXTURE_DIR);
            setRoad(solo.active(), "North-south",
                    straightRun(0.0f, -60.0f, 60.0f, 5), 8.0f);
            fitzel::TerrainEditField se;
            glm::vec2 sa(0.0f), sb(0.0f);
            solo.buildAll(se, sa, sb);
            const glm::vec2 far(0.0f, -55.0f);   // 55 m out, past 3x the 18 m blend
            float withJ = 0.0f, alone = 0.0f;
            const bool p = rs.at(0).surfaceHeightAt(far, rs.at(0).surfaceHalf(), withJ);
            const bool q = solo.at(0).surfaceHeightAt(far, solo.at(0).surfaceHalf(),
                                                      alone);
            std::snprintf(msg, sizeof(msg), "%.3f with the junction, %.3f without",
                          withJ, alone);
            check(p && q && std::fabs(withJ - alone) < 0.01f,
                  "the pull stays local", msg);
        }

        // 13) Building again changes nothing, junction and all. The property the
        //     two-pass build is most likely to have broken.
        {
            fitzel::TerrainEditField again = e;
            glm::vec2 x(0.0f), y(0.0f);
            rs.buildAll(again, x, y);
            bool same = again.deltas.size() == e.deltas.size();
            float worst = 0.0f;
            if (same)
                for (const auto& kv : again.deltas) {
                    const auto it = e.deltas.find(kv.first);
                    if (it == e.deltas.end()) { same = false; break; }
                    worst = std::max(worst, std::fabs(it->second - kv.second));
                }
            std::snprintf(msg, sizeof(msg), "%zu cells, worst drift %.6f m",
                          again.deltas.size(), worst);
            check(same && worst == 0.0f, "a second Build over a junction is identical",
                  msg);
        }

        // 14) The apron is drawn AND collided -- one mesh, and its triangles are
        //     in the array Play hands to the physics world.
        {
            const RoadSystem& owner = rs.at(0);   // the lower-indexed road owns it
            bool onIt = false;
            for (const glm::vec3& v : owner.collVerts())
                if (glm::length(glm::vec2(v.x, v.z)) < 12.0f &&
                    std::fabs(v.y - js[0].y) < 0.3f) { onIt = true; break; }
            std::snprintf(msg, sizeof(msg), "apron mesh %s, %zu collider verts",
                          owner.hasJunctions() ? "built" : "MISSING",
                          owner.collVerts().size());
            check(owner.hasJunctions() && onIt, "the apron is ground too", msg);

            // ...and the right way up. The apron is one flat polygon fanned from
            // its centroid, so a reversed winding is invisible rather than wrong
            // -- back-face culled from above, and lit from underneath if it were
            // not. Reproduce the first triangle build() emits and look at its
            // normal, which is the only way to catch it without a screenshot.
            glm::vec2 c(0.0f);
            for (const glm::vec2& p : js[0].plate) c += p;
            c /= static_cast<float>(js[0].plate.size());
            const glm::vec3 t0(c.x, 0.0f, c.y);
            const glm::vec3 t1(js[0].plate[0].x, 0.0f, js[0].plate[0].y);
            const glm::vec3 t2(js[0].plate[1].x, 0.0f, js[0].plate[1].y);
            const float up = glm::cross(t1 - t0, t2 - t0).y;
            std::snprintf(msg, sizeof(msg), "first triangle's normal.y = %+.2f", up);
            check(up > 0.0f, "the apron faces up", msg);
        }

        // 14b) ...with no daylight between the two. Both carriageways are walked
        //      right through the crossing at three offsets across, and every
        //      probe has to land on asphalt -- ribbon or apron, either will do.
        {
            glm::vec2 gap(0.0f);
            const int a = probeForGaps(rs, glm::vec2(0.0f, 1.0f), 3.0f, 25.0f, gap);
            glm::vec2 gap2(0.0f);
            const int b = probeForGaps(rs, glm::vec2(1.0f, 0.0f), 3.0f, 25.0f, gap2);
            if (a == 0 && b > 0) gap = gap2;
            std::snprintf(msg, sizeof(msg),
                          "%d of ~300 probes uncovered%s", a + b,
                          (a + b) ? "" : " -- the joint is closed");
            if (a + b)
                std::snprintf(msg, sizeof(msg),
                              "%d probes uncovered, first at %.1f, %.1f", a + b,
                              gap.x, gap.y);
            check(a + b == 0, "no gap where the ribbon meets the apron", msg);
        }

        // 15) A scene load re-derives it, without a Build.
        {
            nlohmann::json j;
            rs.save(j);
            RoadSet back(lit, assetDb, streamer, FITZEL_TEXTURE_DIR);
            back.load(j);
            back.rebuildMeshes();
            std::snprintf(msg, sizeof(msg), "%zu crossing(s) after the load",
                          back.junctions().size());
            check(back.junctions().size() == js.size(),
                  "a reloaded scene still has its junctions", msg);
        }
    }

    // 10) The escape hatch: lift one branch past the clearance and it is an
    //     over/under again, exactly as it was before junctions existed.
    {
        RoadSet rs(lit, assetDb, streamer, FITZEL_TEXTURE_DIR);
        crossScene(rs, rs.active().junctionStyle.clearance + 3.0f);
        fitzel::TerrainEditField e;
        glm::vec2 a(0.0f), b(0.0f);
        rs.buildAll(e, a, b);
        float low = 0.0f, high = 0.0f;
        rs.at(0).surfaceHeightAt(glm::vec2(0.0f, 3.0f), rs.at(0).surfaceHalf(), low);
        rs.at(1).surfaceHeightAt(glm::vec2(3.0f, 0.0f), rs.at(1).surfaceHalf(), high);
        char msg[160];
        std::snprintf(msg, sizeof(msg), "%zu crossing(s), %.1f m apart",
                      rs.junctions().size(), high - low);
        check(rs.junctions().empty() &&
                  high - low >= rs.at(0).junctionStyle.clearance,
              "a lifted branch is still an over/under", msg);
    }

    // 11) A ring does not meet itself at its own seam -- and a figure-of-eight
    //     does meet itself, exactly once, on one road.
    {
        RoadSet oval(lit, assetDb, streamer, FITZEL_TEXTURE_DIR);
        setRoad(oval.active(), "Oval",
                {{-40.0f, -30.0f}, {-40.0f, 30.0f}, {40.0f, 30.0f}, {40.0f, -30.0f}},
                8.0f);
        oval.active().closed = true;
        fitzel::TerrainEditField oe;
        glm::vec2 a(0.0f), b(0.0f);
        oval.buildAll(oe, a, b);
        check(oval.junctions().empty(), "a ring has no junction at its seam",
              std::to_string(oval.junctions().size()) + " crossing(s)");

        RoadSet eight(lit, assetDb, streamer, FITZEL_TEXTURE_DIR);
        setRoad(eight.active(), "Figure of eight",
                {{-40.0f, -30.0f}, {-40.0f, 30.0f}, {40.0f, -30.0f}, {40.0f, 30.0f}},
                8.0f);
        eight.active().closed = true;
        fitzel::TerrainEditField ee;
        eight.buildAll(ee, a, b);
        const std::vector<roadjunction::Crossing>& js = eight.junctions();
        char msg[160];
        std::snprintf(msg, sizeof(msg), "%zu crossing(s), roads %d and %d", js.size(),
                      js.empty() ? -1 : js[0].roadA, js.empty() ? -1 : js[0].roadB);
        check(js.size() == 1 && js[0].roadA == js[0].roadB && js[0].roadA == 0,
              "a figure-of-eight meets itself once", msg);

        // The self-crossing has to close its joint too, and it is the harder
        // case by some way: the road is cut twice by ONE apron, at two different
        // headings, on a curve, and at an oblique angle -- so the arc length the
        // cut was measured in runs longer than the straight the apron was cut
        // from. Probed across the full carriageway, not just down the middle:
        // the corner of a quad is where an apron runs out first.
        if (!js.empty()) {
            eight.rebuildMeshes();
            const RoadSystem& r = eight.at(0);
            const std::vector<glm::vec2>& cl = r.centerline();
            const float lat = r.width * 0.5f - 0.2f;   // just inside the edge
            int bad = 0;
            glm::vec2 gap(0.0f);
            // Probed BETWEEN two samples, not at one: a point on a sample sits
            // on the seam between two quads, where being covered by neither is
            // the one thing a whole-quad hole cannot be. Halfway along is inside
            // exactly one quad, so a dropped one shows up as nothing at all.
            for (std::size_t i = 1; i + 2 < cl.size(); ++i) {
                const glm::vec2 mid = 0.5f * (cl[i] + cl[i + 1]);
                if (glm::distance(mid, js[0].at) > 20.0f) continue;
                glm::vec2 fwd = cl[i + 1] - cl[i];
                if (glm::length(fwd) < 1e-4f) continue;
                fwd = glm::normalize(fwd);
                const glm::vec2 side(fwd.y, -fwd.x);
                for (int k = -1; k <= 1; ++k) {
                    const glm::vec2 p = mid + side * (lat * static_cast<float>(k));
                    if (anyRoadCovers(eight, p)) continue;
                    if (bad == 0) gap = p;
                    ++bad;
                }
            }
            if (bad)
                std::snprintf(msg, sizeof(msg), "%d uncovered, first at %.1f, %.1f",
                              bad, gap.x, gap.y);
            else
                std::snprintf(msg, sizeof(msg),
                              "the whole carriageway is covered through the crossing");
            check(bad == 0, "the self-crossing has no gap either", msg);
        }
    }

    // 12) Alongside is not a junction; head-on is.
    {
        RoadSet para(lit, assetDb, streamer, FITZEL_TEXTURE_DIR);
        setRoad(para.active(), "Main", straightRun(0.0f, -60.0f, 60.0f, 5), 8.0f);
        const int ib = para.add("Slip");
        // Four metres off the main road's centreline -- INSIDE the carriageway
        // plus its margin, so only the angle can refuse it -- and stopping there.
        setRoad(para.at(ib), "Slip", straightRun(4.0f, -60.0f, 0.0f, 4), 4.0f);
        fitzel::TerrainEditField pe;
        glm::vec2 a(0.0f), b(0.0f);
        para.buildAll(pe, a, b);
        check(para.junctions().empty(), "a road ending alongside is not a junction",
              std::to_string(para.junctions().size()) + " crossing(s)");

        RoadSet tee(lit, assetDb, streamer, FITZEL_TEXTURE_DIR);
        setRoad(tee.active(), "Main", straightRun(0.0f, -60.0f, 60.0f, 5), 8.0f);
        const int it = tee.add("Side");
        setRoad(tee.at(it), "Side", acrossRun(0.0f, -60.0f, 0.0f, 4), 6.0f);
        fitzel::TerrainEditField te;
        tee.buildAll(te, a, b);
        const std::vector<roadjunction::Crossing>& js = tee.junctions();
        char msg[160];
        std::snprintf(msg, sizeof(msg), "%zu crossing(s), tee=%d", js.size(),
                      js.empty() ? -1 : static_cast<int>(js[0].tee));
        check(js.size() == 1 && js[0].tee, "a road ending head-on is a T", msg);
    }

    // --- 16) A GENERATED circuit survives being built --------------------------
    // levelcheck measures the generator against plain data and has no GL, so it
    // can say the description is sound and nothing about what RoadSystem makes of
    // it. This is the other half, and it is here rather than there because the
    // context and the terrain field are already open: a whole generated circuit,
    // put into a real road, cut into real ground, twice.
    //
    // The figure of eight with a flyover on purpose -- it is the shape with a
    // bridge, a lift and a self-crossing all on one lap, which is the most the
    // two-pass build is ever asked to keep straight.
    {
        levelgen::Params gp;
        gp.seed  = 5;
        gp.shape = levelgen::Shape::Eight;
        gp.length = 2400.0f;
        const levelgen::Level lvl = levelgen::generate(
            gp, [](const fitzel::TerrainSettings& s, float x, float z) {
                return terrainBaseHeight(s, x, z);
            });

        RoadSet gen(lit, assetDb, streamer, FITZEL_TEXTURE_DIR);
        RoadSystem& gr = gen.at(0);
        gr.name = "Circuit";
        gr.width = lvl.track.width;
        gr.grade = lvl.track.grade;
        gr.shoulder = lvl.track.shoulder;
        gr.edgeWidth = lvl.track.edgeWidth;
        gr.edgeAngle = lvl.track.edgeAngle;
        RoadSystem::Shape sh;
        sh.points = lvl.track.points;
        sh.lifts  = lvl.track.lift;
        sh.banks  = lvl.track.bank;
        sh.closed = true;
        for (const levelgen::Span& b : lvl.track.bridges) sh.bridges.push_back({b.a, b.b});
        for (const levelgen::Span& t : lvl.track.tunnels) sh.tunnels.push_back({t.a, t.b});
        sh.loops = lvl.track.loops;
        sh.label = "Circuit";
        gr.setShape(sh);

        fitzel::TerrainEditField ge;
        glm::vec2 a(0.0f), b(0.0f);
        const bool builtIt = gen.buildAll(ge, a, b);
        char msg[192];
        std::snprintf(msg, sizeof(msg), "%zu points, %zu cells, %d ribbon verts",
                      lvl.track.points.size(), ge.deltas.size(), gr.verts());
        check(builtIt && gr.built() && !ge.deltas.empty(),
              "a generated circuit builds", msg);

        // ...and building it again is identical. The generated shape is the one
        // most likely to break that, and levelcheck cannot see it at all.
        {
            fitzel::TerrainEditField again = ge;
            gen.buildAll(again, a, b);
            bool same = again.deltas.size() == ge.deltas.size();
            float worst = 0.0f;
            if (same)
                for (const auto& kv : again.deltas) {
                    const auto it = ge.deltas.find(kv.first);
                    if (it == ge.deltas.end()) { same = false; break; }
                    worst = std::max(worst, std::fabs(it->second - kv.second));
                }
            std::snprintf(msg, sizeof(msg), "%zu cells, worst drift %.6f m",
                          again.deltas.size(), worst);
            check(same && worst == 0.0f,
                  "a second Build of a generated circuit is identical", msg);
        }

        // The lifted branch must be an over/under, not a junction: the generator
        // asked for a flyover, and roadjunction is what decides whether it got
        // one. Nothing else in either harness closes that loop.
        std::snprintf(msg, sizeof(msg), "%zu junction(s) found, %zu bridge(s) asked for",
                      gen.junctions().size(), lvl.track.bridges.size());
        check(gen.junctions().empty(), "a generated flyover stays an over/under", msg);

        // ...and the ground query answers on the circuit, everywhere along it.
        int off = 0;
        for (std::size_t i = 0; i < gr.centerline().size(); i += 7) {
            float y = 0.0f;
            if (!gen.surfaceHeightAt(gr.centerline()[i], y, 1.0e9f)) ++off;
        }
        std::snprintf(msg, sizeof(msg), "%d of %zu stations off the road", off,
                      gr.centerline().size() / 7);
        check(off == 0, "every station of a generated circuit is ground", msg);
    }

    // The junction sheet a pack ships is found by name, the way the normal map
    // is. Cheap to get wrong and silent when it is: the apron just keeps wearing
    // the carriageway's asphalt and nobody knows the sheet was there.
    {
        const std::string got = roads.at(0).crossingFor("roads_basic.png");
        check(got == "roads_basic_crossing.png",
              "the junction sheet follows its surface",
              got.empty() ? "nothing found for roads_basic.png" : got);
    }

    glfwDestroyWindow(win);
    glfwTerminate();
    std::printf(g_fails ? "\nroadcheck: %d FAILED\n" : "\nroadcheck: all good\n",
                g_fails);
    return g_fails ? 1 : 0;
}
