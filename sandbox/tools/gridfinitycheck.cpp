// gridfinitycheck -- Gridfinity bins and baseplates (Gridfinity.hpp), without GL.
//
// A bin that is off by half a millimetre still looks like a bin -- it just does
// not drop into the plate, or rocks on the one below. So the parts are measured
// against each other, from their meshes: a bin's feet in a plate's cell, height
// by height, and a bin's feet in another's stacking lip, lowered until they
// touch. Next to that the standard's own numbers (sizes, the lip's height, the
// magnet and screw holes where they belong and as wide), and the solid itself:
// every edge crossed once each way, no corner a face uses twice, sections at
// many heights that never cross themselves, its volume against one worked out
// from the profile by hand (Simpson's rule is exact on these bands) and against
// the compartments' capacity, through the 3MF export's weld as one closed part.
//
// With an out dir it writes .3mf files for a slicer and gridfinity-*.png:
// sections and the plan, drawn from the triangles.
//   build/linux/bin/gridfinitycheck [outdir]

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "../src/Component.hpp"
#include "../src/Document.hpp"
#include "../src/EditMesh.hpp"
#include "../src/Export3mf.hpp"
#include "../src/Gridfinity.hpp"
#include "../src/ModelLibrary.hpp"
#include "../src/SceneGraph.hpp"

using gridfinity::Params;
using gridfinity::Kind;
using DV2 = glm::dvec2;
using DV3 = glm::dvec3;

