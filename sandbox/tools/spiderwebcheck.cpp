// spiderwebcheck -- spider webs (SpiderWeb.hpp), without GL.
//
// Round, framed and corner webs in many settings and over many seeds, every
// one closed (every edge crossed once each way, no triangle without area) and
// one piece, its sections never crossing themselves (no two threads lying over
// each other) and adding up to the volume, the size it reports. And what a web
// that prints flat has to be, measured from the mesh: every face level or
// upright, the bottom on the bed as large as the top (nothing hanging over),
// the plate as thick as asked, holes between the threads at least a millimetre
// across. The same seed the same web, another seed another; torn ones still
// one piece. Saved and loaded, and through the 3MF export as one closed part.
//
// With an out dir it writes .3mf files for a slicer and spiderweb-*.png: the
// plate from above, filled from its top triangles.
//   build/linux/bin/spiderwebcheck [outdir]

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <numeric>
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
#include "../src/SceneGraph.hpp"
#include "../src/SpiderWeb.hpp"

using spiderweb::Ends;
using spiderweb::Params;
using spiderweb::Shape;
using DV2 = glm::dvec2;
using DV3 = glm::dvec3;

namespace {

int failures = 0;
bool quiet = false;   // the seed sweeps print their failures only
void check(bool ok, const std::string& what, const std::string& detail = {}) {
    if (!ok || !quiet)
        std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what.c_str(), detail.empty() ? "" : "  -- ", detail.c_str());
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

// How many pieces the faces make, joined through shared corners.
int pieces(const EditMesh& m) {
    std::vector<int> up(m.verts.size());
    std::iota(up.begin(), up.end(), 0);
    auto root = [&](int v) {
        while (up[static_cast<std::size_t>(v)] != v) v = up[static_cast<std::size_t>(v)] = up[static_cast<std::size_t>(up[static_cast<std::size_t>(v)])];
        return v;
    };
    std::vector<char> used(m.verts.size(), 0);
    for (const std::vector<int>& f : m.faces)
        for (int v : f) {
            used[static_cast<std::size_t>(v)] = 1;
            up[static_cast<std::size_t>(root(v))] = root(f[0]);
        }
    int n = 0;
    for (std::size_t v = 0; v < m.verts.size(); ++v)
        if (used[v] && root(static_cast<int>(v)) == static_cast<int>(v)) ++n;
    return n;
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
        const DV3 nrm = glm::cross(t.b - t.a, t.c - t.a);
        const DV2 dir(-nrm.y, nrm.x);
        if (glm::dot(pts[1] - pts[0], dir) < 0.0) std::swap(pts[0], pts[1]);
        out.push_back({pts[0], pts[1]});
    }
    return out;
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
    std::iota(order.begin(), order.end(), 0);
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

bool crossesAt(const std::vector<Seg>& ss, DV2& at) {
    std::string where;
    if (!crosses(ss, where)) return false;
    std::sscanf(where.c_str(), "%lf, %lf", &at.x, &at.y);
    return true;
}

// --- Pictures -----------------------------------------------------------------------

// The plate from above: its top triangles filled, `scale` pixels a millimetre;
// with `mark`, a red ring round that place (mm) and only 40 mm round it.
void picture(const std::string& path, const std::vector<Tri>& ts, double scale, const DV2* mark = nullptr) {
    DV3 lo, hi;
    bounds(ts, lo, hi);
    if (mark) {
        lo = glm::max(lo, DV3(*mark - DV2(20.0), lo.z));
        hi = glm::min(hi, DV3(*mark + DV2(20.0), hi.z));
    }
    const int pad = 12;
    const int w = static_cast<int>((hi.x - lo.x) * scale) + 2 * pad, h = static_cast<int>((hi.y - lo.y) * scale) + 2 * pad;
    std::vector<unsigned char> px(static_cast<std::size_t>(w * h * 3), 0);
    for (std::size_t i = 0; i < px.size(); i += 3) {
        px[i] = 236;
        px[i + 1] = 232;
        px[i + 2] = 224;
    }
    for (const Tri& t : ts) {
        if (t.a.z < hi.z - 1e-6 || t.b.z < hi.z - 1e-6 || t.c.z < hi.z - 1e-6) continue;
        auto P = [&](const DV3& v) { return DV2(pad + (v.x - lo.x) * scale, h - pad - (v.y - lo.y) * scale); };
        const DV2 a = P(t.a), b = P(t.b), c = P(t.c);
        const int x0 = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
        const int x1 = std::min(w - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
        const int y0 = std::max(0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
        const int y1 = std::min(h - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
        auto e = [](DV2 p, DV2 q, DV2 r) { return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x); };
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const DV2 q(x + 0.5, y + 0.5);
                const double u = e(a, b, q), v = e(b, c, q), s = e(c, a, q);
                if ((u >= 0 && v >= 0 && s >= 0) || (u <= 0 && v <= 0 && s <= 0)) {
                    unsigned char* p = &px[static_cast<std::size_t>((y * w + x) * 3)];
                    p[0] = 30;
                    p[1] = 32;
                    p[2] = 40;
                }
            }
    }
    if (mark)
        for (int i = 0; i < 400; ++i) {
            const double a = i * 6.2831853 / 400.0;
            const int x = static_cast<int>(pad + (mark->x + 1.5 * std::cos(a) - lo.x) * scale);
            const int y = static_cast<int>(h - pad - (mark->y + 1.5 * std::sin(a) - lo.y) * scale);
            if (x >= 0 && y >= 0 && x < w && y < h) {
                unsigned char* p = &px[static_cast<std::size_t>((y * w + x) * 3)];
                p[0] = 230;
                p[1] = 30;
                p[2] = 30;
            }
        }
    stbi_write_png(path.c_str(), w, h, 3, px.data(), w * 3);
}

struct Png {
    std::string dir;
};

// --- One web ------------------------------------------------------------------------

struct Built {
    spiderweb::Model model;
    std::vector<Tri> body;
    DV3 lo{0.0}, hi{0.0};
};

Built make(const Params& p) {
    Built b;
    b.model = spiderweb::build(p);
    b.body = tris(b.model.body);
    if (!b.body.empty()) bounds(b.body, b.lo, b.hi);
    return b;
}

void checks(const std::string& what, const Params& in, const Png& png, const std::string& file) {
    const Params p = spiderweb::sane(in);
    const Built b = make(p);
    const spiderweb::Model& m = b.model;
    std::string why;
    check(m.ok, what + ": built", m.error);
    const bool shut = closed(m.body, why);
    check(shut, what + ": closed, every edge once each way",
          why.empty() ? num(static_cast<double>(m.body.faces.size())) + " faces" : why);
    if (b.body.empty()) return;
    const int n = pieces(m.body);
    check(n == 1, what + ": one piece", std::to_string(n) + " pieces");
    const double v = volume(b.body);
    check(v > 0.0 && std::abs(v - m.volume) < 1e-3 * v,
          what + ": " + num(v / 1000.0) + " cm3, " + num(m.grams) + " g of PLA, " + num(m.metres) + " m of filament");
    DV3 sp(0.0);
    const double sliver = minArea(b.body, &sp);
    check(sliver > 1e-6, what + ": no triangle without area",
          "smallest " + num(sliver) + " mm2 at " + num(sp.x) + ", " + num(sp.y) + ", " + num(sp.z));
    // No two threads over each other: sections that never cross themselves.
    bool ok = true;
    std::string where;
    for (double f : {0.25, 0.5, 0.75}) {
        const std::vector<Seg> ss = slice(b.body, f * p.thickness);
        if (ok && crosses(ss, where)) {
            ok = false;
            where = "at z " + num(f * p.thickness) + ": " + where;
        }
        if (f == 0.5) {
            const double a = area(ss);
            check(std::abs(a * p.thickness - v) < 1e-3 * v && std::abs(a - m.area) < 1e-3 * a,
                  what + ": the section's " + num(a) + " mm2 times the thickness is the volume");
        }
    }
    check(ok, what + ": sections never cross themselves -- no thread lies over another", where);
    const DV3 size = b.hi - b.lo;
    check(std::abs(m.size.x - size.x) < 1e-3 && std::abs(m.size.y - size.y) < 1e-3 && std::abs(b.lo.z) < 1e-6 &&
              std::abs(size.z - p.thickness) < 1e-4 && std::abs(m.size.z - size.z) < 1e-4,
          what + ": " + num(size.x) + " x " + num(size.y) + " x " + num(size.z) + " mm, as it reports, on the bed");
    // Flat: every face level or upright, the bottom as large as the top.
    {
        double tilted = 0.0, topA = 0.0, botA = 0.0;
        for (const Tri& t : b.body) {
            const DV3 nr = glm::cross(t.b - t.a, t.c - t.a);
            const double l = glm::length(nr);
            if (l <= 0.0) continue;
            const double z = nr.z / l;
            tilted = std::max(tilted, std::min(std::abs(z), 1.0 - std::abs(z)));
            if (z > 0.999) topA += 0.5 * l;
            if (z < -0.999) botA += 0.5 * l;
        }
        check(tilted < 1e-6 && std::abs(topA - botA) < 1e-6 * topA,
              what + ": every face level or upright, " + num(botA) + " mm2 on the bed under " + num(topA) +
                  " on top -- nothing hangs over");
    }
    check(m.holes > m.spokes && m.smallestHole >= 1.0 - 1e-6,
          what + ": " + std::to_string(m.holes) + " holes between the threads, the narrowest " +
              num(m.smallestHole) + " mm across",
          "at " + num(m.smallestHoleAt.x) + ", " + num(m.smallestHoleAt.y));
    check(m.spokes == p.spokes && m.turns >= 1,
          what + ": " + std::to_string(m.spokes) + " radii, " + std::to_string(m.turns) + " turns, " +
              std::to_string(m.threads) + " threads between " + std::to_string(m.knots) + " knots");
    if (!png.dir.empty() && !file.empty()) picture(png.dir + "/spiderweb-" + file + ".png", b.body, 4.0);
}

// Only what decides whether it prints at all, for the seed sweeps.
bool printable(const Params& p, std::string& why, const std::string* pic = nullptr) {
    const Built b = make(p);
    const spiderweb::Model& m = b.model;
    if (!m.ok) { why = m.error; return false; }
    if (!closed(m.body, why)) return false;
    if (pieces(m.body) != 1) { why = "more than one piece"; return false; }
    if (minArea(b.body) <= 1e-6) { why = "a triangle without area"; return false; }
    const std::vector<Seg> ss = slice(b.body, 0.5 * spiderweb::sane(p).thickness);
    std::string where;
    DV2 at;
    if (crossesAt(ss, at)) {
        why = "threads over each other at " + num(at.x) + ", " + num(at.y);
        if (pic) picture(*pic, b.body, 12.0, &at);
        return false;
    }
    if (m.smallestHole < 1.0 - 1e-6) {
        at = DV2(m.smallestHoleAt);
        why = "a hole " + num(m.smallestHole) + " mm across at " + num(at.x) + ", " + num(at.y);
        if (pic) picture(*pic, b.body, 12.0, &at);
        return false;
    }
    return true;
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
        checks("in a hoop, the defaults", p, png, "round");
        check(spiderweb::build(p).size.x < 256.0f && spiderweb::build(p).size.y < 256.0f, "the defaults fit a 256 mm bed");
        p.eyelet = false;
        checks("no loop to hang it by", p, png, "");
        Params e;
        e.irregular = 0.0f;
        e.sag = 0.0f;
        checks("even, straight threads", e, png, "round-even");
        Params w;
        w.irregular = 1.0f;
        w.sag = 0.3f;
        w.seed = 7;
        checks("as uneven as it goes, drooping", w, png, "round-wild");
        Params t;
        t.torn = 0.3f;
        t.seed = 3;
        checks("torn: a third of the capture threads missing", t, png, "round-torn");
        check(spiderweb::build(t).threads < spiderweb::build(Params{}).threads, "torn has fewer threads");
        Params d;
        d.spokes = 40;
        d.spacing = 3.0f;
        d.spiralWidth = 0.8f;
        d.spokeWidth = 1.0f;
        checks("40 radii, the spiral 3 mm apart", d, png, "round-dense");
        Params f;
        f.spokes = 6;
        f.spacing = 12.0f;
        checks("6 radii, 12 mm apart", f, png, "");
        Params s;
        s.size = 40.0f;
        checks("40 mm across", s, png, "round-small");
        Params l;
        l.size = 250.0f;
        l.spokes = 32;
        checks("250 mm across, 32 radii", l, png, "");
        Params h;
        h.hubTurns = 0;
        h.freeZone = 0.0f;
        checks("no hub turns, no free zone", h, png, "");
        Params b;
        b.spiralWidth = 2.0f;
        b.spokeWidth = 3.0f;
        b.frameWidth = 6.0f;
        b.thickness = 3.0f;
        checks("thick threads", b, png, "");
        Params n;
        n.spiralWidth = n.spokeWidth = n.frameWidth = 0.8f;
        n.thickness = 0.4f;
        checks("every thread two lines, two layers", n, png, "");
    }
    std::printf("Framed\n");
    {
        Params p;
        p.shape = Shape::Frame;
        checks("five corners, pads", p, png, "frame");
        p.ends = Ends::Loops;
        checks("loops", p, png, "frame-loops");
        p.ends = Ends::Free;
        p.corners = 3;
        checks("three corners, free ends", p, png, "");
        p.corners = 8;
        p.irregular = 1.0f;
        p.seed = 11;
        checks("eight corners, uneven", p, png, "frame-8");
        p.corners = 12;
        p.spokes = 6;
        check(spiderweb::sane(p).corners == 6, "no more corners than radii");
        checks("six radii, as many corners", p, png, "");
    }
    std::printf("Corner\n");
    {
        Params p;
        p.shape = Shape::Corner;
        p.spokes = 7;
        checks("seven radii between the walls", p, png, "corner");
        p.spokes = 2;
        checks("two radii", p, png, "");
        p.spokes = 12;
        p.size = 200.0f;
        checks("as many radii as fit, 200 mm walls", p, png, "");
        Params t;
        t.shape = Shape::Corner;
        t.spokes = 8;
        t.torn = 0.4f;
        t.irregular = 1.0f;
        t.sag = 0.3f;
        checks("torn, uneven, drooping", t, png, "corner-torn");
        Params s;
        s.shape = Shape::Corner;
        s.size = 40.0f;
        checks("40 mm walls", s, png, "");
        Params c;
        c.shape = Shape::Corner;
        c.spokes = 20;
        check(spiderweb::sane(c).spokes <= 12, "a corner takes up to 12 radii: kept to " + std::to_string(spiderweb::sane(c).spokes));
    }
    std::printf("Kept to what prints\n");
    {
        Params p;
        p.spiralWidth = 0.2f;
        p.spacing = 0.5f;
        p.sag = 1.0f;
        const Params q = spiderweb::sane(p);
        check(q.spiralWidth >= 0.8f && q.spacing >= q.spiralWidth + 1.5f && q.sag <= 0.3f,
              "threads two lines at least, a hole between two turns: " + num(q.spiralWidth) + " mm threads " +
                  num(q.spacing) + " mm apart, drooping " + num(q.sag));
        checks("asked for threads too thin and too close", p, png, "");
        check(spiderweb::sane(q) == q, "kept the same when kept again");
        Params m;
        m.spokes = 100;
        m.size = 60.0f;
        check(spiderweb::sane(m).spokes < 48, "as many radii as leave a small hub: " + std::to_string(spiderweb::sane(m).spokes));
        checks("100 radii asked for on 60 mm", m, png, "");
    }
    std::printf("Seeds\n");
    {
        quiet = true;
        int good = 0, tried = 0;
        for (int shape = 0; shape < 3; ++shape)
            for (int seed = 1; seed <= 40; ++seed) {
                Params p;
                p.shape = static_cast<Shape>(shape);
                p.seed = seed;
                p.irregular = static_cast<float>(seed % 5) / 4.0f;
                p.sag = static_cast<float>(seed % 4) / 10.0f;
                p.torn = static_cast<float>(seed % 3) / 5.0f;
                p.spokes = shape == 2 ? 2 + seed % 9 : 8 + seed % 30;
                p.spacing = 3.0f + static_cast<float>(seed % 7);
                p.size = 60.0f + static_cast<float>((seed * 37) % 180);
                p.corners = 3 + seed % 6;
                p.ends = static_cast<Ends>(seed % 3);
                p.hubTurns = seed % 4;
                p.freeZone = static_cast<float>(seed % 3) * 4.0f;
                std::string why;
                const std::string pic = png.dir + "/fail-" + std::to_string(shape) + "-" + std::to_string(seed) + ".png";
                const bool ok = printable(p, why, png.dir.empty() ? nullptr : &pic);
                ++tried;
                if (ok) ++good;
                else check(false, std::string(spiderweb::shapeName(p.shape)) + " seed " + std::to_string(seed), why);
            }
        quiet = false;
        check(good == tried, std::to_string(good) + " of " + std::to_string(tried) + " webs of every shape, seed and setting print");
        Params a, b;
        a.seed = b.seed = 5;
        const spiderweb::Model ma = spiderweb::build(a), mb = spiderweb::build(b);
        check(ma.body.verts == mb.body.verts && ma.body.faces == mb.body.faces, "the same seed, the same web");
        b.seed = 6;
        check(spiderweb::build(b).body.verts != ma.body.verts, "another seed, another web");
    }
    std::printf("Large\n");
    {
        Params p;
        p.size = 250.0f;
        p.spokes = 48;
        p.spacing = 2.5f;
        p.spiralWidth = 0.8f;
        p.spokeWidth = 0.8f;
        const auto t0 = std::chrono::steady_clock::now();
        const spiderweb::Model m = spiderweb::build(p);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::string why;
        const bool shut = closed(m.body, why);
        check(m.ok && shut && ms < 1500.0,
              "250 mm, " + std::to_string(m.spokes) + " radii, " + std::to_string(m.turns) + " turns: " +
                  std::to_string(m.body.faces.size()) + " faces, closed in " + num(ms) + " ms",
              m.ok ? why : m.error);
    }

    std::printf("Saved and loaded\n");
    {
        Params p;
        p.shape = Shape::Frame; p.size = 120.0f; p.spokes = 17; p.spacing = 4.5f; p.hubTurns = 3; p.freeZone = 9.0f;
        p.sag = 0.2f; p.irregular = 0.6f; p.seed = 42; p.torn = 0.1f; p.spiralWidth = 1.1f; p.spokeWidth = 1.5f;
        p.frameWidth = 2.0f; p.thickness = 1.6f; p.eyelet = false; p.corners = 4; p.anchor = 15.0f; p.ends = Ends::Loops;
        p.color = {0.1f, 0.2f, 0.3f};
        p = spiderweb::sane(p);
        nlohmann::json j;
        spiderweb::saveParams(j, p);
        Params q;
        spiderweb::loadParams(nlohmann::json::parse(j.dump()), q);
        check(q == p, "every parameter comes back", j.dump());
        Params d;
        spiderweb::loadParams(nlohmann::json::object(), d);
        check(d == spiderweb::sane(Params{}), "nothing saved: the defaults");
        std::unique_ptr<ComponentBase> c = components::create("spiderWeb");
        check(c && dynamic_cast<SpiderWebComponent*>(c.get()), "the component is registered as spiderWeb");
        check(spiderweb::autoName(Params{}) == "Spider web round 150 mm", "named " + spiderweb::autoName(Params{}));
    }

    std::printf("Through the 3MF export\n");
    auto export3 = [&](const Params& p, const std::string& file) {
        ModelLibrary models;
        Document doc;
        int counter = 1;
        const fitzel::AssetId mat = spiderweb::ensureMaterial(doc.materials(), p);
        std::vector<Entity> es = spiderweb::entities(p, mat, counter, glm::vec3(3.0f, 1.0f, -2.0f));
        check(es.size() == 2 && es[0].components.get<SpiderWebComponent>(),
              file + ": a root keeping the parameters and the web", es.empty() ? "" : es[0].name);
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
        const spiderweb::Model m = spiderweb::build(p);
        check(std::abs(mm.x - m.size.x) < 1e-2f && std::abs(mm.y - m.size.y) < 1e-2f && std::abs(mm.z - m.size.z) < 1e-2f,
              file + ": " + num(mm.x) + " x " + num(mm.y) + " x " + num(mm.z) + " mm on the bed");
        if (!png.dir.empty()) {
            std::string err;
            check(export3mf::write(png.dir + "/" + file + ".3mf", sh, o, err), "written as " + png.dir + "/" + file + ".3mf", err);
        }
    };
    {
        export3(Params{}, "spiderweb-round");
        Params f;
        f.shape = Shape::Frame;
        export3(f, "spiderweb-frame");
        Params c;
        c.shape = Shape::Corner;
        c.spokes = 7;
        export3(c, "spiderweb-corner");
    }

    std::printf("\n%s (%d failed)\n", failures ? "FAILED" : "all ok", failures);
    return failures ? 1 : 0;
}
