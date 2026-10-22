// staircheck -- does every stair the generator can make come out walkable?
//
// A wrong stair still looks like a stair from across the room. What this
// catches is the kind of wrong you find by walking up it:
//
//   1. Treads that do not follow on: a back edge that is not the next front
//      edge, a gap or an overlap in plan between neighbours.
//   2. Heights off: a rise that is not the same every time, a top that does not
//      arrive at the height asked for, a riser the player cannot step up.
//   3. Polygons that are not convex or not counter-clockwise (what the mesh
//      builder and the panel's hover test both assume).
//   4. Degenerate geometry: faces with fewer than three corners, NaNs,
//      out-of-range indices, meshes not centred on their entity.
//   5. The parameters not surviving a save/load round trip on the root.
//
// Every preset, and every kind x structure x railing x hand on top.
// Pure geometry: no GL, no window.
//
//   build/linux/bin/staircheck [--obj <dir>] [--quiet]
//
// --obj writes each preset as <dir>/stair_<n>.obj (one object per part).

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../src/Component.hpp"
#include "../src/EditMesh.hpp"
#include "../src/StairGen.hpp"

namespace {

int  failures = 0;
int  passes   = 0;
bool quiet    = false;

void check(bool ok, const std::string& what, const std::string& detail = {}) {
    if (ok) ++passes; else ++failures;
    if (ok && quiet) return;
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what.c_str(),
                detail.empty() ? "" : "  -- ", detail.c_str());
}

std::string f3(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.3f", v);
    return b;
}

float cross(const glm::vec2& a, const glm::vec2& b) { return a.x * b.y - a.y * b.x; }

bool convexCCW(const std::vector<glm::vec2>& poly) {
    if (poly.size() < 3) return false;
    float area = 0.0f;
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const glm::vec2& a = poly[i];
        const glm::vec2& b = poly[(i + 1) % poly.size()];
        const glm::vec2& c = poly[(i + 2) % poly.size()];
        if (cross(b - a, c - b) < -1e-4f) return false;   // a right turn
        area += cross(a, b);
    }
    return area > 1e-4f;
}

glm::vec2 centroid(const std::vector<glm::vec2>& poly) {
    glm::vec2 c(0.0f);
    for (const glm::vec2& q : poly) c += q;
    return c / static_cast<float>(std::max<std::size_t>(poly.size(), 1));
}

bool insideConvex(const std::vector<glm::vec2>& poly, const glm::vec2& p) {
    for (std::size_t i = 0; i < poly.size(); ++i)
        if (cross(poly[(i + 1) % poly.size()] - poly[i], p - poly[i]) < -1e-4f) return false;
    return true;
}

void checkPlan(const std::string& name, const stairgen::Plan& plan) {
    const stairgen::Params& p = plan.params;
    const auto& ts = plan.treads;
    const int arrivals = (!ts.empty() && ts.back().arrival) ? 1 : 0;
    check(static_cast<int>(ts.size()) == plan.risers - 1 + arrivals,
          name + ": one tread per riser but the last", std::to_string(ts.size()) + " treads, " +
          std::to_string(plan.risers) + " risers");
    check(plan.rise > 0.0f && plan.rise <= 0.30f + 1e-4f, name + ": rise walkable", f3(plan.rise));
    check(std::abs(plan.rise * plan.risers - p.height) < 1e-3f, name + ": risers add up to the height");

    bool heights = true, convex = true, follow = true, apart = true;
    std::string why;
    for (std::size_t i = 0; i < ts.size(); ++i) {
        const auto& t = ts[i];
        heights &= std::abs(t.top - plan.rise * static_cast<float>(i + 1)) < 1e-3f;
        const auto poly = t.polygon();
        if (!convexCCW(poly)) { convex = false; why = "tread " + std::to_string(i); }
        if (i + 1 < ts.size()) {
            const auto& n = ts[i + 1];
            const bool f = glm::length(t.left.back() - n.left.front()) < 1e-3f &&
                           glm::length(t.right.back() - n.right.front()) < 1e-3f;
            if (!f) { follow = false; why = "after tread " + std::to_string(i); }
            // Neighbours meet along an edge; neither's middle is in the other.
            const auto np = n.polygon();
            if (insideConvex(np, centroid(poly)) || insideConvex(poly, centroid(np))) apart = false;
        }
    }
    if (!ts.empty()) {
        const float top = ts.back().arrival ? ts.back().top : ts.back().top + plan.rise;
        heights &= std::abs(top - p.height) < 1e-3f;
    }
    check(heights, name + ": every tread one rise above the last, arriving at the top");
    check(convex, name + ": tread polygons convex and counter-clockwise", why);
    check(follow, name + ": each back edge is the next front edge", why);
    check(apart, name + ": neighbouring treads do not overlap");
    check(plan.bbMax.x > plan.bbMin.x && plan.bbMax.y > plan.bbMin.y, name + ": has a footprint");
    // The foot of the stair is the origin: the first front edge straddles it.
    if (!ts.empty()) {
        const glm::vec2 mid = 0.5f * (ts.front().left.front() + ts.front().right.front());
        check(glm::length(mid) < 1e-3f, name + ": stair starts at the root");
    }
}