namespace {

int failures = 0;
void check(bool ok, const std::string& what, const std::string& detail = {}) {
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what.c_str(), detail.empty() ? "" : "  -- ",
                detail.c_str());
    if (!ok) ++failures;
}
std::string num(double v) {
    char b[64];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

// --- The mesh, back in the print's frame (mm, Z up) -----------------------------------

struct Tri {
    DV3 a, b, c;
};

DV3 print(const glm::vec3& v) { return DV3(v.x, -v.z, v.y) * 1000.0; }

std::vector<Tri> tris(const EditMesh& m) {
    std::vector<Tri> out;
    for (const std::vector<int>& f : m.faces)
        for (std::size_t k = 1; k + 1 < f.size(); ++k)
            out.push_back({print(m.verts[static_cast<std::size_t>(f[0])]), print(m.verts[static_cast<std::size_t>(f[k])]),
                           print(m.verts[static_cast<std::size_t>(f[k + 1])])});
    return out;
}

// Every edge crossed once each way; no face through one corner twice.
bool closed(const EditMesh& m, std::string& why) {
    std::map<std::pair<int, int>, int> edges;
    for (const std::vector<int>& f : m.faces) {
        for (std::size_t k = 0; k < f.size(); ++k) {
            const int a = f[k], b = f[(k + 1) % f.size()];
            for (std::size_t j = k + 1; j < f.size(); ++j)
                if (f[j] == a) {
                    why = "a face through one corner twice";
                    return false;
                }
            ++edges[{a, b}];
        }
    }
    for (const auto& [e, n] : edges) {
        const auto back = edges.find({e.second, e.first});
        if (n != 1 || back == edges.end() || back->second != 1) {
            const DV3 p = print(m.verts[static_cast<std::size_t>(e.first)]);
            why = "edge used " + std::to_string(n) + "x, back " +
                  std::to_string(back == edges.end() ? 0 : back->second) + "x, at " + num(p.x) + ", " + num(p.y) +
                  ", " + num(p.z);
            return false;
        }
    }
    if (m.faces.empty()) {
        why = "empty";
        return false;
    }
    return true;
}

double volume(const std::vector<Tri>& ts) {
    double v = 0.0;
    for (const Tri& t : ts) v += glm::dot(t.a, glm::cross(t.b, t.c)) / 6.0;
    return v;
}

double minArea(const std::vector<Tri>& ts, DV3* where = nullptr) {
    double a = 1e30;
    for (const Tri& t : ts) {
        const double s = 0.5 * glm::length(glm::cross(t.b - t.a, t.c - t.a));
        if (s < a) {
            a = s;
            if (where) *where = (t.a + t.b + t.c) / 3.0;
        }
    }
    return a;
}

void bounds(const std::vector<Tri>& ts, DV3& lo, DV3& hi) {
    lo = DV3(1e30);
    hi = DV3(-1e30);
    for (const Tri& t : ts)
        for (const DV3& p : {t.a, t.b, t.c}) {
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
}

// The section at height z: one segment a triangle that crosses it, directed so
// the solid lies on its left (outlines counter-clockwise, holes clockwise).
struct Seg {
    DV2 p, q;
};
std::vector<Seg> slice(const std::vector<Tri>& ts, double z) {
    std::vector<Seg> out;
    for (const Tri& t : ts) {
        const DV3 v[3] = {t.a, t.b, t.c};
        DV2 pts[2];
        int n = 0;
        for (int k = 0; k < 3 && n < 2; ++k) {
            const DV3& a = v[k];
            const DV3& b = v[(k + 1) % 3];
            if ((a.z < z) == (b.z < z)) continue;
            const double f = (z - a.z) / (b.z - a.z);
            pts[n++] = DV2(a + (b - a) * f);
        }
        if (n != 2) continue;
        // Orient: the face's normal crossed with up points along the segment.
        const DV3 nrm = glm::cross(t.b - t.a, t.c - t.a);
        const DV2 dir(-nrm.y, nrm.x);
        if (glm::dot(pts[1] - pts[0], dir) < 0.0) std::swap(pts[0], pts[1]);
        out.push_back({pts[0], pts[1]});
    }
    return out;
}

// Where the section crosses the line y = y0, left to right.
std::vector<double> crossX(const std::vector<Seg>& ss, double y0) {
    std::vector<double> xs;
    for (const Seg& s : ss) {
        if ((s.p.y < y0) == (s.q.y < y0)) continue;
        xs.push_back(s.p.x + (s.q.x - s.p.x) * (y0 - s.p.y) / (s.q.y - s.p.y));
    }
    std::sort(xs.begin(), xs.end());
    return xs;
}

double area(const std::vector<Seg>& ss) {
    double a = 0.0;
    for (const Seg& s : ss) a += 0.5 * (s.p.x * s.q.y - s.q.x * s.p.y);
    return a;
}

// Two segments of a section crossing (not just meeting at an end).
bool crosses(const std::vector<Seg>& ss, std::string& where) {
    auto orient = [](DV2 a, DV2 b, DV2 c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); };
    std::vector<std::size_t> order(ss.size());
    for (std::size_t i = 0; i < ss.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](std::size_t a, std::size_t b) { return std::min(ss[a].p.x, ss[a].q.x) < std::min(ss[b].p.x, ss[b].q.x); });
    for (std::size_t ii = 0; ii < order.size(); ++ii) {
        const Seg& a = ss[order[ii]];
        const double ax1 = std::max(a.p.x, a.q.x);
        for (std::size_t jj = ii + 1; jj < order.size(); ++jj) {
            const Seg& b = ss[order[jj]];
            if (std::min(b.p.x, b.q.x) > ax1 + 1e-9) break;
            if (std::max(a.p.y, a.q.y) < std::min(b.p.y, b.q.y) || std::max(b.p.y, b.q.y) < std::min(a.p.y, a.q.y)) continue;
            const double e = 1e-9;
            const double d1 = orient(a.p, a.q, b.p), d2 = orient(a.p, a.q, b.q);
            const double d3 = orient(b.p, b.q, a.p), d4 = orient(b.p, b.q, a.q);
            if (((d1 > e && d2 < -e) || (d1 < -e && d2 > e)) && ((d3 > e && d4 < -e) || (d3 < -e && d4 > e))) {
                where = num(a.p.x) + ", " + num(a.p.y);
                return true;
            }
        }
    }
    return false;
}

// --- Pictures -----------------------------------------------------------------------

struct Png {
    std::string dir;
};

struct Canvas {
    int w, h;
    std::vector<unsigned char> px;
    Canvas(int w_, int h_) : w(w_), h(h_), px(static_cast<std::size_t>(w_ * h_ * 3), 250) {}
    void dot(int x, int y, glm::ivec3 c) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        unsigned char* p = &px[static_cast<std::size_t>((y * w + x) * 3)];
        p[0] = static_cast<unsigned char>(c.r);
        p[1] = static_cast<unsigned char>(c.g);
        p[2] = static_cast<unsigned char>(c.b);
    }
    void line(DV2 a, DV2 b, glm::ivec3 c) {
        const int n = static_cast<int>(std::ceil(std::max(std::abs(b.x - a.x), std::abs(b.y - a.y)))) + 1;
        for (int i = 0; i <= n; ++i) {
            const DV2 p = a + (b - a) * (static_cast<double>(i) / n);
            dot(static_cast<int>(std::lround(p.x)), static_cast<int>(std::lround(p.y)), c);
        }
    }
    void save(const std::string& path) const { stbi_write_png(path.c_str(), w, h, 3, px.data(), w * 3); }
};

