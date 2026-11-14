// cableholdercheck -- cable holders (CableHolder.hpp) and the solids they are
// built of (SolidBuild.hpp), without GL.
//
// Every kind in many settings: the holder and its labels each closed (every
// edge crossed once each way, no triangle without area), sections at many
// heights that never cross themselves and add up to the volume, the size it
// reports. And what each is for, measured from the mesh: a comb's seats as wide
// as the cable and the clearance, its necks narrower by the share asked for;
// a desk clip one piece with its bar, the gap to its lip the desk's thickness
// less the grip; a tray's inside as wide as asked; a hook's plate cut by its
// screw holes; a winder's notches as deep; labels standing in front of the face
// they are on. Saved and loaded, and through the 3MF export as two closed parts
// in two colours.
//
// With an out dir it writes .3mf files for a slicer and cableholder-*.png:
// a vertical cut and horizontal sections, drawn from the triangles.
//   build/linux/bin/cableholdercheck [outdir]

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

#include "../src/CableHolder.hpp"
#include "../src/Component.hpp"
#include "../src/Document.hpp"
#include "../src/EditMesh.hpp"
#include "../src/Export3mf.hpp"
#include "../src/ModelLibrary.hpp"
#include "../src/SceneGraph.hpp"
#include "../src/SolidBuild.hpp"

using cableholder::Params;
using cableholder::Style;
using cableholder::Mount;
using cableholder::HookMount;
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
    cableholder::Model model;
    std::vector<Tri> body, labels;
    DV3 lo{0.0}, hi{0.0};   // the body's bounds
};

Built make(const Params& p) {
    Built b;
    b.model = cableholder::build(p);
    b.body = tris(b.model.body);
    b.labels = tris(b.model.labels);
    if (!b.body.empty()) bounds(b.body, b.lo, b.hi);
    return b;
}

// Where the outlines of the cut x = x0 (see solidbuild::section) cross the line
// at `fixed` across them: z values along y = fixed (`alongZ`), or y values
// along z = fixed.
std::vector<double> crossCut(const EditMesh& m, double x0, double fixed, bool alongZ) {
    std::vector<double> out;
    for (const auto& loop : solidbuild::section(m, static_cast<float>(x0)))
        for (std::size_t i = 0; i < loop.size(); ++i) {
            const glm::dvec2 a(loop[i]), b(loop[(i + 1) % loop.size()]);
            const double ka = alongZ ? a.x : a.y, kb = alongZ ? b.x : b.y;
            if ((ka < fixed) == (kb < fixed)) continue;
            const double f = (fixed - ka) / (kb - ka);
            out.push_back(alongZ ? a.y + (b.y - a.y) * f : a.x + (b.x - a.x) * f);
        }
    std::sort(out.begin(), out.end());
    return out;
}

