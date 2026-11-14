// penholdercheck -- pen holders (PenHolder.hpp), without GL.
//
// Round ones and rows in many settings, every one closed (every edge crossed
// once each way, no triangle without area), sections at many heights that
// never cross themselves and add up to the volume, the size it reports. And
// what a pen holder that takes little plastic and nothing but pens has to be,
// measured from the mesh: every cell as wide as the pen and its play and
// nothing wider than the smallest euro coin on edge; open from the top to the
// desk through each cell's middle (no floor), its drain as wide as asked and a
// pencil's point standing in it clear of the desk; the top only the walls'
// edges, each as thin as a wall; under the funnels hollow; no face leaning out
// further than 45 degrees, so it prints without support. Saved and loaded, and
// through the 3MF export as one closed part.
//
// With an out dir it writes .3mf files for a slicer and penholder-*.png: a
// vertical cut and horizontal sections, drawn from the triangles, and the
// panel's views.
//   build/linux/bin/penholdercheck [outdir]

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
#include "../src/ModelLibrary.hpp"
#include "../src/PenHolder.hpp"
#include "../src/SceneGraph.hpp"

using penholder::Layout;
using penholder::Params;
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
    if (zs.empty()) {}
    for (std::size_t k = 0; k < zs.size(); ++k)
        for (const Seg& s : slice(ts, zs[k]))
            c.line(DV2(w1 + pad + (s.p.x - lo.x) * scale, pad + (hi.y - s.p.y) * scale),
                   DV2(w1 + pad + (s.q.x - lo.x) * scale, pad + (hi.y - s.q.y) * scale), colours[k % 4]);
    c.save(path);
}


// --- One holder ---------------------------------------------------------------------

struct Built {
    penholder::Model model;
    std::vector<Tri> body;
    DV3 lo{0.0}, hi{0.0};
};

Built make(const Params& p) {
    Built b;
    b.model = penholder::build(p);
    b.body = tris(b.model.body);
    if (!b.body.empty()) bounds(b.body, b.lo, b.hi);
    return b;
}

// The width of the opening round (x, y) in the section at z, along x: the
// crossings on either side of it. 0 where (x, y) is in plastic or outside.
double openingAt(const std::vector<Tri>& ts, double z, DV2 c) {
    const std::vector<double> xs = crossX(slice(ts, z), c.y);
    for (std::size_t i = 1; i + 1 < xs.size(); i += 2)
        if (xs[i] < c.x && c.x < xs[i + 1]) return xs[i + 1] - xs[i];
    return 0.0;
}

// Triangles a vertical line through (x, y) passes through.
int hitsDown(const std::vector<Tri>& ts, DV2 c) {
    int n = 0;
    for (const Tri& t : ts) {
        const DV2 a(t.a), b(t.b), d(t.c);
        const double s = (b.x - a.x) * (d.y - a.y) - (b.y - a.y) * (d.x - a.x);
        if (std::abs(s) < 1e-9) continue;   // upright: seen edge on
        auto side = [](DV2 p, DV2 q, DV2 r) { return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x); };
        const double u = side(a, b, c), v = side(b, d, c), w = side(d, a, c);
        if ((u > -1e-9 && v > -1e-9 && w > -1e-9) || (u < 1e-9 && v < 1e-9 && w < 1e-9)) ++n;
    }
    return n;
}

