// proccheck: the procedural graphs (ProcGraph.hpp, ProcNodes.cpp,
// ProcPresets.cpp), measured. Every node kind is cooked and checked for what
// its result must be whatever it looks like -- closed where it should be
// closed, wound outward, the number of faces worked out by hand -- then the
// graph's own rules (no loops, deleting a step closes the chain, the output
// flag follows), the file round trip, and the presets cooked whole.
//   build/release/bin/proccheck.exe [--scene <projectDir>]
// --scene also writes a small project with every preset standing in a row, for
// viewcheck to look at -- the maps a Rock texture painted included. With the
// name of a preset, only that one; with "Rocks", the Boulder in every kind of
// rock side by side.

#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "../src/Component.hpp"
#include "../src/EditMesh.hpp"
#include "../src/ProcGraph.hpp"
#include "../src/ProcInterior.hpp"
#include "../src/ProcNoise.hpp"
#include "../src/ProcRockTex.hpp"
#include "../src/ProcPresets.hpp"
#include "../src/PropertyMeta.hpp"

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

// Edges with other than two faces on them (0 = closed), and edges walked the
// same way by both their faces (0 = consistently wound).
struct Topo { int open = 0, twisted = 0; };
Topo topo(const EditMesh& m) {
    std::unordered_map<std::uint64_t, std::pair<int, int>> use;
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
        for (std::size_t i = 1; i + 1 < f.size(); ++i) {
            const glm::dvec3 a(m.verts[f[0]]), b(m.verts[f[i]]), c(m.verts[f[i + 1]]);
            v += glm::dot(a, glm::cross(b, c)) / 6.0;
        }
    return v;
}

int node(proc::Graph& g, const char* kind, const nlohmann::json& set = nlohmann::json::object(),
         const std::vector<int>& in = {}) {
    std::unique_ptr<proc::Node> n = proc::make(kind);
    if (!n) { check(false, std::string("kind exists: ") + kind); return -1; }
    readProps(set, n->props(), n.get());
    const int id = g.add(std::move(n)).id;
    for (std::size_t s = 0; s < in.size(); ++s) g.connect(id, static_cast<int>(s), in[s]);
    return id;
}

EditMesh cookOut(const proc::Graph& g, proc::CookInfo* info = nullptr) {
    return proc::cook(g, g.output, info);
}

void solid(const char* what, const EditMesh& m, std::size_t faces, double vol = -1.0,
           double tol = 0.02) {
    const Topo t = topo(m);
    const double v = volume(m);
    check(m.faces.size() == faces, std::string(what) + ": face count",
          std::to_string(m.faces.size()) + " (want " + std::to_string(faces) + ")");
    check(t.open == 0 && t.twisted == 0, std::string(what) + ": closed and consistently wound",
          "open " + std::to_string(t.open) + ", twisted " + std::to_string(t.twisted));
    if (vol > 0.0)
        check(std::fabs(v - vol) <= tol * vol, std::string(what) + ": volume, outward",
              num(v) + " (want " + num(vol) + ")");
    else
        check(v > 0.0, std::string(what) + ": wound outward", "volume " + num(v));
}

// --- Shapes ---------------------------------------------------------------------

void shapes() {
    std::printf("Shapes\n");
    {
        proc::Graph g;
        g.output = node(g, "box", {{"size", {2.0, 3.0, 4.0}}, {"center", {5.0, 0.0, 0.0}}});
        const EditMesh m = cookOut(g);
        solid("box", m, 6, 24.0);
        glm::vec3 mn, mx;
        m.bounds(mn, mx);
        check(std::fabs(mn.x - 4.0f) < 1e-4f && std::fabs(mx.x - 6.0f) < 1e-4f, "box: centre moves it");
    }
    {
        proc::Graph g;
        g.output = node(g, "tube", {{"radius", 1.0}, {"radiusTop", 1.0}, {"length", 4.0},
                                    {"segments", 24}, {"rows", 3}});
        const EditMesh m = cookOut(g);
        // A 24-gon of circumradius 1 has area 12 sin(15 deg) * 2 = 3.1058.
        solid("tube", m, 24 * 3 + 2, 4.0 * 24.0 * 0.5 * std::sin(2.0 * 3.14159265358979 / 24.0));
        check(m.paint.empty() && m.faceMat.empty(), "tube: no parallel arrays made up");
    }
    {
        proc::Graph g;
        g.output = node(g, "cylinder", {{"radius", 1.5}, {"height", 2.0}, {"segments", 32}});
        const EditMesh m = cookOut(g);
        // A 32-gon of circumradius 1.5: 16 sin(11.25 deg) * 2.25 = 7.0225.
        solid("cylinder", m, 32 + 2, 2.0 * 32.0 * 0.5 * 2.25 * std::sin(2.0 * 3.14159265358979 / 32.0));
        glm::vec3 mn, mx;
        m.bounds(mn, mx);
        check(std::fabs(mn.y + 1.0f) < 1e-4f && std::fabs(mx.y - 1.0f) < 1e-4f && std::fabs(mx.x - 1.5f) < 1e-4f,
              "cylinder: as tall as its height, centred, as wide as its radius");
        proc::Graph h;
        h.output = node(h, "cylinder", {{"radius", 1.0}, {"height", 6.0}, {"axis", 0}, {"caps", false}});
        const EditMesh o = cookOut(h);
        o.bounds(mn, mx);
        check(o.faces.size() == 24 && topo(o).open == 48 && std::fabs(mx.x - 3.0f) < 1e-4f,
              "cylinder: along X, open at both ends without caps");
    }
    {
        proc::Graph g;
        g.output = node(g, "tube", {{"radius", 1.0}, {"radiusTop", 0.0}, {"length", 3.0},
                                    {"segments", 16}, {"rows", 2}});
        const EditMesh m = cookOut(g);
        solid("cone (top radius 0)", m, 16 * 2 + 1);
    }
    {
        proc::Graph g;
        g.output = node(g, "tube", {{"radius", 1.0}, {"radiusTop", 1.0}, {"length", 6.0},
                                    {"segments", 8}, {"axis", 0}});
        const EditMesh m = cookOut(g);
        glm::vec3 mn, mx;
        m.bounds(mn, mx);
        check(std::fabs((mx.x - mn.x) - 6.0f) < 1e-3f && mx.y - mn.y < 2.01f, "tube: axis X lies along X");
        check(volume(m) > 0.0, "tube on X: still wound outward");
    }
    {
        proc::Graph g;
        g.output = node(g, "sphere", {{"radius", 2.0}, {"rings", 8}, {"segments", 12}});
        solid("sphere", cookOut(g), 12 * 2 + 12 * 6);
    }
    {
        proc::Graph g;
        g.output = node(g, "torus", {{"radius", 10.0}, {"width", 2.0}, {"height", 1.0},
                                     {"segments", 64}, {"sides", 4}});
        const EditMesh m = cookOut(g);
        // A box section 2 x 1 swept round a 64-gon: Pappus with the polygon's
        // perimeter at the section's centre, 2 pi R squeezed to 64 chords.
        const double ring = 64.0 * 2.0 * 10.0 * std::sin(3.14159265358979 / 64.0);
        solid("torus (box section)", m, 64 * 4, 2.0 * 1.0 * ring, 0.01);
        glm::vec3 mn, mx;
        m.bounds(mn, mx);
        check(std::fabs(mx.y - 0.5f) < 1e-4f && std::fabs(mx.x - 11.0f) < 1e-3f,
              "torus: four sides give flat faces at width and height",
              "top " + num(mx.y) + ", outer " + num(mx.x));
    }
    {
        proc::Graph g;
        g.output = node(g, "grid", {{"sizeX", 4.0}, {"sizeZ", 3.0}, {"cellsX", 4}, {"cellsZ", 3}});
        const EditMesh m = cookOut(g);
        bool up = m.faces.size() == 12;
        for (int f = 0; f < static_cast<int>(m.faces.size()); ++f) up = up && m.faceNormal(f).y > 0.99f;
        check(up, "grid: 12 cells, all facing up");
    }
}

// --- Combining and copies ---------------------------------------------------------

void copies() {
    std::printf("Combining and copies\n");
    {
        proc::Graph g;
        const int b = node(g, "box", {{"size", {1.0, 1.0, 1.0}}});
        g.output = node(g, "transform", {{"scale", {-1.0, 2.0, 1.0}}, {"move", {3.0, 0.0, 0.0}}}, {b});
        solid("transform with a mirroring scale", cookOut(g), 6, 2.0);
    }
    {
        proc::Graph g;
        const int b = node(g, "box", {{"size", {1.0, 1.0, 1.0}}, {"center", {2.0, 0.0, 0.0}}});
        g.output = node(g, "mirror", {{"axis", 0}}, {b});
        solid("mirror keeps both, both outward", cookOut(g), 12, 2.0);
    }
    {
        proc::Graph g;
        const int b = node(g, "box", {{"size", {1.0, 1.0, 1.0}}});
        g.output = node(g, "copyradial", {{"count", 6}, {"radius", 5.0}}, {b});
        const EditMesh m = cookOut(g);
        solid("copy radial: 6 boxes", m, 36, 6.0);
        glm::vec3 mn, mx;
        m.bounds(mn, mx);
        check(std::fabs(mx.x - 5.5f) < 1e-3f, "copy radial: pushed out by the radius", num(mx.x));
    }
    {
        proc::Graph g;
        const int b = node(g, "box", {{"size", {1.0, 1.0, 1.0}}});
        g.output = node(g, "copylinear", {{"count", 3}, {"step", {2.0, 0.0, 0.0}}}, {b});
        const EditMesh m = cookOut(g);
        glm::vec3 mn, mx;
        m.bounds(mn, mx);
        check(m.faces.size() == 18 && std::fabs(mn.x + 2.5f) < 1e-4f && std::fabs(mx.x - 2.5f) < 1e-4f,
              "copy in a row: 3 boxes, centred");
    }
    {
        proc::Graph g;
        const int dot = node(g, "box", {{"size", {0.2, 0.2, 0.2}}});
        const int on  = node(g, "box", {{"size", {4.0, 4.0, 4.0}}});
        g.output = node(g, "copytopoints", {{"where", 1}}, {dot, on});
        const EditMesh m = cookOut(g);
        glm::vec3 mn, mx;
        m.bounds(mn, mx);
        check(m.faces.size() == 36 && std::fabs(mx.y - 2.1f) < 1e-3f,
              "copy onto points: a box on each face centre, stood on the surface",
              std::to_string(m.faces.size()) + " faces, top " + num(mx.y));
        check(volume(m) > 0.0, "copy onto points: copies turned by the normal stay outward");
    }
    {
        proc::Graph g;
        const int a = node(g, "box");
        const int b = node(g, "sphere");
        g.output = node(g, "merge", nlohmann::json::object(), {a, b});
        check(cookOut(g).faces.size() == 6 + 24 * 2 + 24 * 10, "merge: everything wired in");
    }
}

// --- Detail and look ---------------------------------------------------------------

void detail() {
    std::printf("Detail and look\n");
    {
        proc::Graph g;
        const int b = node(g, "box", {{"size", {2.0, 2.0, 2.0}}});
        g.output = node(g, "panels", {{"faces", 0}, {"share", 1.0}, {"inset", 0.25},
                                      {"depthMin", 0.2}, {"depthMax", 0.2}}, {b});
        // Each face: a raised plate 1.5 x 1.5 x 0.2 on top of the cube.
        solid("panels on a cube", cookOut(g), 6 * 9, 8.0 + 6 * 1.5 * 1.5 * 0.2, 0.001);
    }
    {
        proc::Graph g;
        const int b = node(g, "box", {{"size", {2.0, 2.0, 2.0}}});
        g.output = node(g, "panels", {{"faces", 0}, {"share", 1.0}, {"inset", 0.25},
                                      {"depthMin", -0.2}, {"depthMax", -0.2}}, {b});
        solid("sunk panels", cookOut(g), 6 * 9, 8.0 - 6 * 1.5 * 1.5 * 0.2, 0.001);
    }
    {
        proc::Graph g;
        const int b = node(g, "box", {{"size", {2.0, 2.0, 2.0}}});
        g.output = node(g, "extrude", {{"faces", 1}, {"distance", 1.0}}, {b});
        solid("extrude the top", cookOut(g), 10, 12.0, 0.001);
    }
    {
        proc::Graph g;
        const int b = node(g, "box", {{"size", {2.0, 2.0, 2.0}}});
        g.output = node(g, "extrude", {{"faces", 1}, {"distance", 1.0}, {"inset", 0.5}}, {b});
        solid("inset, then extrude", cookOut(g), 14, 9.0, 0.001);
    }
    {
        proc::Graph g;
        const int t = node(g, "tube", {{"segments", 6}, {"rows", 4}, {"caps", false}});
        g.output = node(g, "lattice", {{"thickness", 0.1}}, {t});
        const EditMesh m = cookOut(g);
        const Topo tp = topo(m);
        check(tp.open == 0 && tp.twisted == 0 && volume(m) > 0.0, "lattice of an open tube: a closed truss",
              std::to_string(m.faces.size()) + " faces");
    }
    {
        proc::Graph g;
        const int b = node(g, "box");
        g.output = node(g, "deletefaces", {{"faces", 1}}, {b});
        const EditMesh m = cookOut(g);
        check(m.faces.size() == 5 && m.verts.size() == 8 && topo(m).open == 4, "delete faces: the top goes");
        g.find(g.output)->bypass = true;
        check(cookOut(g).faces.size() == 6, "bypass passes the input through");
    }
    {
        proc::Graph g;
        const int b = node(g, "box");
        g.output = node(g, "deletefaces", {{"faces", 1}, {"keepOnly", true}}, {b});
        const EditMesh m = cookOut(g);
        check(m.faces.size() == 1 && m.verts.size() == 4, "delete faces, keep only: the top stays alone");
    }
    {
        proc::Graph g;
        const int t = node(g, "tube", {{"segments", 12}, {"rows", 2}});
        const fitzel::AssetId gold = fitzel::AssetId::generate();
        const int m1 = node(g, "material", {{"material", gold.toString()}, {"faces", 3}}, {t});
        g.output = node(g, "panels", {{"faces", 3}, {"share", 1.0}}, {m1});
        const EditMesh m = cookOut(g);
        int dressed = 0, plain = 0;
        for (int f = 0; f < static_cast<int>(m.faces.size()); ++f)
            (m.faceMaterial(f) == gold ? dressed : plain)++;
        check(dressed == 24 * 9 && plain == 2, "material on the sides; panels made from them wear it",
              std::to_string(dressed) + " dressed, " + std::to_string(plain) + " plain");
    }
    {
        proc::Graph g;
        const int b = node(g, "grid", {{"cellsX", 10}, {"cellsZ", 10}});
        g.output = node(g, "material", {{"material", fitzel::AssetId::generate().toString()},
                                        {"share", 0.3}, {"seed", 4}}, {b});
        const EditMesh a = cookOut(g);
        const EditMesh b2 = cookOut(g);
        int n = 0;
        for (int f = 0; f < 100; ++f) n += a.faceMaterial(f).valid() ? 1 : 0;
        bool same = true;
        for (int f = 0; f < 100; ++f) same = same && a.faceMaterial(f) == b2.faceMaterial(f);
        check(n > 15 && n < 45 && same, "share 0.3: about a third, the same faces every cook",
              std::to_string(n) + " of 100");
    }
}

// --- 2D shapes, curves into surfaces --------------------------------------------------

proc::Geo cookG(const proc::Graph& g) { return proc::cookGeo(g, g.output); }

