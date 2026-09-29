// modifiercheck: the modifier stack's geometry (EditMeshModifiers.cpp) and the
// stack itself (Modifiers.cpp), measured. Every result is checked for what it
// must be whatever it looks like -- closed where it should be closed, wound
// outward, its paint and materials carried -- and against numbers worked out
// by hand where there are any (a Catmull-Clark cube corner lands on 5/18).
//   build/release/bin/modifiercheck.exe

#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include "../src/EditMesh.hpp"
#include "../src/EditMeshModifiers.hpp"
#include "../src/Modifiers.hpp"

namespace {

int g_fail = 0;
void check(bool ok, const std::string& what, const std::string& detail = "") {
    if (!ok) ++g_fail;
    std::printf("  [%s] %s%s%s\n", ok ? " ok " : "FAIL", what.c_str(),
                detail.empty() ? "" : "  -- ", detail.c_str());
}
std::string num(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.4g", v);
    return b;
}

// Edges with other than two faces on them (0 = closed, and every edge shared
// by exactly two faces), and edges walked the same way by both faces on them
// (0 = consistently wound).
struct Topo { int open = 0, twisted = 0; };
Topo topo(const EditMesh& m) {
    std::unordered_map<std::uint64_t, std::pair<int, int>> use;   // key -> (count, sum of directions)
    for (const std::vector<int>& f : m.faces)
        for (std::size_t i = 0; i < f.size(); ++i) {
            const int a = f[i], b = f[(i + 1) % f.size()];
            const std::uint64_t k = (static_cast<std::uint64_t>(std::min(a, b)) << 32) |
                                    static_cast<std::uint32_t>(std::max(a, b));
            auto& u = use[k];
            ++u.first;
            u.second += a < b ? 1 : -1;
        }
    Topo t;
    for (const auto& [k, u] : use) {
        if (u.first != 2) ++t.open;
        else if (u.second != 0) ++t.twisted;
    }
    return t;
}

// Signed volume (fans from the origin): positive when the faces look out.
double volume(const EditMesh& m) {
    double v = 0.0;
    for (const std::vector<int>& f : m.faces)
        for (std::size_t k = 1; k + 1 < f.size(); ++k) {
            const glm::dvec3 a(m.verts[f[0]]), b(m.verts[f[k]]), c(m.verts[f[k + 1]]);
            v += glm::dot(a, glm::cross(b, c)) / 6.0;
        }
    return v;
}

void subdivision() {
    std::printf("\n== Subdivision Surface ==\n");
    EditMesh cube = EditMesh::box(glm::vec3(0.5f));
    cube.setFaceMaterial(0, fitzel::AssetId{0x1234, 0x5678});
    cube.syncPaint();
    for (glm::vec4& w : cube.paint) w = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);

    EditMesh one = cube;
    const int done = editmesh::subdivideSurface(one, 1, true);
    check(done == 1 && one.faces.size() == 24 && one.verts.size() == 26,
          "a cube once: 24 quads on 26 corners",
          std::to_string(one.faces.size()) + " faces, " + std::to_string(one.verts.size()) + " corners");
    // The corner of the cube: Q = 1/6, R = 1/3, n = 3 -> (Q + 2R) / 3 = 5/18.
    float best = 1e9f;
    for (const glm::vec3& v : one.verts)
        best = std::min(best, glm::length(v - glm::vec3(5.0f / 18.0f)));
    check(best < 1e-5f, "the cube corner lands on (5/18, 5/18, 5/18)", "off by " + num(best));
    const Topo t1 = topo(one);
    check(t1.open == 0 && t1.twisted == 0 && volume(one) > 0.0, "closed, wound outward",
          "volume " + num(volume(one)));
    int red = 0, dressed = 0;
    for (std::size_t f = 0; f < one.faces.size(); ++f) dressed += one.faceMaterial(static_cast<int>(f)).valid() ? 1 : 0;
    for (const glm::vec4& w : one.paint) red += w.x > 0.99f ? 1 : 0;
    check(dressed == 4 && red == static_cast<int>(one.verts.size()) &&
              one.paint.size() == one.verts.size() && one.faceMat.size() == one.faces.size(),
          "a dressed face gives four dressed quads; paint carried to every corner",
          std::to_string(dressed) + " dressed, " + std::to_string(red) + " painted");

