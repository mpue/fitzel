// terrainmodelcheck -- terrain models (TerrainModel.hpp), without GL.
//
// Landscapes made up for the check -- round hills, a lake, saddles (where
// marching squares has to choose), ridges that run off the square, a plain with
// no relief at all, a heightmap image -- built as a relief and as terraces:
// every part closed (every edge crossed once each way, no triangle without
// area), sections that never cross, the size it reports. Then what they are
// for: the relief's ground at every sample exactly as high as the land there
// (scaled, exaggerated, on its base, a lake levelled to the water); the
// terraces' top over a point exactly the top of the highest slab whose contour
// holds it; north staying north; saved and loaded; through the 3MF export one
// closed part a slab, a colour a zone.
//   build/linux/bin/terrainmodelcheck [outdir]

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
#include "../src/SceneGraph.hpp"
#include "../src/TerrainModel.hpp"

using terrainmodel::Params;
using terrainmodel::Source;
using terrainmodel::Style;
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

// --- Landscapes -------------------------------------------------------------------------

float hills(float x, float z) {
    auto bump = [](float x, float z, float cx, float cz, float r, float h) {
        const float d2 = ((x - cx) * (x - cx) + (z - cz) * (z - cz)) / (r * r);
        return h * std::exp(-d2);
    };
    return 20.0f + bump(x, z, -150.0f, 100.0f, 180.0f, 160.0f) + bump(x, z, 220.0f, -180.0f, 120.0f, 220.0f) +
           bump(x, z, 60.0f, 240.0f, 90.0f, 80.0f) - bump(x, z, 50.0f, -20.0f, 140.0f, 45.0f) +
           6.0f * std::sin(x * 0.03f) * std::cos(z * 0.027f);
}
float saddles(float x, float z) { return 40.0f * std::sin(x * 0.02f) * std::sin(z * 0.02f) + 50.0f; }
float ridge(float x, float z) { return 100.0f - std::abs(x * 0.5f + z * 0.3f) * 0.4f + 10.0f * std::sin(z * 0.01f); }
float plain(float, float) { return 12.0f; }
float north(float, float z) { return -z * 0.2f; }   // rising to the north (the scene's -z)
float east(float x, float) { return x * 0.2f; }      // rising to the east (the scene's +x)

// --- Rays -------------------------------------------------------------------------------

// The highest point of a mesh straight above (x, y) -- -1e30 when it misses.
double topAt(const std::vector<Tri>& ts, double x, double y) {
    double best = -1e30;
    for (const Tri& t : ts) {
        const DV2 a(t.a), b(t.b), c(t.c), p(x, y);
        const double d = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (std::abs(d) < 1e-12) continue;
        const double u = ((b.x - p.x) * (c.y - p.y) - (b.y - p.y) * (c.x - p.x)) / d;
        const double v = ((c.x - p.x) * (a.y - p.y) - (c.y - p.y) * (a.x - p.x)) / d;
        const double w = 1.0 - u - v;
        if (u < -1e-6 || v < -1e-6 || w < -1e-6) continue;   // the mesh is floats
        best = std::max(best, u * t.a.z + v * t.b.z + w * t.c.z);
    }
    return best;
}

// --- One model ----------------------------------------------------------------------------

struct Built {
    terrainmodel::Model model;
    std::vector<std::vector<Tri>> parts;
};

Built make(const Params& p, const terrainmodel::Heights& h) {
    Built b;
    b.model = terrainmodel::build(p, h);
    for (const terrainmodel::Part& part : b.model.parts) b.parts.push_back(tris(part.mesh));
    return b;
}