void curves() {
    std::printf("2D shapes and curves\n");
    const double pi = 3.14159265358979;
    {
        proc::Graph g;
        g.output = node(g, "circle", {{"radius", 3.0}, {"segments", 24}});
        const proc::Geo c = cookG(g);
        bool onRim = c.curves.size() == 1 && c.curves[0].pts.size() == 24 && c.curves[0].closed;
        for (const glm::vec3& p : c.curves.empty() ? std::vector<glm::vec3>{} : c.curves[0].pts)
            onRim = onRim && std::fabs(glm::length(p) - 3.0f) < 1e-4f && std::fabs(p.y) < 1e-6f;
        check(onRim && c.mesh.faces.empty(), "circle: a closed line of 24 points on the rim, no faces");
        g.find(g.output)->props();   // (settings are read back below through a new node)
        proc::Graph a;
        a.output = node(a, "circle", {{"radius", 3.0}, {"segments", 24}, {"sweep", 180.0}});
        const proc::Geo arc = cookG(a);
        check(arc.curves.size() == 1 && !arc.curves[0].closed && arc.curves[0].pts.size() == 25,
              "circle with a 180 degree arc: an open line, both ends included");
    }
    {
        proc::Graph g;
        g.output = node(g, "circle", {{"radius", 2.0}, {"segments", 32}, {"filled", true}});
        const EditMesh m = cookOut(g);
        check(m.faces.size() == 1 && m.faceNormal(0).y > 0.999f &&
                  std::fabs(m.faceArea(0) - 16.0 * 4.0 * std::sin(2.0 * pi / 32.0)) < 1e-3,
              "filled circle: one face, facing up, the polygon's area", num(m.faceArea(0)));
        const int disc = g.output;
        g.output = node(g, "extrude", {{"faces", 0}, {"distance", 3.0}}, {disc});
        solid("filled circle, extruded: a closed solid", cookOut(g), 2 + 32,
              3.0 * 16.0 * 4.0 * std::sin(2.0 * pi / 32.0), 0.001);
    }
    {
        proc::Graph g;
        g.output = node(g, "rect", {{"sizeX", 4.0}, {"sizeZ", 2.0}, {"filled", true}});
        const EditMesh m = cookOut(g);
        check(m.faces.size() == 1 && m.faceNormal(0).y > 0.999f && std::fabs(m.faceArea(0) - 8.0f) < 1e-4f,
              "filled rectangle: one face up, 4 x 2");
        proc::Graph x;
        x.output = node(x, "rect", {{"filled", true}, {"axis", 0}});
        check(cookOut(x).faceNormal(0).x > 0.999f, "...turned to face X with its axis");
        proc::Graph l;
        l.output = node(l, "rect", {{"sizeX", 4.0}, {"sizeZ", 2.0}});
        const proc::Geo c = cookG(l);
        check(c.curves.size() == 1 && c.curves[0].closed && c.curves[0].pts.size() == 4,
              "rectangle as a line: four corners, closed");
    }
    {
        proc::Graph g;
        g.output = node(g, "curve", {{"points", "0 0 0; 10 0 0; 20 5 0; 30 0 0"}, {"smooth", 8}});
        const proc::Geo c = cookG(g);
        const auto& pts = c.curves.empty() ? std::vector<glm::vec3>{} : c.curves[0].pts;
        check(pts.size() == 25 && glm::length(pts.front()) < 1e-5f &&
                  glm::length(pts[16] - glm::vec3(20, 5, 0)) < 1e-3f &&
                  glm::length(pts.back() - glm::vec3(30, 0, 0)) < 1e-5f,
              "smooth curve: 8 samples a span, through every point", std::to_string(pts.size()));
        const auto back = proc::parsePoints(proc::formatPoints({{1.5f, -2.0f, 3.25f}, {0.0f, 1.0f, 0.0f}}));
        check(back.size() == 2 && back[0] == glm::vec3(1.5f, -2.0f, 3.25f), "points survive the file's text form");
        proc::Graph f;
        f.output = node(f, "curve", {{"points", "0 0 0; 0 0 4; 4 0 4; 4 0 0"}, {"closed", true}, {"filled", true}});
        const EditMesh fm = cookOut(f);
        check(fm.faces.size() == 1 && fm.faceNormal(0).y > 0.999f,
              "a closed, filled curve: one face, turned to face up whichever way it was drawn");
    }
    {
        proc::Graph g;
        const int line = node(g, "curve", {{"points", "0 0 0; 30 0 0"}});
        g.output = node(g, "resample", {{"spacing", 2.0}}, {line});
        const proc::Geo c = cookG(g);
        bool even = !c.curves.empty() && c.curves[0].pts.size() == 16;
        for (std::size_t i = 1; even && i < c.curves[0].pts.size(); ++i)
            even = std::fabs(glm::length(c.curves[0].pts[i] - c.curves[0].pts[i - 1]) - 2.0f) < 1e-4f;
        check(even, "resample: a 30 m line in 2 m steps, 16 points");
    }
    {
        // Lines into Extrude: bands of quads, the line itself gone.
        proc::Graph g;
        const int line = node(g, "curve", {{"points", "0 0 0; 10 0 0; 10 0 5"}});
        g.output = node(g, "extrude", {{"distance", 3.0}}, {line});
        const proc::Geo w = cookG(g);
        check(w.mesh.faces.size() == 2 && w.mesh.verts.size() == 6 && w.curves.empty() &&
                  std::fabs(w.mesh.faceArea(0) + w.mesh.faceArea(1) - 45.0f) < 1e-3f &&
                  w.mesh.faceNormal(0).z > 0.999f,
              "extrude an open line: a wall of two faces, 3 m high", std::to_string(w.mesh.faces.size()));
        check(topo(w.mesh).twisted == 0, "...wound the same way all along");
        proc::Graph x;
        const int zl = node(x, "curve", {{"points", "0 0 0; 0 0 4"}});
        x.output = node(x, "extrude", {{"distance", 2.0}, {"lineAxis", 0}}, {zl});
        float mn = 1e9f, mx = -1e9f;
        for (const glm::vec3& v : cookOut(x).verts) { mn = std::min(mn, v.x); mx = std::max(mx, v.x); }
        check(std::fabs(mn) < 1e-5f && std::fabs(mx - 2.0f) < 1e-5f, "...pulled along X when told to");
    }
    {
        // A closed line makes a sleeve whose faces look out, however it was
        // drawn and whichever way it is pulled.
        auto outward = [](const EditMesh& m) {
            glm::vec3 c(0.0f);
            for (const glm::vec3& v : m.verts) c += v;
            c /= static_cast<float>(std::max<std::size_t>(m.verts.size(), 1));
            bool ok = !m.faces.empty();
            for (int f = 0; ok && f < static_cast<int>(m.faces.size()); ++f) {
                glm::vec3 r = m.faceCenter(f) - c;
                r.y = 0.0f;
                ok = glm::dot(r, m.faceNormal(f)) > 0.0f;
            }
            return ok;
        };
        proc::Graph g;
        const int circle = node(g, "circle", {{"radius", 2.0}, {"segments", 16}});
        g.output = node(g, "extrude", {{"distance", 3.0}}, {circle});
        const EditMesh s = cookOut(g);
        check(s.faces.size() == 16 && topo(s).open == 32 && topo(s).twisted == 0 && outward(s),
              "extrude a circle line: a sleeve of 16 faces, open at both ends, facing out");
        proc::Graph d;
        const int dc = node(d, "circle", {{"radius", 2.0}, {"segments", 16}});
        d.output = node(d, "extrude", {{"distance", -3.0}}, {dc});
        check(outward(cookOut(d)), "...pulled down: still facing out");
        for (const char* pts : {"0 0 0; 4 0 0; 4 0 4; 0 0 4", "0 0 0; 0 0 4; 4 0 4; 4 0 0"}) {
            proc::Graph r;
            const int c = node(r, "curve", {{"points", pts}, {"closed", true}});
            r.output = node(r, "extrude", {{"distance", 2.0}}, {c});
            check(outward(cookOut(r)), std::string("a closed curve drawn either way faces out: ") + pts);
        }
        // Faces and lines in one stream: both pulled.
        proc::Graph m;
        const int roof = node(m, "rect", {{"filled", true}});
        const int wall = node(m, "curve", {{"points", "10 0 0; 20 0 0"}});
        const int both = node(m, "merge", nlohmann::json::object(), {roof, wall});
        m.output = node(m, "extrude", {{"faces", 0}, {"distance", 1.0}}, {both});
        const std::size_t made = cookOut(m).faces.size();
        check(made == 6 + 1, "a filled rectangle and a line together: a block and a wall", std::to_string(made));
        // Loose points are no line: alone they give nothing to pull, beside a
        // line they stay points.
        proc::Graph p;
        const int bx  = node(p, "box");
        const int pts = node(p, "meshtopoints", nlohmann::json::object(), {bx});
        p.output = node(p, "extrude", {{"distance", 1.0}}, {pts});
        proc::CookInfo info;
        proc::cookGeo(p, p.output, &info);
        check(info.errors.count(p.output) == 1, "extrude of loose points alone: refused, and says why");
        const int ln = node(p, "curve", {{"points", "0 0 0; 5 0 0"}});
        const int mix = node(p, "merge", nlohmann::json::object(), {pts, ln});
        p.output = node(p, "extrude", {{"distance", 1.0}}, {mix});
        const proc::Geo lp = cookG(p);
        check(lp.curves.size() == 1 && lp.curves[0].loose && lp.mesh.faces.size() == 1,
              "...beside a line: the line becomes a wall, the points stay points");
    }
    // Sweep: round, a drawn profile, a face as profile -- all the same tube.
    const double tube = 10.0 * 8.0 * std::sin(2.0 * pi / 16.0);   // 16-gon of radius 1, 10 m long
    {
        proc::Graph g;
        const int path = node(g, "curve", {{"points", "0 0 0; 10 0 0"}});
        g.output = node(g, "sweep", {{"radius", 1.0}, {"sides", 16}}, {path});
        solid("sweep, round profile, straight path", cookOut(g), 16 + 2, tube, 0.001);
        const int circ = node(g, "circle", {{"radius", 1.0}, {"segments", 16}});
        g.output = node(g, "sweep", nlohmann::json::object(), {path, circ});
        solid("sweep with a circle as the profile", cookOut(g), 16 + 2, tube, 0.001);
        const int disc = node(g, "circle", {{"radius", 1.0}, {"segments", 16}, {"filled", true}});
        g.output = node(g, "sweep", nlohmann::json::object(), {path, disc});
        solid("sweep with a filled circle (a face) as the profile", cookOut(g), 16 + 2, tube, 0.001);
        g.output = node(g, "sweep", {{"flip", true}}, {path, circ});
        check(volume(cookOut(g)) < 0.0, "Flip faces turns the tube inside out");
    }
    {
        proc::Graph g;
        const int ring = node(g, "circle", {{"radius", 10.0}, {"segments", 48}});
        g.output = node(g, "sweep", {{"radius", 1.0}, {"sides", 12}}, {ring});
        const EditMesh m = cookOut(g);
        solid("sweep along a closed path: a ring, no caps, no seam", m, 48 * 12);
        proc::Graph s;
        const int wig = node(s, "curve", {{"points", "0 0 0; 8 3 2; 14 -2 6; 20 4 0; 26 0 -4"}, {"smooth", 10}});
        s.output = node(s, "sweep", {{"radius", 0.5}, {"sides", 10}, {"twist", 90.0}}, {wig});
        solid("sweep along a smooth 3D path, twisted", cookOut(s), 40 * 10 + 2);
    }
    {
        proc::Graph g;
        const int prof = node(g, "curve", {{"points", "0 0 0; 3 0 0; 3 4 0; 0 5 0"}});
        g.output = node(g, "revolve", {{"segments", 24}}, {prof});
        const double base = 12.0 * std::sin(2.0 * pi / 24.0) * 9.0;   // 24-gon of radius 3
        solid("revolve: a tank with a cone roof, tips welded", cookOut(g), 24 * 3,
              base * 4.0 + base / 3.0, 0.002);
        const int rev = node(g, "curve", {{"points", "0 5 0; 3 4 0; 3 0 0; 0 0 0"}});
        g.output = node(g, "revolve", {{"segments", 24}}, {rev});
        check(volume(cookOut(g)) > 0.0, "...drawn the other way round, still facing out");
        const int c = node(g, "circle", {{"radius", 1.0}, {"segments", 12}, {"axis", 2},
                                         {"center", {5.0, 0.0, 0.0}}});
        g.output = node(g, "revolve", {{"segments", 32}}, {c});
        solid("a circle revolved: a torus", cookOut(g), 32 * 12);
    }
    {
        // Spin: a 2 x 2 square face standing on X = 5, facing Z, turned round Y.
        proc::Graph g;
        const int sq = node(g, "rect", {{"sizeX", 2.0}, {"sizeZ", 2.0}, {"filled", true}, {"axis", 2},
                                        {"center", {5.0, 0.0, 0.0}}});
        g.output = node(g, "spin", {{"steps", 16}}, {sq});
        const EditMesh ring = cookOut(g);
        solid("spin a face a full turn: a ring, no ends", ring, 16 * 4);
        const double pappus = 2.0 * 8.0 * std::sin(2.0 * pi / 16.0) * (36.0 - 16.0);   // 16-gon ring, r 4..6, 2 tall
        check(std::fabs(volume(ring) - pappus) < 0.01 * pappus, "...its volume (a 16-gon ring)",
              num(volume(ring)) + " (want " + num(pappus) + ")");
        g.output = node(g, "spin", {{"steps", 4}, {"angle", 90.0}}, {sq});
        solid("spin a face a quarter turn: both ends closed", cookOut(g), 4 * 4 + 2);
        g.output = node(g, "spin", {{"steps", 4}, {"angle", -90.0}}, {sq});
        solid("...the other way round, still facing out", cookOut(g), 4 * 4 + 2);
        g.output = node(g, "spin", {{"steps", 32}, {"angle", 720.0}, {"rise", 6.0}}, {sq});
        const EditMesh screw = cookOut(g);
        solid("spin with rise: two turns of a screw", screw, 32 * 4 + 2);
        glm::vec3 mn, mx;
        screw.bounds(mn, mx);
        check(std::fabs((mx.y - mn.y) - 8.0f) < 0.01f, "...it climbs the rise plus its own height",
              num(mx.y - mn.y) + " (want 8)");
        g.output = node(g, "spin", {{"steps", 4}, {"angle", 90.0}, {"flip", true}}, {sq});
        check(volume(cookOut(g)) < 0.0, "spin: Flip faces turns it inside out");
        g.output = node(g, "spin", {{"steps", 8}, {"angle", 90.0}, {"center", {5.0, 3.0, 0.0}}, {"axis", 0}}, {sq});
        solid("spin round a centre of its own (a bend about X)", cookOut(g), 8 * 4 + 2);

        // A grid of 2 x 2 cells: only its rim makes walls, the inner edges do not.
        const int grid = node(g, "grid", {{"sizeX", 2.0}, {"sizeZ", 2.0}, {"cellsX", 2}, {"cellsZ", 2},
                                          {"center", {5.0, 0.0, 0.0}}});
        g.output = node(g, "spin", {{"steps", 6}, {"angle", 90.0}, {"axis", 2}}, {grid});
        solid("spin a sheet of four cells: one solid", cookOut(g), 6 * 8 + 4 * 2);

        // Lines: a profile with both ends on the axis closes like Revolve's tank.
        const int prof = node(g, "curve", {{"points", "0 0 0; 3 0 0; 3 4 0; 0 5 0"}});
        g.output = node(g, "spin", {{"steps", 24}}, {prof});
        const double base = 12.0 * std::sin(2.0 * pi / 24.0) * 9.0;
        solid("spin a line: the revolve tank", cookOut(g), 24 * 3, base * 4.0 + base / 3.0, 0.002);
        const int rev = node(g, "curve", {{"points", "0 5 0; 3 4 0; 3 0 0; 0 0 0"}});
        g.output = node(g, "spin", {{"steps", 24}}, {rev});
        check(volume(cookOut(g)) > 0.0, "...drawn the other way round, still facing out");

        // What is not picked passes through: the box's top spun, the rest kept.
        const int box = node(g, "box");
        g.output = node(g, "spin", {{"faces", static_cast<int>(proc::FaceSet::Up)}, {"angle", 90.0},
                                    {"steps", 3}, {"axis", 2}, {"center", {3.0, 0.0, 0.0}}}, {box});
        check(cookOut(g).faces.size() == 5 + 3 * 4 + 2, "spin only the top: the box keeps its other five",
              std::to_string(cookOut(g).faces.size()) + " (want 19)");
    }
}

// --- Selecting and filtering points ---------------------------------------------------