    EditMesh three = cube;
    editmesh::subdivideSurface(three, 3, true);
    float rMin = 1e9f, rMax = 0.0f;
    for (const glm::vec3& v : three.verts) {
        rMin = std::min(rMin, glm::length(v));
        rMax = std::max(rMax, glm::length(v));
    }
    check(three.faces.size() == 384 && rMax / rMin < 1.3f,
          "three times: 384 quads, nearly round",
          "radius " + num(rMin) + " .. " + num(rMax));

    EditMesh simple = cube;
    editmesh::subdivideSurface(simple, 2, false);
    bool onBox = true;
    for (const glm::vec3& v : simple.verts)
        onBox = onBox && std::abs(std::max({std::abs(v.x), std::abs(v.y), std::abs(v.z)}) - 0.5f) < 1e-5f;
    check(simple.faces.size() == 96 && onBox, "Simple only cuts: 96 quads, still the box");

    EditMesh plane = EditMesh::plane(glm::vec3(1.0f, 0.0f, 1.0f));
    editmesh::subdivideSurface(plane, 1, true);
    glm::vec3 mn, mx;
    plane.bounds(mn, mx);
    check(plane.faces.size() == 4 && std::abs(mx.x - 1.0f) < 1e-5f && std::abs(mn.z + 1.0f) < 1e-5f,
          "an open plane keeps its corners: a border is held, not shrunk",
          "x " + num(mn.x) + " .. " + num(mx.x));

    EditMesh big = EditMesh::sphere(glm::vec3(1.0f), 12, 24);
    const int lv = editmesh::subdivideSurface(big, 5, true, 20000);
    check(lv < 5 && big.faces.size() <= 20000, "a face budget stops the levels before it is passed",
          std::to_string(lv) + " levels, " + std::to_string(big.faces.size()) + " faces");
}

void decimate() {
    std::printf("\n== Decimate ==\n");
    EditMesh s = EditMesh::sphere(glm::vec3(1.0f), 16, 32);
    std::size_t before = 0;
    for (const auto& f : s.faces) before += f.size() - 2;
    EditMesh half = s;
    editmesh::decimate(half, 0.5f);
    std::size_t after = half.faces.size();
    float rMin = 1e9f, rMax = 0.0f;
    for (const glm::vec3& v : half.verts) {
        rMin = std::min(rMin, glm::length(v));
        rMax = std::max(rMax, glm::length(v));
    }
    check(after <= before * 55 / 100 && after >= before * 30 / 100,
          "ratio 0.5 keeps about half the triangles",
          std::to_string(before) + " -> " + std::to_string(after));
    check(rMax - rMin < 1e-4f && half.verts.size() < s.verts.size(),
          "corners are the original ones (still on the sphere), the unused gone",
          std::to_string(s.verts.size()) + " -> " + std::to_string(half.verts.size()) + " corners");
    const Topo t = topo(half);
    check(t.open == 0 && t.twisted == 0 && volume(half) > 0.0, "still closed and wound outward");
    EditMesh same = s;
    editmesh::decimate(same, 1.0f);
    check(same.faces.size() == s.faces.size(), "ratio 1 changes nothing");
}