void solidChecks(const std::string& what, const Params& p, const terrainmodel::Heights& h) {
    const Built b = make(p, h);
    const terrainmodel::Model& m = b.model;
    check(m.ok && !m.parts.empty(), what + ": built, " + std::to_string(m.parts.size()) + " part(s)", m.error);
    bool allClosed = true, noSliver = true, noCross = true, volOk = true;
    std::string why, where;
    double vol = 0.0;
    DV3 lo(1e30), hi(-1e30);
    for (std::size_t k = 0; k < m.parts.size(); ++k) {
        std::string w;
        if (!closed(m.parts[k].mesh, w)) {
            allClosed = false;
            if (why.empty()) why = m.parts[k].name + ": " + w;
        }
        DV3 sp;
        if (minArea(b.parts[k], &sp) <= 1e-7) {
            noSliver = false;
            if (where.empty()) {
                where = m.parts[k].name + " at " + num(sp.x) + ", " + num(sp.y) + ", " + num(sp.z);
                for (const Tri& t : b.parts[k])
                    if (0.5 * glm::length(glm::cross(t.b - t.a, t.c - t.a)) <= 1e-7) {
                        for (const DV3& v : {t.a, t.b, t.c}) where += " | " + num(v.x) + " " + num(v.y) + " " + num(v.z);
                        break;
                    }
            }
        }
        const double v = volume(b.parts[k]);
        if (v <= 0.0) volOk = false;
        vol += v;
        DV3 l, u;
        bounds(b.parts[k], l, u);
        lo = glm::min(lo, l);
        hi = glm::max(hi, u);
        // Sections through the part never cross.
        for (int s = 1; s < 6 && noCross; ++s) {
            std::string at;
            if (crosses(slice(b.parts[k], l.z + (u.z - l.z) * s / 6.0 + 0.0013), at)) {
                noCross = false;
                where = m.parts[k].name + ": a section crosses at " + at;
            }
        }
    }
    check(allClosed, what + ": every part closed, every edge once each way", why);
    check(noSliver, what + ": no triangle without area", where);
    check(noCross, what + ": sections never cross themselves", where);
    check(volOk && std::abs(vol - m.volume) < 1e-3 * vol, what + ": volume " + num(vol / 1000.0) + " cm3");
    const DV3 size = hi - lo;
    check(std::abs(size.x - p.printWidth) < 1e-3 && std::abs(lo.z) < 1e-9 &&
              std::abs(size.x - m.size.x) < 1e-2 && std::abs(size.y - m.size.y) < 1e-2 && std::abs(size.z - m.size.z) < 1e-2,
          what + ": " + num(size.x) + " x " + num(size.y) + " x " + num(size.z) + " mm, on Z 0, as it reports");
}

} // namespace

