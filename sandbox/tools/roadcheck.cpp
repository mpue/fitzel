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
//
// None of that is visible by looking at one screenshot, so it is measured here.
// A GL context is opened because a RoadSystem owns materials and textures; no
// pixel is ever drawn.
//
//   roadcheck [shaderDir]
// Exits non-zero if any measurement fails.

#include <cmath>
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

void setRoad(RoadSystem& r, const std::string& name,
             const std::vector<glm::vec2>& pts, float width) {
    r.name = name;
    r.clearPoints();
    for (const glm::vec2& p : pts)
        r.insertPoint(static_cast<int>(r.roadPts.size()), p);
    r.width = width;
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

    glfwDestroyWindow(win);
    glfwTerminate();
    std::printf(g_fails ? "\nroadcheck: %d FAILED\n" : "\nroadcheck: all good\n",
                g_fails);
    return g_fails ? 1 : 0;
}