// A vertical section (x = x0: front to back) and a horizontal one through the
// feet and the compartments, side by side, `scale` pixels a millimetre.
void picture(const std::string& path, const std::vector<Tri>& ts, double x0, const std::vector<double>& zs,
             double scale) {
    DV3 lo, hi;
    bounds(ts, lo, hi);
    const int pad = 20;
    const int w1 = static_cast<int>((hi.y - lo.y) * scale) + 2 * pad;
    const int h1 = static_cast<int>((hi.z - lo.z) * scale) + 2 * pad;
    const int w2 = static_cast<int>((hi.x - lo.x) * scale) + 2 * pad;
    const int h2 = static_cast<int>((hi.y - lo.y) * scale) + 2 * pad;
    Canvas c(w1 + w2, std::max(h1, h2));
    // The vertical cut: triangles crossing the plane x = x0, as (y, z).
    for (const Tri& t : ts) {
        const DV3 v[3] = {t.a, t.b, t.c};
        DV2 pts[2];
        int n = 0;
        for (int k = 0; k < 3 && n < 2; ++k) {
            const DV3& a = v[k];
            const DV3& b = v[(k + 1) % 3];
            if ((a.x < x0) == (b.x < x0)) continue;
            const DV3 p = a + (b - a) * ((x0 - a.x) / (b.x - a.x));
            pts[n++] = DV2(pad + (p.y - lo.y) * scale, c.h - pad - (p.z - lo.z) * scale);
        }
        if (n == 2) c.line(pts[0], pts[1], {20, 20, 30});
    }
    const glm::ivec3 colours[] = {{200, 40, 30}, {30, 110, 200}, {30, 150, 60}, {150, 60, 160}};
    for (std::size_t k = 0; k < zs.size(); ++k)
        for (const Seg& s : slice(ts, zs[k]))
            c.line(DV2(w1 + pad + (s.p.x - lo.x) * scale, pad + (hi.y - s.p.y) * scale),
                   DV2(w1 + pad + (s.q.x - lo.x) * scale, pad + (hi.y - s.q.y) * scale), colours[k % 4]);
    c.save(path);
}

// --- One object ----------------------------------------------------------------------

struct Built {
    gridfinity::Model model;
    std::vector<Tri> tris;
};

Built make(const Params& p) {
    Built b;
    b.model = gridfinity::build(p);
    b.tris = tris(b.model.mesh);
    return b;
}

// Closed, no slivers, the volume adding up, the size it says, sections that
// never cross.
void solidChecks(const std::string& what, const Params& p, const Png& png, const std::string& file) {
    const Params s = gridfinity::sane(p);
    const Built b = make(p);
    const gridfinity::Model& m = b.model;
    std::string why;
    check(m.ok, what + ": built", m.error);
    check(closed(m.mesh, why), what + ": closed, every edge once each way",
          why.empty() ? num(static_cast<double>(m.mesh.faces.size())) + " faces" : why);
    const double v = volume(b.tris);
    check(std::abs(v - m.volume) < 1e-3 * std::abs(m.volume) && v > 0.0, what + ": volume " + num(v / 1000.0) + " cm3");
    DV3 sp(0.0);
    const double sliver = minArea(b.tris, &sp);
    check(sliver > 1e-7, what + ": no triangle without area",
          "smallest " + num(sliver) + " mm2 at " + num(sp.x) + ", " + num(sp.y) + ", " + num(sp.z));
    DV3 lo, hi;
    bounds(b.tris, lo, hi);
    const bool bin = s.kind == Kind::Bin;
    const double W = bin ? 42.0 * s.unitsX - 0.5 : std::max(42.0 * s.plateX, s.fitDrawer ? double(s.drawerW) : 0.0);
    const double D = bin ? 42.0 * s.unitsY - 0.5 : std::max(42.0 * s.plateY, s.fitDrawer ? double(s.drawerD) : 0.0);
    const double H = bin ? 7.0 * s.unitsZ + (s.lip ? 4.4 - 0.6 * std::sqrt(2.0) : 0.0)
                         : (s.plateFloor ? s.floorThickness : 0.0) + 5.0;
    const DV3 size = hi - lo;
    check(std::abs(size.x - W) < 1e-4 && std::abs(size.y - D) < 1e-4 && std::abs(size.z - H) < 1e-4 &&
              std::abs(lo.z) < 1e-6 && std::abs(lo.x + hi.x) < 1e-4 && std::abs(lo.y + hi.y) < 1e-4,
          what + ": " + num(size.x) + " x " + num(size.y) + " x " + num(size.z) + " mm, centred, on Z 0",
          "want " + num(W) + " x " + num(D) + " x " + num(H));
    check(std::abs(m.size.x - size.x) < 1e-3 && std::abs(m.size.y - size.y) < 1e-3 && std::abs(m.size.z - size.z) < 1e-3,
          what + ": the size it reports");
    // Sections at heights off every profile corner: closed, never crossing, the
    // area enclosed integrated back into the volume.
    bool ok = true;
    std::string where;
    double integral = 0.0;
    const int steps = 240;
    const double dz = size.z / steps;
    for (int k = 0; k < steps; ++k) {
        const double z = (k + 0.5) * dz;
        const std::vector<Seg> ss = slice(b.tris, z);
        integral += area(ss) * dz;
        if (k % 8 == 3 && ok && crosses(ss, where)) {
            ok = false;
            where = "at z " + num(z) + ": " + where;
        }
    }
    check(ok, what + ": sections never cross themselves", where);
    check(std::abs(integral - v) < 0.01 * v, what + ": the sections' areas add up to the volume",
          num(integral / 1000.0) + " cm3");
    if (!png.dir.empty() && !file.empty()) {
        // Through the first column's holes (13 mm off its middle), a hair aside
        // from any corner.
        const double x0 = bin ? -(42.0 * s.unitsX - 0.5) / 2.0 + 20.75 - 13.0 + 0.37
                              : -21.0 * s.plateX + 21.0 - 13.0 + 0.37;
        picture(png.dir + "/gridfinity-" + file + ".png", b.tris, x0,
                bin ? std::vector<double>{1.0, 4.0, s.floor + 2.0, 7.0 * s.unitsZ - 0.5}
                    : std::vector<double>{0.2, 2.0, 4.0},
                6.0);
    }
}