int main(int argc, char** argv) {
    std::string out;
    if (argc > 1) {
        out = argv[1];
        std::filesystem::create_directories(out);
    }

    std::printf("Reliefs\n");
    {
        Params p;
        p.width = p.depth = 900.0f;
        p.waterLevel = 15.0f;
        solidChecks("hills with a lake", p, hills);
        p.water = false;
        solidChecks("hills, no water", p, hills);
        p.depth = 500.0f;
        p.resolution = 90;
        solidChecks("a wide strip, coarse", p, ridge);
        solidChecks("a plain", p, plain);
        Params s;
        s.width = s.depth = 600.0f;
        s.water = false;
        solidChecks("saddles", s, saddles);

        // Every sample at its height: scaled, exaggerated, on the base, the lake level.
        Params q;
        q.width = q.depth = 900.0f;
        q.waterLevel = 15.0f;
        q.resolution = 60;
        const Built b = make(q, hills);
        const terrainmodel::Model& m = b.model;
        const double k = m.mmPerMetre * q.exaggeration;
        const double zero = q.waterLevel;
        double worst = 0.0;
        for (int j = 1; j < m.ny; j += 7)
            for (int i = 1; i < m.nx; i += 7) {
                const double x = -0.5 * q.printWidth + q.printWidth * i / (m.nx - 1);
                const double y = -0.5 * m.size.y + m.size.y * j / (m.ny - 1);
                const double hgt = m.heights[static_cast<std::size_t>(j * m.nx + i)];
                const double want = q.base + std::max(0.0, hgt - zero) * k;
                worst = std::max(worst, std::abs(topAt(b.parts[0], x, y) - want));
            }
        check(worst < 1e-3, "the ground at every sample as high as the land, the lake level", num(worst) + " mm off at worst");
        check(m.lo < q.waterLevel && !m.changes.empty() && std::abs(m.changes[0].first - (q.base + 0.2)) < 1e-4,
              "a lake: the first filament change just above its surface", num(m.changes.empty() ? 0.0 : m.changes[0].first));
    }

    std::printf("Terraces\n");
    {
        Params p;
        p.style = Style::Terraces;
        p.width = p.depth = 900.0f;
        p.waterLevel = 15.0f;
        solidChecks("hills with a lake, 10 slabs", p, hills);
        p.water = false;
        p.bands = 16;
        solidChecks("hills, 16 slabs", p, hills);
        Params s;
        s.style = Style::Terraces;
        s.width = s.depth = 600.0f;
        s.water = false;
        s.bands = 7;
        solidChecks("saddles, 7 slabs", s, saddles);
        s.resolution = 41;
        solidChecks("saddles, coarse", s, saddles);
        Params r;
        r.style = Style::Terraces;
        r.width = 900.0f;
        r.depth = 400.0f;
        solidChecks("ridges running off the square", r, ridge);
        solidChecks("a plain", r, plain);

        // Over a point the top is the top of the highest slab whose contour
        // holds it (points well clear of the slabs' edges).
        Params q;
        q.style = Style::Terraces;
        q.width = q.depth = 900.0f;
        q.waterLevel = 15.0f;
        q.resolution = 80;
        const Built b = make(q, hills);
        const terrainmodel::Model& m = b.model;
        const double k = m.mmPerMetre * q.exaggeration;
        const double zero = q.waterLevel;
        int tested = 0, wrong = 0;
        std::string first;
        std::vector<double> edges(m.levels.begin(), m.levels.end());
        edges.push_back(m.hi);
        for (int j = 3; j < m.ny; j += 5)
            for (int i = 3; i < m.nx; i += 5) {
                const double hgt = m.heights[static_cast<std::size_t>(j * m.nx + i)];
                // Its slab: the highest level at or below it, far enough from any.
                std::size_t band = 0;
                bool clear = true;
                for (std::size_t s = 0; s < m.levels.size(); ++s) {
                    if (hgt >= m.levels[s]) band = s;
                    if (s > 0 && std::abs(hgt - m.levels[s]) < 2.0) clear = false;
                }
                if (!clear) continue;
                const double topH = band + 1 < edges.size() ? edges[band + 1] : m.hi;
                const double want = q.base + std::max(0.0, topH - zero) * k;
                const double x = -0.5 * q.printWidth + q.printWidth * i / (m.nx - 1);
                const double y = -0.5 * m.size.y + m.size.y * j / (m.ny - 1);
                double got = -1e30;
                for (const auto& part : b.parts) got = std::max(got, topAt(part, x, y));
                ++tested;
                if (std::abs(got - want) > 1e-3) {
                    ++wrong;
                    if (first.empty()) first = "at " + num(x) + ", " + num(y) + ": " + num(got) + ", want " + num(want);
                }
            }
        check(tested > 100 && wrong == 0, "every point's top the top of its slab (" + std::to_string(tested) + " points)", first);
        // The slabs stack: each one's bottom the one below's top.
        bool stacked = true;
        for (std::size_t s = 1; s < b.parts.size(); ++s) {
            DV3 l0, h0, l1, h1;
            bounds(b.parts[s - 1], l0, h0);
            bounds(b.parts[s], l1, h1);
            if (std::abs(l1.z - h0.z) > 1e-6) stacked = false;
        }
        check(stacked, "the slabs stand on each other, no gap, no overlap in height");
        int colours = 0;
        std::vector<glm::vec3> seen;
        for (const auto& part : m.parts)
            if (std::find(seen.begin(), seen.end(), part.color) == seen.end()) seen.push_back(part.color);
        colours = static_cast<int>(seen.size());
        check(colours == 5 && m.parts.front().color == q.waterColor && m.parts.back().color == q.peakColor,
              "five colours: water at the bottom, snow at the top", std::to_string(colours));
    }

    std::printf("North stays north\n");
    {
        Params p;
        p.water = false;
        p.width = p.depth = 200.0f;
        const Built b = make(p, north);
        const double s = b.model.size.y;
        const double front = topAt(b.parts[0], 0.0, -0.45 * s), back = topAt(b.parts[0], 0.0, 0.45 * s);
        check(back > front + 5.0, "land rising to the north rises to the print's +y (the scene's -z)",
              num(front) + " -> " + num(back));
        p.centreX = 500.0f;   // and off the origin
        const Built e = make(p, east);
        const double w = topAt(e.parts[0], -0.45 * p.printWidth, 0.0), ea = topAt(e.parts[0], 0.45 * p.printWidth, 0.0);
        check(ea > w + 5.0 && std::abs(e.model.lo - 0.2 * 400.0) < 0.5,
              "land rising to the east rises to the print's +x, the square round its centre (x 400 .. 600)",
              num(w) + " -> " + num(ea) + ", lowest " + num(e.model.lo) + " m");
    }

    std::printf("A heightmap image\n");
    {
        const std::string dir = out.empty() ? std::filesystem::temp_directory_path().string() : out;
        const std::string png = dir + "/terrainmodel-heightmap.png";
        const int w = 96, h = 64;
        std::vector<unsigned char> px(static_cast<std::size_t>(w * h));
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                // A peak in the north-east, low ground in the south-west.
                const double u = static_cast<double>(x) / (w - 1), v = static_cast<double>(y) / (h - 1);
                const double d = std::hypot(u - 0.75, v - 0.25);
                px[static_cast<std::size_t>(y * w + x)] = static_cast<unsigned char>(std::lround(255.0 * std::exp(-d * d * 8.0)));
            }
        stbi_write_png(png.c_str(), w, h, 1, px.data(), w);
        Params p;
        p.source = Source::Image;
        p.image = png;
        p.imageWidth = 1500.0f;
        p.imageRelief = 250.0f;
        p.water = false;
        const glm::vec2 reg = terrainmodel::regionOf(p);
        check(std::abs(reg.y - 1000.0f) < 1e-3f, "the region keeps the image's shape: 1500 x 1000 m", num(reg.y));
        solidChecks("from the image", p, nullptr);
        p.style = Style::Terraces;
        solidChecks("from the image, terraces", p, nullptr);
        p.style = Style::Relief;
        const Built b = make(p, nullptr);
        const double peak = topAt(b.parts[0], 0.25 * p.printWidth, 0.25 * b.model.size.y);
        const double low = topAt(b.parts[0], -0.45 * p.printWidth, -0.45 * b.model.size.y);
        check(peak > low + 20.0 && b.model.nx == w, "the image's light corner the high one, sampled at its own resolution",
              num(low) + " -> " + num(peak));
        Params bad = p;
        bad.image = dir + "/no-such-file.png";
        check(!terrainmodel::build(bad, nullptr).ok, "an image that is not there: not built, and says so");
        if (out.empty()) std::filesystem::remove(png);
    }

    std::printf("Speed\n");
    {
        Params p;
        p.style = Style::Terraces;
        p.resolution = 250;
        p.bands = 14;
        p.width = p.depth = 900.0f;
        p.waterLevel = 15.0f;
        auto t0 = std::chrono::steady_clock::now();
        const terrainmodel::Model m = terrainmodel::build(p, hills);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        p.style = Style::Relief;
        t0 = std::chrono::steady_clock::now();
        const terrainmodel::Model r = terrainmodel::build(p, hills);
        const double ms2 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        check(m.ok && r.ok && ms < 3000.0 && ms2 < 3000.0,
              "250 samples across: 14 terraces in " + num(ms) + " ms, the relief in " + num(ms2) + " ms");
    }

    std::printf("Saved and loaded\n");
    {
        Params p;
        p.source = Source::Image;
        p.centreX = 123.0f; p.centreZ = -456.0f; p.width = 1200.0f; p.depth = 700.0f;
        p.image = "/x/y.png"; p.imageWidth = 3000.0f; p.imageRelief = 450.0f; p.printWidth = 150.0f;
        p.exaggeration = 2.5f; p.base = 4.0f; p.resolution = 200; p.style = Style::Terraces; p.bands = 12;
        p.water = false; p.waterLevel = 7.0f; p.reliefColor = {0.1f, 0.2f, 0.3f}; p.waterColor = {0.2f, 0.3f, 0.4f};
        p.lowColor = {0.3f, 0.4f, 0.5f}; p.midColor = {0.4f, 0.5f, 0.6f}; p.highColor = {0.5f, 0.6f, 0.7f};
        p.peakColor = {0.6f, 0.7f, 0.8f};
        nlohmann::json j;
        terrainmodel::saveParams(j, p);
        Params q;
        terrainmodel::loadParams(nlohmann::json::parse(j.dump()), q);
        check(q == p, "every parameter comes back", j.dump());
        Params d;
        terrainmodel::loadParams(nlohmann::json::object(), d);
        check(d == Params{}, "nothing saved: the defaults");
        std::unique_ptr<ComponentBase> c = components::create("terrainModel");
        check(c && dynamic_cast<TerrainModelComponent*>(c.get()), "the component is registered as terrainModel");
    }

    std::printf("Through the 3MF export\n");
    auto export3 = [&](const Params& p, const std::string& file) {
        ModelLibrary models;
        Document doc;
        int counter = 1;
        const terrainmodel::Model m = terrainmodel::build(p, hills);
        std::vector<Entity> es = terrainmodel::entities(p, m, doc.materials(), counter, glm::vec3(3.0f, 1.0f, -2.0f));
        check(es.size() == m.parts.size() + 1 && es[0].components.get<TerrainModelComponent>(),
              file + ": a root keeping the parameters and " + std::to_string(m.parts.size()) + " part(s)",
              es.empty() ? "" : es[0].name);
        const int root = es.empty() ? -1 : es[0].id;
        for (Entity& e : es) doc.entities().push_back(std::move(e));
        scenegraph::resolve(doc.entities());
        const export3mf::Shape sh = export3mf::gather(doc, models, {root});
        bool allClosed = sh.parts.size() == m.parts.size();
        for (const export3mf::Part& part : sh.parts) {
            std::map<std::pair<int, int>, int> edges;
            for (const glm::ivec3& t : part.tris)
                for (int k = 0; k < 3; ++k) ++edges[{t[k], t[(k + 1) % 3]}];
            for (const auto& [e, n] : edges) {
                const auto back = edges.find({e.second, e.first});
                if (n != 1 || back == edges.end() || back->second != 1) allClosed = false;
            }
        }
        check(allClosed, file + ": every part closed after the weld, " + std::to_string(sh.colors.size()) + " colour(s)");
        export3mf::Options o;
        const glm::vec3 mm = export3mf::printSize(sh, o);
        check(std::abs(mm.x - m.size.x) < 1e-2f && std::abs(mm.y - m.size.y) < 1e-2f && std::abs(mm.z - m.size.z) < 1e-2f,
              file + ": " + num(mm.x) + " x " + num(mm.y) + " x " + num(mm.z) + " mm on the bed");
        if (!out.empty()) {
            std::string err;
            check(export3mf::write(out + "/" + file + ".3mf", sh, o, err), "written as " + out + "/" + file + ".3mf", err);
        }
    };
    {
        Params p;
        p.width = p.depth = 900.0f;
        p.waterLevel = 15.0f;
        export3(p, "terrain-relief");
        p.style = Style::Terraces;
        export3(p, "terrain-terraces");
    }

    std::printf("\n%s (%d failed)\n", failures ? "FAILED" : "all ok", failures);
    return failures ? 1 : 0;
}