void points() {
    std::printf("Selecting and filtering points\n");
    proc::Graph g;
    const int grid = node(g, "grid", {{"sizeX", 10.0}, {"sizeZ", 10.0}, {"cellsX", 4}, {"cellsZ", 4}});
    const int inner = node(g, "selectpoints", {{"by", 0}, {"size", {5.1, 1.0, 5.1}}}, {grid});
    g.output = inner;
    proc::Geo s = cookG(g);
    check(s.hasSel && s.selectedCount() == 9, "inside a box: the 3 x 3 middle of a 5 x 5 grid",
          std::to_string(s.selectedCount()));
    g.output = node(g, "material", {{"material", fitzel::AssetId::generate().toString()}, {"faces", 10}}, {inner});
    {
        const EditMesh m = cookOut(g);
        int dressed = 0;
        for (int f = 0; f < static_cast<int>(m.faces.size()); ++f) dressed += m.faceMaterial(f).valid() ? 1 : 0;
        check(dressed == 4, "faces between selected points: the four in the middle", std::to_string(dressed));
    }
    const int dot = node(g, "box", {{"size", {0.2, 0.2, 0.2}}});
    g.output = node(g, "copytopoints", nlohmann::json::object(), {dot, inner});
    check(cookOut(g).faces.size() == 9 * 6, "copy onto points: only onto the selected ones");
    g.output = node(g, "transform", {{"move", {0.0, 1.0, 0.0}}, {"onlySelected", true}}, {inner});
    {
        const proc::Geo t = cookG(g);
        int up = 0, down = 0;
        for (const glm::vec3& v : t.mesh.verts) (v.y > 0.5f ? up : down)++;
        check(up == 9 && down == 16, "transform only the selected points: a raised middle");
    }
    g.output = node(g, "deletepoints", {{"keepSelected", true}}, {inner});
    {
        const proc::Geo d = cookG(g);
        check(d.mesh.faces.size() == 4 && d.mesh.verts.size() == 9, "delete points, keeping the selected");
    }
    g.output = node(g, "deletepoints", nlohmann::json::object(), {grid});
    check(cookOut(g).faces.size() == 16, "delete points with nothing selected leaves everything");
    {
        proc::CookInfo info;
        proc::cookGeo(g, g.output, &info);
        check(info.errors.count(g.output) == 1, "...and says why");
    }
    // A line of 16 points: every third, then filters chained.
    const int line = node(g, "curve", {{"points", "0 0 0; 30 0 0"}});
    const int pts16 = node(g, "resample", {{"spacing", 2.0}}, {line});
    const int third = node(g, "selectpoints", {{"by", 3}, {"every", 3}}, {pts16});
    g.output = third;
    check(cookG(g).selectedCount() == 6, "every third point of 16: six");
    const int left = node(g, "selectpoints", {{"by", 0}, {"center", {0.0, 0.0, 0.0}}, {"size", {30.0, 2.0, 2.0}},
                                              {"mode", 3}}, {third});
    g.output = left;
    check(cookG(g).selectedCount() == 3, "...keep only those inside a box: a filter, three left",
          std::to_string(cookG(g).selectedCount()));
    g.output = node(g, "selectpoints", {{"by", 1}, {"center", {30.0, 0.0, 0.0}}, {"radius", 4.5}, {"mode", 1}}, {left});
    check(cookG(g).selectedCount() == 6, "...add those within 4.5 m of the far end");
    g.output = node(g, "selectpoints", {{"by", 5}, {"mode", 2}, {"invert", true}}, {third});
    check(cookG(g).selectedCount() == 6, "take nothing away (an inverted 'all'): six still");
    g.output = node(g, "deletepoints", nlohmann::json::object(), {third});
    {
        const proc::Geo d = cookG(g);
        check(d.curves.size() == 1 && d.curves[0].pts.size() == 10, "delete points on a curve: it runs on without them");
    }
    // The selection through other steps.
    const int plain = node(g, "box");
    g.output = node(g, "merge", nlohmann::json::object(), {inner, plain});
    check(cookG(g).selectedCount() == 9, "merged with a stream that has no selection: those come in unselected");
    g.output = node(g, "copylinear", {{"count", 3}}, {inner});
    check(cookG(g).selectedCount() == 27, "copies keep their selection");
    g.output = node(g, "subdivide", nlohmann::json::object(), {inner});
    {
        const proc::Geo d = cookG(g);
        check(d.hasSel && d.selectedCount() == 0, "subdivide makes the corners anew: none of them selected");
    }
    const int up = node(g, "selectpoints", {{"by", 2}, {"facing", 0}}, {grid});
    g.output = up;
    check(cookG(g).selectedCount() == 25, "facing up: every point of a flat grid");
}

// --- Mesh to points --------------------------------------------------------------------

void meshToPoints() {
    std::printf("Mesh to points\n");
    proc::Graph g;
    const int box  = node(g, "box", {{"size", {2.0, 2.0, 2.0}}});
    const int corn = node(g, "meshtopoints", nlohmann::json::object(), {box});
    g.output = corn;
    {
        const proc::Geo p = cookG(g);
        const bool one = p.curves.size() == 1 && p.curves[0].loose;
        check(one && p.mesh.faces.empty() && p.curves[0].pts.size() == 8 && p.curves[0].nrm.size() == 8,
              "corners of a box: 8 loose points, the faces gone");
        bool outward = one;
        for (int i = 0; outward && i < 8; ++i)
            outward = glm::dot(p.curves[0].normalAt(i), p.curves[0].pts[static_cast<std::size_t>(i)] -
                                                        glm::vec3(0.0f)) > 0.0f;
        check(outward, "...each facing away from the box");
        proc::CookInfo info;
        proc::cookGeo(g, g.output, &info);
        check(info.curves[g.output] == 0 && info.corners[g.output] == 8, "...counted as points, not as a curve");
    }
    g.output = node(g, "meshtopoints", {{"from", 1}}, {box});
    {
        const proc::Geo p = cookG(g);
        bool ok = p.curves.size() == 1 && p.curves[0].pts.size() == 6;
        for (int i = 0; ok && i < 6; ++i)
            ok = std::fabs(glm::length(p.curves[0].pts[static_cast<std::size_t>(i)] - glm::vec3(0.0f)) - 1.0f) < 1e-4f &&
                 glm::length(p.curves[0].normalAt(i) - (p.curves[0].pts[static_cast<std::size_t>(i)] - glm::vec3(0.0f))) < 1e-4f;
        check(ok, "face centres of a box: 6, each with its face's normal");
    }
    const int grid = node(g, "grid", {{"sizeX", 10.0}, {"sizeZ", 10.0}, {"cellsX", 4}, {"cellsZ", 4}});
    const int strewn = node(g, "meshtopoints", {{"from", 2}, {"count", 500}, {"seed", 3}}, {grid});
    g.output = strewn;
    {
        const proc::Geo p = cookG(g);
        bool inside = p.curves.size() == 1 && p.curves[0].pts.size() == 500;
        int left = 0;
        for (std::size_t i = 0; inside && i < p.curves[0].pts.size(); ++i) {
            const glm::vec3& q = p.curves[0].pts[i];
            inside = std::fabs(q.x) <= 5.0001f && std::fabs(q.z) <= 5.0001f && std::fabs(q.y) < 1e-4f;
            left += q.x < 0.0f ? 1 : 0;
        }
        check(inside && left > 200 && left < 300, "strewn: 500 points on the sheet, spread evenly",
              std::to_string(left) + " on the left half");
        check(cookG(g).curves[0].pts == p.curves[0].pts, "...the same points every cook");
    }
    // Onward: copied onto, standing on the surface; selected; deleted; moved.
    const int dot = node(g, "box", {{"size", {0.2, 0.2, 0.2}}});
    const int cent = node(g, "meshtopoints", {{"from", 1}}, {box});
    g.output = node(g, "copytopoints", nlohmann::json::object(), {dot, cent});
    {
        const EditMesh m = cookOut(g);
        glm::vec3 mn(1e9f), mx(-1e9f);
        for (const glm::vec3& v : m.verts) { mn = glm::min(mn, v); mx = glm::max(mx, v); }
        check(m.faces.size() == 36 && std::fabs(mx.y - 1.1f) < 1e-3f && std::fabs(mx.x - 1.1f) < 1e-3f,
              "copy onto face-centre points: a dot standing out of every side");
        check(volume(m) > 0.0, "...wound outward");
    }
    const int sel = node(g, "selectpoints", {{"by", 0}, {"size", {5.0, 1.0, 10.0}}, {"center", {-2.5, 0.0, 0.0}}}, {strewn});
    g.output = node(g, "deletepoints", nlohmann::json::object(), {sel});
    {
        const proc::Geo d = cookG(g);
        bool right = d.curves.size() == 1 && d.curves[0].loose && d.curves[0].nrm.size() == d.curves[0].pts.size();
        for (const glm::vec3& q : right ? d.curves[0].pts : std::vector<glm::vec3>{}) right = right && q.x >= 0.0f;
        check(right && d.curves[0].pts.size() > 200 && d.curves[0].pts.size() < 300,
              "select + delete points: only the right half is left, normals kept");
    }
    g.output = node(g, "selectpoints", {{"by", 2}, {"facing", 1}}, {cent});
    check(cookG(g).selectedCount() == 1, "facing down: the bottom face's point, by its own normal");
    g.output = node(g, "transform", {{"turn", {180.0, 0.0, 0.0}}}, {corn});
    {
        const proc::Geo t = cookG(g);
        const glm::vec3 top = t.curves[0].pts[0];
        check(glm::dot(t.curves[0].normalAt(0), top) > 0.0f,
              "turned upside down: the normals turn with the points");
    }
    g.output = node(g, "sweep", nlohmann::json::object(), {corn});
    {
        proc::CookInfo info;
        proc::cookGeo(g, g.output, &info);
        check(info.errors.count(g.output) == 1, "sweep along loose points: refused, they are no path");
    }
    g.output = node(g, "meshtopoints", {{"keepFaces", true}}, {box});
    {
        const proc::Geo p = cookG(g);
        check(p.mesh.faces.size() == 6 && p.curves.size() == 1 && p.curves[0].pts.size() == 8,
              "keep the faces: the box and its 8 corners");
    }
    g.output = node(g, "meshtopoints", nlohmann::json::object(), {node(g, "selectpoints",
                    {{"by", 0}, {"size", {5.1, 1.0, 5.1}}}, {grid})});
    {
        const proc::Geo p = cookG(g);
        check(p.curves.size() == 1 && p.curves[0].pts.size() == 9 && !p.hasSel,
              "only the selected corners: 9, and they come out unselected");
    }
}

// --- Buildings and bridges --------------------------------------------------------------

std::size_t facesWearing(const EditMesh& m, const fitzel::AssetId& id) {
    std::size_t n = 0;
    for (int f = 0; f < static_cast<int>(m.faces.size()); ++f) n += m.faceMaterial(f) == id ? 1 : 0;
    return n;
}
bool closedOutward(const EditMesh& m) {
    const Topo t = topo(m);
    return t.open == 0 && t.twisted == 0 && volume(m) > 0.0;
}
std::string topoText(const EditMesh& m) {
    const Topo t = topo(m);
    return "open " + std::to_string(t.open) + ", twisted " + std::to_string(t.twisted) + ", volume " + num(volume(m)) +
           ", " + std::to_string(m.faces.size()) + " faces";
}
// A box block: a filled rectangle pulled up.
int block(proc::Graph& g, double x, double z, double h, const glm::vec3& at = glm::vec3(0.0f)) {
    const int r = node(g, "rect", {{"sizeX", x}, {"sizeZ", z}, {"filled", true}, {"center", {at.x, at.y, at.z}}});
    return node(g, "extrude", {{"faces", 0}, {"distance", h}}, {r});
}