void checkModel(const std::string& name, const stairgen::Params& params, const char* objDir,
                int index) {
    int counter = 1;
    const std::vector<Entity> es = stairgen::generate(params, stairgen::Palette{}, counter,
                                                      glm::vec3(0.0f));
    check(!es.empty() && es.front().type == EntityType::Empty, name + ": root is an Empty");
    if (es.empty()) return;
    const StairComponent* sc = es.front().components.get<StairComponent>();
    check(sc != nullptr, name + ": root carries its parameters");
    check(es.size() >= 2, name + ": parts built", std::to_string(es.size() - 1) + " parts");

    std::ofstream obj;
    if (objDir) obj.open(std::string(objDir) + "/stair_" + std::to_string(index) + ".obj");
    int base = 1;
    std::size_t faces = 0;
    float topY = -1e9f;
    for (std::size_t k = 1; k < es.size(); ++k) {
        const Entity& e = es[k];
        const MeshComponent* mc = e.components.get<MeshComponent>();
        check(mc && !mc->mesh.faces.empty(), name + " " + e.name + ": has geometry");
        if (!mc) continue;
        const EditMesh& m = mc->mesh;
        bool ok = true;
        for (const auto& f : m.faces) {
            ok &= f.size() >= 3;
            for (int i : f) ok &= i >= 0 && i < static_cast<int>(m.verts.size());
        }
        for (const glm::vec3& v : m.verts)
            ok &= std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        ok &= m.faceMat.empty() || m.faceMat.size() == m.faces.size();
        check(ok, name + " " + e.name + ": faces and corners valid",
              std::to_string(m.faces.size()) + " faces");
        glm::vec3 mn, mx;
        m.bounds(mn, mx);
        const glm::vec3 c = 0.5f * (mn + mx), s = editmesh::fitScale(m, e.half);
        check(glm::length(c) < 1e-3f && glm::length(s - glm::vec3(1.0f)) < 1e-3f,
              name + " " + e.name + ": mesh centred and unscaled");
        check(mn.y + e.localCenter.y > -1e-3f, name + " " + e.name + ": nothing below the ground");
        if (e.name == "Steps") topY = mx.y + e.localCenter.y;
        faces += m.faces.size();
        if (obj) {
            obj << "o " << e.name << "\n";
            for (const glm::vec3& v : m.verts) {
                const glm::vec3 w = v + e.localCenter;
                obj << "v " << w.x << ' ' << w.y << ' ' << w.z << "\n";
            }
            for (const auto& f : m.faces) {
                obj << "f";
                for (int i : f) obj << ' ' << (base + i);
                obj << "\n";
            }
            base += static_cast<int>(m.verts.size());
        }
    }
    const stairgen::Plan plan = stairgen::layout(params);
    const float wantTop = (!plan.treads.empty() && plan.treads.back().arrival)
                              ? plan.params.height : plan.params.height - plan.rise;
    check(std::abs(topY - wantTop) < 1e-3f, name + ": top tread at its height",
          f3(topY) + " vs " + f3(wantTop));
    if (!quiet) std::printf("      %s: %zu faces in %zu parts\n", name.c_str(), faces, es.size() - 1);

    nlohmann::json j;
    sc->save(j);
    StairComponent back;
    back.load(j);
    check(back.params == sc->params, name + ": parameters survive save/load");
}

} // namespace

