// housecheck -- does every house preset come out as a house you can live in?
//
// A generated house that is wrong still looks like a house from the road. What
// this catches is the kind of wrong you only find by walking through it:
//
//   1. Rooms that overlap, leave the footprint, or that no door leads into.
//   2. A stair that does not stand in the hall, or a hall too narrow for it.
//   3. Openings that are not in a wall (a window floating in a room).
//   4. Living areas off the rules: stairs not deducted, the attic slope not
//      counted at half, a total that is nowhere near what the rooms add up to.
//   5. Degenerate geometry: faces with fewer than three corners, NaNs,
//      out-of-range indices, a part that builds nothing, a mesh whose entity
//      does not sit where its bounds say.
//   6. The parameters not surviving a save/load round trip on the house's root.
//
// Pure geometry: no GL, no window.
//
//   build/release/bin/housecheck.exe [--obj <dir>] [--table]
//
// --obj writes each preset as <dir>/house_<n>.obj (one object per part), for
// looking at them outside the engine; --table lists every room of every
// preset with the area it came out at next to the one it asked for.

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../src/Component.hpp"
#include "../src/EditMesh.hpp"
#include "../src/HouseGen.hpp"

namespace {

int failures = 0;

void check(bool ok, const std::string& what, const std::string& detail = {}) {
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what.c_str(),
                detail.empty() ? "" : "  -- ", detail.c_str());
    if (!ok) ++failures;
}

std::string f2(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", v);
    return b;
}

float overlap(const housegen::Rect& a, const housegen::Rect& b) {
    const float w = std::min(a.x1, b.x1) - std::max(a.x0, b.x0);
    const float h = std::min(a.y1, b.y1) - std::max(a.y0, b.y0);
    return (w > 0.0f && h > 0.0f) ? w * h : 0.0f;
}

bool inside(const housegen::Rect& in, const housegen::Rect& out, float eps = 1e-3f) {
    return in.x0 >= out.x0 - eps && in.y0 >= out.y0 - eps && in.x1 <= out.x1 + eps &&
           in.y1 <= out.y1 + eps;
}

void checkPlan(const std::string& name, const housegen::Plan& plan) {
    const housegen::Params& p = plan.params;
    const housegen::Rect inner{p.outerWall, p.outerWall, p.width - p.outerWall,
                               p.depth - p.outerWall};
    float sumLiving = 0.0f;
    for (const housegen::LevelPlan& lv : plan.levels) {
        const std::string at = name + " " + lv.label;
        bool roomsInside = true, noOverlap = true, allReachable = true;
        for (std::size_t i = 0; i < lv.rooms.size(); ++i) {
            const auto& r = lv.rooms[i];
            roomsInside &= inside(r.r, inner) && r.r.w() > 0.5f && r.r.h() > 0.5f;
            allReachable &= r.reachable;
            for (std::size_t j = i + 1; j < lv.rooms.size(); ++j)
                noOverlap &= overlap(r.r, lv.rooms[j].r) < 1e-3f;
            check(r.livingArea <= r.floorArea + 1e-3f && r.livingArea >= 0.0f,
                  at + " " + r.name + ": living area within the floor area",
                  f2(r.livingArea) + " of " + f2(r.floorArea));
            sumLiving += r.livingArea;
        }
        check(roomsInside, at + ": rooms inside the footprint");
        check(noOverlap, at + ": rooms do not overlap");
        check(allReachable, at + ": every room has a way in");

        // Every opening sits in one of the walls.
        int floating = 0;
        for (const auto& o : lv.openings) {
            bool found = false;
            for (const auto& w : lv.walls)
                if (o.alongX == w.alongX && inside(o.r, w.r)) { found = true; break; }
            // A pass between two main rooms cuts a partition the level lists too.
            if (!found) ++floating;
        }
        check(floating == 0, at + ": every opening is in a wall",
              std::to_string(floating) + " outside");

        if (plan.hasStair && (lv.stairUp || lv.stairDown)) {
            const housegen::PlanRoom* hall = nullptr;
            for (const auto& r : lv.rooms) if (r.hall) hall = &r;
            check(hall && inside(plan.stair.r, hall->r), at + ": stair stands in the hall");
            if (hall && !lv.attic)
                check(std::abs(hall->livingArea - (hall->floorArea - plan.stair.r.area())) < 1e-2f,
                      at + ": stair deducted from the hall");
        }
        if (lv.label == "EG") {
            int entries = 0;
            for (const auto& o : lv.openings)
                entries += o.kind == housegen::Opening::Kind::EntryDoor ? 1 : 0;
            check(entries == 1, at + ": one front door", std::to_string(entries));
        }
        if (lv.attic && !lv.loft) {
            // Somewhere in every attic room the slope has to cost area.
            float full = 0.0f, living = 0.0f;
            for (const auto& r : lv.rooms) { full += r.floorArea; living += r.livingArea; }
            check(living < full - 1.0f, at + ": sloped floor counted at less than full",
                  f2(living) + " of " + f2(full));
        }
    }
    check(std::abs(sumLiving - plan.living) < 1e-2f, name + ": total = sum of the rooms",
          f2(plan.living));
    check(plan.ridgeZ > plan.eaveZ + 1.0f, name + ": ridge above the eaves",
          f2(plan.eaveZ) + " / " + f2(plan.ridgeZ));
}