void architecture() {
    std::printf("Buildings and bridges\n");
    const double pi = 3.14159265358979;
    {
        proc::Graph g;
        g.output = node(g, "archcurve", {{"span", 10.0}, {"rise", 5.0}, {"segments", 16}});
        const proc::Geo c = cookG(g);
        bool round = c.curves.size() == 1 && c.curves[0].pts.size() == 17;
        for (const glm::vec3& p : round ? c.curves[0].pts : std::vector<glm::vec3>{})
            round = round && std::fabs(glm::length(p) - 5.0f) < 1e-3f && p.y > -1e-4f;
        check(round, "arch curve: rise half the span is a half circle, 17 points");
        proc::Graph pg;
        pg.output = node(pg, "archcurve", {{"span", 10.0}, {"rise", 8.0}, {"shape", 2}, {"segments", 16}});
        float top = -1.0f;
        glm::vec3 apex(0.0f);
        const proc::Geo pointed = cookG(pg);
        for (const glm::vec3& p : pointed.curves[0].pts) if (p.y > top) { top = p.y; apex = p; }
        check(std::fabs(apex.y - 8.0f) < 1e-3f && std::fabs(apex.x) < 1e-3f, "pointed arch: apex at the rise");
        proc::Graph hg;
        hg.output = node(hg, "archcurve", {{"span", 90.0}, {"rise", -24.0}, {"shape", 3}, {"along", 1}});
        float lo = 1e9f, zmax = 0.0f;
        const proc::Geo hung = cookG(hg);
        for (const glm::vec3& p : hung.curves[0].pts) { lo = std::min(lo, p.y); zmax = std::max(zmax, std::fabs(p.z)); }
        check(std::fabs(lo + 24.0f) < 1e-3f && std::fabs(zmax - 45.0f) < 1e-3f,
              "a negative rise hangs (a cable), spanning Z when told to", num(lo) + ", " + num(zmax));
    }
    {
        // The arch wall: closed, outward, its volume the wall less the opening.
        proc::Graph g;
        g.output = node(g, "arch", {{"width", 10.0}, {"height", 9.0}, {"thickness", 2.0}, {"span", 6.0},
                                    {"rise", 3.0}, {"spring", 3.5}, {"segments", 16}});
        const EditMesh m = cookOut(g);
        const double half = 8.0 * 9.0 * std::sin(pi / 16.0);   // 16 chords of a half circle of radius 3
        const double want = 2.0 * (90.0 - 6.0 * 3.5 - half);
        check(closedOutward(m) && std::fabs(volume(m) - want) < 1e-3 * want, "arch: closed solid, wall less opening",
              topoText(m) + " (want " + num(want) + ")");
        for (int shape = 1; shape <= 3; ++shape) {
            proc::Graph s;
            s.output = node(s, "arch", {{"shape", shape}, {"rise", 4.5}, {"spring", 0.0}});
            check(closedOutward(cookOut(s)), "arch shape " + std::to_string(shape) + ", standing on the ground: closed",
                  topoText(cookOut(s)));
        }
        proc::Graph f;
        f.output = node(f, "arch", {{"rise", 0.0}});
        check(closedOutward(cookOut(f)), "a flat lintel (rise 0): closed", topoText(cookOut(f)));
    }
    {
        // Roofs on a 12 x 8 block 5 m tall, pitch 35, no overhang: closed with
        // the walls, the volume of the block and the roof.
        const double h = 4.0 * std::tan(35.0 * pi / 180.0);
        const struct { int kind; double vol; std::size_t faces; const char* what; } roofs[] = {
            {2, 480.0 + 8.0 * h / 2.0 * 4.0 + 64.0 * h / 3.0, 9, "hip"},
            {1, 480.0 + 8.0 * h / 2.0 * 12.0, 9, "gable"},
            {4, 480.0 + 8.0 * 2.0 * h / 2.0 * 12.0, 9, "shed"},
            {3, 480.0 + 96.0 * (5.0 * std::tan(35.0 * pi / 180.0)) / 3.0, 9, "pyramid"},
            {0, 480.0 + (96.0 - 11.4 * 7.4) * 1.0, 4 + 1 + 4 * 3 + 1, "flat with a parapet"},
        };
        for (const auto& r : roofs) {
            proc::Graph g;
            const int b = block(g, 12.0, 8.0, 5.0);
            g.output = node(g, "roof", {{"shape", r.kind}, {"pitch", 35.0}, {"overhang", 0.0}}, {b});
            const EditMesh m = cookOut(g);
            check(closedOutward(m) && m.faces.size() == r.faces && std::fabs(volume(m) - r.vol) < 2e-3 * r.vol,
                  std::string("roof, ") + r.what + ": closed on its walls, the right volume",
                  topoText(m) + " (want " + num(r.vol) + ", " + std::to_string(r.faces) + " faces)");
        }
        proc::Graph o;
        const int b = block(o, 12.0, 8.0, 5.0);
        o.output = node(o, "roof", {{"shape", 2}, {"pitch", 35.0}, {"overhang", 0.5}}, {b});
        float eave = 1e9f;
        for (const glm::vec3& v : cookOut(o).verts) eave = std::min(eave, v.y > 4.0f ? v.y : 1e9f);
        check(std::fabs(eave - (5.0f - 0.5f * std::tan(35.0f * 0.0174533f))) < 1e-3f,
              "an overhang carries the slope down past the wall", num(eave));
        // An L can have no hip: flat, and said.
        proc::Graph l;
        const int plan = node(l, "curve", {{"points", "0 0 0; 10 0 0; 10 0 4; 4 0 4; 4 0 10; 0 0 10"},
                                           {"closed", true}, {"filled", true}});
        const int walls = node(l, "extrude", {{"faces", 0}, {"distance", 3.0}}, {plan});
        l.output = node(l, "roof", {{"shape", 2}}, {walls});
        proc::CookInfo info;
        const EditMesh lm = cookOut(l, &info);
        check(info.errors.count(l.output) == 1 && closedOutward(lm), "a hip on an L: flat behind a parapet, and says so",
              topoText(lm));
        // The renderer fans faces: every face it gets is convex.
        proc::Graph lf;
        const int p2 = node(lf, "curve", {{"points", "0 0 0; 10 0 0; 10 0 4; 4 0 4; 4 0 10; 0 0 10"},
                                          {"closed", true}, {"filled", true}});
        lf.output = node(lf, "extrude", {{"faces", 0}, {"distance", 3.0}}, {p2});
        const EditMesh lfm = cookOut(lf);
        bool convex = true;
        for (const std::vector<int>& f : lfm.faces) {
            if (f.size() < 4) continue;
            for (std::size_t i = 0; i < f.size(); ++i) {
                const glm::vec3 a = lfm.verts[f[(i + f.size() - 1) % f.size()]], b2 = lfm.verts[f[i]],
                                c = lfm.verts[f[(i + 1) % f.size()]];
                convex = convex && glm::dot(glm::cross(b2 - a, c - b2), lfm.faceNormal(static_cast<int>(&f - &lfm.faces[0]))) > -1e-6f;
            }
        }
        check(convex && closedOutward(lfm) && std::fabs(volume(lfm) - 192.0) < 1e-3,
              "an L-shaped floor reaches the renderer as triangles: closed, 64 m2 x 3 m", topoText(lfm));
    }
    {
        // A facade on a 12 x 8 block of a 4 m ground floor and two 3.2 m storeys.
        const fitzel::AssetId glass = fitzel::AssetId::generate(), door = fitzel::AssetId::generate();
        proc::Graph g;
        const int b = block(g, 12.0, 8.0, 10.4);
        g.output = node(g, "facade", {{"materialGlass", glass.toString()}, {"materialDoor", door.toString()}}, {b});
        proc::CookInfo info;
        const EditMesh m = cookOut(g, &info);
        check(info.errors.empty(), "facade: cooks");
        check(closedOutward(m), "facade: still closed, every window set back into the wall", topoText(m));
        // Bays: 12 m -> 4, 8 m -> 3. Two storeys of 14 windows; on the ground
        // a door in each wall's middle bay and windows in the other ten.
        check(facesWearing(m, glass) == 28 + 10 && facesWearing(m, door) == 4, "facade: 38 panes of glass, 4 doors",
              std::to_string(facesWearing(m, glass)) + " glass, " + std::to_string(facesWearing(m, door)) + " doors");
        // Then a hip roof first (the order that works): still closed.
        proc::Graph r;
        const int rb = block(r, 12.0, 8.0, 10.4);
        const int roof = node(r, "roof", {{"shape", 2}, {"overhang", 0.0}}, {rb});
        r.output = node(r, "facade", {{"ledge", 0.0}}, {roof});
        check(closedOutward(cookOut(r)), "roof, then facade: the cuts in the eaves go into the roof too",
              topoText(cookOut(r)));
        // A setback block standing on the first: no doors up there.
        proc::Graph s;
        const int low = block(s, 12.0, 8.0, 7.2);
        const int high = block(s, 8.0, 6.0, 6.4, glm::vec3(0.0f, 7.2f, 0.0f));
        const int both = node(s, "merge", nlohmann::json::object(), {low, high});
        s.output = node(s, "facade", {{"materialDoor", door.toString()}}, {both});
        const EditMesh sm = cookOut(s);
        std::size_t doorWalls = 0;
        for (int f = 0; f < static_cast<int>(sm.faces.size()); ++f)
            if (sm.faceMaterial(f) == door && sm.faceCenter(f).y < 3.0f) ++doorWalls;
        check(facesWearing(sm, door) == 4 && doorWalls == 4, "a setback gets windows, the doors stay on the ground",
              std::to_string(facesWearing(sm, door)) + " door faces, " + std::to_string(doorWalls) + " low");
        check(closedOutward(sm) || topo(sm).twisted == 0, "...and nothing turned inside out", topoText(sm));
        // Too low for a storey: a parapet gets no windows.
        proc::Graph p;
        const int pb = block(p, 12.0, 8.0, 0.9);
        p.output = node(p, "facade", {{"materialGlass", glass.toString()}, {"ground", 0}}, {pb});
        check(facesWearing(cookOut(p), glass) == 0, "under two metres: no windows");
    }
    {
        // Offset: right of the run, outward round a closed outline either way.
        proc::Graph g;
        const int line = node(g, "curve", {{"points", "0 0 0; 10 0 0"}});
        g.output = node(g, "offset", {{"distance", 2.0}}, {line});
        const proc::Geo o = cookG(g);
        check(o.curves.size() == 1 && std::fabs(o.curves[0].pts[0].z - 2.0f) < 1e-5f &&
                  std::fabs(o.curves[0].pts[1].z - 2.0f) < 1e-5f,
              "offset: a line moved 2 m to its right");
        for (const char* pts : {"0 0 0; 4 0 0; 4 0 4; 0 0 4", "0 0 0; 0 0 4; 4 0 4; 4 0 0"}) {
            proc::Graph s;
            const int sq = node(s, "curve", {{"points", pts}, {"closed", true}});
            s.output = node(s, "offset", {{"distance", 1.0}, {"both", true}}, {sq});
            const proc::Geo so = cookG(s);
            float mx = -1e9f, mn = 1e9f;
            for (const glm::vec3& v : so.curves[0].pts) { mx = std::max(mx, v.x); mn = std::min(mn, v.x); }
            float mx2 = -1e9f;
            for (const glm::vec3& v : so.curves[1].pts) mx2 = std::max(mx2, v.x);
            check(so.curves.size() == 2 && std::fabs(mn + 1.0f) < 1e-5f && std::fabs(mx - 5.0f) < 1e-5f &&
                      std::fabs(mx2 - 3.0f) < 1e-5f,
                  std::string("offset of a closed square, drawn either way: out by 1 and in by 1: ") + pts);
        }
    }
    {
        // Drop lines: onto a sheet where it is below, to the height elsewhere.
        proc::Graph g;
        const int sheet = node(g, "grid", {{"sizeX", 10.0}, {"sizeZ", 10.0}, {"center", {0.0, 5.0, 0.0}}});
        const int pts = node(g, "curve", {{"points", "0 10 0; 2 10 1; 20 10 0"}});
        g.output = node(g, "droplines", nlohmann::json::object(), {pts, sheet});
        const proc::Geo d = cookG(g);
        check(d.curves.size() == 3 && std::fabs(d.curves[0].pts[1].y - 5.0f) < 1e-4f &&
                  std::fabs(d.curves[1].pts[1].y - 5.0f) < 1e-4f && std::fabs(d.curves[2].pts[1].y) < 1e-4f,
              "drop lines: two onto the sheet, one past it down to height 0");
    }
    {
        // Railing and truss: every strut a closed box.
        proc::Graph g;
        const int line = node(g, "curve", {{"points", "0 0 0; 10 0 0"}});
        g.output = node(g, "railing", {{"spacing", 2.0}, {"midRails", 1}}, {line});
        const EditMesh m = cookOut(g);
        // 6 posts, 5 top rails, 5 middle rails.
        check(closedOutward(m) && m.faces.size() == (6 + 5 + 5) * 6, "railing: 6 posts, two rails per bay, all closed",
              topoText(m));
        proc::Graph c;
        const int sq = node(c, "rect", {{"sizeX", 4.0}, {"sizeZ", 4.0}});
        c.output = node(c, "railing", {{"spacing", 2.0}, {"midRails", 0}, {"fill", 2}}, {sq});
        check(closedOutward(cookOut(c)) && cookOut(c).faces.size() == (8 + 8 + 8) * 6,
              "a railing round a closed square: a post at every corner, panels between", topoText(cookOut(c)));
        proc::Graph t;
        const int path = node(t, "curve", {{"points", "0 0 0; 24 0 0"}});
        t.output = node(t, "truss", {{"panel", 6.0}, {"pattern", 1}}, {path});
        const EditMesh tm = cookOut(t);
        check(closedOutward(tm) && tm.faces.size() == 48u * 6u, "truss, Pratt: 48 struts over 4 panels",
              topoText(tm));
        proc::Graph w;
        const int wp = node(w, "curve", {{"points", "0 0 0; 24 0 0"}});
        w.output = node(w, "truss", {{"panel", 6.0}, {"pattern", 0}, {"sides", 1}}, {wp});
        check(cookOut(w).faces.size() == 14u * 6u, "truss, Warren, one side: 14 struts",
              std::to_string(cookOut(w).faces.size() / 6));
    }
    {
        // The Add menus show each category once.
        std::vector<std::string> seen;
        bool once = true;
        for (const proc::TypeInfo& t : proc::registry()) {
            if (!seen.empty() && seen.back() == t.category) continue;
            once = once && std::find(seen.begin(), seen.end(), t.category) == seen.end();
            seen.push_back(t.category);
        }
        check(once, "the kinds come a category at a time");
    }
}

// --- The graph's rules ---------------------------------------------------------------

void rules() {
    std::printf("Graph rules\n");
    proc::Graph g;
    const int a = node(g, "box");
    const int t = node(g, "transform", nlohmann::json::object(), {a});
    const int s = node(g, "subdivide", nlohmann::json::object(), {t});
    g.output = s;
    check(!g.connect(t, 0, s), "a loop is refused");
    check(g.find(t)->inputs[0] == a, "...and the old wire stays");
    check(!g.connect(t, 0, t), "a node cannot feed itself");
    check(g.find(a)->name == "box1" && g.find(t)->name == "transform1", "names are unique by kind");
    check(g.uniqueName("box1") == "box2", "another box is box2");

    proc::Graph h = g;   // deep copy
    h.find(a)->name = "changed";
    check(g.find(a)->name == "box1", "copying a graph copies its nodes");

    g.remove(t);
    check(g.find(s)->inputs[0] == a, "deleting a step closes the chain");
    g.remove(s);
    check(g.output == a, "deleting the output hands the flag to its input");

    proc::Graph m;
    const int x = node(m, "box");
    const int y = node(m, "sphere");
    const int mg = node(m, "merge", nlohmann::json::object(), {x, y});
    m.output = mg;
    m.remove(x);
    check(m.find(mg)->inputs.size() == 1 && m.find(mg)->inputs[0] == y,
          "deleting a merge's source drops that strand");
    check(m.connect(mg, 1, x) == false, "wiring a node that is gone is refused");

    // Where a node sits is the editor's business, not what it makes.
    proc::Graph k;
    const int box = node(k, "box");
    k.output = box;
    const std::size_t h1 = proc::hashOf(k);
    k.find(box)->pos    = glm::vec2(12.0f, 3.0f);
    k.find(box)->placed = true;
    check(proc::hashOf(k) == h1, "moving a node is not a change to cook");
    check(k.placedByHand(), "...but the graph knows it is laid out by hand");
    nlohmann::json kj;
    k.save(kj);
    proc::Graph k2;
    k2.load(kj);
    check(k2.find(box) && k2.find(box)->placed && k2.find(box)->pos == glm::vec2(12.0f, 3.0f),
          "a node's place is kept in the file");
    k.find(box)->placed = false;
    nlohmann::json kj2;
    k.save(kj2);
    check(!kj2["nodes"][0].contains("pos"), "a node nobody placed keeps no place in the file");

    // "Merge into output" (proc::mergeIntoOutput).
    const int sph = node(k, "sphere");
    proc::mergeIntoOutput(k, sph);
    const proc::Node* mrg = k.find(k.output);
    check(mrg && mrg->variadic() && mrg->inputs.size() == 2 && mrg->inputs[0] == box && mrg->inputs[1] == sph,
          "Merge into output: a merge of the old output and the node");
    const int tor = node(k, "torus");
    proc::mergeIntoOutput(k, tor);
    check(k.find(k.output) == mrg && mrg->inputs.size() == 3 && mrg->inputs[2] == tor,
          "...and the next goes into that same merge");
    proc::Graph none;
    const int first = node(none, "box");
    proc::mergeIntoOutput(none, first);
    check(none.output == first, "with no output yet, the node becomes it");
}

void roundTrip() {
    std::printf("File round trip\n");
    std::vector<MaterialDef> mats;
    const proc::Graph g = procpreset::build(0, mats);
    ProcGraphComponent pc;
    pc.graph = g;
    pc.pivot = glm::vec3(1.0f, 2.0f, 3.0f);
    nlohmann::json j;
    pc.save(j);
    j["type"] = "procgraph";
    std::unique_ptr<ComponentBase> back = components::create("procgraph");
    check(back != nullptr, "the component is registered");
    if (!back) return;
    back->load(j);
    auto* pc2 = dynamic_cast<ProcGraphComponent*>(back.get());
    check(pc2 && proc::hashOf(pc2->graph) == proc::hashOf(g), "the graph comes back the same");
    check(pc2 && pc2->pivot == pc.pivot, "the pivot comes back");
    if (pc2)
        check(cookOut(pc2->graph).faces.size() == cookOut(g).faces.size(), "...and cooks the same");
    nlohmann::json bad = j;
    bad["nodes"].push_back({{"id", 999}, {"kind", "no-such-node"}});
    ProcGraphComponent pc3;
    pc3.load(bad);
    check(pc3.graph.nodes.size() == g.nodes.size(), "an unknown kind is dropped, the rest kept");
}

// --- Parameters and expressions -------------------------------------------------

void parameters() {
    std::printf("Parameters and expressions\n");
    proc::Graph g;
    proc::Parm r;
    r.name = "ring_radius"; r.label = "Ring radius"; r.value.x = 10.0f; r.min = 1.0f; r.max = 100.0f;
    g.parms.push_back(r);
    proc::Parm segs;
    segs.name = "segs"; segs.type = proc::ParmType::Int; segs.value.x = 12.0f;
    g.parms.push_back(segs);
    proc::Parm off;
    off.name = "off"; off.type = proc::ParmType::Vector; off.value = glm::vec3(1.0f, 2.0f, 3.0f);
    g.parms.push_back(off);
    proc::Parm mat;
    mat.name = "hull"; mat.type = proc::ParmType::Material; mat.text = "abc";
    g.parms.push_back(mat);

    auto ev = [&](const char* e) { return proc::evalExpr(g, e); };
    auto near = [](double a, double b) { return std::fabs(a - b) < 1e-6; };
    check(near(ev("1 + 2 * 3").value, 7.0) && near(ev("(1 + 2) * 3").value, 9.0), "arithmetic, in order");
    check(near(ev("-2^2").value, -4.0) && near(ev("2^3^2").value, 512.0), "powers bind tighter than the sign, right to left");
    check(near(ev("ring_radius / 2").value, 5.0), "a parameter by name");
    check(near(ev("off.y + off.z").value, 5.0), "a vector parameter's numbers");
    check(near(ev("sin(90) + cos(0)").value, 2.0), "trigonometry in degrees, as in Houdini");
    check(near(ev("fit(5, 0, 10, 0, 100)").value, 50.0) && near(ev("fit(20, 0, 10, 0, 100)").value, 100.0),
          "fit() maps and clamps");
    check(near(ev("segs > 10 ? 1 : 2").value, 1.0) && near(ev("if(segs == 12 && !false, 3, 4)").value, 3.0),
          "comparisons, logic, ?: and if()");
    check(near(ev("max(1, 7, 3) + min(4, 2)").value, 9.0) && near(ev("clamp(15, 0, 10)").value, 10.0),
          "min, max, clamp");
    check(near(ev("rand(4)").value, ev("rand(4)").value) && ev("rand(4)").value < 1.0, "rand() is the same every time");
    check(!ev("radus * 2").ok() && ev("radus * 2").error.find("radus") != std::string::npos,
          "an unknown name is an error that says it", ev("radus * 2").error);
    check(!ev("off").ok() && !ev("hull + 1").ok() && !ev("1 +").ok() && !ev("1 / 0").ok(),
          "a vector without its number, text, a half formula, /0: errors");

    // A node follows a parameter...
    const int t = node(g, "tube", {{"radius", 2.0}, {"segments", 8}, {"rows", 1}});
    proc::Node* tube = g.find(t);
    tube->exprs["radius"]   = "ring_radius * 0.5";
    tube->exprs["segments"] = "segs";
    tube->exprs["center.y"] = "off.y * 10";
    g.output = t;
    proc::Graph direct;
    const int d = node(direct, "tube", {{"radius", 5.0}, {"segments", 12}, {"rows", 1},
                                        {"center", {0.0, 20.0, 0.0}}});
    direct.output = d;
    proc::CookInfo info;
    const EditMesh a = cookOut(g, &info);
    const EditMesh b = cookOut(direct);
    glm::vec3 amn, amx, bmn, bmx;
    a.bounds(amn, amx);
    b.bounds(bmn, bmx);
    check(a.faces.size() == b.faces.size() && glm::length(amn - bmn) < 1e-4f && glm::length(amx - bmx) < 1e-4f,
          "a node cooks with the numbers its expressions give", std::to_string(a.faces.size()) + " faces");
    check(info.errors.empty(), "...without an error");
    check(proc::shownValue(g, *tube, "radius") == "5" && proc::shownValue(g, *tube, "center.y") == "20",
          "the editor is told the value beside the formula", proc::shownValue(g, *tube, "radius"));
    // ...clamped to the setting's range, rounded where it is whole.
    g.parm("segs")->value.x = 3000.0f;
    std::unique_ptr<proc::Node> rs = proc::resolved(g, *tube);
    check(rs && proc::shownValue(g, *tube, "segments") == "256", "an expression's value keeps to the setting's range");
    g.parm("segs")->value.x = 12.0f;

    // ...and another node follows that node (Houdini's channel reference).
    const int t2 = node(g, "tube", {{"radius", 1.0}});
    proc::Node* tube2 = g.find(t2);
    tube2->exprs["radius"] = std::string(tube->name) + ".radius + 1";
    tube2->exprs["length"] = std::string("ch(\"../") + tube->name + "/center.y\")";
    check(proc::shownValue(g, *tube2, "radius") == "6" && proc::shownValue(g, *tube2, "length") == "20",
          "another node's setting, by name and by ch(\"../node/key\")", proc::shownValue(g, *tube2, "length"));
    tube->exprs["center.y"] = std::string(tube2->name) + ".length";
    check(proc::shownValue(g, *tube2, "length").find("round to itself") != std::string::npos,
          "a loop between two settings is said, not followed", proc::shownValue(g, *tube2, "length"));
    proc::CookInfo loop;
    cookOut(g, &loop);
    check(loop.errors.count(t) == 1, "...and the cook says it on the node");
    tube->exprs["center.y"] = "off.y * 10";

    // A text setting: "$hull" fills in the material.
    const int m = node(g, "material", {{"material", "zzz"}}, {t});
    proc::Node* mn = g.find(m);
    mn->exprs["material"] = "$hull";
    check(proc::shownValue(g, *mn, "material") == "abc", "a text setting fills in $name");
    check(proc::evalText(g, "r=${ring_radius * 2} $$1 ${hull}") == "r=20 $1 abc", "${formula}, $$ and ${name} in a text",
          proc::evalText(g, "r=${ring_radius * 2} $$1 ${hull}"));

    // An expression that fails: said on the node, the field's own value used.
    tube->exprs["rows"] = "nope + 1";
    proc::CookInfo bad;
    cookOut(g, &bad);
    check(bad.errors.count(t) && bad.errors[t].find("Rows") != std::string::npos,
          "a failing expression is the node's error, naming the setting", bad.errors.count(t) ? bad.errors[t] : "");
    tube->exprs.erase("rows");

    // Who uses a parameter; renaming follows into every formula.
    const std::vector<std::string> users = proc::parmUsers(g, "ring_radius");
    check(users.size() == 1 && users[0] == tube->name + ".radius", "parmUsers finds the settings that read it");
    g.parm("segs")->hideWhen = "segs < 4";
    proc::renameParm(g, "segs", "sides");
    check(tube->exprs["segments"] == "sides" && g.parm("sides") && g.parm("sides")->hideWhen == "sides < 4",
          "renaming a parameter renames it in every expression", tube->exprs["segments"]);
    proc::renameParm(g, "hull", "paint");
    check(mn->exprs["material"] == "$paint", "...and in a text's $name");
    proc::renameParm(g, "ring_radius", "rr");
    check(tube2->exprs["radius"] == tube->name + ".radius + 1", "...but not a node's setting of the same word");

    // Hide / disable when.
    g.parm("sides")->value.x = 3.0f;
    check(proc::parmHidden(g, *g.parm("sides")), "hide when: an expression over the parameters");
    g.parm("sides")->value.x = 12.0f;
    check(!proc::parmHidden(g, *g.parm("sides")), "...shown again when it says no");

    // Taking a parameter out freezes what followed it.
    proc::removeParm(g, "rr");
    check(!g.parm("rr") && !tube->exprs.count("radius"), "removing a parameter takes its expressions off");
    nlohmann::json tj;
    writeProps(tj, tube->props(), tube);
    check(tj.value("radius", 0.0) == 5.0, "...leaving the value it gave", tj.dump());

    // Promoting a setting.
    const int t3 = node(g, "tube", {{"radius", 3.5}, {"center", {1.0, 2.0, 3.0}}});
    proc::Node* tube3 = g.find(t3);
    const Property* prRadius = nullptr;
    const Property* prCenter = nullptr;
    for (const Property& pr : tube3->props()) {
        if (pr.key == "radius") prRadius = &pr;
        if (pr.key == "center") prCenter = &pr;
    }
    const std::string pn = proc::promote(g, *tube3, *prRadius);
    check(pn == "radius" && g.parm(pn) && g.parm(pn)->value.x == 3.5f && tube3->exprs["radius"] == "radius",
          "promote: a parameter with the setting's value, the setting following it", pn);
    const std::string pn2 = proc::promote(g, *tube2, *prRadius);
    check(pn2 != pn && !pn2.empty() && g.parm(pn2), "...a second one of the same name gets the node's in front", pn2);
    const std::string pv = proc::promote(g, *tube3, *prCenter);
    check(g.parm(pv) && g.parm(pv)->type == proc::ParmType::Vector && tube3->exprs["center.z"] == pv + ".z" &&
              proc::shownValue(g, *tube3, "center.z") == "3",
          "promoting a vector makes a Vector parameter, one expression a number");

    // The file keeps all of it.
    nlohmann::json j;
    g.save(j);
    proc::Graph back;
    back.load(j);
    check(proc::hashOf(back) == proc::hashOf(g) && back.parms.size() == g.parms.size(),
          "parameters and expressions survive the file", std::to_string(back.parms.size()) + " parameters");
    check(cookOut(back).faces.size() == cookOut(g).faces.size(), "...and cook the same");
    const std::size_t h0 = proc::hashOf(g);
    g.parm(pn)->value.x = 4.0f;
    check(proc::hashOf(g) != h0, "a parameter's value is part of what the editor compares");
    proc::Graph copy = g;
    check(proc::hashOf(copy) == proc::hashOf(g), "a copy of the graph carries its parameters");
}

