// proccheck: the procedural graphs (ProcGraph.hpp, ProcNodes.cpp,
// ProcPresets.cpp), measured. Every node kind is cooked and checked for what
// its result must be whatever it looks like -- closed where it should be
// closed, wound outward, the number of faces worked out by hand -- then the
// graph's own rules (no loops, deleting a step closes the chain, the output
// flag follows), the file round trip, and the presets cooked whole.
//   build/release/bin/proccheck.exe [--scene <projectDir>]
// --scene also writes a small project with every preset standing in a row, for
// viewcheck to look at.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include "../src/Component.hpp"
#include "../src/EditMesh.hpp"
#include "../src/ProcGraph.hpp"
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
        check(!m.faces.empty() && volume(m) > 0.0, name + ": something, wound outward",
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

// --- Furniture ---------------------------------------------------------------------------
// Every piece, at its defaults and with every pair of its choices and switches
// set every way: each must cook, come out closed and wound outward (every
// solid it is built of is), be all numbers, and stand on the floor. Then the
// sizes asked for are the sizes made, and Position and Turn put it there.

void furniture() {
    std::printf("Furniture\n");
    const char* kinds[] = {"table", "chair", "sofa", "bed", "cabinet", "shelf", "rug", "lamp"};
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
                                           (std::string(kind) == "cabinet" && set.dump().find("\"base\":2") != std::string::npos);
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

// --- A project for viewcheck ------------------------------------------------------------

int writeScene(const std::string& dir) {
    namespace fs = std::filesystem;
    fs::create_directories(dir);
    std::vector<MaterialDef> mats;
    nlohmann::json ents = nlohmann::json::array();
    float x = 0.0f;
    int id = 1;
    for (int i = 0; i < static_cast<int>(procpreset::list().size()) - 1; ++i) {
        ProcGraphComponent pc;
        pc.graph = procpreset::build(i, mats);
        EditMesh m = proc::cook(pc.graph, pc.graph.output);
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
        ents.push_back({{"active", true}, {"center", {x, h.y + 2.0f, 0.0}},
                        {"half", {h.x, h.y, h.z}}, {"id", id++},
                        {"name", procpreset::list()[static_cast<std::size_t>(i)].name},
                        {"parent", -1}, {"rotation", {0.0, 0.0, 0.0}}, {"type", 0},
                        {"components", nlohmann::json::array({cm, cs, cp})}});
        x += h.x + 30.0f;
    }
    nlohmann::json jm = nlohmann::json::array();
    for (const MaterialDef& m : mats)
        jm.push_back({{"id", m.assetId.toString()}, {"name", m.name},
                      {"albedo", {m.albedo.x, m.albedo.y, m.albedo.z}},
                      {"reflectivity", m.reflectivity}, {"roughness", m.roughness},
                      {"emission", {m.emission.x, m.emission.y, m.emission.z}},
                      {"emissionStrength", m.emissionStrength}});
    const nlohmann::json scene = {{"materials", jm}, {"entities", ents}};
    const fs::path file = fs::path(dir) / (fs::path(dir).filename().string() + ".fitzel");
    std::ofstream(file) << scene.dump(1);
    std::printf("wrote %zu objects and %zu materials to %s\n", ents.size(), mats.size(),
                file.string().c_str());
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
    if (argc > 2 && std::string(argv[1]) == "--scene") return writeScene(argv[2]);
    realize();
    shapes();
    copies();
    detail();
    curves();
    points();
    meshToPoints();
    architecture();
    furniture();
    rules();
    roundTrip();
    parameters();
    presets();
    std::printf("\nproccheck: %s (%d failed)\n", g_fail ? "FAILED" : "all passed", g_fail);
    return g_fail ? 1 : 0;
}