// Closed, no slivers, the volume adding up, sections that never cross, the size.
void solidChecks(const std::string& what, const Params& p, const Png& png, const std::string& file) {
    const Built b = make(p);
    const cableholder::Model& m = b.model;
    std::string why;
    check(m.ok, what + ": built", m.error);
    const bool shut = closed(m.body, why);
    check(shut, what + ": the holder is closed, every edge once each way",
          why.empty() ? num(static_cast<double>(m.body.faces.size())) + " faces" : why);
    const double v = volume(b.body);
    check(v > 0.0 && std::abs(v - m.volume) < 1e-3 * v, what + ": volume " + num(v / 1000.0) + " cm3");
    DV3 sp(0.0);
    const double sliver = minArea(b.body, &sp);
    check(sliver > 1e-7, what + ": no triangle without area",
          "smallest " + num(sliver) + " mm2 at " + num(sp.x) + ", " + num(sp.y) + ", " + num(sp.z));
    if (!m.labels.faces.empty()) {
        why.clear();
        const bool labelsShut = closed(m.labels, why);
        check(labelsShut, what + ": the labels are closed",
              why.empty() ? num(static_cast<double>(m.labels.faces.size())) + " faces" : why);
        check(volume(b.labels) > 0.0 && minArea(b.labels) > 1e-9, what + ": the labels have volume, no flat triangle");
    }
    bool ok = true;
    std::string where;
    double integral = 0.0;
    const DV3 size = b.hi - b.lo;
    const int steps = 300;
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
    DV3 lo = b.lo, hi = b.hi;
    if (!b.labels.empty()) {
        DV3 l2, h2;
        bounds(b.labels, l2, h2);
        lo = glm::min(lo, l2);
        hi = glm::max(hi, h2);
    }
    const glm::vec3 all(hi - lo);
    check(std::abs(m.size.x - all.x) < 1e-3f && std::abs(m.size.y - all.y) < 1e-3f && std::abs(m.size.z - all.z) < 1e-3f,
          what + ": " + num(all.x) + " x " + num(all.y) + " x " + num(all.z) + " mm, as it reports");
    if (!png.dir.empty() && !file.empty()) {
        picture(png.dir + "/cableholder-" + file + ".png", b.body, 0.5 * (b.lo.x + b.hi.x) + 0.37,
                {b.lo.z + 0.3 * size.z, b.lo.z + 0.7 * size.z}, 5.0);
        // The views the panel draws, side by side.
        const double sc = 6.0;
        int w = 20, h = 40;
        std::vector<std::pair<glm::vec2, glm::vec2>> boxes;
        for (const cableholder::View& vw : m.views) {
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
                    const glm::vec2 a = loop[i], c = loop[(i + 1) % loop.size()];
                    cv.line(DV2(x0 + (a.x - l.x) * sc, h - 20 - (a.y - l.y) * sc),
                            DV2(x0 + (c.x - l.x) * sc, h - 20 - (c.y - l.y) * sc), {20, 20, 30});
                }
            x0 += static_cast<int>((u.x - l.x) * sc) + 20;
        }
        cv.save(png.dir + "/cableholder-" + file + "-views.png");
    }
}

} // namespace

