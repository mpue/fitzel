// splineplacecheck -- do objects placed along a spline path land where the panel
// says, and come out as the objects they were copied from?
//
// Placing along a path is a one-shot: the copies are ordinary entities the moment
// the button is pressed, and a mistake in them is a mistake in the scene. The
// ways it can go wrong all look like a plausible row of objects:
//
//   1. The spacing drifts, the first copy ignores "First at", or a closed loop
//      gets two copies stacked on its seam.
//   2. "Side" puts them on the LEFT. The spline generator's frame calls up x t
//      "right", which is the traveller's left in this right-handed world -- a
//      fence never noticed, because a fence is symmetric.
//   3. A copied subtree keeps pointing at the original's ids, so the children of
//      every copy hang off the first one; or a copy of a prefab instance loses the
//      link to its prefab; or a copy of a plain object gains a link to nothing.
//   4. A bare path quietly creates three palette materials in the library, or a
//      scene that holds one loads it back as a fence.
//
// Measures against a real SplineSystem with a sloped ground; a bare path builds
// no geometry, so there is no GL and no window.
//
//   build/release/bin/splineplacecheck.exe

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "../src/Component.hpp"
#include "../src/PrefabSystem.hpp"
#include "../src/SceneTypes.hpp"
#include "../src/SplineGen.hpp"
#include "../src/SplinePlace.hpp"
#include "../src/SplineSystem.hpp"

namespace {

int failures = 0;

void check(bool ok, const std::string& what, const std::string& detail = {}) {
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what.c_str(),
                detail.empty() ? "" : "  -- ", detail.c_str());
    if (!ok) ++failures;
}

bool near(float a, float b, float tol = 0.02f) { return std::abs(a - b) <= tol; }

std::string v3(const glm::vec3& v) {
    char b[96];
    std::snprintf(b, sizeof b, "(%.3f, %.3f, %.3f)", v.x, v.y, v.z);
    return b;
}

// Ground rising 0.1 m per metre of +X, so "back on the ground at the side" is
// measurable: two metres either side of x = 0 is 0.2 m of height either way.
float ground(float x, float) { return 0.1f * x; }

Entity mk(int id, int parent, const std::string& name, const glm::vec3& pos) {
    Entity e;
    e.id = id; e.parent = parent; e.name = name;
    e.type = EntityType::Empty;
    e.center = e.localCenter = pos;
    return e;
}

const Entity* byId(const std::vector<Entity>& v, int id) {
    for (const Entity& e : v) if (e.id == id) return &e;
    return nullptr;
}

} // namespace