int main(int argc, char** argv) {
    const char* objDir = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--obj" && i + 1 < argc) objDir = argv[i + 1];
        if (std::string(argv[i]) == "--quiet") quiet = true;
    }

    // Presets: each must lay out without a single problem.
    const int np = static_cast<int>(stairgen::Preset::Count);
    for (int i = 0; i < np; ++i) {
        const auto pr = static_cast<stairgen::Preset>(i);
        stairgen::Params p;
        stairgen::applyPreset(p, pr);
        const std::string name = stairgen::presetName(pr);
        const stairgen::Plan plan = stairgen::layout(p);
        checkPlan(name, plan);
        checkModel(name, p, objDir, i);
        check(plan.warnings == 0, name + ": no warnings", plan.warnings ? plan.notes.front() : "");
        std::printf("      %s: %d x %.1f cm, going %.1f cm, pitch %.0f deg, %.2f x %.2f m\n",
                    name.c_str(), plan.risers, plan.rise * 100.0f, plan.going * 100.0f, plan.pitch,
                    plan.bbMax.x - plan.bbMin.x, plan.bbMax.y - plan.bbMin.y);
        for (const std::string& note : plan.notes) std::printf("      note: %s\n", note.c_str());
    }

    // Every kind with every structure, railing style and hand: valid, even
    // where the combination is odd.
    const bool wasQuiet = quiet;
    quiet = true;
    int combos = 0;
    for (int k = 0; k < static_cast<int>(stairgen::Kind::Count); ++k)
        for (int s = 0; s < static_cast<int>(stairgen::Structure::Count); ++s)
            for (int r = 0; r < static_cast<int>(stairgen::RailStyle::Count); ++r)
                for (int hand = 0; hand < 2; ++hand) {
                    stairgen::Params p;
                    p.kind = static_cast<stairgen::Kind>(k);
                    p.structure = static_cast<stairgen::Structure>(s);
                    p.railStyle = static_cast<stairgen::RailStyle>(r);
                    p.turnRight = hand == 1;
                    p.railSides = stairgen::Sides::Both;
                    p.wallSides = (r % 2) ? stairgen::Sides::Right : stairgen::Sides::None;
                    p.closedRisers = (s + r) % 2 == 0;
                    p.midLanding = true;
                    p.topPlatform = hand ? 0.8f : 0.0f;
                    p.eye = (k + s) % 2 ? 0.0f : 0.2f;
                    const std::string name = std::string(stairgen::kindName(p.kind)) + "/" +
                                             stairgen::structureName(p.structure) + "/" +
                                             stairgen::railStyleName(p.railStyle) +
                                             (hand ? "/right" : "/left");
                    checkPlan(name, stairgen::layout(p));
                    // With --obj, one of each kind x structure (posts, no wall, left hand)
                    // as stair_<100 + 10 kind + structure>.obj.
                    checkModel(name, p, (r == 0 && hand == 0) ? objDir : nullptr, 100 + 10 * k + s);
                    ++combos;
                }
    // Edges: the lowest stair there is, a tall one, manual counts.
    for (float h : {0.30f, 0.45f, 6.0f, 12.0f})
        for (int k = 0; k < static_cast<int>(stairgen::Kind::Count); ++k) {
            stairgen::Params p;
            p.kind = static_cast<stairgen::Kind>(k);
            p.height = h;
            p.autoGoing = false;
            p.going = 0.30f;
            const std::string name = std::string("h=") + f3(h) + " " + stairgen::kindName(p.kind);
            checkPlan(name, stairgen::layout(p));
            checkModel(name, p, nullptr, 0);
            ++combos;
        }
    quiet = wasQuiet;
    std::printf("      %d combinations checked\n", combos);

    // Mirror: a right-hand stair is the left-hand one flipped -- the same
    // figures, the footprint on the other side.
    {
        stairgen::Params a;
        a.kind = stairgen::Kind::LLanding;
        stairgen::Params b = a;
        b.turnRight = true;
        const auto pa = stairgen::layout(a), pb = stairgen::layout(b);
        check(std::abs(pa.bbMin.x + pb.bbMax.x) < 1e-3f && std::abs(pa.bbMax.y - pb.bbMax.y) < 1e-3f,
              "Mirror: right-hand footprint is the left-hand one flipped");
        check(pa.railLeft == pb.railRight || a.railSides == stairgen::Sides::Both,
              "Mirror: sides follow the hand");
        a.railSides = b.railSides = stairgen::Sides::Left;   // inner
        const auto qa = stairgen::layout(a), qb = stairgen::layout(b);
        check(qa.railLeft && !qa.railRight && qb.railRight && !qb.railLeft,
              "Mirror: the inner railing stays at the inner side");
    }
    // Spiral: the default turns about once and keeps 2 m of headroom.
    {
        stairgen::Params p;
        stairgen::applyPreset(p, stairgen::Preset::Spiral);
        const auto plan = stairgen::layout(p);
        check(plan.turnDeg > 250.0f && plan.turnDeg < 450.0f, "Spiral: about one turn",
              f3(plan.turnDeg));
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
