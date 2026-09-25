// The town check: does a generated town hold together?
//
// Every preset is laid out and built on synthetic ground, and the things that
// cannot be seen from inside the editor are measured instead:
//   1. The grid: one street per line, every street running on past the edge,
//      every block a proper quad.
//   2. Nothing SOLID stands in a carriageway -- a house collider or a tower
//      podium in the street is a wall you drive into on a road that looked clear.
//   3. No two buildings overlap, and every building faces a street (its front is
//      nearer a carriageway than its back).
//   4. Water: a river through the town takes the lots it runs through, and the
//      streets that cross it get a bridge (or are broken when bridges are off).
//   5. Slope: a cliff through the town empties the lots on it.
//   6. Determinism and save/load: the same rule builds the same town, and a
//      rule survives the scene file.
//   7. Cost: buildings, vertices, draws and milliseconds per preset.
//
// Console program like citycheck:
//   build/release/bin/towncheck.exe
// Exits non-zero on any failure.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include "../src/CityPlan.hpp"
#include "../src/Component.hpp"
#include "../src/EditMesh.hpp"
#include "../src/SceneTypes.hpp"

namespace {

int g_fail = 0;
void check(bool ok, const std::string& what, const std::string& detail = "") {
    if (!ok) ++g_fail;
    std::printf("  [%s] %s%s%s\n", ok ? " ok " : "FAIL", what.c_str(),
                detail.empty() ? "" : "  -- ", detail.c_str());
}

float segDist(glm::vec2 p, glm::vec2 a, glm::vec2 b) {
    const glm::vec2 ab = b - a;
    const float l2 = glm::dot(ab, ab);
    const float t = l2 > 1e-8f ? glm::clamp(glm::dot(p - a, ab) / l2, 0.0f, 1.0f) : 0.0f;
    return glm::length(p - (a + ab * t));
}

// Metres from `p` to the nearest carriageway edge (negative = on it).
float streetClear(const cityplan::Layout& L, glm::vec2 p) {
    float best = 1e9f;
    for (const cityplan::Street& s : L.streets)
        for (std::size_t i = 0; i + 1 < s.pts.size(); ++i)
            best = std::min(best, segDist(p, s.pts[i], s.pts[i + 1]) - s.width * 0.5f);
    return best;
}

std::vector<cityplan::RoadLine> roadsOf(const cityplan::Layout& L) {
    std::vector<cityplan::RoadLine> out;
    for (const cityplan::Street& s : L.streets) out.push_back({s.pts, s.width * 0.5f});
    return out;
}

void corners(const city::Piece& pc, glm::vec2 out[4]) {
    const float r = glm::radians(pc.yaw), c = std::cos(r), s = std::sin(r);
    auto rot = [&](glm::vec2 v) { return glm::vec2(v.x * c + v.y * s, -v.x * s + v.y * c); };
    const glm::vec2 cen(pc.center.x, pc.center.z);
    out[0] = cen + rot({-pc.half.x, -pc.half.z});
    out[1] = cen + rot({ pc.half.x, -pc.half.z});
    out[2] = cen + rot({ pc.half.x,  pc.half.z});
    out[3] = cen + rot({-pc.half.x,  pc.half.z});
}

void checkPreset(cityplan::Preset preset) {
    using namespace cityplan;
    std::printf("\n== %s ==\n", presetName(preset));
    Rule r;
    applyPreset(r, preset);
    r.grid.center = {200.0f, -150.0f};
    r.grid.rotation = 17.0f;

    const Layout L = layout(r);
    check(static_cast<int>(L.streets.size()) == (L.nx + 1) + (L.nz + 1),
          "one street per grid line",
          std::to_string(L.nx) + " x " + std::to_string(L.nz) + " blocks");
    bool stubs = true;
    for (const Street& s : L.streets)
        stubs &= s.pts.size() >= 4 &&
                 glm::length(s.pts.front() - s.pts[1]) > 5.0f &&
                 glm::length(s.pts.back() - s.pts[s.pts.size() - 2]) > 5.0f;
    check(stubs, "every street runs on past the town edge");
    check(static_cast<int>(L.blocks.size()) == L.nx * L.nz, "one block per cell");

    std::vector<MaterialDef> mats;
    const Palettes pal = ensurePalettes(mats, r);
    Context ctx;
    ctx.groundAt = [](float x, float z) { return 0.02f * x + 0.5f * std::sin(z * 0.01f); };
    ctx.roads = roadsOf(L);

    const auto t0 = std::chrono::steady_clock::now();
    const Town T = derive(r, pal, ctx);
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();
    const Stats& st = T.stats;
    std::printf("  %d lots -> %d built (%d towers, %d blocks, %d rows, %d houses, %d parks);"
                " skipped road %d, water %d, slope %d, empty %d\n",
                st.lots, st.built, st.towers, st.blocks, st.rows, st.houses, st.parks,
                st.skippedRoad, st.skippedWater, st.skippedSlope, st.skippedEmpty);
    std::size_t tris = 0;
    for (const city::Batch& b : T.district.batches) tris += b.data.indices.size() / 3;
    std::printf("  %zu draws, %d verts (%.1f MB), %zu tris, %zu colliders, %.0f ms\n",
                T.district.batches.size(), T.district.verts,
                T.district.verts * sizeof(fitzel::Vertex) / 1048576.0, tris,
                T.district.colliders.size(), ms);
    check(st.built > 0, "the town builds something");

    // 2. Solid pieces keep out of every carriageway.
    float worst = 1e9f;
    for (const city::Piece& pc : T.district.colliders) {
        glm::vec2 c[4];
        corners(pc, c);
        for (const glm::vec2& p : c) worst = std::min(worst, streetClear(L, p));
        worst = std::min(worst, streetClear(L, {pc.center.x, pc.center.z}));
    }
    char buf[96];
    std::snprintf(buf, sizeof buf, "worst solid piece %.2f m from a kerb", worst);
    check(worst > 0.0f, "nothing solid in a street", buf);

    // 3. Buildings do not overlap and face a street.
    const std::vector<Lot> plan = lots(r, L);
    int overlapping = 0, backwards = 0;
    for (std::size_t i = 0; i < plan.size(); ++i) {
        const Lot& a = plan[i];
        const glm::vec2 fm = a.pos + a.front * (a.depth * 0.5f);
        const glm::vec2 bm = a.pos - a.front * (a.depth * 0.5f);
        if (streetClear(L, fm) > streetClear(L, bm) + 0.01f) ++backwards;
        for (std::size_t j = i + 1; j < plan.size(); ++j) {
            const Lot& b = plan[j];
            const float reach = 0.5f * (glm::length(glm::vec2(a.width, a.depth)) +
                                        glm::length(glm::vec2(b.width, b.depth)));
            if (glm::length(a.pos - b.pos) > reach) continue;
            // Separating axes of the two footprints.
            const glm::vec2 au(-a.front.y, a.front.x), av = a.front;
            const glm::vec2 bu(-b.front.y, b.front.x), bv = b.front;
            const glm::vec2 axes[4] = {au, av, bu, bv};
            bool sep = false;
            for (const glm::vec2& ax : axes) {
                const float ra = 0.5f * a.width * std::abs(glm::dot(au, ax)) +
                                 0.5f * a.depth * std::abs(glm::dot(av, ax));
                const float rb = 0.5f * b.width * std::abs(glm::dot(bu, ax)) +
                                 0.5f * b.depth * std::abs(glm::dot(bv, ax));
                if (std::abs(glm::dot(b.pos - a.pos, ax)) >= ra + rb - 0.1f) { sep = true; break; }
            }
            if (!sep) ++overlapping;
        }
    }
    check(overlapping == 0, "no two buildings overlap", std::to_string(overlapping) + " pairs");
    check(backwards == 0, "every building faces a street", std::to_string(backwards) + " turned away");

    // 6. Determinism and persistence.
    const Town T2 = derive(r, pal, ctx);
    check(T2.district.verts == T.district.verts &&
          T2.district.batches.size() == T.district.batches.size() &&
          T2.stats.built == T.stats.built, "same rule, same town");
    nlohmann::json j;
    Rule laidToo = r;
    laidToo.laid = r.grid;
    laidToo.laid.center += glm::vec2(5.0f);
    laidToo.hasLaid = true;
    nlohmann::json jl;
    save(jl, laidToo);
    Rule backL;
    load(nlohmann::json::parse(jl.dump()), backL);
    check(backL == laidToo && backL.hasLaid && backL.streetsStale(),
          "a laid grid survives save/load apart from the draft");
    save(j, r);
    Rule back;
    load(nlohmann::json::parse(j.dump()), back);
    check(back == r, "the rule survives save/load");
}

void checkWaterAndSlope() {
    using namespace cityplan;
    std::printf("\n== River and cliff ==\n");
    Rule r;
    applyPreset(r, Preset::SmallTown);
    r.grid.organic = 0.0f;
    r.grid.center  = {0.0f, 0.0f};
    const Layout L = layout(r);
    std::vector<MaterialDef> mats;
    const Palettes pal = ensurePalettes(mats, r);

    // A 24 m river running along Z at x = 40, clear of every grid line.
    auto river = [](float x, float) { return std::abs(x - 40.0f) < 12.0f; };
    Context ctx;
    ctx.groundAt = [](float, float) { return 0.0f; };
    ctx.isWater  = river;
    ctx.roads    = roadsOf(L);
    const Town T = derive(r, pal, ctx);
    bool dry = true;
    for (const Placed& p : T.placed) dry &= std::abs(p.pos.x - 40.0f) > 12.0f;
    check(T.stats.skippedWater > 0 && dry, "the river takes the lots it runs through",
          std::to_string(T.stats.skippedWater) + " lots");

    // The streets along X cross it: each should be one run with one bridge.
    int bridged = 0, crossing = 0;
    for (const Street& s : L.streets) {
        if (s.alongZ) continue;
        ++crossing;
        const std::vector<StreetRun> runs = streetRuns(r.grid, s, river);
        if (runs.size() == 1 && runs[0].bridges.size() == 1) {
            const StreetRun& run = runs[0];
            const glm::vec2 a = run.pts[static_cast<std::size_t>(run.bridges[0].first)];
            const glm::vec2 b = run.pts[static_cast<std::size_t>(run.bridges[0].second)];
            if (!river(a.x, a.y) && !river(b.x, b.y) &&
                std::min(a.x, b.x) < 28.0f && std::max(a.x, b.x) > 52.0f) ++bridged;
        }
    }
    check(crossing > 0 && bridged == crossing, "every street over the river gets a bridge",
          std::to_string(bridged) + "/" + std::to_string(crossing));

    Rule nb = r;
    nb.grid.bridges = false;
    int broken = 0;
    for (const Street& s : L.streets)
        if (!s.alongZ && streetRuns(nb.grid, s, river).size() == 2) ++broken;
    check(broken == crossing, "without bridges the streets stop at the banks");

    // A lake over the town's first street: cut back to the shore, not bridged.
    auto lake = [](float x, float) { return x < -250.0f; };
    int trimmed = 0;
    for (const Street& s : L.streets) {
        if (s.alongZ) continue;
        const std::vector<StreetRun> runs = streetRuns(r.grid, s, lake);
        if (runs.size() == 1 && runs[0].bridges.empty() && runs[0].pts.front().x > -250.0f &&
            runs[0].pts.back().x > -250.0f) ++trimmed;
    }
    check(trimmed == crossing, "a street running into a lake ends at its shore");

    // A cliff: 30 m drop across x = 100.
    Context cliff;
    cliff.groundAt = [](float x, float) { return 30.0f / (1.0f + std::exp(-(x - 100.0f) * 0.6f)); };
    cliff.roads = roadsOf(L);
    const Town C = derive(r, pal, cliff);
    bool offCliff = true;
    for (const Placed& p : C.placed) offCliff &= std::abs(p.pos.x - 100.0f) > 3.0f;
    check(C.stats.skippedSlope > 0 && offCliff, "a cliff empties the lots on it",
          std::to_string(C.stats.skippedSlope) + " lots");

    // A foreign road through the middle of town: nothing may stand on it.
    Context foreign = ctx;
    foreign.isWater = nullptr;
    foreign.roads.push_back({{{-400.0f, -400.0f}, {400.0f, 400.0f}}, 5.0f});
    const Town F = derive(r, pal, foreign);
    float worst = 1e9f;
    for (const city::Piece& pc : F.district.colliders) {
        glm::vec2 c[4];
        corners(pc, c);
        for (const glm::vec2& p : c)
            worst = std::min(worst, segDist(p, {-400.0f, -400.0f}, {400.0f, 400.0f}) - 5.0f);
    }
    char buf[96];
    std::snprintf(buf, sizeof buf, "%d lots dropped, worst %.2f m", F.stats.skippedRoad, worst);
    check(F.stats.skippedRoad > 0 && worst > 0.0f, "a road drawn through the town keeps its lots off", buf);
}

} // namespace

