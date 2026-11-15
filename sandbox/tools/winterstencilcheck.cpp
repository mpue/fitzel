// winterstencilcheck -- winter stencils (WinterStencil.hpp), without GL.
//
// Round, rectangular and arched ones in many settings and over many seeds,
// every one closed (every edge crossed once each way, no triangle without
// area) and one piece -- a stencil holds together: the stars, the moon and the
// snowflakes on their threads, the cabin and the firs on the snow, the snow on
// the frame -- its sections never crossing themselves and adding up to the
// volume, the size it reports. And what a plate that prints flat has to be,
// measured from the mesh: every face level or upright, the bottom as large as
// the top, as thick as asked, every opening at least 0.8 mm across. The same
// seed the same stencil, another seed another. Saved and loaded, and through
// the 3MF export as one closed part.
//
// With an out dir it writes .3mf files and winterstencil-*.png: the plate from
// above, dark against the light it would hang in.
//   build/linux/bin/winterstencilcheck [outdir]

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
#include "../src/WinterStencil.hpp"

using winterstencil::Frame;
using winterstencil::Params;
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


// --- Pictures -----------------------------------------------------------------------

// The plate from above, dark against warm light, `scale` pixels a millimetre;
// with `mark`, a red ring there.
void picture(const std::string& path, const winterstencil::Model& m, double scale, const glm::vec2* mark = nullptr) {
    glm::vec2 lo(1e30f), hi(-1e30f);
    for (const glm::vec2& q : m.tris) {
        lo = glm::min(lo, q);
        hi = glm::max(hi, q);
    }
    const int pad = 12;
    const int w = static_cast<int>((hi.x - lo.x) * scale) + 2 * pad, h = static_cast<int>((hi.y - lo.y) * scale) + 2 * pad;
    std::vector<unsigned char> px(static_cast<std::size_t>(w * h * 3));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            unsigned char* p = &px[static_cast<std::size_t>((y * w + x) * 3)];
            const double t = static_cast<double>(y) / h;
            p[0] = static_cast<unsigned char>(250 - 30 * t);
            p[1] = static_cast<unsigned char>(205 - 60 * t);
            p[2] = static_cast<unsigned char>(120 - 50 * t);
        }
    auto P = [&](glm::vec2 v) { return DV2(pad + (v.x - lo.x) * scale, h - pad - (v.y - lo.y) * scale); };
    for (std::size_t k = 0; k + 2 < m.tris.size(); k += 3) {
        const DV2 a = P(m.tris[k]), b = P(m.tris[k + 1]), c = P(m.tris[k + 2]);
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
                    p[0] = 28; p[1] = 30; p[2] = 38;
                }
            }
    }
    if (mark)
        for (int i = 0; i < 400; ++i) {
            const double t = i * 6.2831853 / 400.0;
            const DV2 q = P(*mark + 2.0f * glm::vec2(std::cos(t), std::sin(t)));
            const int x = static_cast<int>(q.x), y = static_cast<int>(q.y);
            if (x >= 0 && y >= 0 && x < w && y < h) {
                unsigned char* p = &px[static_cast<std::size_t>((y * w + x) * 3)];
                p[0] = 230; p[1] = 20; p[2] = 20;
            }
        }
    stbi_write_png(path.c_str(), w, h, 3, px.data(), w * 3);
}

struct Png {
    std::string dir;
};

