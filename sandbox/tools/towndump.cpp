// The town dump: a generated town (CityPlan, the town generator) written out as
// plain data, for a pipeline outside the editor to build on -- a level's own
// Blender build, say, that wants the town's houses as models of its own and its
// streets and lamps for a light bake, and cannot run the generator itself.
//
// One JSON file and one binary file beside it:
//   <out>.json  the materials (the MaterialDef fields that matter for drawing,
//               the procedural window grid included), the merged batches (which
//               material, where in the binary), the streets (centre lines,
//               width, name), the lamps, the built lots and the paved plots;
//   <out>.bin   per batch its vertices -- position, normal, uv, float32 each,
//               world space -- then its triangle indices, uint32, counted from
//               the batch's first vertex.
// Engine axes throughout (x, y up, z).
//
// The town is described by a small JSON of its own (any field may be left out):
//   { "preset": "City", "seed": 11, "ground": 0.0,
//     "grid": { "center": [x, z], "sizeX": 520, "sizeZ": 440, "rotation": 0,
//               "blockX": 95, "blockZ": 75, "organic": 0.15, "streetWidth": 7,
//               "avenueWidth": 12, "avenueEvery": 3 },
//     "rule": { "towerRing": 0.2, "blockRing": 0.5, "rowRing": 0.7, "windowLit": 0.45,
//               "towerFloorsMin": 12, "towerFloorsMax": 45, "blockFloorsMin": 3,
//               "blockFloorsMax": 6, "fill": 0.94, "budget": 1500, "lamps": true },
//     "keepClear": [ { "pts": [[x, z], ...], "half": 12 } ] }
// "keepClear" are roads the town must leave free (a railway through it): lots
// that reach into one are not built, as with any road in a scene.
//
// Console program, like towncheck:
//   build/linux/bin/towndump town.json out
// Exits non-zero if the description cannot be read or the files written.

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include "../src/CityPlan.hpp"
#include "../src/SceneTypes.hpp"

using namespace cityplan;

namespace {

nlohmann::json v3(const glm::vec3& v) { return {v.x, v.y, v.z}; }
nlohmann::json v2(const glm::vec2& v) { return {v.x, v.y}; }

Preset presetNamed(const std::string& s) {
    for (int i = 0; i < static_cast<int>(Preset::Count); ++i)
        if (s == presetName(static_cast<Preset>(i))) return static_cast<Preset>(i);
    return Preset::City;
}

template <class T>
void take(const nlohmann::json& j, const char* key, T& field) {
    if (j.contains(key)) field = j[key].get<T>();
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: towndump town.json out   (writes out.json and out.bin)\n");
        return 2;
    }
    nlohmann::json cfg;
    {
        std::ifstream f(argv[1]);
        if (!f) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
        f >> cfg;
    }
    const std::string out = argv[2];

    Rule r;
    applyPreset(r, presetNamed(cfg.value("preset", std::string("City"))));
    take(cfg, "seed", r.seed);
    if (cfg.contains("grid")) {
        const auto& g = cfg["grid"];
        if (g.contains("center")) r.grid.center = glm::vec2(g["center"][0].get<float>(), g["center"][1].get<float>());
        take(g, "sizeX", r.grid.sizeX);
        take(g, "sizeZ", r.grid.sizeZ);
        take(g, "rotation", r.grid.rotation);
        take(g, "blockX", r.grid.blockX);
        take(g, "blockZ", r.grid.blockZ);
        take(g, "organic", r.grid.organic);
        take(g, "streetWidth", r.grid.streetWidth);
        take(g, "avenueWidth", r.grid.avenueWidth);
        take(g, "avenueEvery", r.grid.avenueEvery);
        take(g, "seed", r.grid.seed);
    }
    bool lamps = true;
    if (cfg.contains("rule")) {
        const auto& j = cfg["rule"];
        take(j, "towerRing", r.towerRing);
        take(j, "blockRing", r.blockRing);
        take(j, "rowRing", r.rowRing);
        take(j, "zoneNoise", r.zoneNoise);
        take(j, "parkChance", r.parkChance);
        take(j, "industryShare", r.industryShare);
        take(j, "industryAngle", r.industryAngle);
        take(j, "windowLit", r.windowLit);
        take(j, "towerFloorsMin", r.towerFloorsMin);
        take(j, "towerFloorsMax", r.towerFloorsMax);
        take(j, "blockFloorsMin", r.blockFloorsMin);
        take(j, "blockFloorsMax", r.blockFloorsMax);
        take(j, "fill", r.fill);
        take(j, "budget", r.budget);
        take(j, "weathering", r.weathering);
        take(j, "colourVariety", r.colourVariety);
        take(j, "powerLines", r.powerLines);
        take(j, "signs", r.signs);
        take(j, "lampEvery", r.lampEvery);
        take(j, "lampBothSides", r.lampBothSides);
        take(j, "lamps", lamps);
    }
    // No prefab, no lamps (Rule::lampPrefabs) -- and only their places are
    // wanted here: one stand-in prefab, which nothing ever loads.
    if (lamps && r.lampPrefabs.empty()) r.lampPrefabs.push_back(LampPrefab{"street_lamp", 1.0f, 0});
    r.traffic = 0.0f;
    r.people  = 0.0f;