void presets() {
    std::printf("Presets\n");
    std::vector<MaterialDef> mats;
    for (int i = 0; i < static_cast<int>(procpreset::list().size()); ++i) {
        const std::size_t before = mats.size();
        const proc::Graph g = procpreset::build(i, mats);
        proc::CookInfo info;
        const auto t0 = std::chrono::steady_clock::now();
        const EditMesh m = cookOut(g, &info);
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count();
        std::string errs;
        for (const auto& [id, e] : info.errors) {
            const proc::Node* n = g.find(id);
            errs += (n ? n->name : std::to_string(id)) + ": " + e + "; ";
        }
        std::size_t tris = 0;
        for (const auto& f : m.faces) tris += f.size() - 2;
        const std::string name = procpreset::list()[static_cast<std::size_t>(i)].name;
        check(info.errors.empty(), name + ": every node cooks", errs);
        // A hollow building is mostly air: what it encloses says nothing about its winding.
        const bool hollow = name == "Furnished town house";
        check(!m.faces.empty() && (hollow || volume(m) > 0.0), name + ": something, wound outward",
              std::to_string(m.faces.size()) + " faces, " + std::to_string(tris) + " triangles, " +
              num(ms) + " ms, " + std::to_string(g.nodes.size()) + " nodes");
        std::size_t used = 0;
        for (const auto& n : g.nodes) {
            const std::vector<int> up = g.upstream(g.output);
            used += std::find(up.begin(), up.end(), n->id) != up.end() ? 1 : 0;
        }
        std::size_t bad = 0;
        for (const glm::vec3& v : m.verts) bad += std::isfinite(v.x + v.y + v.z) ? 0 : 1;
        check(bad == 0, name + ": every corner is a number", std::to_string(bad) + " are not");
        {
            // What the scene file keeps comes back whole.
            MeshComponent mc;
            mc.mesh = m;
            nlohmann::json j;
            mc.save(j);
            MeshComponent back;
            back.load(j);
            check(back.mesh.faces.size() == m.faces.size() && back.mesh.verts.size() == m.verts.size(),
                  name + ": survives the scene file",
                  std::to_string(back.mesh.faces.size()) + " of " + std::to_string(m.faces.size()) + " faces");
        }
        check(used == g.nodes.size(), name + ": no node left dangling",
              std::to_string(used) + " of " + std::to_string(g.nodes.size()));
        if (i == 0)
            check(mats.size() - before == 6, "the station palette is made once", std::to_string(mats.size()));
        const std::size_t made = mats.size();
        procpreset::build(i, mats);
        check(mats.size() == made, name + ": its palette is found, not made again");
    }
}

// --- Rocks: noise, smoothing, facets and the painted texture -------------------------------
// The shape steps must never open what was closed (corners at one place move
// as one), stay within what they were asked for, and be the same on every
// cook; the painting must give every face coordinates on the tile, maps of the
// asked size that are not flat, and the same maps again from its cache.

double meanRadius(const EditMesh& m, const glm::vec3& c, double* spread = nullptr) {
    double s = 0.0, s2 = 0.0;
    for (const glm::vec3& v : m.verts) {
        const double r = glm::length(v - c);
        s += r;
        s2 += r * r;
    }
    const double n = static_cast<double>(std::max<std::size_t>(m.verts.size(), 1));
    if (spread) *spread = std::sqrt(std::max(0.0, s2 / n - (s / n) * (s / n)));
    return s / n;
}