// Closed, one piece, flat, no slivers, no thread over another, the openings
// open. Quietly: the failures only, `why` the first.
bool printable(const Params& in, const winterstencil::Model& m, std::string& why, bool loud, const std::string& what,
               double* ms = nullptr) {
    const Params p = winterstencil::sane(in);
    bool all = true;
    auto fail = [&](bool ok, const std::string& title, const std::string& detail = {}) {
        if (loud || !ok) check(ok, what + ": " + title, detail);
        if (!ok && all) why = title + " " + detail;
        all &= ok;
    };
    fail(m.ok, "built", m.error);
    std::string w;
    const bool shut = closed(m.body, w);
    fail(shut, "closed, every edge once each way", shut ? num(static_cast<double>(m.body.faces.size())) + " faces" : w);
    const std::vector<Tri> ts = tris(m.body);
    if (ts.empty()) return false;
    const int n = pieces(m.body);
    fail(n == 1 && m.pieces == 1 && m.dropped == 0, "one piece: nothing floats",
         std::to_string(n) + " pieces, " + std::to_string(m.dropped) + " specks left out, the first at " +
             num(m.droppedAt.x) + ", " + num(m.droppedAt.y));
    const double v = volume(ts);
    fail(v > 0.0 && std::abs(v - m.volume) < 1e-3 * v,
         num(v / 1000.0) + " cm3, " + num(m.grams) + " g of PLA, " + num(m.metres) + " m of filament");
    DV3 sp(0.0);
    const double sliver = minArea(ts, &sp);
    fail(sliver > 1e-7, "no triangle without area", "smallest " + num(sliver) + " mm2 at " + num(sp.x) + ", " + num(sp.y));
    const std::vector<Seg> ss = slice(ts, 0.5 * p.thickness);
    std::string where;
    fail(!crosses(ss, where), "the section never crosses itself", where);
    const double a = area(ss);
    fail(std::abs(a * p.thickness - v) < 1e-3 * v, "the section's " + num(a) + " mm2 times the thickness is the volume");
    {
        double tilted = 0.0, topA = 0.0, botA = 0.0, zlo = 1e30, zhi = -1e30;
        for (const Tri& t : ts) {
            const DV3 nr = glm::cross(t.b - t.a, t.c - t.a);
            const double l = glm::length(nr);
            for (const DV3& q : {t.a, t.b, t.c}) {
                zlo = std::min(zlo, q.z);
                zhi = std::max(zhi, q.z);
            }
            if (l <= 0.0) continue;
            const double z = nr.z / l;
            tilted = std::max(tilted, std::min(std::abs(z), 1.0 - std::abs(z)));
            if (z > 0.999) topA += 0.5 * l;
            if (z < -0.999) botA += 0.5 * l;
        }
        fail(tilted < 1e-6 && std::abs(topA - botA) < 1e-6 * topA && std::abs(zlo) < 1e-6 &&
                 std::abs(zhi - p.thickness) < 1e-4,
             "flat: every face level or upright, " + num(botA) + " mm2 on the bed under " + num(topA) + " on top, " +
                 num(zhi - zlo) + " mm thick");
    }
    fail(m.holes > 1 && m.smallestHole >= 0.8 - 1e-6,
         std::to_string(m.holes) + " openings, the narrowest " + num(m.smallestHole) + " mm across",
         "at " + num(m.smallestHoleAt.x) + ", " + num(m.smallestHoleAt.y));
    if (ms) fail(*ms < 2000.0, "built in " + num(*ms) + " ms");
    return all;
}