// Volume of a rounded square of side s and corner r as drawn: each corner a
// fan of kArcSegments triangles.
double squareArea(double s, double r) {
    const int k = gridfinity::kArcSegments;
    return s * s - r * r * (4.0 - 2.0 * k * std::sin(3.14159265358979323846 / (2.0 * k)));
}
// Simpson between two rounded squares (exact: the area is quadratic in z).
double band(double s0, double r0, double s1, double r1, double h) {
    return h / 6.0 * (squareArea(s0, r0) + 4.0 * squareArea(0.5 * (s0 + s1), 0.5 * (r0 + r1)) + squareArea(s1, r1));
}

// The foot's half-width at height z above its bottom, from the standard.
double footHalf(double z) {
    if (z < 0.8) return 20.75 - 2.95 + z;
    if (z < 2.6) return 20.75 - 2.15;
    return 20.75 - 2.15 + (z - 2.6);
}

} // namespace

int main(int argc, char** argv) {
    Png png;
    if (argc > 1) {
        png.dir = argv[1];
        std::filesystem::create_directories(png.dir);
    }

    // --- The solids --------------------------------------------------------------
    std::printf("Bins\n");
    {
        Params p;
        p.unitsX = 1; p.unitsY = 1; p.unitsZ = 1; p.lip = false;
        solidChecks("1x1x1, no lip (solid)", p, png, "");
        p.lip = true;
        solidChecks("1x1x1 with a lip (solid)", p, png, "1x1x1-lip");
        p.unitsZ = 3; p.lip = false;
        solidChecks("1x1x3, no lip", p, png, "1x1x3");
        p.lip = true;
        solidChecks("1x1x3 with a lip", p, png, "1x1x3-lip");
        p.unitsX = 2; p.unitsZ = 6;
        solidChecks("2x1x6, the default", p, png, "2x1x6");
        p.magnets = true;
        solidChecks("2x1x6, magnets", p, png, "");
        p.screws = true;
        solidChecks("2x1x6, magnets and screws", p, png, "2x1x6-holes");
        p.magnets = false;
        solidChecks("2x1x6, screws only", p, png, "");
        Params c;
        c.unitsX = 3; c.unitsY = 2; c.unitsZ = 4; c.compX = 4; c.compY = 2;
        solidChecks("3x2x4, 4 x 2 compartments", c, png, "3x2x4-comp");
        c.label = true;
        solidChecks("3x2x4, 4 x 2 with label shelves", c, png, "3x2x4-label");
        c.scoop = true;
        solidChecks("3x2x4, 4 x 2 with shelves and scoops", c, png, "3x2x4-scoop");
        c.lip = false;
        solidChecks("3x2x4, 4 x 2, shelves and scoops, no lip", c, png, "3x2x4-nolip");
        Params n;
        n.unitsX = 1; n.unitsY = 1; n.unitsZ = 2; n.compX = 5; n.compY = 5; n.label = true; n.scoop = true;
        solidChecks("1x1x2, 5 x 5 tiny compartments, shelves and scoops", n, png, "1x1x2-tiny");
        Params l;
        l.label = true;
        solidChecks("2x1x6, one compartment with a shelf", l, png, "2x1x6-label");
        l.label = false; l.scoop = true;
        solidChecks("2x1x6, one compartment with a scoop", l, png, "2x1x6-scoop");
        Params big;
        big.unitsX = 5; big.unitsY = 4; big.unitsZ = 3; big.magnets = true; big.compX = 3; big.compY = 3;
        solidChecks("5x4x3, magnets, 3 x 3 compartments", big, png, "5x4x3");
        Params thick;
        thick.wall = 2.4f; thick.divider = 3.0f; thick.compX = 2; thick.floor = 12.0f; thick.unitsZ = 5;
        solidChecks("2x1x5, 2.4 mm walls, 3 mm divider, floor at 12", thick, png, "2x1x5-thick");
    }
    std::printf("Baseplates\n");
    {
        Params p;
        p.kind = Kind::Baseplate;
        p.plateX = 1; p.plateY = 1;
        solidChecks("1x1 open frame", p, png, "plate-1x1");
        p.plateX = 4; p.plateY = 3;
        solidChecks("4x3 open frame", p, png, "plate-4x3");
        p.plateFloor = true;
        solidChecks("4x3 with a floor", p, png, "");
        p.plateMagnets = true;
        solidChecks("4x3, floor and magnets", p, png, "plate-4x3-magnets");
        p.plateScrews = true;
        solidChecks("4x3, floor, magnets and screws", p, png, "plate-4x3-holes");
        Params d;
        d.kind = Kind::Baseplate;
        d.fitDrawer = true;
        d.drawerW = 231.0f; d.drawerD = 140.5f;
        const Params ds = gridfinity::sane(d);
        check(ds.plateX == 5 && ds.plateY == 3, "a 231 x 140.5 drawer takes 5 x 3 cells",
              std::to_string(ds.plateX) + " x " + std::to_string(ds.plateY));
        solidChecks("5x3 filling a 231 x 140.5 drawer", d, png, "plate-drawer");
    }

    // --- Big ones, as fast as the panel needs them -------------------------------------
    std::printf("Big ones\n");
    {
        Params big;
        big.unitsX = 8; big.unitsY = 8; big.unitsZ = 3; big.magnets = true; big.screws = true;
        big.compX = 8; big.compY = 8; big.label = true; big.scoop = true;
        const auto t0 = std::chrono::steady_clock::now();
        const gridfinity::Model m = gridfinity::build(big);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::string why;
        check(m.ok && closed(m.mesh, why), "an 8x8x3 bin, magnets, screws, 64 compartments: closed",
              num(static_cast<double>(m.mesh.faces.size())) + " faces, " + num(ms) + " ms" + (why.empty() ? "" : ", " + why));
        Params plate;
        plate.kind = Kind::Baseplate;
        plate.fitDrawer = true; plate.drawerW = 500.0f; plate.drawerD = 420.0f;
        plate.plateFloor = true; plate.plateMagnets = true; plate.plateScrews = true;
        const auto t1 = std::chrono::steady_clock::now();
        const gridfinity::Model pm = gridfinity::build(plate);
        const double ms2 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
        check(pm.ok && closed(pm.mesh, why), "a 500 x 420 drawer's plate (11 x 10 cells, magnets, screws): closed",
              num(static_cast<double>(pm.mesh.faces.size())) + " faces, " + num(ms2) + " ms" + (why.empty() ? "" : ", " + why));
        check(ms < 1500.0 && ms2 < 1500.0, "both built in well under the time a click takes to feel slow",
              num(ms) + " and " + num(ms2) + " ms");
    }

    // --- Against the standard, by hand --------------------------------------------
    std::printf("Volumes worked out from the profile\n");
    {
        Params p;
        p.unitsX = 1; p.unitsY = 1; p.unitsZ = 1; p.lip = false;
        const gridfinity::Model m = gridfinity::build(p);
        // The foot's bands, then the slab to 7 mm.
        const double s0 = 41.5, R = 3.75;
        const double want = band(s0 - 5.9, R - 2.95, s0 - 4.3, R - 2.15, 0.8) +
                            band(s0 - 4.3, R - 2.15, s0 - 4.3, R - 2.15, 1.8) +
                            band(s0 - 4.3, R - 2.15, s0, R, 2.15) + band(s0, R, s0, R, 7.0 - 4.75);
        check(m.compartments == 0 && std::abs(m.volume - want) < 1e-6 * want,
              "a 1x1x1 bin is a foot and a slab to 7 mm", num(m.volume) + " mm3, want " + num(want));
        // Hollow: the solid less one compartment.
        p.unitsZ = 3;
        const gridfinity::Model h = gridfinity::build(p);
        Params solid = p;
        solid.floor = 20.5f;   // too high for a compartment: solid
        const gridfinity::Model sm = gridfinity::build(solid);
        check(h.compartments == 1 && sm.compartments == 0 &&
                  std::abs(h.volume + h.capacity - sm.volume) < 1e-6 * sm.volume,
              "a 1x1x3 bin: the solid less what its compartment holds",
              num(h.volume) + " + " + num(h.capacity) + " vs " + num(sm.volume));
        check(h.capacity > 20000.0 && h.capacity < 39.6 * 39.6 * 14.0,
              "the compartment holds " + num(h.capacity / 1000.0) + " ml");
        // A wall's worth: the 1x1x3 without a lip is the outer box less the hollow.
        const double outer = band(s0 - 5.9, R - 2.95, s0 - 4.3, R - 2.15, 0.8) +
                             band(s0 - 4.3, R - 2.15, s0 - 4.3, R - 2.15, 1.8) +
                             band(s0 - 4.3, R - 2.15, s0, R, 2.15) + band(s0, R, s0, R, 21.0 - 4.75);
        check(std::abs(sm.volume - outer) < 1e-6 * outer, "the solid 1x1x3 is the foot and a block to 21 mm");
    }

    // --- The feet and holes where the standard puts them --------------------------
    std::printf("Feet and holes\n");
    {
        Params p;
        p.unitsX = 1; p.unitsY = 1; p.unitsZ = 3;
        const Built b = make(p);
        bool ok = true;
        std::string worst;
        for (double z = 0.05; z < 4.75; z += 0.1) {
            const std::vector<double> xs = crossX(slice(b.tris, z), 0.0);
            if (xs.size() != 2 || std::abs(xs.back() - footHalf(z)) > 1e-3 || std::abs(xs.front() + footHalf(z)) > 1e-3) {
                ok = false;
                worst = "at z " + num(z) + ": " + (xs.empty() ? std::string("nothing") : num(xs.back())) + ", want " +
                        num(footHalf(z));
            }
        }
        check(ok, "the foot's profile: 0.8 out, 1.8 up, 2.15 out, 35.6 to 41.5 mm", worst);
        p.magnets = true;
        p.screws = true;
        const Built h = make(p);
        const std::vector<double> lowX = crossX(slice(h.tris, 1.2), 13.0);
        const std::vector<double> highX = crossX(slice(h.tris, 4.0), 13.0);
        check(lowX.size() == 6 && std::abs(lowX[2] - (-13.0 + 3.25)) < 1e-6 && std::abs(lowX[4] - (13.0 + 3.25)) < 1e-6 &&
                  std::abs(lowX[3] - (13.0 - 3.25)) < 1e-6,
              "magnet holes 6.5 mm wide, 13 mm out", std::to_string(lowX.size()) + " crossings");
        check(highX.size() == 6 && std::abs(highX[4] - 14.5) < 1e-6 && std::abs(highX[3] - 11.5) < 1e-6,
              "screw holes 3 mm wide above them");
        check(crossX(slice(h.tris, 2.3), 13.0).size() == 6 && crossX(slice(h.tris, 5.9), 13.0).size() == 6 &&
                  crossX(slice(h.tris, 6.1), 13.0).size() == 2,
              "magnets 2.4 deep, screws to 6 mm");
    }

    // --- A bin in a plate, a bin on a bin ---------------------------------------------
    std::printf("Fit\n");
    {
        Params bp;
        bp.unitsX = 1; bp.unitsY = 1; bp.unitsZ = 3;
        const Built bin = make(bp);
        Params pp;
        pp.kind = Kind::Baseplate;
        pp.plateX = 1; pp.plateY = 1;
        const Built plate = make(pp);
        // Standing on the drawer's bottom, as the plate does: its slopes on the
        // cell's slopes, the straight parts 0.25 apart, never into the plate.
        double least = 1e9, most = -1e9, at = 0.0, touching = 0.0;
        for (double z = 0.01; z < 4.75; z += 0.02) {
            const std::vector<double> f = crossX(slice(bin.tris, z), 0.0);
            const std::vector<double> o = crossX(slice(plate.tris, z), 0.0);
            if (f.size() != 2 || o.size() != 4) continue;
            const double gap = o[2] - f[1];
            if (gap < least) { least = gap; at = z; }
            most = std::max(most, gap);
            if (std::abs(gap) < 1e-6) touching += 0.02;
        }
        check(least > -1e-6 && most > 0.3 && most < 0.36,
              "a bin in a plate's cell never reaches into it: " + num(std::max(least, 0.0)) + " to " + num(most) +
                  " mm off", "closest at z " + num(at));
        check(touching > 2.0, "its slopes lie on the cell's: touching along " + num(touching) + " mm of its height");

        // Another bin let down onto the lip until it touches.
        const double H = 7.0 * bp.unitsZ;
        auto gapAt = [&](double o) {
            double g = 1e9;
            for (double z = o + 0.02; z < o + 4.75; z += 0.02) {
                if (z < H - 1.2) continue;
                const std::vector<double> lipX = crossX(slice(bin.tris, z), 0.0);
                if (lipX.size() != 4) continue;
                g = std::min(g, lipX[2] - footHalf(z - o));
            }
            return g;
        };
        double lo = H - 3.0, hi = H + 1.0;
        for (int i = 0; i < 40; ++i) {
            const double mid = 0.5 * (lo + hi);
            (gapAt(mid) < 0.0 ? lo : hi) = mid;
        }
        const double rest = hi;
        check(rest > H - 0.5 && rest <= H + 1e-6 && rest > H - 1.2,
              "a bin stacked on it rests " + num(H - rest) + " mm below the walls' top, its feet clear of the dividers");
        check(gapAt(H) > 0.2 && gapAt(H) < 0.4, "lifted to the walls' top, " + num(gapAt(H)) + " mm off the lip all round");
    }

    // --- The panel's cut-away view ----------------------------------------------------------
    std::printf("Cut-away\n");
    {
        Params p;
        p.compX = 3;
        p.label = true;
        p.scoop = true;
        p.magnets = true;
        const Built b = make(p);
        const float x0 = -41.75f + 0.95f + 0.5f * (81.6f - 2.4f) / 3.0f + 0.013f;
        const std::vector<std::vector<glm::vec2>> loops = gridfinity::section(b.model.mesh, x0);
        double a = 0.0;
        std::size_t pts = 0;
        for (const auto& l : loops) {
            pts += l.size();
            for (std::size_t i = 0; i < l.size(); ++i) {
                const glm::vec2 u = l[i], v = l[(i + 1) % l.size()];
                a += 0.5 * (u.x * v.y - v.x * u.y);
            }
        }
        // The same area from the horizontal sections: how much of the line x = x0
        // lies in the solid, summed up the height.
        double want = 0.0;
        const double dz = 0.01;
        for (double z = 0.5 * dz; z < b.model.size.z; z += dz) {
            std::vector<double> ys;
            for (const Seg& sg : slice(b.tris, z)) {
                if ((sg.p.x < x0) == (sg.q.x < x0)) continue;
                ys.push_back(sg.p.y + (sg.q.y - sg.p.y) * (x0 - sg.p.x) / (sg.q.x - sg.p.x));
            }
            std::sort(ys.begin(), ys.end());
            for (std::size_t i = 0; i + 1 < ys.size(); i += 2) want += (ys[i + 1] - ys[i]) * dz;
        }
        bool closedLoops = !loops.empty();
        for (const auto& l : loops) closedLoops = closedLoops && l.size() >= 3;
        check(closedLoops && std::abs(std::abs(a) - want) < 0.01 * want,
              "the bin cut front to back: " + std::to_string(loops.size()) + " closed outline(s), " + num(std::abs(a)) +
                  " mm2", "want " + num(want) + ", " + std::to_string(pts) + " corners");
    }

    // --- The parameters -----------------------------------------------------------------
    std::printf("Parameters\n");
    {
        check(gridfinity::maxCompartments(1, 0.95f, 1.2f) == 5, "five compartments fit in one unit",
              std::to_string(gridfinity::maxCompartments(1, 0.95f, 1.2f)));
        Params p;
        p.unitsX = 1; p.compX = 9;
        check(gridfinity::sane(p).compX == 5, "more are taken back to five");
        p.magnets = true; p.screws = true; p.floor = 5.0f;
        check(std::abs(gridfinity::sane(p).floor - 6.6f) < 1e-5f, "the floor stays above the screw holes",
              num(gridfinity::sane(p).floor));
        check(gridfinity::autoName(p) == "Gridfinity bin 1x1x6", "named " + gridfinity::autoName(p));
        Params q;
        q.kind = Kind::Baseplate;
        q.plateMagnets = true;
        check(!gridfinity::sane(q).plateMagnets, "no magnets without a floor to hold them");
    }

    // --- Saved and loaded -----------------------------------------------------------------
    std::printf("Saved and loaded\n");
    {
        Params p;
        p.kind = Kind::Baseplate;
        p.unitsX = 3; p.unitsY = 2; p.unitsZ = 9; p.compX = 3; p.compY = 2;
        p.lip = false; p.magnets = true; p.screws = true; p.label = true; p.labelDepth = 11.0f;
        p.scoop = true; p.scoopRadius = 9.0f; p.wall = 1.2f; p.divider = 1.6f; p.floor = 8.0f;
        p.binColor = {0.1f, 0.2f, 0.3f};
        p.plateX = 6; p.plateY = 5; p.plateFloor = true; p.floorThickness = 4.0f; p.plateMagnets = true;
        p.plateScrews = true; p.plateColor = {0.9f, 0.8f, 0.7f};
        nlohmann::json j;
        gridfinity::saveParams(j, p);
        Params q;
        gridfinity::loadParams(nlohmann::json::parse(j.dump()), q);
        check(q == p, "every parameter comes back", j.dump());
        Params d;
        gridfinity::loadParams(nlohmann::json::object(), d);
        check(d == Params{}, "nothing saved: the defaults");
        std::unique_ptr<ComponentBase> c = components::create("gridfinity");
        check(c && dynamic_cast<GridfinityComponent*>(c.get()), "the component is registered as gridfinity");
    }

    // --- Through the 3MF export -------------------------------------------------------------
    std::printf("Through the 3MF export\n");
    auto export3 = [&](const Params& p, const std::string& file) {
        ModelLibrary models;
        Document doc;
        int counter = 1;
        const fitzel::AssetId mat = gridfinity::ensureMaterial(doc.materials(), p);
        std::vector<Entity> es = gridfinity::entities(p, mat, counter, glm::vec3(3.0f, 1.0f, -2.0f));
        const Params s = gridfinity::sane(p);
        check(es.size() == 2 && es[0].components.get<GridfinityComponent>() &&
                  es[1].name == (s.kind == Kind::Bin ? "Bin" : "Baseplate"),
              file + ": a root keeping the parameters and one part", es.empty() ? "" : es[0].name);
        const int root = es.empty() ? -1 : es[0].id;
        for (Entity& e : es) doc.entities().push_back(std::move(e));
        scenegraph::resolve(doc.entities());
        const export3mf::Shape sh = export3mf::gather(doc, models, {root});
        bool allClosed = sh.parts.size() == 1;
        for (const export3mf::Part& part : sh.parts) {
            std::map<std::pair<int, int>, int> edges;
            for (const glm::ivec3& t : part.tris)
                for (int k = 0; k < 3; ++k) ++edges[{t[k], t[(k + 1) % 3]}];
            for (const auto& [e, n] : edges) {
                const auto back = edges.find({e.second, e.first});
                if (n != 1 || back == edges.end() || back->second != 1) allClosed = false;
            }
        }
        check(allClosed, file + ": one part, closed after the weld");
        export3mf::Options o;
        const glm::vec3 mm = export3mf::printSize(sh, o);
        const gridfinity::Model m = gridfinity::build(p);
        check(std::abs(mm.x - m.size.x) < 1e-2f && std::abs(mm.y - m.size.y) < 1e-2f && std::abs(mm.z - m.size.z) < 1e-2f,
              file + ": " + num(mm.x) + " x " + num(mm.y) + " x " + num(mm.z) + " mm on the bed");
        if (!png.dir.empty()) {
            std::string err;
            check(export3mf::write(png.dir + "/" + file + ".3mf", sh, o, err), "written as " + png.dir + "/" + file + ".3mf", err);
        }
    };
    {
        Params p;
        p.magnets = true;
        p.compX = 3;
        p.label = true;
        p.scoop = true;
        export3(p, "gridfinity-bin");
        Params q;
        q.kind = Kind::Baseplate;
        export3(q, "gridfinity-plate");
        q.plateFloor = true;
        q.plateMagnets = true;
        q.plateScrews = true;
        export3(q, "gridfinity-plate-magnets");
    }

    std::printf("\n%s (%d failed)\n", failures ? "FAILED" : "all ok", failures);
    return failures ? 1 : 0;
}