int main(int argc, char** argv) {
    Png png;
    if (argc > 1) {
        png.dir = argv[1];
        std::filesystem::create_directories(png.dir);
    }

    std::printf("Combs\n");
    {
        Params p;
        solidChecks("4 x 5 mm, free-standing", p, png, "comb");
        p.mount = Mount::Adhesive;
        solidChecks("on an adhesive pad", p, png, "comb-adhesive");
        p.mount = Mount::Magnets;
        solidChecks("on magnets", p, png, "comb-magnets");
        p.mount = Mount::Screws;
        solidChecks("screwed down", p, png, "comb-screws");
        Params m;
        m.slots = 6;
        m.diameters = "3.5, 5, 5, 6.5, 8, 2.5";
        m.labels = "USB-C, Lightning, HDMI, 230V, LAN, Audio";
        m.labelHeight = 3.0f;
        solidChecks("6 seats of 2.5 to 8 mm, labelled", m, png, "comb-labels");
        Params u;
        u.slots = 3;
        u.neck = 1.0f;
        u.diameters = "6";
        solidChecks("open U seats", u, png, "comb-u");
        Params r;
        r.rounding = 0.0f;
        solidChecks("no rounding", r, png, "");
        Params t;
        t.slots = 12;
        t.diameters = "3";
        t.tooth = 1.0f;
        t.margin = 1.5f;
        t.rounding = 3.0f;
        solidChecks("12 seats of 3 mm, thin teeth, rounding asked too big", t, png, "comb-thin");

        // The seats and necks as asked, measured half way back.
        const Built b = make(Params{});
        const Params s = cableholder::sane(Params{});
        const double rr = 0.5 * (5.0 + s.clearance);
        bool seatsOk = true, necksOk = true;
        std::string detail;
        const double D = s.depth;
        const std::vector<double> xs = crossX(slice(b.body, s.base + rr), 0.5 * D);
        // Free-standing: the bar from 0 to W with four gaps, each a seat.
        if (xs.size() != 10) {
            seatsOk = false;
            detail = std::to_string(xs.size()) + " crossings";
        } else {
            for (int i = 0; i < 4; ++i) {
                const double w = xs[static_cast<std::size_t>(2 * i + 2)] - xs[static_cast<std::size_t>(2 * i + 1)];
                if (std::abs(w - 2.0 * rr) > 0.05) seatsOk = false;
                detail += num(w) + " ";
            }
        }
        check(seatsOk, "the seats as wide as 5 mm and the 0.3 mm clearance", detail);
        const double topZ = b.hi.z;
        const std::vector<double> xn = crossX(slice(b.body, topZ - 1.5), 0.5 * D);
        detail.clear();
        if (xn.size() != 10) {
            necksOk = false;
            detail = std::to_string(xn.size()) + " crossings";
        } else {
            for (int i = 0; i < 4; ++i) {
                const double w = xn[static_cast<std::size_t>(2 * i + 2)] - xn[static_cast<std::size_t>(2 * i + 1)];
                if (std::abs(w - 2.0 * s.neck * rr) > 0.01) necksOk = false;
                detail += num(w) + " ";
            }
        }
        check(necksOk, "the necks 75 % of the seats: the cable snaps in", detail);
        // The edges rounded: at the front the seat is wider by the rounding.
        const std::vector<double> xf = crossX(slice(b.body, s.base + rr), 0.02);
        check(xf.size() == 10 && xf[2] - xf[1] > 2.0 * rr + 0.3, "the seats' edges rounded at the front",
              xf.size() == 10 ? num(xf[2] - xf[1]) : std::to_string(xf.size()));
        // Labels stand in front of the face.
        const Built lb = make(m);
        DV3 llo, lhi;
        bounds(lb.labels, llo, lhi);
        check(!lb.labels.empty() && lhi.y <= lb.lo.y + 1e-6 && llo.y > lb.lo.y - 0.61 &&
                  lb.model.missingGlyphs == 0,
              "the labels stand 0.6 mm out of the front, every letter there",
              num(llo.y) + " .. " + num(lhi.y) + ", " + std::to_string(lb.model.missingGlyphs) + " missing");
        // Screws: the flanges are cut through where the screws go.
        Params sc;
        sc.mount = Mount::Screws;
        const Built bs = make(sc);
        const std::vector<double> xh = crossX(slice(bs.body, 0.5), 0.5 * D);
        check(xh.size() == 6, "two screw holes through the flanges", std::to_string(xh.size()) + " crossings");
    }

    std::printf("Desk clips\n");
    {
        Params p;
        p.style = Style::DeskClip;
        solidChecks("for a 25 mm desk", p, png, "clip");
        p.labels = "Phone, Watch, Laptop, Lamp";
        p.diameters = "4, 3, 6, 5";
        solidChecks("labelled, mixed seats", p, png, "clip-labels");
        p.deskThickness = 40.0f;
        p.reach = 30.0f;
        p.rounding = 0.0f;
        solidChecks("for a 40 mm desk, a long lip, no rounding", p, png, "");
        Params q;
        q.style = Style::DeskClip;
        q.slots = 1;
        q.diameters = "8";
        q.depth = 8.0f;
        solidChecks("one seat, as shallow as it goes", q, png, "clip-one");

        const Params s = cableholder::sane(Params{});
        Params c;
        c.style = Style::DeskClip;
        const Built b = make(c);
        const double D = std::max(static_cast<double>(s.depth), s.clipWall + 8.0);
        // A tooth's middle (between the first two seats): one piece, bar and clamp.
        const double xTooth = b.lo.x + s.margin + 2.0 * 0.5 * (5.0 + s.clearance) + 0.5 * s.tooth;
        const auto loops = solidbuild::section(b.model.body, static_cast<float>(xTooth));
        check(loops.size() == 1, "the bar and the clamp are one piece", std::to_string(loops.size()) + " outlines");
        // The gap for the desk, 3 mm in front of the wall.
        const double y0 = D - s.clipWall - 3.0;   // the plate's front is y 0
        const std::vector<double> zs = crossCut(b.model.body, xTooth, y0, true);
        const double want = s.deskThickness - s.grip * 3.0 / s.reach;
        bool gapOk = zs.size() == 4;
        double gap = 0.0;
        if (gapOk) {
            gap = zs[2] - zs[1];
            gapOk = std::abs(gap - want) < 0.02;
        }
        check(gapOk, "the gap for the desk: 25 mm less the lip's lean", num(gap) + ", want " + num(want));
    }

    std::printf("Trays\n");
    {
        Params p;
        p.style = Style::Tray;
        solidChecks("200 mm, open front, outlets", p, png, "tray");
        p.trayOpenFront = false;
        solidChecks("closed, flanges both sides", p, png, "tray-closed");
        p.trayOutlets = false;
        p.trayLength = 420.0f;
        solidChecks("420 mm, no outlets", p, png, "");
        Params q;
        q.style = Style::Tray;
        const Built b = make(q);
        const Params s = cableholder::sane(q);
        const std::vector<double> ys = crossCut(b.model.body, 0.5 * (b.lo.x + b.hi.x) + 31.0, -0.5 * s.trayHeight - 3.0, false);
        check(ys.size() == 4 && std::abs((ys[2] - ys[1]) - s.trayWidth) < 1e-3, "the inside as wide as asked",
              ys.size() == 4 ? num(ys[2] - ys[1]) : std::to_string(ys.size()));
    }

    std::printf("Hooks\n");
    {
        Params p;
        p.style = Style::Hook;
        solidChecks("for the wall", p, png, "hook-wall");
        p.hookMount = HookMount::UnderDesk;
        solidChecks("under the desk", p, png, "hook-desk");
        p.hookPlate = 20.0f;
        p.hookRadius = 25.0f;
        solidChecks("under the desk, a short plate (one screw), a wide bend", p, png, "");
        for (const HookMount hm : {HookMount::Wall, HookMount::UnderDesk}) {
            Params h;
            h.style = Style::Hook;
            h.hookMount = hm;
            const Built b = make(h);
            const auto loops = solidbuild::section(b.model.body, static_cast<float>(0.5 * (b.lo.x + b.hi.x)));
            check(loops.size() == 3, std::string(hm == HookMount::Wall ? "wall" : "desk") +
                  ": the plate cut by two screw holes", std::to_string(loops.size()) + " outlines");
        }
    }

    std::printf("Winders\n");
    {
        Params p;
        p.style = Style::Winder;
        solidChecks("70 x 32 mm", p, png, "winder");
        p.winderLabel = "USB-C";
        solidChecks("labelled", p, png, "winder-label");
        p.rounding = 0.0f;
        p.winderThickness = 2.0f;
        solidChecks("thin, square edges", p, png, "");
        Params q;
        q.style = Style::Winder;
        const Built b = make(q);
        const Params s = cableholder::sane(q);
        const std::vector<double> xs = crossX(slice(b.body, 0.5 * s.winderThickness), 0.0);
        check(xs.size() == 2 && std::abs((xs[1] - xs[0]) - (s.winderLength - 2.0 * s.notchDepth)) < 0.02,
              "the notches as deep as asked", xs.size() == 2 ? num(xs[1] - xs[0]) : std::to_string(xs.size()));
    }

    std::printf("Speed\n");
    {
        Params p;
        p.slots = 24;
        p.diameters = "4";
        p.labels = "a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p,q,r,s,t,u,v,w,x";
        p.mount = Mount::Screws;
        const auto t0 = std::chrono::steady_clock::now();
        const cableholder::Model m = cableholder::build(p);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::string why;
        check(m.ok && closed(m.body, why) && closed(m.labels, why) && ms < 1000.0,
              "24 labelled seats, screwed: closed in " + num(ms) + " ms", why);
    }

    std::printf("Saved and loaded\n");
    {
        Params p;
        p.style = Style::Hook;
        p.slots = 7; p.diameters = "3, 4"; p.clearance = 0.5f; p.neck = 0.6f; p.tooth = 3.0f; p.margin = 4.0f;
        p.depth = 20.0f; p.base = 6.0f; p.rounding = 0.8f; p.labels = "a, b"; p.labelHeight = 2.5f;
        p.mount = Mount::Magnets; p.deskThickness = 30.0f; p.grip = 1.2f; p.reach = 22.0f; p.clipWall = 5.0f;
        p.trayLength = 300.0f; p.trayWidth = 50.0f; p.trayHeight = 40.0f; p.trayWall = 3.0f; p.trayOpenFront = false;
        p.trayOutlets = false; p.outletDiameter = 25.0f; p.outletSpacing = 70.0f; p.hookMount = HookMount::UnderDesk;
        p.hookWidth = 25.0f; p.hookThickness = 6.0f; p.hookPlate = 50.0f; p.hookReach = 40.0f; p.hookRadius = 15.0f;
        p.hookTip = 15.0f; p.winderLength = 80.0f; p.winderWidth = 35.0f; p.winderThickness = 4.0f;
        p.notchWidth = 15.0f; p.notchDepth = 12.0f; p.slit = 3.0f; p.winderLabel = "x"; p.screwShank = 4.0f;
        p.screwHead = 8.0f; p.magnetDiameter = 8.0f; p.magnetHeight = 3.0f; p.color = {0.1f, 0.2f, 0.3f};
        p.labelColor = {0.9f, 0.8f, 0.7f};
        p = cableholder::sane(p);
        nlohmann::json j;
        cableholder::saveParams(j, p);
        Params q;
        cableholder::loadParams(nlohmann::json::parse(j.dump()), q);
        check(q == p, "every parameter comes back", j.dump());
        Params d;
        cableholder::loadParams(nlohmann::json::object(), d);
        check(d == cableholder::sane(Params{}), "nothing saved: the defaults");
        std::unique_ptr<ComponentBase> c = components::create("cableHolder");
        check(c && dynamic_cast<CableHolderComponent*>(c.get()), "the component is registered as cableHolder");
        check(cableholder::autoName(Params{}) == "Cable comb 4 x 5 mm", "named " + cableholder::autoName(Params{}));
    }

    std::printf("Through the 3MF export\n");
    auto export3 = [&](const Params& p, const std::string& file, std::size_t parts) {
        ModelLibrary models;
        Document doc;
        int counter = 1;
        const cableholder::Palette pal = cableholder::ensurePalette(doc.materials(), p);
        std::vector<Entity> es = cableholder::entities(p, pal, counter, glm::vec3(3.0f, 1.0f, -2.0f));
        check(es.size() == parts + 1 && es[0].components.get<CableHolderComponent>(),
              file + ": a root keeping the parameters and " + std::to_string(parts) + " part(s)",
              es.empty() ? "" : es[0].name);
        const int root = es.empty() ? -1 : es[0].id;
        for (Entity& e : es) doc.entities().push_back(std::move(e));
        scenegraph::resolve(doc.entities());
        const export3mf::Shape sh = export3mf::gather(doc, models, {root});
        bool allClosed = sh.parts.size() == parts;
        for (const export3mf::Part& part : sh.parts) {
            std::map<std::pair<int, int>, int> edges;
            for (const glm::ivec3& t : part.tris)
                for (int k = 0; k < 3; ++k) ++edges[{t[k], t[(k + 1) % 3]}];
            for (const auto& [e, n] : edges) {
                const auto back = edges.find({e.second, e.first});
                if (n != 1 || back == edges.end() || back->second != 1) allClosed = false;
            }
        }
        check(allClosed && sh.colors.size() == parts, file + ": closed after the weld, a colour each");
        export3mf::Options o;
        const glm::vec3 mm = export3mf::printSize(sh, o);
        const cableholder::Model m = cableholder::build(p);
        check(std::abs(mm.x - m.size.x) < 1e-2f && std::abs(mm.y - m.size.y) < 1e-2f && std::abs(mm.z - m.size.z) < 1e-2f,
              file + ": " + num(mm.x) + " x " + num(mm.y) + " x " + num(mm.z) + " mm on the bed");
        if (!png.dir.empty()) {
            std::string err;
            check(export3mf::write(png.dir + "/" + file + ".3mf", sh, o, err), "written as " + png.dir + "/" + file + ".3mf", err);
        }
    };
    {
        Params c;
        c.slots = 5;
        c.diameters = "3.5, 5, 5, 6.5, 8";
        c.labels = "USB-C, Phone, Watch, 230V, LAN";
        c.mount = Mount::Adhesive;
        export3(c, "cableholder-comb", 2);
        Params d = c;
        d.style = Style::DeskClip;
        export3(d, "cableholder-clip", 2);
        Params t;
        t.style = Style::Tray;
        export3(t, "cableholder-tray", 1);
        Params h;
        h.style = Style::Hook;
        export3(h, "cableholder-hook", 1);
        Params w;
        w.style = Style::Winder;
        w.winderLabel = "USB-C";
        export3(w, "cableholder-winder", 2);
    }

    std::printf("\n%s (%d failed)\n", failures ? "FAILED" : "all ok", failures);
    return failures ? 1 : 0;
}