void wireframe() {
    std::printf("\n== Wireframe ==\n");
    EditMesh cube = EditMesh::box(glm::vec3(0.5f));
    editmesh::wireframe(cube, 0.1f, 0.0f, true);
    const Topo t = topo(cube);
    check(cube.faces.size() == 72, "a cube: 24 frame quads, both skins, 24 inner walls",
          std::to_string(cube.faces.size()) + " faces");
    check(t.open == 0 && t.twisted == 0 && volume(cube) > 0.0, "a closed solid, wound outward",
          std::to_string(t.open) + " open, " + std::to_string(t.twisted) + " twisted edges");
    glm::vec3 mn, mx;
    cube.bounds(mn, mx);
    check(mx.x > 0.5f + 0.04f && mx.x < 0.5f + 0.1f, "the struts stand half their thickness proud",
          "reaches " + num(mx.x));

    EditMesh plane = EditMesh::plane(glm::vec3(1.0f, 0.0f, 1.0f));
    editmesh::wireframe(plane, 0.2f, 0.0f, true);
    const Topo tp = topo(plane);
    check(plane.faces.size() == 16 && tp.open == 0 && volume(plane) > 0.0,
          "an open plane: a closed frame, its border walled in",
          std::to_string(plane.faces.size()) + " faces, " + std::to_string(tp.open) + " open edges");

    EditMesh keep = EditMesh::box(glm::vec3(0.5f));
    editmesh::wireframe(keep, 0.1f, 0.0f, false);
    check(keep.faces.size() == 78, "Replace original off keeps the six faces too");
}

void solidify() {
    std::printf("\n== Solidify ==\n");
    EditMesh plane = EditMesh::plane(glm::vec3(1.0f, 0.0f, 1.0f));
    editmesh::solidify(plane, 0.2f, -1.0f, true, true);
    glm::vec3 mn, mx;
    plane.bounds(mn, mx);
    const Topo t = topo(plane);
    check(plane.faces.size() == 6 && t.open == 0 && volume(plane) > 0.0,
          "a plane becomes a closed slab: two skins, four rims");
    check(std::abs(mx.y) < 1e-5f && std::abs(mn.y + 0.2f) < 1e-5f,
          "offset -1 grows it inward, 0.2 deep", "y " + num(mn.y) + " .. " + num(mx.y));
    EditMesh cube = EditMesh::box(glm::vec3(0.5f));
    editmesh::solidify(cube, 0.1f, -1.0f, true, true);
    const double v = volume(cube);
    // A hollow box with 0.1 walls: 1 - 0.8^3.
    check(cube.faces.size() == 12 && std::abs(v - (1.0 - 0.512)) < 1e-3,
          "a closed box becomes hollow with even walls, no rims", "volume " + num(v));
}

void stack() {
    std::printf("\n== The stack ==\n");
    const char* kinds[] = {"subsurf", "decimate", "wireframe"};
    for (const char* k : kinds)
        check(modifiers::make(k) != nullptr, std::string("registered: ") + k);

    ModifierStackComponent ms;
    ms.stack.push_back(modifiers::make("subsurf"));
    ms.stack.push_back(modifiers::make("wireframe"));
    ms.stack[1]->enabled = false;
    nlohmann::json j;
    ms.save(j);
    ModifierStackComponent back;
    back.load(j);
    nlohmann::json j2;
    back.save(j2);
    check(j == j2 && back.stack.size() == 2 && !back.stack[1]->enabled,
          "save and load give the same stack, a switched-off one kept off");
    ModifierStackComponent copy = ms;
    copy.stack[0]->enabled = false;
    check(ms.stack[0]->enabled, "a copy is its own (undo snapshots copy components)");
    nlohmann::json bad = j;
    bad["stack"].push_back({{"kind", "noSuchModifier"}});
    ModifierStackComponent lenient;
    lenient.load(bad);
    check(lenient.stack.size() == 2, "an unknown kind is left out, the rest kept");

    MeshComponent mc;
    mc.mesh = EditMesh::box(glm::vec3(0.5f));
    mc.touch();
    const modifiers::Shown a = modifiers::shown(9001, mc, &ms);
    const modifiers::Shown b = modifiers::shown(9001, mc, &ms);
    check(a.mesh->faces.size() == 96 && a.revision == b.revision && a.revision != mc.revision,
          "shown: evaluated once, kept while nothing changes",
          std::to_string(a.mesh->faces.size()) + " faces");
    ms.stack[1]->enabled = true;
    const modifiers::Shown c = modifiers::shown(9001, mc, &ms);
    check(c.revision != b.revision && c.mesh->faces.size() > 96, "...and made again when the stack changes");
    const glm::vec3 half(2.0f, 1.0f, 3.0f);
    const glm::vec3 fs = editmesh::fitScale(*c.mesh, half), fb = editmesh::fitScale(mc.mesh, half);
    check(glm::length(fs - fb) < 1e-6f && c.mesh->smoothAngle > 0.0f,
          "drawn in the base mesh space (its frame), shaded smooth");
    ModifierStackComponent none;
    none.smooth = false;
    const modifiers::Shown d = modifiers::shown(9002, mc, &none);
    check(d.mesh == &mc.mesh && d.revision == mc.revision, "an idle stack shows the mesh itself");
}