void rocks() {
    std::printf("Rocks\n");
    {
        proc::Graph g;
        g.output = node(g, "icosphere", {{"radius", 2.0}, {"detail", 0}});
        solid("icosphere detail 0", cookOut(g), 20);
        proc::Graph g4;
        g4.output = node(g4, "icosphere", {{"radius", 2.0}, {"detail", 4}});
        const EditMesh m = cookOut(g4);
        solid("icosphere detail 4", m, 5120, 4.0 / 3.0 * 3.14159265 * 8.0, 0.02);
        double worst = 0.0;
        for (const glm::vec3& v : m.verts) worst = std::max(worst, std::fabs(glm::length(v) - 2.0));
        check(worst < 1e-4, "icosphere: every corner on the sphere", num(worst));
        check(m.verts.size() == 2562, "icosphere: corners shared", std::to_string(m.verts.size()));
    }
    {
        // Noise: within the amplitude, really moving, the same every cook,
        // another pattern for another seed, closed afterwards.
        proc::Graph g;
        const int ball = node(g, "icosphere", {{"radius", 2.0}, {"detail", 4}});
        const int nz = node(g, "noise", {{"amplitude", 0.3}, {"size", 1.0}, {"octaves", 4}, {"warp", 0.5}}, {ball});
        g.output = nz;
        const EditMesh a = cookOut(g), b = cookOut(g);
        double most = 0.0, sum = 0.0;
        for (const glm::vec3& v : a.verts) {
            const double d = std::fabs(glm::length(v) - 2.0);
            most = std::max(most, d);
            sum += d;
        }
        check(most <= 0.3 + 1e-4, "noise: within the amplitude", num(most));
        check(sum / static_cast<double>(a.verts.size()) > 0.03, "noise: moves the points",
              "mean " + num(sum / static_cast<double>(a.verts.size())));
        check(a.verts == b.verts, "noise: the same on every cook");
        solid("noise on a ball", a, 5120);
        readProps({{"seed", 2}}, g.find(nz)->props(), g.find(nz));
        check(cookOut(g).verts != a.verts, "noise: another seed, another pattern");
        bool finite = true;
        for (int k = 0; k < static_cast<int>(proc::noise::kindLabels().size()); ++k) {
            readProps({{"kind", k}, {"direction", k % 3}}, g.find(nz)->props(), g.find(nz));
            for (const glm::vec3& v : cookOut(g).verts) finite = finite && std::isfinite(v.x + v.y + v.z);
        }
        check(finite, "noise: every kind and direction gives numbers");
    }
    {
        // Twins: six quads with corners of their own each -- a box whose faces
        // share nothing. Noise along the normals must move the three corners
        // at every box corner as one.
        proc::Geo box;
        const glm::vec3 c[8] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                                {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}};
        const int quads[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {2, 3, 7, 6}, {1, 2, 6, 5}, {0, 4, 7, 3}};
        for (const auto& q : quads) {
            std::vector<int> loop;
            for (int k : q) {
                loop.push_back(static_cast<int>(box.mesh.verts.size()));
                box.mesh.verts.push_back(c[k]);
            }
            box.mesh.faces.push_back(loop);
        }
        std::unique_ptr<proc::Node> nz = proc::make("noise");
        readProps({{"amplitude", 0.4}, {"size", 0.7}}, nz->props(), nz.get());
        proc::Geo out;
        nz->cook({&box}, out);
        int torn = 0;
        for (std::size_t i = 0; i < box.mesh.verts.size(); ++i)
            for (std::size_t j = i + 1; j < box.mesh.verts.size(); ++j)
                if (box.mesh.verts[i] == box.mesh.verts[j] &&
                    glm::length(out.mesh.verts[i] - out.mesh.verts[j]) > 1e-6f) ++torn;
        check(torn == 0, "noise: corners at one place move as one", std::to_string(torn) + " torn apart");
    }
    {
        // Only the selected points: the top half moves, the bottom stays.
        proc::Graph g;
        const int ball = node(g, "icosphere", {{"radius", 2.0}, {"detail", 3}});
        const int top = node(g, "selectpoints", {{"by", 0}, {"center", {0.0, 2.0, 0.0}}, {"size", {10.0, 4.0, 10.0}}}, {ball});
        g.output = node(g, "noise", {{"amplitude", 0.3}}, {top});
        const EditMesh before = proc::cook(g, ball), after = cookOut(g);
        int lowMoved = 0, highMoved = 0;
        for (std::size_t i = 0; i < before.verts.size(); ++i) {
            const bool moved = glm::length(after.verts[i] - before.verts[i]) > 1e-6f;
            if (before.verts[i].y < -1e-3f) lowMoved += moved;
            else if (before.verts[i].y > 1e-3f) highMoved += moved;
        }
        check(lowMoved == 0 && highMoved > 50, "noise: only the selected points",
              std::to_string(lowMoved) + " below moved, " + std::to_string(highMoved) + " above");
        // ...and Select points picks by noise: about half above 0.
        proc::Graph s;
        const int b2 = node(s, "icosphere", {{"radius", 2.0}, {"detail", 4}});
        s.output = node(s, "selectpoints", {{"by", 6}, {"noiseSize", 0.8}, {"threshold", 0.0}}, {b2});
        proc::CookInfo info;
        proc::cookGeo(s, s.output, &info);
        const double share = static_cast<double>(info.selected[s.output]) / 2562.0;
        check(share > 0.25 && share < 0.75, "select points by noise: patches, not all or nothing", num(share));
    }
    {
        // Smooth: a noisy ball comes out evener; keeping the size keeps it,
        // plain Laplace shrinks it. A sheet keeps its outline.
        proc::Graph g;
        const int ball = node(g, "icosphere", {{"radius", 2.0}, {"detail", 4}});
        const int nz = node(g, "noise", {{"amplitude", 0.25}, {"size", 0.4}, {"octaves", 3}}, {ball});
        const int sm = node(g, "smooth", {{"iterations", 8}, {"strength", 0.5}, {"keepSize", true}}, {nz});
        g.output = sm;
        double rough0 = 0.0, rough1 = 0.0;
        const EditMesh noisy = proc::cook(g, nz), kept = cookOut(g);
        meanRadius(noisy, glm::vec3(0.0f), &rough0);
        const double r1 = meanRadius(kept, glm::vec3(0.0f), &rough1);
        check(rough1 < rough0 * 0.8, "smooth: evens the surface out", num(rough0) + " -> " + num(rough1));
        const double v0 = volume(noisy), v1 = volume(kept);
        check(std::fabs(v1 / v0 - 1.0) < 0.05, "smooth: keeping the size keeps the volume", num(v1 / v0));
        readProps({{"keepSize", false}}, g.find(sm)->props(), g.find(sm));
        const EditMesh shrunk = cookOut(g);
        check(volume(shrunk) < v1 * 0.97, "smooth: plain smoothing shrinks", num(volume(shrunk) / v0));
        solid("smooth keeps it closed", kept, 5120);
        (void)r1;
        proc::Graph sh;
        const int grid = node(sh, "grid", {{"sizeX", 4.0}, {"sizeZ", 4.0}, {"cellsX", 16}, {"cellsZ", 16}});
        const int hills = node(sh, "noise", {{"amplitude", 0.5}, {"size", 1.0}, {"direction", 1}}, {grid});
        sh.output = node(sh, "smooth", {{"iterations", 10}}, {hills});
        const EditMesh m0 = proc::cook(sh, hills), m1 = cookOut(sh);
        int rimMoved = 0;
        for (std::size_t i = 0; i < m0.verts.size(); ++i)
            if ((std::fabs(std::fabs(m0.verts[i].x) - 2.0f) < 1e-4f || std::fabs(std::fabs(m0.verts[i].z) - 2.0f) < 1e-4f) &&
                glm::length(m1.verts[i] - m0.verts[i]) > 1e-6f) ++rimMoved;
        check(rimMoved == 0, "smooth: a sheet keeps its outline", std::to_string(rimMoved) + " edge points moved");
    }
    {
        // Facets: a flat base, flat cuts, still closed, smaller; two pieces
        // each cut on their own.
        proc::Graph g;
        const int ball = node(g, "icosphere", {{"radius", 1.0}, {"detail", 4}});
        const int fc = node(g, "facets", {{"cuts", 6}, {"depth", 0.3}, {"unevenness", 0.0},
                                          {"flatBase", true}, {"base", 0.2}}, {ball});
        g.output = fc;
        const EditMesh m = cookOut(g);
        glm::vec3 lo, hi;
        m.bounds(lo, hi);
        int onBase = 0;
        for (const glm::vec3& v : m.verts) onBase += std::fabs(v.y - lo.y) < 1e-4f;
        check(std::fabs(lo.y - (-1.0f + 0.4f)) < 1e-3f, "facets: the base cut where asked", num(lo.y));
        check(onBase > 150, "facets: a flat base to stand on", std::to_string(onBase) + " points on it");
        solid("facets keep it closed", m, 5120);
        check(volume(m) < 4.0 / 3.0 * 3.14159265 * 0.9, "facets: cut away", num(volume(m)));
        proc::Graph two;
        const int b1 = node(two, "icosphere", {{"radius", 1.0}, {"detail", 3}});
        const int b2 = node(two, "icosphere", {{"radius", 0.5}, {"detail", 3}, {"center", {4.0, 1.5, 0.0}}});
        const int mg = node(two, "merge", nlohmann::json::object(), {b1, b2});
        two.output = node(two, "facets", {{"cuts", 0}, {"flatBase", true}, {"base", 0.25}, {"unevenness", 0.0}}, {mg});
        const EditMesh p = cookOut(two);
        float low1 = 1e9f, low2 = 1e9f;
        for (const glm::vec3& v : p.verts) {
            if (v.x < 2.0f) low1 = std::min(low1, v.y);
            else            low2 = std::min(low2, v.y);
        }
        check(std::fabs(low1 - (-0.5f)) < 1e-3f && std::fabs(low2 - 1.25f) < 1e-3f,
              "facets: every piece cut on its own", num(low1) + ", " + num(low2));
    }
    {
        // Copies of different sizes.
        proc::Graph g;
        const int pt = node(g, "grid", {{"sizeX", 10.0}, {"sizeZ", 10.0}, {"cellsX", 4}, {"cellsZ", 4}});
        const int bx = node(g, "box", {{"size", {1.0, 1.0, 1.0}}});
        g.output = node(g, "copytopoints", {{"align", false}, {"scaleVar", 0.5}, {"seed", 3}}, {bx, pt});
        const EditMesh m = cookOut(g);
        float small = 1e9f, big = 0.0f;
        for (std::size_t f = 0; f + 5 < m.faces.size(); f += 6) {
            glm::vec3 lo(1e9f), hi(-1e9f);
            for (std::size_t k = f; k < f + 6; ++k)
                for (int v : m.faces[k]) { lo = glm::min(lo, m.verts[static_cast<std::size_t>(v)]); hi = glm::max(hi, m.verts[static_cast<std::size_t>(v)]); }
            small = std::min(small, hi.x - lo.x);
            big = std::max(big, hi.x - lo.x);
        }
        check(small >= 0.5f - 1e-4f && big <= 1.5f + 1e-4f && big - small > 0.4f, "copy onto points: scale variation",
              num(small) + " .. " + num(big));
    }
    {
        // The painting: every face on the tile, maps of the asked size that
        // are not flat, the same again from the cache, carried through copies
        // and merges, gone after a material over all of it.
        proc::Graph g;
        const int ball = node(g, "icosphere", {{"radius", 1.0}, {"detail", 3}});
        const int nz = node(g, "noise", {{"amplitude", 0.2}, {"size", 1.0}}, {ball});
        const int paint = node(g, "rocktexture", {{"resolution", 0}, {"moss", 0.4}, {"lichen", 0.4}}, {nz});
        g.output = paint;
        const auto t0 = std::chrono::steady_clock::now();
        const proc::Geo a = proc::cookGeo(g, g.output);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const proc::Geo b = proc::cookGeo(g, g.output);
        check(a.bakes.size() == 1 && a.bakes[0].maps && a.bakes[0].maps->w == 256 && a.bakes[0].maps->h == 256,
              "rock texture: one bake, 256 texels a side", num(ms) + " ms");
        if (a.bakes.size() != 1 || !a.bakes[0].maps) return;
        const proc::BakedMaps& mp = *a.bakes[0].maps;
        check(b.bakes.size() == 1 && b.bakes[0].maps == a.bakes[0].maps && b.bakes[0].hash == a.bakes[0].hash,
              "rock texture: the same maps again from the cache");
        int wearing = 0, onTile = 0;
        for (int f = 0; f < static_cast<int>(a.mesh.faces.size()); ++f) {
            wearing += a.mesh.faceMaterial(f) == a.bakes[0].slot;
            bool in = a.mesh.hasCornerUvs(f);
            if (in)
                for (const auto& c : a.mesh.faceUV[static_cast<std::size_t>(f)].corners)
                    in = in && c.uv.x >= 0.0f && c.uv.y >= 0.0f && c.uv.x <= 1.0f && c.uv.y <= 1.0f;
            onTile += in;
        }
        const int faces = static_cast<int>(a.mesh.faces.size());
        check(wearing == faces && onTile == faces, "rock texture: every face wears it, on the tile",
              std::to_string(wearing) + " / " + std::to_string(onTile) + " of " + std::to_string(faces));
        double lum = 0.0, lum2 = 0.0, nz2 = 0.0, rough = 0.0;
        const std::size_t texels = mp.color.size() / 4;
        for (std::size_t i = 0; i < texels; ++i) {
            const double l = (mp.color[i * 4] + mp.color[i * 4 + 1] + mp.color[i * 4 + 2]) / (3.0 * 255.0);
            lum += l;
            lum2 += l * l;
            nz2 += mp.normal[i * 4 + 2] / 255.0;
            rough += mp.orm[i * 4 + 1] / 255.0;
        }
        lum /= static_cast<double>(texels);
        const double spread = std::sqrt(std::max(0.0, lum2 / static_cast<double>(texels) - lum * lum));
        check(lum > 0.15 && lum < 0.85 && spread > 0.03, "rock texture: a colour map with something in it",
              "mean " + num(lum) + ", spread " + num(spread));
        check(nz2 / static_cast<double>(texels) > 0.8, "rock texture: a normal map facing out",
              num(nz2 / static_cast<double>(texels)));
        check(std::fabs(rough / static_cast<double>(texels) - mp.roughness) < 0.05 && mp.roughness > 0.4,
              "rock texture: its roughness and the mean it says", num(mp.roughness));
        // Another look, another bake.
        readProps({{"rock", 3}}, g.find(paint)->props(), g.find(paint));
        const proc::Geo c = proc::cookGeo(g, g.output);
        check(!c.bakes.empty() && c.bakes[0].hash != a.bakes[0].hash, "rock texture: another rock, another bake");
        readProps({{"rock", 0}}, g.find(paint)->props(), g.find(paint));
        // Through copies and a merge.
        const int pts = node(g, "grid", {{"sizeX", 6.0}, {"sizeZ", 6.0}, {"cellsX", 1}, {"cellsZ", 1}});
        const int copies = node(g, "copytopoints", {{"align", false}}, {paint, pts});
        const int extra = node(g, "box", {{"center", {0.0, 5.0, 0.0}}});
        const int all = node(g, "merge", nlohmann::json::object(), {copies, extra});
        const proc::Geo d = proc::cookGeo(g, all);
        check(d.bakes.size() == 1 && d.bakes[0].maps == a.bakes[0].maps, "rock texture: carried through copies and merges");
        const int over = node(g, "material", {{"material", "0123456789abcdef0123456789abcdef"}, {"faces", 0}}, {all});
        check(proc::cookGeo(g, over).bakes.empty(), "rock texture: dropped once no face wears it");
    }
    {
        // The normal map read back the way lit.frag's applyNormalMap reads it
        // -- the file's rows bottom up (a texture asset is flipped on load),
        // the frame built from how position and coordinates run across the
        // face -- must lean the way the height slopes.
        proc::Graph g;
        const int sheet = node(g, "grid", {{"sizeX", 2.0}, {"sizeZ", 2.0}, {"cellsX", 1}, {"cellsZ", 1}});
        g.output = node(g, "rocktexture", {{"rock", 1}, {"moss", 0.0}, {"lichen", 0.0}, {"resolution", 1},
                                           {"bumps", 6.0}, {"cracks", 1.0}}, {sheet});
        const proc::Geo geo = proc::cookGeo(g, g.output);
        if (geo.bakes.empty() || geo.mesh.faces.size() != 1 || !geo.mesh.hasCornerUvs(0)) {
            check(false, "rock texture: a sheet to read the normal map on");
            return;
        }
        const proc::BakedMaps& mp = *geo.bakes[0].maps;
        proc::rock::Look look;
        look.rock = 1; look.moss = 0.0f; look.lichen = 0.0f; look.bumps = 6.0f; look.cracks = 1.0f; look.size = 512;
        const std::vector<int>& fv = geo.mesh.faces[0];
        const auto& uc = geo.mesh.faceUV[0].corners;
        const glm::vec3 N(0.0f, 1.0f, 0.0f);
        double sxx = 0.0, syy = 0.0, sxy = 0.0, err = 0.0;
        int n = 0;
        for (std::size_t k = 1; k + 1 < fv.size(); ++k) {
            const std::size_t c[3] = {0, k, k + 1};
            glm::vec3 P[3];
            glm::vec2 U[3];
            for (int i = 0; i < 3; ++i) {
                P[i] = geo.mesh.verts[static_cast<std::size_t>(fv[c[i]])];
                U[i] = uc[c[i]].uv;
            }
            // lit.frag, word for word, with the face's edges for the screen's derivatives.
            const glm::vec3 dp1 = P[1] - P[0], dp2 = P[2] - P[0];
            const glm::vec2 du1 = U[1] - U[0], du2 = U[2] - U[0];
            const glm::vec3 dp2p = glm::cross(dp2, N), dp1p = glm::cross(N, dp1);
            const glm::vec3 T = dp2p * du1.x + dp1p * du2.x;
            const glm::vec3 B = dp2p * du1.y + dp1p * du2.y;
            const float inv = 1.0f / std::sqrt(std::max(glm::dot(T, T), glm::dot(B, B)));
            const float e = glm::length(dp1) / (glm::length(du1) * static_cast<float>(mp.w));   // a texel, in metres
            for (int y = 0; y < mp.h; y += 3)
                for (int x = 0; x < mp.w; x += 3) {
                    const glm::vec2 q((static_cast<float>(x) + 0.5f) / static_cast<float>(mp.w),
                                      (static_cast<float>(y) + 0.5f) / static_cast<float>(mp.h));
                    const float den = (U[1].y - U[2].y) * (U[0].x - U[2].x) + (U[2].x - U[1].x) * (U[0].y - U[2].y);
                    const float l0 = ((U[1].y - U[2].y) * (q.x - U[2].x) + (U[2].x - U[1].x) * (q.y - U[2].y)) / den;
                    const float l1 = ((U[2].y - U[0].y) * (q.x - U[2].x) + (U[0].x - U[2].x) * (q.y - U[2].y)) / den;
                    const float l2 = 1.0f - l0 - l1;
                    if (l0 < 0.03f || l1 < 0.03f || l2 < 0.03f) continue;
                    const glm::vec3 at = P[0] * l0 + P[1] * l1 + P[2] * l2;
                    const std::size_t px = (static_cast<std::size_t>(mp.h - 1 - y) * static_cast<std::size_t>(mp.w) + static_cast<std::size_t>(x)) * 4;
                    const glm::vec3 nt(mp.normal[px] / 127.5f - 1.0f, mp.normal[px + 1] / 127.5f - 1.0f,
                                       mp.normal[px + 2] / 127.5f - 1.0f);
                    const glm::vec3 got = glm::normalize(T * inv * nt.x + B * inv * nt.y + N * nt.z);
                    auto h = [&](const glm::vec3& o) { return proc::rock::heightAt(look, at + o, N, 0.0f, 0.0f); };
                    const float gx = (h({e, 0.0f, 0.0f}) - h({-e, 0.0f, 0.0f})) / (2.0f * e);
                    const float gz = (h({0.0f, 0.0f, e}) - h({0.0f, 0.0f, -e})) / (2.0f * e);
                    const glm::vec3 want = glm::normalize(N - 6.0f * glm::vec3(gx, 0.0f, gz));
                    sxx += static_cast<double>(got.x) * want.x + static_cast<double>(got.z) * want.z;
                    syy += static_cast<double>(want.x) * want.x + static_cast<double>(want.z) * want.z;
                    sxy += static_cast<double>(got.x) * got.x + static_cast<double>(got.z) * got.z;
                    err += std::acos(std::clamp(static_cast<double>(glm::dot(got, want)), -1.0, 1.0));
                    ++n;
                }
        }
        const double corr = sxx / std::sqrt(std::max(syy * sxy, 1e-30));
        check(n > 1000 && corr > 0.85, "rock texture: the normal map leans the way the height slopes, as the shader reads it",
              "correlation " + num(corr) + ", mean error " + num(glm::degrees(err / std::max(n, 1))) + " deg over " +
                  std::to_string(n) + " texels");
    }
}

// --- Furniture ---------------------------------------------------------------------------
// Every piece, at its defaults and with every pair of its choices and switches
// set every way: each must cook, come out closed and wound outward (every
// solid it is built of is), be all numbers, and stand on the floor. Then the
// sizes asked for are the sizes made, and Position and Turn put it there.

void furniture() {
    std::printf("Furniture\n");
    const char* kinds[] = {"table", "chair", "sofa", "bed", "cabinet", "shelf", "rug", "lamp", "sanitary", "appliance"};
    auto bounds = [](const EditMesh& m, glm::vec3& mn, glm::vec3& mx) {
        mn = glm::vec3(1e9f);
        mx = glm::vec3(-1e9f);
        for (const auto& f : m.faces)
            for (int v : f) { mn = glm::min(mn, m.verts[v]); mx = glm::max(mx, m.verts[v]); }
    };
    // What a variant must be, whatever it looks like. "" or what is wrong.
    auto sound = [&](const proc::Graph& g, bool standing) -> std::string {
        proc::CookInfo info;
        const EditMesh m = cookOut(g, &info);
        if (!info.errors.empty()) return info.errors.begin()->second;
        if (m.faces.empty()) return "nothing made";
        for (const glm::vec3& v : m.verts)
            if (!std::isfinite(v.x + v.y + v.z)) return "a corner is not a number";
        const Topo t = topo(m);
        if (t.open || t.twisted)
            return "open " + std::to_string(t.open) + ", twisted " + std::to_string(t.twisted);
        if (volume(m) <= 0.0) return "wound inward";
        glm::vec3 mn, mx;
        bounds(m, mn, mx);
        if (mn.y < -1e-4f) return "below the floor (" + num(mn.y) + ")";
        if (standing && mn.y > 1e-3f) return "floats (" + num(mn.y) + ")";
        return "";
    };
    for (const char* kind : kinds) {
        std::unique_ptr<proc::Node> probe = proc::make(kind);
        check(probe != nullptr, std::string(kind) + ": the kind exists");
        if (!probe) continue;
        const proc::TypeInfo* ti = proc::typeInfo(kind);
        check(ti && ti->category == "Furniture", std::string(kind) + ": listed under Furniture");
        {
            proc::Graph g;
            g.output = node(g, kind);
            const std::string why = sound(g, std::string(kind) != "lamp");
            check(why.empty(), std::string(kind) + ": its defaults make a closed, outward piece on the floor", why);
        }
        // Every value of every choice and switch, two at a time.
        struct Axis { std::string key; int values; bool flag; };
        std::vector<Axis> axes;
        for (const Property& p : probe->props()) {
            if (p.kind == PropKind::EnumInt) axes.push_back({p.key, static_cast<int>(p.enumLabels.size()), false});
            else if (p.kind == PropKind::Bool) axes.push_back({p.key, 2, true});
        }
        int variants = 0, bad = 0;
        std::string first;
        auto value = [](const Axis& a, int v) { return a.flag ? nlohmann::json(v != 0) : nlohmann::json(v); };
        for (std::size_t i = 0; i < axes.size(); ++i)
            for (std::size_t j = i + 1; j < axes.size(); ++j)
                for (int a = 0; a < axes[i].values; ++a)
                    for (int b = 0; b < axes[j].values; ++b) {
                        const nlohmann::json set = {{axes[i].key, value(axes[i], a)}, {axes[j].key, value(axes[j], b)}};
                        proc::Graph g;
                        g.output = node(g, kind, set);
                        // A pendant hangs, a wall unit hangs on the wall.
                        const bool hangs = (std::string(kind) == "lamp") ||
                                           (std::string(kind) == "shelf" && set.dump().find("\"style\":2") != std::string::npos) ||
                                           (std::string(kind) == "cabinet" && set.dump().find("\"base\":2") != std::string::npos) ||
                                           (std::string(kind) == "sanitary" && set.dump().find("\"stand\":2") != std::string::npos) ||
                                           (std::string(kind) == "appliance" && (set.dump().find("\"kind\":3") != std::string::npos ||
                                                                                 set.dump().find("\"kind\":4") != std::string::npos));
                        const std::string why = sound(g, !hangs);
                        ++variants;
                        if (!why.empty()) {
                            ++bad;
                            if (first.empty()) first = set.dump() + ": " + why;
                        }
                    }
        check(bad == 0, std::string(kind) + ": every pair of choices sound",
              std::to_string(variants) + " variants" + (bad ? ", " + std::to_string(bad) + " bad, e.g. " + first : ""));
    }

    // The sizes asked for are the sizes made.
    auto extent = [&](const char* kind, const nlohmann::json& set, glm::vec3& mn, glm::vec3& mx) {
        proc::Graph g;
        g.output = node(g, kind, set);
        bounds(cookOut(g), mn, mx);
    };
    auto near = [](float a, float b, float tol = 2e-3f) { return std::fabs(a - b) <= tol; };
    glm::vec3 mn, mx;
    extent("table", {{"width", 1.8}, {"depth", 0.9}, {"height", 0.75}}, mn, mx);
    check(near(mx.x - mn.x, 1.8f) && near(mx.z - mn.z, 0.9f) && near(mx.y, 0.75f) && near(mn.y, 0.0f),
          "table: 1.80 x 0.90, 0.75 high", num(mx.x - mn.x) + " x " + num(mx.z - mn.z) + ", " + num(mx.y));
    extent("table", {{"shape", 2}, {"width", 1.2}, {"base", 2}}, mn, mx);
    check(near(mx.x - mn.x, 1.2f, 0.01f) && near(mx.z - mn.z, 1.2f, 0.01f), "round table: as wide as it is deep",
          num(mx.x - mn.x) + " x " + num(mx.z - mn.z));
    extent("cabinet", {{"width", 2.4}, {"height", 0.9}, {"depth", 0.6}, {"worktop", true}, {"overhang", 0.02},
                       {"handles", 0}}, mn, mx);
    check(near(mx.x - mn.x, 2.4f) && near(mx.y, 0.9f) && near(mx.z - mn.z, 0.62f),
          "kitchen base: 2.40 wide, 0.90 to the worktop, 0.60 deep plus its overhang",
          num(mx.x - mn.x) + ", " + num(mx.y) + ", " + num(mx.z - mn.z));
    extent("cabinet", {{"base", 2}, {"baseHeight", 1.45}, {"height", 2.2}}, mn, mx);
    check(near(mn.y, 1.45f) && near(mx.y, 2.2f), "wall unit: hangs from 1.45 to 2.20", num(mn.y) + " .. " + num(mx.y));
    extent("sofa", {{"width", 2.4}, {"height", 0.85}, {"pillows", 0}}, mn, mx);
    check(near(mx.x - mn.x, 2.4f) && mx.y <= 0.85f + 1e-3f, "sofa: 2.40 wide, no higher than asked",
          num(mx.x - mn.x) + ", " + num(mx.y));
    extent("sofa", {{"depth", 0.9}, {"chaise", 2}, {"chaiseDepth", 1.6}}, mn, mx);
    check(near(mx.z - mn.z, 1.6f, 0.02f) && mn.z < -0.44f, "chaise: runs forward to 1.60 from the back",
          num(mn.z) + " .. " + num(mx.z));
    extent("bed", {{"width", 1.6}, {"length", 2.0}, {"rim", 0.04}, {"headboard", 0}, {"bedding", 0}, {"pillows", 0}},
           mn, mx);
    check(near(mx.x - mn.x, 1.68f) && near(mx.z - mn.z, 2.08f), "bed 160 x 200: the frame round the mattress",
          num(mx.x - mn.x) + " x " + num(mx.z - mn.z));
    extent("shelf", {{"width", 0.8}, {"height", 1.9}, {"depth", 0.3}, {"books", 0.0}}, mn, mx);
    check(near(mx.x - mn.x, 0.8f) && near(mx.y, 1.9f) && near(mx.z - mn.z, 0.3f), "bookcase: 0.80 x 1.90 x 0.30");
    {
        proc::Graph a, b;
        a.output = node(a, "shelf", {{"books", 0.0}});
        b.output = node(b, "shelf", {{"books", 0.8}});
        const std::size_t bare = cookOut(a).faces.size(), full = cookOut(b).faces.size();
        check(full > bare + 6 * 40, "books fill the boards", std::to_string((full - bare) / 6) + " books or so");
        proc::Graph c;
        c.output = node(c, "shelf", {{"books", 0.8}, {"seed", 2}});
        check(cookOut(c).faces.size() != full || cookOut(c).verts != cookOut(b).verts,
              "another seed, other books");
    }
    // Position and Turn: a table turned a quarter stands across, where it was put.
    extent("table", {{"width", 1.8}, {"depth", 0.9}, {"turn", 90.0}, {"at", {3.0, 0.0, -2.0}}}, mn, mx);
    check(near(mx.x - mn.x, 0.9f) && near(mx.z - mn.z, 1.8f) && near(0.5f * (mn.x + mx.x), 3.0f) &&
              near(0.5f * (mn.z + mx.z), -2.0f),
          "Turn 90 stands it across, Position moves it", num(0.5f * (mn.x + mx.x)) + ", " + num(0.5f * (mn.z + mx.z)));
    // Front toward +Z: a chair's back is behind it.
    {
        proc::Graph g;
        g.output = node(g, "chair", {{"back", 3}});
        const EditMesh m = cookOut(g);
        float zTop = 0.0f, yTop = -1.0f;
        for (const glm::vec3& v : m.verts) if (v.y > yTop) { yTop = v.y; zTop = v.z; }
        check(zTop < -0.1f, "chair: the back is at the back (-Z), the front faces +Z", num(zTop));
    }
}