int main() {
    std::vector<MaterialDef> materials;
    SplineSystem sp;
    sp.groundAt = ground;

    // --- A bare path --------------------------------------------------------
    // Straight along +Z, 100 m, at x = 0.
    const int bare = sp.addPath(splinegen::Preset::Bare, "Lamp line");
    sp.insertPoint(bare, 0, {0.0f, 0.0f});
    sp.insertPoint(bare, 1, {0.0f, 100.0f});
    sp.update(materials);
    check(sp.paths[bare].kind == splinegen::Kind::Path, "Bare preset makes a Path");
    check(materials.empty(), "a bare path creates no palette materials",
          std::to_string(materials.size()) + " created");
    check(sp.runs()[bare].geo.batches.empty() && sp.runs()[bare].geo.colliders.empty(),
          "a bare path builds no geometry and no colliders");
    check(near(sp.runs()[bare].geo.length, 100.0f, 0.5f), "...but knows its length",
          std::to_string(sp.runs()[bare].geo.length));

    // --- Spacing and start --------------------------------------------------
    splineplace::Settings cfg;
    cfg.spacing = 10.0f;
    auto at = splineplace::spots(sp, bare, cfg);
    check(at.size() == 10, "every 10 m over 100 m: 10 copies (the end is not a station)",
          std::to_string(at.size()));
    bool even = true, yaw0 = true;
    for (std::size_t k = 0; k < at.size(); ++k) {
        even &= near(at[k].pos.z, 10.0f * k, 0.05f) && near(at[k].pos.x, 0.0f);
        yaw0 &= near(at[k].yawDeg, 0.0f, 0.1f);
    }
    check(even, "copies stand on the path at 0, 10, 20 ... m",
          at.empty() ? "" : v3(at.back().pos));
    check(yaw0, "a path running +Z turns them to yaw 0");

    cfg.start = 25.0f;
    at = splineplace::spots(sp, bare, cfg);
    check(!at.empty() && near(at.front().pos.z, 25.0f, 0.05f), "First at 25 m is 25 m",
          at.empty() ? "none" : v3(at.front().pos));
    check(at.size() == 8, "...and nothing before it (25, 35 ... 95)",
          std::to_string(at.size()));
    cfg.start = 0.0f;

    // --- Side: right means right --------------------------------------------
    cfg.side = 2.0f;
    at = splineplace::spots(sp, bare, cfg);
    check(!at.empty() && near(at.front().pos.x, -2.0f),
          "Side +2 walking +Z is x = -2 (the traveller's right)",
          at.empty() ? "none" : v3(at.front().pos));
    check(!at.empty() && near(at.front().pos.y, ground(-2.0f, 0.0f)),
          "...standing on the ground there, not at the path's height",
          at.empty() ? "none" : v3(at.front().pos));

    cfg.bothSides = true;
    at = splineplace::spots(sp, bare, cfg);
    check(at.size() == 20, "Both sides doubles them", std::to_string(at.size()));
    if (at.size() >= 2) {
        check(near(at[1].pos.x, 2.0f) && near(at[1].pos.y, ground(2.0f, 0.0f)),
              "the mirror copy stands on the left, on its own ground", v3(at[1].pos));
        check(near(at[1].yawDeg, 180.0f, 0.1f), "...and faces back across the path",
              std::to_string(at[1].yawDeg));
    }
    cfg.bothSides = false;
    cfg.side = 0.0f;

    cfg.align = false;
    cfg.turn  = 45.0f;
    at = splineplace::spots(sp, bare, cfg);
    check(!at.empty() && near(at.front().yawDeg, 45.0f, 0.1f),
          "not turning with the path leaves only the Turn");
    cfg.align = true;
    cfg.turn  = 0.0f;

    cfg.height = 3.0f;
    at = splineplace::spots(sp, bare, cfg);
    check(!at.empty() && near(at.front().pos.y, 3.0f), "Height lifts them");
    cfg.height = 0.0f;

    // --- Heading follows the path ------------------------------------------
    const int eastward = sp.addPath(splinegen::Preset::Bare);
    sp.insertPoint(eastward, 0, {0.0f, 0.0f});
    sp.insertPoint(eastward, 1, {50.0f, 0.0f});
    sp.update(materials);
    at = splineplace::spots(sp, eastward, cfg);
    check(!at.empty() && near(at.front().yawDeg, 90.0f, 0.1f),
          "a path running +X turns them to yaw 90",
          at.empty() ? "none" : std::to_string(at.front().yawDeg));
    check(!at.empty() && near(at.back().pos.y, ground(at.back().pos.x, 0.0f)),
          "on a slope they follow the ground up it",
          at.empty() ? "none" : v3(at.back().pos));

    // --- A closed loop: nothing doubled on the seam -------------------------
    const int loop = sp.addPath(splinegen::Preset::Bare);
    sp.insertPoint(loop, 0, {0.0f, 0.0f});
    sp.insertPoint(loop, 1, {40.0f, 0.0f});
    sp.insertPoint(loop, 2, {40.0f, 40.0f});
    sp.insertPoint(loop, 3, {0.0f, 40.0f});
    sp.paths[loop].closed = true;
    sp.touch(loop);
    sp.update(materials);
    // 7 m does not divide the loop, so the spacing is evened out: the last copy
    // must be a full (evened) step from the first, not crowding it at the seam.
    cfg.spacing = 7.0f;
    const float loopLen = sp.runs()[loop].geo.length;
    const float used    = splineplace::spacingOn(sp, loop, cfg);
    check(near(loopLen / used, std::round(loopLen / used), 1e-3f) && near(used, 7.0f, 0.5f),
          "a loop's spacing is evened out to divide it",
          std::to_string(loopLen) + " m loop at " + std::to_string(used) + " m");
    at = splineplace::spots(sp, loop, cfg);
    float closest = 1e9f;
    for (std::size_t a = 0; a < at.size(); ++a)
        for (std::size_t b = a + 1; b < at.size(); ++b)
            closest = std::min(closest, glm::distance(at[a].pos, at[b].pos));
    check(static_cast<int>(at.size()) == static_cast<int>(std::round(loopLen / used)) &&
          closest > used * 0.8f,
          "a closed loop goes all the way round with no copy crowding the seam",
          std::to_string(at.size()) + " copies, closest pair " + std::to_string(closest) + " m");
    cfg.start = 30.0f;
    const std::size_t before = at.size();
    at = splineplace::spots(sp, loop, cfg);
    check(at.size() == before, "on a loop, First at turns the pattern rather than dropping copies",
          std::to_string(at.size()));
    cfg.start = 0.0f;

    // --- The budget ---------------------------------------------------------
    cfg.spacing = 0.1f;
    at = splineplace::spots(sp, bare, cfg, 300);
    check(at.size() == 300, "a spacing typo stops at the budget", std::to_string(at.size()));
    cfg.spacing = 10.0f;

    // --- Copying a subtree out of the scene ---------------------------------
    // A lamp: a post (the root, turned 30 degrees, parented to something else),
    // an arm on the post, a bulb on the arm -- plus an unrelated object.
    std::vector<Entity> scene;
    scene.push_back(mk(1, -1, "Street", {0.0f, 0.0f, 0.0f}));
    Entity post = mk(10, 1, "Lamp", {5.0f, 1.0f, 5.0f});
    post.rotation = glm::vec3(0.0f, 30.0f, 0.0f);
    post.localCenter = {99.0f, 99.0f, 99.0f};   // local to "Street": must not be used
    scene.push_back(std::move(post));
    scene.push_back(mk(11, 10, "Arm",  {1.0f, 2.0f, 0.0f}));
    Entity bulb = mk(12, 11, "Bulb", {0.5f, 0.0f, 0.0f});
    auto link = std::make_unique<PrefabComponent>();
    link->source  = fitzel::AssetId::generate();   // the bulb is itself a prefab part
    link->localId = 3;
    const fitzel::AssetId bulbSource = link->source;
    bulb.components.items.push_back(std::move(link));
    scene.push_back(std::move(bulb));
    scene.push_back(mk(13, -1, "Bench", {20.0f, 0.0f, 0.0f}));

    const prefab::Prefab tmpl = splineplace::fromScene(scene, 10);
    check(tmpl.entities.size() == 3, "the template is the lamp and its two parts, not the bench",
          std::to_string(tmpl.entities.size()));
    check(!tmpl.guid.valid(), "a template from the scene is not a prefab file");
    if (tmpl.entities.size() == 3) {
        const Entity& r = tmpl.entities[0];
        check(r.id == 0 && r.parent == -1, "its root is local id 0 with no parent");
        check(near(r.localCenter.x, 5.0f) && near(r.localRotation.y, 30.0f),
              "the root takes its WORLD transform, not the one local to its old parent",
              v3(r.localCenter));
        check(tmpl.entities[1].parent == 0 && tmpl.entities[2].parent == 1,
              "the parts hang off each other by local id");
    }

    std::vector<splineplace::Spot> three = {
        {{0.0f, 0.0f, 0.0f}, 0.0f}, {{0.0f, 0.0f, 10.0f}, 90.0f}, {{0.0f, 0.0f, 20.0f}, 180.0f}};
    int counter = 100;
    std::vector<Entity> copies = splineplace::stamp(tmpl, three, 1.0f, counter);
    check(copies.size() == 9, "three spots, three lamps of three parts",
          std::to_string(copies.size()));
    std::unordered_set<int> ids;
    for (const Entity& e : copies) ids.insert(e.id);
    check(ids.size() == copies.size() && counter == 109, "every part gets a fresh, unique id");
    int roots = 0;
    bool ownParents = true, noNewLinks = true, keptLink = true;
    for (const Entity& e : copies) {
        if (e.parent < 0) { ++roots; continue; }
        ownParents &= byId(copies, e.parent) != nullptr;
        const auto* pc = e.components.get<PrefabComponent>();
        if (e.name == "Bulb") keptLink &= pc && pc->source == bulbSource;
        else                  noNewLinks &= pc == nullptr;
    }
    check(roots == 3, "each copy has one root, left unparented for the caller's group");
    check(ownParents, "every part hangs off its OWN copy, not the original or the first copy");
    check(keptLink, "a part that was a prefab instance is still one, of the same prefab");
    check(noNewLinks, "a plain object does not become a prefab instance by being copied");
    if (copies.size() == 9) {
        check(near(copies[3].localCenter.z, 10.0f) && near(copies[3].localRotation.y, 120.0f),
              "a copy stands on its spot, turned by the path on top of its own turn",
              v3(copies[3].localCenter) + " yaw " + std::to_string(copies[3].localRotation.y));
        check(near(copies[4].localCenter.x, 1.0f) && near(copies[4].localCenter.y, 2.0f),
              "its parts keep their offsets", v3(copies[4].localCenter));
    }

    counter = 100;
    copies = splineplace::stamp(tmpl, three, 2.0f, counter);
    check(copies.size() == 9 && near(copies[1].localCenter.y, 4.0f),
          "Scale 2 doubles the parts' offsets",
          copies.size() == 9 ? v3(copies[1].localCenter) : "");

    // A prefab file (valid GUID) is instantiated as instances of that prefab.
    prefab::Prefab file = tmpl;
    file.guid = fitzel::AssetId::generate();
    counter = 100;
    copies = splineplace::stamp(file, three, 1.0f, counter);
    bool allLinked = !copies.empty();
    for (const Entity& e : copies) {
        const auto* pc = e.components.get<PrefabComponent>();
        allLinked &= pc && pc->source == file.guid;
    }
    check(allLinked, "a prefab's copies are all instances of that prefab");

    // --- Persistence --------------------------------------------------------
    nlohmann::json j;
    sp.save(j);
    SplineSystem back;
    back.groundAt = ground;
    back.load(j);
    check(back.paths.size() == sp.paths.size() &&
          back.paths[bare].kind == splinegen::Kind::Path &&
          back.paths[bare].preset == splinegen::Preset::Bare,
          "a saved bare path loads back as a bare path, not a fence");
    back.update(materials);
    check(materials.empty(), "...and loading it creates no materials either");

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