void checkModel(const std::string& name, const housegen::Params& params,
                const char* objDir, int index) {
    int counter = 1;
    const std::vector<Entity> es = housegen::generate(params, housegen::Palette{}, counter,
                                                      glm::vec3(0.0f));
    check(!es.empty() && es.front().type == EntityType::Empty, name + ": root is an Empty");
    if (es.empty()) return;
    const HouseComponent* hc = es.front().components.get<HouseComponent>();
    check(hc != nullptr, name + ": root carries its parameters");
    check(es.size() >= 6, name + ": parts built", std::to_string(es.size() - 1) + " parts");

    std::ofstream obj;
    if (objDir) obj.open(std::string(objDir) + "/house_" + std::to_string(index) + ".obj");
    int base = 1;
    std::size_t faces = 0;
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
        for (const glm::vec3& v : m.verts) ok &= std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        ok &= m.faceMat.empty() || m.faceMat.size() == m.faces.size();
        check(ok, name + " " + e.name + ": faces and corners valid",
              std::to_string(m.faces.size()) + " faces");
        glm::vec3 mn, mx;
        m.bounds(mn, mx);
        const glm::vec3 c = 0.5f * (mn + mx), s = editmesh::fitScale(m, e.half);
        check(glm::length(c) < 1e-3f && glm::length(s - glm::vec3(1.0f)) < 1e-3f,
              name + " " + e.name + ": mesh centred and unscaled");
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
    std::printf("      %s: %zu faces in %zu parts\n", name.c_str(), faces, es.size() - 1);

    // Round trip through the component's own save/load.
    nlohmann::json j;
    hc->save(j);
    HouseComponent back;
    back.load(j);
    check(back.params == hc->params, name + ": parameters survive save/load");
}

} // namespace

int main(int argc, char** argv) {
    const char* objDir = nullptr;
    bool table = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--obj" && i + 1 < argc) objDir = argv[i + 1];
        if (std::string(argv[i]) == "--table") table = true;
    }

    const int n = static_cast<int>(housegen::Preset::Count);
    for (int i = 0; i < n; ++i) {
        housegen::Params p;
        housegen::applyPreset(p, static_cast<housegen::Preset>(i));
        const std::string name = housegen::presetName(static_cast<housegen::Preset>(i));
        const housegen::Plan plan = housegen::layout(p);
        checkPlan(name, plan);
        checkModel(name, p, objDir, i);
        // A preset is a promise: it must lay out without a single problem.
        check(plan.warnings == 0, name + ": no warnings",
              plan.warnings ? plan.notes.front() : "");
        std::printf("      %s: %.1f m2 living area\n", name.c_str(), plan.living);
        if (table)   // what each room came out at, next to what was asked for
            for (const auto& lv : plan.levels)
                for (const auto& r : lv.rooms)
                    std::printf("        %-6s %-18s floor %6.2f  target %6.2f%s\n", lv.label.c_str(),
                                r.name.c_str(), r.floorArea, r.target, r.autoAdded ? "  (auto)" : "");
        for (const std::string& note : plan.notes) std::printf("      note: %s\n", note.c_str());
    }

    // The classic house is the one on the drawing board: its figures are known.
    {
        const housegen::Plan plan = housegen::layout(housegen::Params{});
        check(plan.levels.size() == 3, "Classic: EG, OG and DG");
        check(plan.living > 160.0f && plan.living < 200.0f, "Classic: living area plausible",
              f2(plan.living) + " m2");
        check(plan.hasStair, "Classic: has a stair");
        check(plan.warnings == 0, "Classic: no warnings",
              plan.notes.empty() ? "" : plan.notes.front());
    }

    // Stress: odd sizes, one storey, many rooms, no attic -- still a valid plan.
    {
        housegen::Params p;
        p.width = 8.0f; p.depth = 7.0f; p.storeys = 3;
        const housegen::Plan plan = housegen::layout(p);
        checkPlan("Narrow", plan);
        checkModel("Narrow", p, nullptr, 90);
        housegen::Params q;
        q.storeyRooms[0].push_back({housegen::RoomType::Essen, "Essen", 14.0f});
        q.storeyRooms[0].push_back({housegen::RoomType::HWR, "HWR", 6.0f});
        q.storeyRooms[0].push_back({housegen::RoomType::Garderobe, "", 3.0f});
        q.width = 12.0f;
        checkPlan("Crowded", housegen::layout(q));
        checkModel("Crowded", q, nullptr, 91);
        housegen::Params r;
        r.cutLevel = 0; r.roof = false;
        checkModel("Cut-away", r, nullptr, 92);
    }

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all good", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