// --- Interiors --------------------------------------------------------------------------
// What a furnished building must be, whatever it looks like. Every piece stands
// inside its room, on its floor; no two share floor; none stands on the floor
// kept in front of another; none blocks a door's swing or a stair, or stands
// taller than the sill in front of a window; the same seed gives the same room.

struct Foot2 {
    glm::vec2 c, x, z, half;
    std::array<glm::vec2, 4> corners() const {
        return {c - x * half.x - z * half.y, c + x * half.x - z * half.y, c + x * half.x + z * half.y, c - x * half.x + z * half.y};
    }
};
Foot2 footOf(const proc::FurnishedPiece& p) {
    const float r = glm::radians(p.turn);
    return {p.center, {std::cos(r), -std::sin(r)}, {std::sin(r), std::cos(r)}, p.half};
}
Foot2 frontOf(const proc::FurnishedPiece& p) {
    Foot2 f = footOf(p);
    f.c += f.z * (p.half.y + 0.5f * p.clear);
    f.half.y = 0.5f * p.clear;
    return f;
}
bool crosses(const Foot2& a, const Foot2& b) {
    const auto ca = a.corners(), cb = b.corners();
    for (const glm::vec2 ax : {a.x, a.z, b.x, b.z}) {
        float a0 = 1e30f, a1 = -1e30f, b0 = 1e30f, b1 = -1e30f;
        for (const glm::vec2& p : ca) { a0 = std::min(a0, glm::dot(p, ax)); a1 = std::max(a1, glm::dot(p, ax)); }
        for (const glm::vec2& p : cb) { b0 = std::min(b0, glm::dot(p, ax)); b1 = std::max(b1, glm::dot(p, ax)); }
        if (a1 <= b0 + 2e-3f || b1 <= a0 + 2e-3f) return false;
    }
    return true;
}
bool inOutline(const std::vector<glm::vec3>& poly, const glm::vec2& q) {
    bool in = false;
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
        if ((poly[i].z > q.y) != (poly[j].z > q.y) &&
            q.x < (poly[j].x - poly[i].x) * (q.y - poly[i].z) / (poly[j].z - poly[i].z) + poly[i].x)
            in = !in;
    return in;
}
bool isRoomLine(const proc::Curve& c) {
    if (!c.closed || c.loose || c.pts.size() < 3) return false;
    if (c.tag.empty()) return true;
    for (const proc::RoomKind& k : proc::roomKinds()) if (c.tag == k.id) return true;
    return false;
}

// Every rule above on one stream. How many pieces it placed comes back.
std::size_t soundRooms(const std::string& what, const proc::Geo& geo, int seed = 1) {
    proc::FurnishSettings set;
    set.seed = seed;
    const std::vector<proc::FurnishedPiece> plan = proc::furnishPlan(geo, set);
    std::vector<const proc::Curve*> rooms;
    for (const proc::Curve& c : geo.curves) if (isRoomLine(c)) rooms.push_back(&c);
    int outside = 0, clash = 0, frontBlocked = 0, doorBlocked = 0, stairBlocked = 0, windowBlocked = 0;
    std::string first;
    auto note = [&](const std::string& s) { if (first.empty()) first = s; };
    for (std::size_t i = 0; i < plan.size(); ++i) {
        const proc::FurnishedPiece& p = plan[i];
        const Foot2 f = footOf(p);
        const std::vector<glm::vec3>& poly = rooms[static_cast<std::size_t>(p.room)]->pts;
        for (const glm::vec2& q : f.corners())
            if (!inOutline(poly, q)) { ++outside; note(p.kind + "/" + p.what + " outside its room"); break; }
        for (std::size_t j = 0; j < plan.size(); ++j) {
            const proc::FurnishedPiece& o = plan[j];
            if (i == j || o.room != p.room) continue;
            if (j > i && crosses(f, footOf(o))) { ++clash; note(p.kind + ": " + p.what + " in " + o.what); }
            if (o.clear > 0.01f && crosses(f, frontOf(o))) { ++frontBlocked; note(p.kind + ": " + p.what + " in front of " + o.what); }
        }
        for (const proc::Curve& m : geo.curves) {
            if (m.tag != "door" && m.tag != "window" && m.tag != "stair") continue;
            glm::vec3 mn(1e30f), mx(-1e30f);
            for (const glm::vec3& v : m.pts) { mn = glm::min(mn, v); mx = glm::max(mx, v); }
            // Floor to ceiling of this storey: not the door right above, nor the head of the window below.
            if (mx.y < p.floor + 0.2f || mn.y > p.floor + 2.4f) continue;
            if (m.tag == "stair") {
                if (std::fabs(mn.y - p.floor) > 0.3f) continue;
                const Foot2 z{0.5f * glm::vec2(mn.x + mx.x, mn.z + mx.z), {1, 0}, {0, 1}, 0.5f * glm::vec2(mx.x - mn.x, mx.z - mn.z)};
                if (crosses(f, z)) { ++stairBlocked; note(p.kind + "/" + p.what + " on the stair"); }
                continue;
            }
            // The opening itself, a little proud into the room both ways.
            const glm::vec2 c = 0.5f * glm::vec2(mn.x + mx.x, mn.z + mx.z);
            const glm::vec2 run = glm::vec2(mx.x - mn.x, mx.z - mn.z);
            const bool alongX = run.x > run.y;
            const float w = 0.5f * std::max(run.x, run.y);
            const Foot2 z{c, {1, 0}, {0, 1}, alongX ? glm::vec2(w, 0.25f) : glm::vec2(0.25f, w)};
            if (!crosses(f, z)) continue;
            const float sill = mn.y - p.floor;
            if (m.tag == "door" || sill < 0.2f) { ++doorBlocked; note(p.kind + "/" + p.what + " in a door"); }
            else if (p.height > sill + 0.01f) { ++windowBlocked; note(p.kind + "/" + p.what + " before a window"); }
        }
    }
    check(outside == 0, what + ": every piece inside its room", std::to_string(plan.size()) + " pieces" + (first.empty() ? "" : ", e.g. " + first));
    check(clash == 0, what + ": no two pieces on the same floor", std::to_string(clash));
    check(frontBlocked == 0, what + ": nothing on the floor kept in front", std::to_string(frontBlocked));
    check(doorBlocked == 0, what + ": no door blocked", std::to_string(doorBlocked));
    check(stairBlocked == 0, what + ": no stair blocked", std::to_string(stairBlocked));
    check(windowBlocked == 0, what + ": nothing taller than the sill before a window", std::to_string(windowBlocked));
    return plan.size();
}