// Closed, no slivers, the volume adding up, sections that never cross, the
// size; then what makes it a pen holder and nothing else.
void checks(const std::string& what, const Params& in, const Png& png, const std::string& file) {
    const Params p = penholder::sane(in);
    const Built b = make(p);
    const penholder::Model& m = b.model;
    std::string why;
    check(m.ok, what + ": built", m.error);
    const bool shut = closed(m.body, why);
    check(shut, what + ": closed, every edge once each way",
          why.empty() ? num(static_cast<double>(m.body.faces.size())) + " faces" : why);
    if (b.body.empty()) return;
    const double v = volume(b.body);
    check(v > 0.0 && std::abs(v - m.volume) < 1e-3 * v,
          what + ": " + num(v / 1000.0) + " cm3, " + num(m.grams) + " g of PLA, " + num(m.metres) + " m of filament");
    DV3 sp(0.0);
    const double sliver = minArea(b.body, &sp);
    check(sliver > 1e-7, what + ": no triangle without area",
          "smallest " + num(sliver) + " mm2 at " + num(sp.x) + ", " + num(sp.y) + ", " + num(sp.z));
    bool ok = true;
    std::string where;
    double integral = 0.0;
    const DV3 size = b.hi - b.lo;
    const int steps = 400;
    const double dz = size.z / steps;
    for (int k = 0; k < steps; ++k) {
        const double z = b.lo.z + (k + 0.5) * dz;
        const std::vector<Seg> ss = slice(b.body, z);
        integral += area(ss) * dz;
        if (k % 10 == 4 && ok && crosses(ss, where)) {
            ok = false;
            where = "at z " + num(z) + ": " + where;
        }
    }
    check(ok, what + ": sections never cross themselves", where);
    check(std::abs(integral - v) < 0.01 * v, what + ": the sections' areas add up to the volume",
          num(integral / 1000.0) + " cm3");
    check(std::abs(m.size.x - size.x) < 1e-3 && std::abs(m.size.y - size.y) < 1e-3 && std::abs(m.size.z - size.z) < 1e-3 &&
              std::abs(b.lo.z) < 1e-4 && std::abs(size.z - p.height) < 1e-3,
          what + ": " + num(size.x) + " x " + num(size.y) + " x " + num(size.z) + " mm, as it reports, on the desk");
    check(static_cast<int>(m.centres.size()) == m.cells && m.cells > 0, what + ": " + std::to_string(m.cells) + " cells");

    // One pen a cell, nothing wider.
    const double cell = p.pen + p.play, rd = 0.5 * p.drain, Rc = cell / std::sqrt(3.0);
    const double zf = p.tipRoom + Rc - rd;
    const double zMid = 0.5 * (zf + m.lowestTop);
    double worstFlat = 0.0, worstCorner = 0.0, worstDrain = 0.0;
    int open = 0;
    double throat = 1e9;
    for (const glm::dvec2& c : m.centres) {
        worstFlat = std::max(worstFlat, std::abs(openingAt(b.body, zMid, c) - cell));
        worstDrain = std::max(worstDrain, std::abs(openingAt(b.body, 0.5 * p.tipRoom, c) - p.drain));
        if (hitsDown(b.body, c) == 0) ++open;
        // How high the throat goes: the opening no wider than the drain.
        double z = 0.02;
        while (z < zf && openingAt(b.body, z, c) < p.drain + 0.02) z += 0.02;
        throat = std::min(throat, z);
    }
    {
        // Corner to corner: along y through the front cell's middle.
        const glm::dvec2 c = m.centres.front();
        std::vector<double> ys;
        for (const Seg& s : slice(b.body, zMid)) {
            if ((s.p.x < c.x) == (s.q.x < c.x)) continue;
            ys.push_back(s.p.y + (s.q.y - s.p.y) * (c.x - s.p.x) / (s.q.x - s.p.x));
        }
        std::sort(ys.begin(), ys.end());
        for (std::size_t i = 1; i + 1 < ys.size(); i += 2)
            if (ys[i] < c.y && c.y < ys[i + 1]) worstCorner = ys[i + 1] - ys[i];
    }
    check(worstFlat < 1e-3, what + ": every cell " + num(cell) + " mm across its flats, the pen and its play",
          "off by " + num(worstFlat));
    check(std::abs(worstCorner - m.widest) < 1e-3 && m.widest < 16.25,
          what + ": " + num(worstCorner) + " mm corner to corner: not even a 1 cent coin (16.25 mm) goes in on edge");
    check(open == m.cells, what + ": no floor -- open from the top to the desk through every cell's middle",
          std::to_string(open) + " of " + std::to_string(m.cells));
    check(worstDrain < 1e-3, what + ": the drains " + num(p.drain) + " mm across", "off by " + num(worstDrain));
    const double point = throat - rd / std::tan(11.0 * 3.14159265358979 / 180.0);
    check(point > 0.5, what + ": a pencil's point (sharpened to 22 degrees) stands " + num(point) +
                           " mm above the desk in the " + num(throat) + " mm throat");

    // The top: every corner above the funnels on the slope asked for -- the
    // highest at the height, falling by tan(slope) a millimetre away from the
    // middle (or towards the front) -- and the shallowest cell 10 mm deep.
    {
        const double k = std::tan(p.slope * 3.14159265358979 / 180.0);
        const bool front = p.slopeTo == penholder::SlopeTo::Front;
        // The middle: the cells' middles in the mesh's plan.
        DV2 lo(1e30), hi(-1e30);
        for (const glm::dvec2& c : m.centres) {
            lo = glm::min(lo, DV2(c));
            hi = glm::max(hi, DV2(c));
        }
        const DV2 mid = 0.5 * (lo + hi);
        auto dist = [&](DV3 v) { return front ? -v.y : glm::length(DV2(v) - mid); };
        double ref = 1e30, zTop = 0.0;
        std::vector<DV3> tops;
        for (const glm::vec3& mv : m.body.verts) {
            const DV3 v = print(mv);
            if (v.z > zf + 0.5) {
                tops.push_back(v);
                ref = std::min(ref, dist(v));
                zTop = std::max(zTop, v.z);
            }
        }
        double off = 0.0;
        for (const DV3& v : tops) off = std::max(off, std::abs(v.z - (p.height - k * (dist(v) - ref))));
        check(!tops.empty() && off < 1e-4 && std::abs(zTop - p.height) < 1e-4,
              what + ": the top " + (p.slope > 0.0f ? "falls " + num(p.slope) + " degrees " +
                                                          (front ? "to the front" : "to the rim")
                                                    : std::string("is level")) +
                  ", " + num(m.lowestTop) + " to " + num(zTop) + " mm",
              "off by " + num(off));
        check(m.shallowest > 10.0 - 1e-3, what + ": the shallowest cell " + num(m.shallowest) + " mm deep");
    }
    // Nothing to put things on: the top is the walls' edges, each a wall thick.
    {
        const glm::dvec2 c = m.centres.front();
        const std::vector<double> xs = crossX(slice(b.body, m.lowestTop - 0.05), c.y);
        double widest = 0.0;
        for (std::size_t i = 0; i + 1 < xs.size(); i += 2) widest = std::max(widest, xs[i + 1] - xs[i]);
        check(!xs.empty() && widest < p.wall + 1e-3, what + ": along the top nothing wider than a wall (" +
                                                          num(widest) + " mm)");
    }
    // Hollow under the funnels: just above the foot only the walls and the
    // drains' tubes, as much plastic as at the top and the tubes' rings.
    {
        const double rt = rd + p.wall;
        const double z = (p.foot > 0.0f ? p.footHeight : 0.0) + 0.1;
        const double low = area(slice(b.body, z)), top = area(slice(b.body, m.lowestTop - 0.05));
        const double tubes = m.cells * 3.14159265358979 * (rt * rt - rd * rd);
        check(std::abs(low - top - tubes) < 0.02 * (top + tubes),
              what + ": hollow under the funnels: " + num(low) + " mm2 of plastic at " + num(z) +
                  " mm, the walls' " + num(top) + " and the tubes' " + num(tubes));
    }
    // No face leaning out further than 45 degrees above the desk.
    {
        double worst = 0.0;
        DV3 at(0.0);
        for (const Tri& t : b.body) {
            const DV3 n = glm::cross(t.b - t.a, t.c - t.a);
            const double l = glm::length(n);
            if (l <= 0.0 || n.z >= 0.0) continue;
            if (std::max({t.a.z, t.b.z, t.c.z}) < 1e-6) continue;   // on the bed
            const double down = -n.z / l;   // 1: facing straight down
            if (down > worst) {
                worst = down;
                at = (t.a + t.b + t.c) / 3.0;
            }
        }
        check(worst < std::sqrt(0.5) + 1e-6, what + ": no overhang beyond 45 degrees -- no support",
              num(std::acos(std::min(1.0, worst)) * 180.0 / 3.14159265358979) + " degrees from flat at " + num(at.x) +
                  ", " + num(at.y) + ", " + num(at.z));
    }

    if (!png.dir.empty() && !file.empty()) {
        const glm::dvec2 c = m.centres.front();
        picture(png.dir + "/penholder-" + file + ".png", b.body, c.x + 0.031,
                {0.5 * p.tipRoom, 0.5 * (p.tipRoom + zf), p.height - 1.0}, 6.0);
        const double sc = 6.0;
        int w = 20, h = 40;
        std::vector<std::pair<glm::vec2, glm::vec2>> boxes;
        for (const penholder::View& vw : m.views) {
            glm::vec2 l(1e9f), u(-1e9f);
            for (const auto& loop : vw.loops)
                for (const glm::vec2& q : loop) {
                    l = glm::min(l, q);
                    u = glm::max(u, q);
                }
            boxes.push_back({l, u});
            w += static_cast<int>((u.x - l.x) * sc) + 20;
            h = std::max(h, static_cast<int>((u.y - l.y) * sc) + 40);
        }
        Canvas cv(w, h);
        int x0 = 20;
        for (std::size_t k = 0; k < m.views.size(); ++k) {
            const auto [l, u] = boxes[k];
            for (const auto& loop : m.views[k].loops)
                for (std::size_t i = 0; i < loop.size(); ++i) {
                    const glm::vec2 a = loop[i], q = loop[(i + 1) % loop.size()];
                    cv.line(DV2(x0 + (a.x - l.x) * sc, h - 20 - (a.y - l.y) * sc),
                            DV2(x0 + (q.x - l.x) * sc, h - 20 - (q.y - l.y) * sc), {20, 20, 30});
                }
            x0 += static_cast<int>((u.x - l.x) * sc) + 20;
        }
        cv.save(png.dir + "/penholder-" + file + "-views.png");
    }
}

} // namespace