    const float ground = cfg.value("ground", 0.0f);
    std::vector<MaterialDef> mats;
    const Palettes pal = ensurePalettes(mats, r);
    const Layout lay = layout(r);
    Context ctx;
    ctx.groundAt = [ground](float, float) { return ground; };
    ctx.isWater  = [](float, float) { return false; };
    for (const Street& s : lay.streets) ctx.roads.push_back({s.pts, s.width * 0.5f, s.name});
    if (cfg.contains("keepClear"))
        for (const auto& k : cfg["keepClear"]) {
            RoadLine rl;
            for (const auto& p : k["pts"]) rl.pts.emplace_back(p[0].get<float>(), p[1].get<float>());
            rl.half = k.value("half", 10.0f);
            ctx.roads.push_back(rl);
        }
    const Town town = derive(r, pal, ctx);

    // --- the materials the batches draw with ------------------------------------
    nlohmann::json J;
    std::vector<fitzel::AssetId> used;
    auto matIndex = [&](const fitzel::AssetId& id) {
        for (std::size_t i = 0; i < used.size(); ++i)
            if (used[i] == id) return static_cast<int>(i);
        used.push_back(id);
        return static_cast<int>(used.size()) - 1;
    };
    // --- the geometry ---------------------------------------------------------
    std::ofstream bin(out + ".bin", std::ios::binary);
    if (!bin) { std::fprintf(stderr, "cannot write %s.bin\n", out.c_str()); return 1; }
    std::size_t offset = 0, verts = 0, tris = 0;
    J["batches"] = nlohmann::json::array();
    for (const city::Batch& b : town.district.batches) {
        const auto& d = b.data;
        if (d.indices.empty()) continue;
        std::vector<float> vb;
        vb.reserve(d.vertices.size() * 8);
        for (const fitzel::Vertex& v : d.vertices)
            vb.insert(vb.end(), {v.position.x, v.position.y, v.position.z, v.normal.x, v.normal.y, v.normal.z,
                                 v.uv.x, v.uv.y});
        bin.write(reinterpret_cast<const char*>(vb.data()), static_cast<std::streamsize>(vb.size() * 4));
        bin.write(reinterpret_cast<const char*>(d.indices.data()),
                  static_cast<std::streamsize>(d.indices.size() * 4));
        J["batches"].push_back({{"material", matIndex(b.material)}, {"offset", offset},
                                {"vertices", d.vertices.size()}, {"indices", d.indices.size()},
                                {"castsShadow", b.castsShadow}, {"lo", v3(b.lo)}, {"hi", v3(b.hi)}});
        offset += vb.size() * 4 + d.indices.size() * 4;
        verts += d.vertices.size();
        tris += d.indices.size() / 3;
    }
    J["materials"] = nlohmann::json::array();
    for (const fitzel::AssetId& id : used) {
        const MaterialDef* md = nullptr;
        for (const MaterialDef& m : mats)
            if (m.assetId == id) md = &m;
        nlohmann::json m;
        if (md) {
            m = {{"name", md->name}, {"albedo", v3(md->albedo)}, {"reflectivity", md->reflectivity},
                 {"roughness", md->roughness}, {"opacity", md->opacity},
                 {"alphaMode", static_cast<int>(md->alphaMode)}, {"glass", md->glass},
                 {"emission", v3(md->emission)}, {"emissionStrength", md->emissionStrength},
                 {"emissionLight", md->emissionLight}, {"doubleSided", md->doubleSided}};
            if (md->windowGrid)
                m.update({{"windowGrid", true}, {"windowCell", v2(md->windowCell)}, {"windowLit", md->windowLit},
                          {"windowSeed", md->windowSeed}, {"windowColor", v3(md->windowColor)},
                          {"windowGlow", md->windowGlow}});
        } else {
            m = {{"name", "unknown"}, {"albedo", {0.5, 0.5, 0.5}}};
        }
        J["materials"].push_back(m);
    }
    // --- the plan: streets, lamps, lots, paved plots --------------------------
    J["streets"] = nlohmann::json::array();
    for (const Street& s : lay.streets) {
        nlohmann::json pts = nlohmann::json::array();
        for (const glm::vec2& p : s.pts) pts.push_back(v2(p));
        J["streets"].push_back({{"pts", pts}, {"width", s.width}, {"name", s.name}, {"alongZ", s.alongZ}});
    }
    J["lamps"] = nlohmann::json::array();
    for (const Lamp& l : town.lamps) J["lamps"].push_back({{"pos", v3(l.pos)}, {"facing", v2(l.facing)}});
    J["placed"] = nlohmann::json::array();
    for (const Placed& p : town.placed)
        J["placed"].push_back({{"pos", v2(p.pos)}, {"radius", p.radius}, {"zone", zoneName(p.zone)}});
    J["bare"] = nlohmann::json::array();
    for (const Bare& b : town.bare)
        J["bare"].push_back({{"c", v2(b.c)}, {"u", v2(b.u)}, {"hu", b.hu}, {"hv", b.hv}});
    J["sidewalk"] = r.sidewalk;
    J["ground"]   = ground;
    const Stats& st = town.stats;
    J["stats"] = {{"built", st.built}, {"towers", st.towers}, {"blocks", st.blocks}, {"rows", st.rows},
                  {"houses", st.houses}, {"parks", st.parks}, {"lamps", st.lamps}, {"skippedRoad", st.skippedRoad},
                  {"vertices", verts}, {"triangles", tris}};
    std::ofstream js(out + ".json");
    if (!js) { std::fprintf(stderr, "cannot write %s.json\n", out.c_str()); return 1; }
    js << J.dump(1);
    std::printf("%d built (%d towers, %d blocks, %d rows, %d houses, %d parks), %zu lamps, %zu streets; "
                "%zu batches, %zu materials, %zu vertices, %zu triangles\n",
                st.built, st.towers, st.blocks, st.rows, st.houses, st.parks, town.lamps.size(),
                lay.streets.size(), J["batches"].size(), used.size(), verts, tris);
    return 0;
}