void interiors() {
    std::printf("Interiors\n");
    // A drawn room, of every kind: nothing but a Rectangle and a Furnish node.
    const std::vector<std::pair<const char*, const char*>> mains = {
        {"living", "sofa"}, {"dining", "table"}, {"kitchen", "kitchen run"}, {"bath", "toilet"}, {"wc", "toilet"},
        {"bedroom", "bed"}, {"child", "bed"}, {"office", "desk"}, {"guest", "bed"}, {"utility", "washing machine"},
        {"storage", "rack"}, {"cloakroom", "wardrobe"}, {"dressing", "wardrobe"}, {"shop", "rack"}, {"meeting", "table"}};
    const auto& kinds = proc::roomKinds();
    for (const auto& [id, main] : mains) {
        int k = 0;
        for (int i = 0; i < static_cast<int>(kinds.size()); ++i) if (std::string(kinds[static_cast<std::size_t>(i)].id) == id) k = i;
        const bool wet = std::string(id) == "wc";
        const bool huge = std::string(id) == "shop" || std::string(id) == "meeting";
        proc::Graph g;
        const int r = node(g, "rect", {{"sizeX", wet ? 1.4 : huge ? 9.0 : 4.6}, {"sizeZ", wet ? 2.0 : huge ? 7.0 : 3.8}, {"filled", false}});
        g.output = node(g, "furnish", {{"kind", k}}, {r});
        proc::CookInfo info;
        const proc::Geo out = proc::cookGeo(g, g.output, &info);
        check(info.errors.empty(), std::string(id) + ": cooks");
        proc::FurnishSettings set;
        set.defaultKind = k;
        const std::vector<proc::FurnishedPiece> plan = proc::furnishPlan(proc::cookGeo(g, r), set);
        bool found = false;
        std::string names;
        for (const proc::FurnishedPiece& p : plan) { found = found || p.what == main; names += p.what + " "; }
        check(found && !out.mesh.faces.empty(), std::string(id) + ": has its " + main, names);
        for (const glm::vec3& v : out.mesh.verts)
            if (!std::isfinite(v.x + v.y + v.z)) { check(false, std::string(id) + ": every corner a number"); break; }
    }
    // A door in the way: the swing stays free in every seed.
    {
        proc::Geo g;
        proc::Curve room;
        room.pts = {{0, 0, 0}, {5, 0, 0}, {5, 0, 4}, {0, 0, 4}};
        room.closed = true;
        room.tag = "bedroom";
        g.curves.push_back(room);
        proc::Curve door;
        door.pts = {{2, 0, 0}, {3, 0, 0}, {3, 2.1f, 0}, {2, 2.1f, 0}};
        door.closed = true;
        door.tag = "door";
        g.curves.push_back(door);
        proc::Curve window;
        window.pts = {{1, 0.9f, 4}, {4, 0.9f, 4}, {4, 2.2f, 4}, {1, 2.2f, 4}};
        window.closed = true;
        window.tag = "window";
        g.curves.push_back(window);
        std::size_t n = 0;
        for (int seed = 1; seed <= 8; ++seed) n += soundRooms("bedroom, seed " + std::to_string(seed), g, seed);
        check(n >= 8, "a bedroom with a door and a window still gets furnished", std::to_string(n) + " pieces in 8 seeds");
        proc::FurnishSettings a;
        const auto p1 = proc::furnishPlan(g, a), p2 = proc::furnishPlan(g, a);
        bool same = p1.size() == p2.size();
        for (std::size_t i = 0; same && i < p1.size(); ++i) same = p1[i].center == p2[i].center && p1[i].what == p2[i].what;
        check(same, "the same seed gives the same room");
    }
    // The marks survive what a stream goes through.
    {
        proc::Graph g;
        const int r = node(g, "rect", {{"sizeX", 4.0}, {"sizeZ", 3.0}, {"filled", false}});
        const int fur = node(g, "furnish", {{"kind", 6}}, {r});
        const int moved = node(g, "transform", {{"move", {10.0, 0.0, 0.0}}}, {fur});
        const proc::Geo out = proc::cookGeo(g, moved);
        glm::vec3 mn, mx;
        out.mesh.bounds(mn, mx);
        check(mn.x > 7.0f, "Transform moves the furniture with the room", num(mn.x));
    }
    // Facade marks: every opening and every storey line, and off when asked.
    {
        proc::Graph g;
        int b0 = node(g, "rect", {{"sizeX", 12.0}, {"sizeZ", 9.0}, {"filled", true}});
        b0 = node(g, "extrude", {{"faces", 0}, {"distance", 10.4}}, {b0});
        const int fac = node(g, "facade", {{"ledge", 0.0}}, {b0});
        const proc::Geo marked = proc::cookGeo(g, fac);
        int doors = 0, windows = 0, storeys = 0;
        for (const proc::Curve& c : marked.curves) {
            doors += c.tag == "door";
            windows += c.tag == "window";
            storeys += c.tag == "storey";
        }
        check(doors == 4 && windows > 20 && storeys == 16, "Facade marks its openings and storeys",
              std::to_string(doors) + " doors, " + std::to_string(windows) + " windows, " + std::to_string(storeys) + " lines");
        const int off = node(g, "facade", {{"ledge", 0.0}, {"mark", false}}, {b0});
        check(proc::cookGeo(g, off).curves.empty(), "Mark off: no lines");
    }
    // Rooms: a four-storey block made hollow.
    {
        proc::Graph g;
        int b0 = node(g, "rect", {{"sizeX", 18.0}, {"sizeZ", 11.0}, {"filled", true}});
        b0 = node(g, "extrude", {{"faces", 0}, {"distance", 13.6}}, {b0});
        b0 = node(g, "roof", {{"shape", 2}, {"pitch", 35.0}, {"overhang", 0.5}}, {b0});
        const int fac = node(g, "facade", {{"ground", 2}}, {b0});
        const int rooms = node(g, "rooms", {}, {fac});
        proc::CookInfo info;
        const proc::Geo out = proc::cookGeo(g, rooms, &info);
        check(info.errors.empty(), "Rooms: cooks", info.errors.empty() ? "" : info.errors.begin()->second);
        std::map<int, int> perFloor;
        int stairs = 0, doors = 0, storeyLeft = 0, halls = 0;
        std::size_t roomLines = 0;
        for (const proc::Curve& c : out.curves) {
            stairs += c.tag == "stair";
            doors += c.tag == "door";
            storeyLeft += c.tag == "storey";
            if (!isRoomLine(c)) continue;
            ++roomLines;
            ++perFloor[static_cast<int>(std::lround(c.pts[0].y * 10.0f))];
            halls += c.tag == "hall";
            check(c.height > 2.0f, "a room has its height", num(c.height));
            for (const glm::vec3& p : c.pts)
                if (std::fabs(p.x) > 9.0f || std::fabs(p.z) > 5.5f) check(false, "a room outside the block");
        }
        check(perFloor.size() == 4, "Rooms: four storeys of rooms, none in the roof", std::to_string(perFloor.size()));
        check(stairs == 4, "Rooms: the stair on every floor", std::to_string(stairs));
        check(storeyLeft == 0, "Rooms: the facade's storey lines are used up", std::to_string(storeyLeft));
        check(doors >= static_cast<int>(roomLines) - 4, "Rooms: doors to the rooms", std::to_string(doors) + " doors, " + std::to_string(roomLines) + " rooms");
        check(halls >= 4, "Rooms: a stair hall on every floor, and the street door's room", std::to_string(halls));
        // The way in: the street door's faces are gone, the windows' are not.
        const proc::Geo solid = proc::cookGeo(g, fac);
        int streetDoors = 0;
        for (const proc::Curve& c : solid.curves) streetDoors += c.tag == "door";
        check(streetDoors == 4, "the block has its street doors", std::to_string(streetDoors));
        {
            proc::Graph sh;
            int b1 = node(sh, "rect", {{"sizeX", 10.0}, {"sizeZ", 9.0}, {"filled", true}});
            b1 = node(sh, "extrude", {{"faces", 0}, {"distance", 7.2}}, {b1});
            const int f4 = node(sh, "facade", {{"ground", 4}}, {b1});
            int shopDoors = 0, shopWindows = 0;
            for (const proc::Curve& c : proc::cookGeo(sh, f4).curves) {
                shopDoors += c.tag == "door";
                shopWindows += c.tag == "window" && c.pts[0].y < 1.0f;
            }
            check(shopDoors == 4 && shopWindows > 4, "Facade: shop windows with a door in the middle",
                  std::to_string(shopDoors) + " doors, " + std::to_string(shopWindows) + " shop windows");
        }
        check(out.mesh.faces.size() > solid.mesh.faces.size(), "Rooms: more faces inside than it took away",
              std::to_string(solid.mesh.faces.size()) + " -> " + std::to_string(out.mesh.faces.size()));
        // Reaching every room from the stair: through its doors, along the corridor.
        std::size_t reachedNone = 0;
        for (const proc::Curve& c : out.curves) {
            if (!isRoomLine(c) || c.tag == "corridor" || c.tag == "hall") continue;
            bool door = false;
            for (const proc::Curve& d : out.curves) {
                if (d.tag != "door") continue;
                glm::vec3 m(0.0f);
                for (const glm::vec3& v : d.pts) m += v * 0.25f;
                if (m.y < c.pts[0].y || m.y > c.pts[0].y + 3.5f) continue;
                glm::vec3 mn(1e30f), mx(-1e30f);
                for (const glm::vec3& v : c.pts) { mn = glm::min(mn, v); mx = glm::max(mx, v); }
                if (m.x > mn.x - 0.2f && m.x < mx.x + 0.2f && m.z > mn.z - 0.2f && m.z < mx.z + 0.2f) door = true;
            }
            reachedNone += door ? 0 : 1;
        }
        check(reachedNone == 0, "Rooms: every room has a door", std::to_string(reachedNone) + " have none");
        const std::size_t placed = soundRooms("flats", out);
        check(placed > roomLines, "Rooms: Furnish fills the flats", std::to_string(placed) + " pieces in " + std::to_string(roomLines) + " rooms");
        const int fur = node(g, "furnish", {}, {rooms});
        proc::CookInfo fi;
        const auto t0 = std::chrono::steady_clock::now();
        const proc::Geo full = proc::cookGeo(g, fur, &fi);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        check(fi.errors.empty(), "Furnish over Rooms: cooks", std::to_string(full.mesh.faces.size()) + " faces, " + num(ms) + " ms");
    }
    // A block without storey marks, an octagon, one that is too small for a stair.
    {
        proc::Graph g;
        int a = node(g, "rect", {{"sizeX", 8.0}, {"sizeZ", 7.0}, {"filled", true}});
        a = node(g, "extrude", {{"faces", 0}, {"distance", 6.6}}, {a});
        const int ra = node(g, "rooms", {}, {a});
        proc::CookInfo ia;
        const proc::Geo oa = proc::cookGeo(g, ra, &ia);
        check(ia.errors.empty(), "Rooms on a bare block: cooks");
        std::set<int> floors;
        for (const proc::Curve& c : oa.curves) if (isRoomLine(c)) floors.insert(static_cast<int>(std::lround(c.pts[0].y * 10.0f)));
        check(floors.size() == 2, "Rooms on a bare block: storeys from its own setting", std::to_string(floors.size()));
        const int oct = node(g, "tube", {{"radius", 5.0}, {"radiusTop", 5.0}, {"length", 8.0}, {"segments", 8}, {"center", {0.0, 4.0, 0.0}}});
        const int fo = node(g, "facade", {{"ground", 0}, {"ledge", 0.0}}, {oct});
        const int ro = node(g, "rooms", {}, {fo});
        proc::CookInfo io;
        const proc::Geo oo = proc::cookGeo(g, ro, &io);
        check(io.errors.empty(), "Rooms on an octagon: cooks", io.errors.empty() ? "" : io.errors.begin()->second);
        std::size_t lines = 0;
        for (const proc::Curve& c : oo.curves) lines += isRoomLine(c);
        check(lines >= 2, "Rooms on an octagon: an open room a floor", std::to_string(lines));
        soundRooms("octagon", oo);
    }
    // Floor plan: HouseGen's plan comes through whole.
    for (int type = 0; type < 9; ++type) {
        proc::Graph g;
        const int plan = node(g, "floorplan", {{"houseType", type}});
        proc::CookInfo info;
        const proc::Geo out = proc::cookGeo(g, plan, &info);
        std::size_t rooms = 0;
        for (const proc::Curve& c : out.curves) rooms += isRoomLine(c);
        check(info.errors.empty() && rooms >= 3 && !out.mesh.faces.empty(), "Floor plan " + std::to_string(type) + ": a house with rooms",
              std::to_string(rooms) + " rooms, " + std::to_string(out.mesh.faces.size()) + " faces");
        soundRooms("Floor plan " + std::to_string(type), out);
    }
    {
        proc::Graph g;
        const int plan = node(g, "floorplan", {{"programme", "kitchen 14, living 30 | bedroom 16, bath 8, child 12"}, {"attic", 2}});
        const proc::Geo out = proc::cookGeo(g, plan);
        int kitchen = 0, bedroom = 0;
        for (const proc::Curve& c : out.curves) { kitchen += c.tag == "kitchen"; bedroom += c.tag == "bedroom"; }
        check(kitchen == 1 && bedroom == 1, "Floor plan: the programme decides the rooms", std::to_string(kitchen) + ", " + std::to_string(bedroom));
        const int bad = node(g, "floorplan", {{"programme", "ballroom"}});
        proc::CookInfo info;
        proc::cookGeo(g, bad, &info);
        check(info.errors.count(bad) == 1, "Floor plan: a room nobody knows is said");
    }
}

// --- A project for viewcheck ------------------------------------------------------------

// `only` (a preset's name) writes that preset alone, at the origin, and `cut` (a
// level, with a Floor plan) takes the roof and everything over that level off.
int writeScene(const std::string& dir, const std::string& only = "", int cut = -1) {
    namespace fs = std::filesystem;
    fs::create_directories(dir);
    std::vector<MaterialDef> mats;
    nlohmann::json ents = nlohmann::json::array();
    float x = 0.0f;
    int id = 1;
    std::string worldShift;
    // "Rocks": the Boulder once in every kind of rock, side by side.
    struct Job { std::string name; proc::Graph graph; };
    std::vector<Job> jobs;
    for (int i = 0; i < static_cast<int>(procpreset::list().size()) - 1; ++i) {
        const std::string name = procpreset::list()[static_cast<std::size_t>(i)].name;
        if (only == "Rocks" && name == "Boulder") {
            for (int r = 0; r < static_cast<int>(proc::rock::rockLabels().size()); ++r) {
                proc::Graph g = procpreset::build(i, mats);
                if (proc::Parm* p = g.parm("rock_type")) p->value.x = static_cast<float>(r);
                if (proc::Parm* p = g.parm("variant")) p->value.x = static_cast<float>(r + 1);
                jobs.push_back({proc::rock::rockLabels()[static_cast<std::size_t>(r)], std::move(g)});
            }
            continue;
        }
        if (!only.empty() && only != name) continue;
        jobs.push_back({name, procpreset::build(i, mats)});
    }
    for (Job& job : jobs) {
        ProcGraphComponent pc;
        pc.graph = std::move(job.graph);
        if (cut >= 0)
            for (const auto& n : pc.graph.nodes)
                if (std::string(n->typeId()) == "floorplan")
                    readProps({{"topLevel", cut}, {"roof", false}}, n->props(), n.get());
        const proc::Geo geo = proc::cookGeo(pc.graph, pc.graph.output);
        EditMesh m = proc::facesOf(geo);
        // Painted textures: written out as the editor writes them, a material
        // of their own each, so viewcheck shows the maps.
        for (const proc::Bake& b : geo.bakes) {
            if (!b.maps) continue;
            const std::string stem = job.name + "_" + b.key;
            std::string safe;
            for (char ch : stem) safe.push_back(std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_');
            fs::create_directories(fs::path(dir) / "textures" / "procedural");
            std::string ids[3];
            const std::pair<const char*, const std::vector<std::uint8_t>*> files[] = {
                {"_color.png", &b.maps->color}, {"_normal.png", &b.maps->normal}, {"_orm.png", &b.maps->orm}};
            for (int k = 0; k < 3; ++k) {
                const fs::path f = fs::path(dir) / "textures" / "procedural" / (safe + files[k].first);
                stbi_write_png(f.string().c_str(), b.maps->w, b.maps->h, 4, files[k].second->data(), b.maps->w * 4);
                ids[k] = fitzel::AssetId::generate().toString();
                std::ofstream(f.string() + ".meta") << nlohmann::json{{"guid", ids[k]}, {"type", "Texture"}}.dump(2);
            }
            MaterialDef md;
            md.assetId   = fitzel::AssetId::generate();
            md.name      = stem;
            md.roughness = b.maps->roughness;
            md.texId       = fitzel::AssetId::fromString(ids[0]);
            md.normalTexId = fitzel::AssetId::fromString(ids[1]);
            md.ormTexId    = fitzel::AssetId::fromString(ids[2]);
            md.ormMean     = glm::vec2(b.maps->roughness, 1.0f);
            for (fitzel::AssetId& f : m.faceMat)
                if (f == b.slot) f = md.assetId;
            mats.push_back(md);
        }
        pc.pivot = editmesh::recenter(m);
        glm::vec3 mn, mx;
        m.bounds(mn, mx);
        const glm::vec3 h = 0.5f * (mx - mn);
        MeshComponent mc;
        mc.mesh = std::move(m);
        nlohmann::json cm, cs, cp;
        mc.save(cm);
        cm["type"] = "mesh";
        cs = {{"type", "modifiers"}, {"smooth", true}, {"smoothAngle", 40.0}, {"stack", nlohmann::json::array()}};
        pc.save(cp);
        cp["type"] = "procgraph";
        x += h.x;
        worldShift = num(x - pc.pivot.x) + " " + num(h.y + 2.0f - pc.pivot.y) + " " + num(-pc.pivot.z);
        ents.push_back({{"active", true}, {"center", {x, h.y + 2.0f, 0.0}},
                        {"half", {h.x, h.y, h.z}}, {"id", id++},
                        {"name", job.name},
                        {"parent", -1}, {"rotation", {0.0, 0.0, 0.0}}, {"type", 0},
                        {"components", nlohmann::json::array({cm, cs, cp})}});
        x += h.x + (only == "Rocks" ? 1.0f : 30.0f);
    }
    nlohmann::json jm = nlohmann::json::array();
    for (const MaterialDef& m : mats) {
        nlohmann::json one = {{"id", m.assetId.toString()}, {"name", m.name},
                              {"albedo", {m.albedo.x, m.albedo.y, m.albedo.z}},
                              {"reflectivity", m.reflectivity}, {"roughness", m.roughness},
                              {"emission", {m.emission.x, m.emission.y, m.emission.z}},
                              {"emissionStrength", m.emissionStrength},
                              {"glass", m.glass}, {"opacity", m.opacity}};
        if (m.texId.valid()) one["texture"] = m.texId.toString();
        if (m.normalTexId.valid()) one["normalMap"] = m.normalTexId.toString();
        if (m.ormTexId.valid()) {
            one["ormMap"]  = m.ormTexId.toString();
            one["ormMean"] = {m.ormMean.x, m.ormMean.y};
        }
        jm.push_back(std::move(one));
    }
    const nlohmann::json scene = {{"materials", jm}, {"entities", ents}};
    const fs::path file = fs::path(dir) / (fs::path(dir).filename().string() + ".fitzel");
    std::ofstream(file) << scene.dump(1);
    std::printf("wrote %zu objects and %zu materials to %s\n", ents.size(), mats.size(),
                file.string().c_str());
    if (!only.empty() && !ents.empty()) std::printf("graph space + %s\n", worldShift.c_str());
    return 0;
}

} // namespace

// --- Realize: placed prefabs melted into the faces -----------------------------
// A stand-in for the editor's look into the prefabs: "crate" is a 1 m box in a
// material of its own, "lamp" holds a light (cannot become faces).
void realize() {
    std::printf("\n-- realize\n");
    const fitzel::AssetId crateMat{0x1234, 0x5678};
    proc::prefabMesh() = [&](const std::string& name, EditMesh& out) {
        if (name != "crate") return false;
        out = EditMesh::box(glm::vec3(0.5f));
        out.syncFaceMat();
        for (fitzel::AssetId& f : out.faceMat) f = crateMat;
        return true;
    };
    proc::Graph g;
    const int a = node(g, "prefab", {{"prefab", "crate"}});
    const int b = node(g, "prefab", {{"prefab", "crate"}, {"move", {3.0, 0.0, 0.0}}, {"turn", {0.0, 90.0, 0.0}}});
    const int c = node(g, "prefab", {{"prefab", "lamp"}, {"move", {0.0, 4.0, 0.0}}});
    const int m = node(g, "merge", {}, {a, b, c});
    g.output = node(g, "realize", {}, {m});
    proc::CookInfo info;
    const proc::Geo geo = proc::cookGeo(g, g.output, &info);
    check(geo.mesh.faces.size() == 12, "two crates melted into 12 faces",
          std::to_string(geo.mesh.faces.size()));
    check(geo.instances.size() == 1 && geo.instances[0].prefab == "lamp", "the lamp stays an instance",
          std::to_string(geo.instances.size()) + " left");
    check(info.errors.count(g.output) == 1, "the node says how many stayed objects");
    glm::vec3 mn, mx;
    geo.mesh.bounds(mn, mx);
    check(std::fabs(mx.x - 3.5f) < 1e-4f && std::fabs(mn.x + 0.5f) < 1e-4f, "the second copy moved by 3 m",
          num(mn.x) + " .. " + num(mx.x));
    bool allMat = geo.mesh.faceMat.size() == geo.mesh.faces.size();
    for (std::size_t f = 0; allMat && f < geo.mesh.faces.size(); ++f) allMat = geo.mesh.faceMaterial(int(f)) == crateMat;
    check(allMat, "every melted face keeps the crate's material");
    solid("realized crates", geo.mesh, 12, 2.0);
    // A mirrored copy is wound back outward.
    proc::Graph h;
    const int p = node(h, "prefab", {{"prefab", "crate"}});
    const int t = node(h, "transform", {{"scale", {-1.0, 1.0, 1.0}}}, {p});
    h.output = node(h, "realize", {}, {t});
    solid("mirrored crate", proc::cookGeo(h, h.output).mesh, 6, 1.0);
    // Without the editor's look (the player, a bare check) nothing is melted.
    proc::prefabMesh() = nullptr;
    const proc::Geo none = proc::cookGeo(g, g.output);
    check(none.mesh.faces.empty() && none.instances.size() == 3, "no look into the prefabs: all stay instances");
}

int main(int argc, char** argv) {
    if (argc > 2 && std::string(argv[1]) == "--scene")
        return writeScene(argv[2], argc > 3 ? argv[3] : "", argc > 4 ? std::atoi(argv[4]) : -1);
    realize();
    shapes();
    copies();
    detail();
    curves();
    points();
    meshToPoints();
    architecture();
    furniture();
    interiors();
    rocks();
    rules();
    roundTrip();
    parameters();
    presets();
    std::printf("\nproccheck: %s (%d failed)\n", g_fail ? "FAILED" : "all passed", g_fail);
    return g_fail ? 1 : 0;
}