void checks(const std::string& what, const Params& in, const Png& png, const std::string& file, bool everything = false) {
    const Params p = winterstencil::sane(in);
    const auto t0 = std::chrono::steady_clock::now();
    const winterstencil::Model m = winterstencil::build(p);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::string why;
    printable(p, m, why, true, what, &ms);
    check(!everything || (m.trees == p.trees && m.stars == p.stars && m.flakes == p.flakes),
          what + ": " + std::to_string(m.trees) + " firs, " + std::to_string(m.stars) + " stars, " +
              std::to_string(m.flakes) + " snowflakes found room; " + num(m.size.x) + " x " + num(m.size.y) + " mm");
    if (!png.dir.empty() && !file.empty()) picture(png.dir + "/winterstencil-" + file + ".png", m, 5.0);
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
        checks("the defaults", p, png, "round", true);
        p.seed = 2;
        checks("another seed", p, png, "round-2", true);
        p.seed = 3;
        p.hills = 1.0f;
        checks("rolling hills", p, png, "round-3");
        Params s;
        s.width = 70.0f;
        checks("70 mm", s, png, "round-small");
        Params l;
        l.width = 240.0f;
        l.trees = 12;
        l.stars = 10;
        l.flakes = 6;
        checks("240 mm, crowded", l, png, "round-large");
        Params n;
        n.house = n.snowman = n.fence = false;
        n.trees = 10;
        checks("a wood under the stars", n, png, "round-wood");
        Params e;
        e.house = e.snowman = e.fence = e.moon = false;
        e.trees = e.stars = e.flakes = 0;
        checks("only snow", e, png, "");
    }
    std::printf("Rectangle and arch\n");
    {
        Params p;
        p.frame = Frame::Rect;
        checks("a rectangle", p, png, "rect", true);
        p.width = 200.0f;
        p.height = 100.0f;
        checks("200 x 100", p, png, "rect-wide");
        Params a;
        a.frame = Frame::Arch;
        a.height = 170.0f;
        checks("an arch", a, png, "arch", true);
        a.height = 50.0f;
        check(winterstencil::sane(a).height >= 0.7f * a.width, "an arch at least 0.7 as high as wide");
        checks("an arch asked too low", a, png, "");
    }
    std::printf("Kept to what prints\n");
    {
        Params p;
        p.line = 0.2f;
        p.border = 0.5f;
        p.thickness = 0.1f;
        const Params q = winterstencil::sane(p);
        check(q.line >= 0.8f && q.border >= 2.0f && q.thickness >= 0.6f,
              "lines two of a 0.4 mm nozzle, the frame 2 mm, the plate 0.6 mm: " + num(q.line) + ", " + num(q.border) +
                  ", " + num(q.thickness));
        check(winterstencil::sane(q) == q, "kept the same when kept again");
        checks("asked too thin", p, png, "");
        Params t;
        t.line = 3.0f;
        t.border = 10.0f;
        t.thickness = 4.0f;
        checks("heavy lines", t, png, "heavy");
    }
    std::printf("Seeds\n");
    {
        quiet = true;
        int good = 0, tried = 0;
        for (int frame = 0; frame < 3; ++frame)
            for (int seed = 1; seed <= 25; ++seed) {
                Params p;
                p.frame = static_cast<Frame>(frame);
                p.seed = seed;
                p.width = 70.0f + static_cast<float>((seed * 37) % 170);
                p.height = 60.0f + static_cast<float>((seed * 53) % 180);
                p.trees = seed % 13;
                p.stars = seed % 9;
                p.flakes = seed % 5;
                p.hills = static_cast<float>(seed % 5) / 4.0f;
                p.house = seed % 6 != 0;
                p.smoke = seed % 4 != 0;
                p.snowman = seed % 5 != 0;
                p.fence = seed % 3 != 0;
                p.moon = seed % 7 != 0;
                p.line = 0.8f + static_cast<float>(seed % 4) * 0.4f;
                const winterstencil::Model m = winterstencil::build(p);
                std::string why;
                const bool ok = printable(p, m, why, false,
                                          std::string(winterstencil::frameName(p.frame)) + " seed " + std::to_string(seed));
                ++tried;
                if (ok) ++good;
                else if (!png.dir.empty())
                    picture(png.dir + "/fail-" + std::to_string(frame) + "-" + std::to_string(seed) + ".png", m, 5.0,
                            m.dropped ? &m.droppedAt : &m.smallestHoleAt);
            }
        quiet = false;
        check(good == tried, std::to_string(good) + " of " + std::to_string(tried) + " stencils of every frame, seed and setting print");
        Params a;
        a.seed = 5;
        const winterstencil::Model ma = winterstencil::build(a), mb = winterstencil::build(a);
        check(ma.body.verts == mb.body.verts && ma.body.faces == mb.body.faces, "the same seed, the same stencil");
        a.seed = 6;
        check(winterstencil::build(a).body.verts != ma.body.verts, "another seed, another stencil");
    }

    std::printf("Saved and loaded\n");
    {
        Params p;
        p.frame = Frame::Arch; p.width = 130.0f; p.height = 180.0f; p.border = 4.0f; p.thickness = 2.0f; p.eyelet = false;
        p.seed = 42; p.hills = 0.8f; p.trees = 4; p.house = false; p.smoke = false; p.snowman = false; p.fence = false;
        p.moon = false; p.stars = 7; p.flakes = 2; p.line = 1.6f; p.color = {0.1f, 0.2f, 0.3f};
        p = winterstencil::sane(p);
        nlohmann::json j;
        winterstencil::saveParams(j, p);
        Params q;
        winterstencil::loadParams(nlohmann::json::parse(j.dump()), q);
        check(q == p, "every parameter comes back", j.dump());
        Params d;
        winterstencil::loadParams(nlohmann::json::object(), d);
        check(d == winterstencil::sane(Params{}), "nothing saved: the defaults");
        std::unique_ptr<ComponentBase> c = components::create("winterStencil");
        check(c && dynamic_cast<WinterStencilComponent*>(c.get()), "the component is registered as winterStencil");
        check(winterstencil::autoName(Params{}) == "Winter stencil round 150 mm", "named " + winterstencil::autoName(Params{}));
    }

    std::printf("Through the 3MF export\n");
    auto export3 = [&](const Params& p, const std::string& file) {
        ModelLibrary models;
        Document doc;
        int counter = 1;
        const fitzel::AssetId mat = winterstencil::ensureMaterial(doc.materials(), p);
        std::vector<Entity> es = winterstencil::entities(p, mat, counter, glm::vec3(3.0f, 1.0f, -2.0f));
        check(es.size() == 2 && es[0].components.get<WinterStencilComponent>(),
              file + ": a root keeping the parameters and the stencil", es.empty() ? "" : es[0].name);
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
        const winterstencil::Model m = winterstencil::build(p);
        check(std::abs(mm.x - m.size.x) < 1e-2f && std::abs(mm.y - m.size.y) < 1e-2f && std::abs(mm.z - m.size.z) < 1e-2f,
              file + ": " + num(mm.x) + " x " + num(mm.y) + " x " + num(mm.z) + " mm on the bed");
        if (!png.dir.empty()) {
            std::string err;
            check(export3mf::write(png.dir + "/" + file + ".3mf", sh, o, err), "written as " + png.dir + "/" + file + ".3mf", err);
        }
    };
    {
        export3(Params{}, "winterstencil-round");
        Params a;
        a.frame = Frame::Arch;
        a.height = 170.0f;
        export3(a, "winterstencil-arch");
    }

    std::printf("\n%s (%d failed)\n", failures ? "FAILED" : "all ok", failures);
    return failures ? 1 : 0;
}