// --parts: what each part of a house costs, to decide what a town can afford.
void partCosts() {
    std::vector<MaterialDef> mats;
    cityplan::Rule r;
    const cityplan::Palettes pal = cityplan::ensurePalettes(mats, r);
    for (int k = 0; k < 2; ++k) {
        const housegen::Params p = cityplan::housePrototype(r, k ? cityplan::Zone::Rows
                                                                 : cityplan::Zone::Houses, 0);
        int counter = 1;
        const std::vector<Entity> es = housegen::generate(p, pal.houses, counter, glm::vec3(0.0f));
        std::printf("%s:\n", k ? "Row house" : "Family house");
        for (const Entity& e : es) {
            const auto* mc = e.components.get<MeshComponent>();
            if (!mc) continue;
            std::size_t v = 0, idx = 0;
            for (const auto& g : editmesh::buildGroups(mc->mesh)) {
                v += g.data.vertices.size();
                idx += g.data.indices.size();
            }
            std::printf("  %-14s %5zu faces %6zu verts %6zu idx\n", e.name.c_str(),
                        mc->mesh.faces.size(), v, idx);
        }
    }
}

// --scene <out.json> <preset> <x> <z>: a town and its streets as the scene
// file's "settings" carries them ("towns" + "roads"), for looking at a town in
// the real renderer without driving the editor by hand. Streets are written as
// laid (no water here), not graded -- the load re-lofts them on the ground.
int sceneFragment(const char* out, int preset, float x, float z) {
    using namespace cityplan;
    Rule r;
    applyPreset(r, static_cast<Preset>(preset));
    r.id = 1;
    r.name = presetName(static_cast<Preset>(preset));
    r.grid.center = {x, z};
    r.laid = r.grid;
    r.hasLaid = true;
    nlohmann::json j, one;
    save(one, r);
    j["towns"] = {{"towns", nlohmann::json::array({one})}, {"nextId", 2}};
    nlohmann::json roads = nlohmann::json::array();
    int n = 0;
    for (const Street& st : layout(r).streets)
        for (const StreetRun& run : streetRuns(r.laid, st, nullptr)) {
            std::string pts, zeros;
            for (const glm::vec2& p : run.pts) {
                pts += std::to_string(p.x) + " " + std::to_string(p.y) + " ";
                zeros += "0 ";
            }
            nlohmann::json br = nlohmann::json::array();
            for (const auto& [a, b] : run.bridges) br.push_back({a, b});
            roads.push_back({{"name", "Street " + std::to_string(++n)}, {"points", pts},
                             {"lifts", zeros}, {"banks", zeros}, {"width", run.width},
                             {"shoulder", 2.5f}, {"bridges", br}, {"town", 1}});
        }
    j["roads"] = roads;
    std::FILE* f = std::fopen(out, "wb");
    if (!f) return 1;
    const std::string txt = j.dump();
    std::fwrite(txt.data(), 1, txt.size(), f);
    std::fclose(f);
    std::printf("%d roads\n", n);
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--parts") { partCosts(); return 0; }
    if (argc > 5 && std::string(argv[1]) == "--scene")
        return sceneFragment(argv[2], std::atoi(argv[3]), static_cast<float>(std::atof(argv[4])),
                             static_cast<float>(std::atof(argv[5])));
    for (int p = 0; p < static_cast<int>(cityplan::Preset::Count); ++p)
        checkPreset(static_cast<cityplan::Preset>(p));
    checkWaterAndSlope();
    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "all good", g_fail,
                g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