// --entities <out.json>: a row of modelled objects with stacks on them, as the
// scene file keeps entities -- for looking at the result in the real renderer.
int writeEntities(const char* out) {
    struct Demo { const char* name; EditMesh mesh; std::vector<std::pair<const char*, nlohmann::json>> mods; bool smooth; };
    std::vector<Demo> demos;
    demos.push_back({"Subsurf", EditMesh::box(glm::vec3(0.5f)), {{"subsurf", {{"levels", 2}}}}, true});
    demos.push_back({"Decimate", EditMesh::sphere(glm::vec3(0.5f), 16, 32), {{"decimate", {{"ratio", 0.12f}}}}, false});
    demos.push_back({"Wireframe", EditMesh::box(glm::vec3(0.5f)), {{"wireframe", {{"thickness", 0.08f}}}}, true});
    demos.push_back({"Subsurf+Wire", EditMesh::box(glm::vec3(0.5f)),
                     {{"subsurf", {{"levels", 2}}}, {"wireframe", {{"thickness", 0.035f}}}}, true});
    demos.push_back({"Cylinder wire", EditMesh::cylinder(glm::vec3(0.5f), 12),
                     {{"wireframe", {{"thickness", 0.05f}}}}, true});
    nlohmann::json arr = nlohmann::json::array();
    int id = 500;
    float x = -6.0f;
    for (Demo& d : demos) {
        MeshComponent mc;
        mc.mesh = d.mesh;
        ModifierStackComponent ms;
        ms.smooth = d.smooth;
        for (auto& [kind, settings] : d.mods) {
            nlohmann::json e = settings;
            e["kind"] = kind;
            e["enabled"] = true;
            nlohmann::json one = {{"stack", nlohmann::json::array({e})}};
            ModifierStackComponent tmp;
            tmp.load(one);
            for (auto& m : tmp.stack) ms.stack.push_back(m->clone());
        }
        nlohmann::json cm, cs;
        mc.save(cm);
        cm["type"] = "mesh";
        ms.save(cs);
        cs["type"] = "modifiers";
        // Twice the mesh's size, standing on the ground; the first five in a
        // row in front, the step-two ones behind them.
        glm::vec3 mn, mx;
        d.mesh.bounds(mn, mx);
        const glm::vec3 h = mx - mn;
        const bool back = id >= 505;
        const float px = back ? -4.0f + (id - 505) * 5.0f : x;
        arr.push_back({{"active", true}, {"center", {px, h.y, back ? -3.5 : 0.0}},
                       {"half", {h.x, h.y, h.z}},
                       {"id", id++}, {"name", d.name}, {"parent", -1},
                       {"rotation", {0.0, back ? 0.0 : 25.0, 0.0}},
                       {"type", 0}, {"components", nlohmann::json::array({cm, cs})}});
        x += 3.0f;
    }
    std::FILE* f = std::fopen(out, "wb");
    if (!f) return 1;
    const std::string txt = arr.dump(1);
    std::fwrite(txt.data(), 1, txt.size(), f);
    std::fclose(f);
    std::printf("wrote %zu entities to %s\n", arr.size(), out);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 2 && std::string(argv[1]) == "--entities") return writeEntities(argv[2]);
    subdivision();
    decimate();
    wireframe();
    solidify();
    stack();
    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "all good", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