int main(int argc, char** argv) {
    Png png;
    if (argc > 1) {
        png.dir = argv[1];
        std::filesystem::create_directories(png.dir);
    }

    std::printf("Round\n");
    {
        Params p;
        checks("7 cells, the defaults", p, png, "round");
        p.rings = 0;
        checks("one cell", p, png, "single");
        p.rings = 2;
        checks("19 cells", p, png, "round19");
        p.rings = 3;
        p.foot = 0.0f;
        checks("37 cells, no foot", p, png, "");
    }
    std::printf("Rows\n");
    {
        Params p;
        p.layout = Layout::Rows;
        checks("4 x 2", p, png, "rows");
        p.columns = 5;
        p.rows = 1;
        checks("one row of 5", p, png, "row");
        p.columns = 1;
        p.rows = 4;
        checks("a column of 4", p, png, "");
        p.columns = 6;
        p.rows = 3;
        p.foot = 0.0f;
        checks("6 x 3, no foot", p, png, "");
        p.columns = 1;
        p.rows = 1;
        p.foot = 8.0f;
        checks("1 x 1 on a wide foot", p, png, "");
    }
    std::printf("Sloped\n");
    {
        Params p;
        p.slope = 15.0f;
        checks("7 cells falling 15 degrees to the rim", p, png, "slope-rim");
        p.rings = 2;
        p.slope = 20.0f;
        checks("19 cells falling 20 degrees to the rim", p, png, "slope-rim19");
        Params r;
        r.layout = Layout::Rows;
        r.columns = 5;
        r.rows = 3;
        r.slope = 25.0f;
        r.slopeTo = penholder::SlopeTo::Front;
        checks("5 x 3 falling 25 degrees to the front", r, png, "slope-front");
        r.slopeTo = penholder::SlopeTo::Rim;
        r.foot = 0.0f;
        checks("5 x 3 to the rim, no foot", r, png, "");
        Params s;
        s.rings = 3;
        s.slope = 45.0f;
        const Params q = penholder::sane(s);
        check(q.slope < 45.0f && q.slope > 0.0f, "45 degrees over 37 cells 50 mm high: kept to " + num(q.slope));
        checks("37 cells, as steep as leaves 10 mm", s, png, "");
        check(penholder::sane(q) == q, "kept the same when kept again");
    }
    std::printf("Sizes\n");
    {
        Params p;
        p.pen = 7.5f;
        p.play = 0.6f;
        p.drain = 2.0f;
        checks("pencils, 7.5 mm", p, png, "pencils");
        Params t;
        t.wall = 0.6f;
        checks("walls of 0.6 mm", t, png, "");
        t.wall = 2.0f;
        t.drain = 5.0f;
        checks("walls of 2 mm, 5 mm drains", t, png, "");
        Params f;
        f.pen = 40.0f;   // asked for a cup
        f.play = 2.0f;
        checks("a 40 mm 'pen' kept to the largest cell", f, png, "");
        check(penholder::sane(f).pen + penholder::sane(f).play <= penholder::kMaxCell + 1e-6f,
              "a cell is never wider than " + num(penholder::kMaxCell) + " mm across its flats");
        Params h;
        h.height = 5.0f;
        h.tipRoom = 2.0f;
        checks("as low as it goes", h, png, "");
        h.height = 150.0f;
        h.tipRoom = 20.0f;
        checks("tall, a long throat", h, png, "");
        Params s;
        s.pen = 4.0f;
        s.play = 0.3f;
        s.wall = 3.0f;
        s.drain = 6.0f;
        checks("the smallest cell, the walls and drain kept to fit", s, png, "");
    }
    std::printf("Large\n");
    {
        Params p;
        p.rings = 4;
        const auto t0 = std::chrono::steady_clock::now();
        const penholder::Model m = penholder::build(p);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::string why;
        const bool shut = closed(m.body, why);
        check(m.ok && shut && m.cells == 61 && ms < 2000.0, "61 cells: closed in " + num(ms) + " ms", why);
    }

    std::printf("Saved and loaded\n");
    {
        Params p;
        p.layout = Layout::Rows; p.rings = 2; p.columns = 5; p.rows = 3; p.pen = 9.5f; p.play = 0.8f;
        p.height = 70.0f; p.slope = 12.0f; p.slopeTo = penholder::SlopeTo::Front; p.wall = 1.2f; p.drain = 2.5f; p.tipRoom = 8.0f; p.foot = 6.0f; p.footHeight = 1.6f;
        p.color = {0.1f, 0.2f, 0.3f};
        p = penholder::sane(p);
        nlohmann::json j;
        penholder::saveParams(j, p);
        Params q;
        penholder::loadParams(nlohmann::json::parse(j.dump()), q);
        check(q == p, "every parameter comes back", j.dump());
        Params d;
        penholder::loadParams(nlohmann::json::object(), d);
        check(d == penholder::sane(Params{}), "nothing saved: the defaults");
        std::unique_ptr<ComponentBase> c = components::create("penHolder");
        check(c && dynamic_cast<PenHolderComponent*>(c.get()), "the component is registered as penHolder");
        check(penholder::autoName(Params{}) == "Pen holder 7 x 11 mm", "named " + penholder::autoName(Params{}));
    }

    std::printf("Through the 3MF export\n");
    auto export3 = [&](const Params& p, const std::string& file) {
        ModelLibrary models;
        Document doc;
        int counter = 1;
        const fitzel::AssetId mat = penholder::ensureMaterial(doc.materials(), p);
        std::vector<Entity> es = penholder::entities(p, mat, counter, glm::vec3(3.0f, 1.0f, -2.0f));
        check(es.size() == 2 && es[0].components.get<PenHolderComponent>(),
              file + ": a root keeping the parameters and the holder", es.empty() ? "" : es[0].name);
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
        const penholder::Model m = penholder::build(p);
        check(std::abs(mm.x - m.size.x) < 1e-2f && std::abs(mm.y - m.size.y) < 1e-2f && std::abs(mm.z - m.size.z) < 1e-2f,
              file + ": " + num(mm.x) + " x " + num(mm.y) + " x " + num(mm.z) + " mm on the bed");
        if (!png.dir.empty()) {
            std::string err;
            check(export3mf::write(png.dir + "/" + file + ".3mf", sh, o, err), "written as " + png.dir + "/" + file + ".3mf", err);
        }
    };
    {
        export3(Params{}, "penholder-round");
        Params r;
        r.layout = Layout::Rows;
        export3(r, "penholder-rows");
    }

    std::printf("\n%s (%d failed)\n", failures ? "FAILED" : "all ok", failures);
    return failures ? 1 : 0;
}
