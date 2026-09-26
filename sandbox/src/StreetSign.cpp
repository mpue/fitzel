#include "StreetSign.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "EditMesh.hpp"

namespace streetsign {

namespace {

using fitzel::AssetId;

constexpr float kPi = 3.14159265358979323846f;

// --- The typeface ----------------------------------------------------------------
namespace font {
#include "SignFont.inc"
}

// A glyph cut into trapezoids: horizontal bottom and top edges, slanted sides.
// Font units, y up from the baseline.
struct Trap { float y0, y1, xl0, xr0, xl1, xr1; };

// A glyph as the sign letters it: convex pieces (triangles from the ear
// clipper, or trapezoids where it had to fall back), font units, y up from the
// baseline, counter-clockwise.
struct GlyphShape {
    float                               advance = 0.0f;
    std::vector<std::vector<glm::vec2>> pieces;
    bool                                fellBack = false;
};

// Cut one glyph's outline (nonzero winding) into trapezoids by sweeping it in
// horizontal bands between its vertices' heights. No outline vertex lies
// strictly inside a band, so every edge crossing a band crosses all of it and
// the pieces are exact. A piece whose two bounding edges go on unchanged into
// the next band is extended instead of restarted -- a straight stem is one
// quad, not one per band.
std::vector<Trap> cut(unsigned firstContour, unsigned contourCount) {
    struct Edge { float xa, ya, xb, yb; int dir; };
    std::vector<Edge> edges;
    std::vector<float> ys;
    for (unsigned c = firstContour; c < firstContour + contourCount; ++c) {
        const unsigned a = font::kContourStart[c], b = font::kContourStart[c + 1];
        for (unsigned i = a; i < b; ++i) {
            const unsigned j = (i + 1 < b) ? i + 1 : a;
            const float x0 = font::kPoints[2 * i], y0 = font::kPoints[2 * i + 1];
            const float x1 = font::kPoints[2 * j], y1 = font::kPoints[2 * j + 1];
            ys.push_back(y0);
            if (y0 == y1) continue;
            if (y0 < y1) edges.push_back({x0, y0, x1, y1, 1});
            else         edges.push_back({x1, y1, x0, y0, -1});
        }
    }
    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
    auto xAt = [&](int e, float y) {
        const Edge& E = edges[static_cast<std::size_t>(e)];
        // Exact at the ends, so the piece below a vertex and the piece above it
        // name that corner with the same float -- and the mesh welds it.
        if (y <= E.ya) return E.xa;
        if (y >= E.yb) return E.xb;
        return E.xa + (y - E.ya) * (E.xb - E.xa) / (E.yb - E.ya);
    };

    struct Open { int l, r; float y0; };
    std::vector<Trap> out;
    std::vector<Open> open, next;
    auto emit = [&](const Open& o, float y1) {
        out.push_back({o.y0, y1, xAt(o.l, o.y0), xAt(o.r, o.y0), xAt(o.l, y1), xAt(o.r, y1)});
    };
    std::vector<std::pair<float, int>> act;
    for (std::size_t k = 0; k + 1 < ys.size(); ++k) {
        const float y0 = ys[k], y1 = ys[k + 1], ym = 0.5f * (y0 + y1);
        act.clear();
        for (int e = 0; e < static_cast<int>(edges.size()); ++e)
            if (edges[static_cast<std::size_t>(e)].ya < ym && edges[static_cast<std::size_t>(e)].yb > ym)
                act.emplace_back(xAt(e, ym), e);
        std::sort(act.begin(), act.end());
        next.clear();
        int wind = 0, left = -1;
        for (const auto& [x, e] : act) {
            const int before = wind;
            wind += edges[static_cast<std::size_t>(e)].dir;
            if (before == 0 && wind != 0) left = e;
            else if (before != 0 && wind == 0) next.push_back({left, e, y0});
        }
        // Carry on the pieces bounded by the same two edges; close the rest.
        for (Open& n : next)
            for (Open& o : open)
                if (o.l == n.l && o.r == n.r && o.l >= 0) { n.y0 = o.y0; o.l = -1; break; }
        for (const Open& o : open)
            if (o.l >= 0) emit(o, y0);
        std::swap(open, next);
    }
    if (!ys.empty())
        for (const Open& o : open) emit(o, ys.back());
    return out;
}


// --- Ear clipping -------------------------------------------------------------
// The cheap way to fill a glyph: one triangle per outline point (plus two per
// hole), so a letter costs about as many vertices as its outline has corners
// -- half of what the trapezoid cut needs, which splits every piece at every
// height any other corner of the letter sits at. Holes (the counter of an O,
// the eye of an e) are joined to their outline by a bridge first, the usual
// way (Eberly, "Triangulation by Ear Clipping"). Doubles throughout: corners
// are font units up to ~2000, and their cross products outrun a float.
using DV = glm::dvec2;

double cross(const DV& a, const DV& b, const DV& c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

double signedArea(const std::vector<DV>& p) {
    double a = 0.0;
    for (std::size_t i = 0; i < p.size(); ++i) {
        const DV& u = p[i];
        const DV& v = p[(i + 1) % p.size()];
        a += u.x * v.y - v.x * u.y;
    }
    return 0.5 * a;
}

bool insidePoly(const std::vector<DV>& poly, const DV& q) {   // even-odd
    bool in = false;
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const DV& a = poly[i];
        const DV& b = poly[j];
        if ((a.y > q.y) != (b.y > q.y) && q.x < (b.x - a.x) * (q.y - a.y) / (b.y - a.y) + a.x)
            in = !in;
    }
    return in;
}

// Drop repeated corners and corners on a straight line: an ear clipper has no
// use for either, and a zero-area "ear" is where they go wrong.
void simplify(std::vector<DV>& p) {
    for (bool again = true; again && p.size() >= 3;) {
        again = false;
        for (std::size_t i = 0; i < p.size() && p.size() >= 3; ++i) {
            const DV& a = p[(i + p.size() - 1) % p.size()];
            const DV& b = p[i];
            const DV& c = p[(i + 1) % p.size()];
            if (b == c || std::abs(cross(a, b, c)) < 1e-9) {
                p.erase(p.begin() + static_cast<std::ptrdiff_t>(i));
                again = true;
                break;
            }
        }
    }
}

// Splice `hole` (clockwise) into `outer` (counter-clockwise, holes already
// joined) through a bridge from the hole's rightmost corner to a corner of the
// outline that sees it.
bool bridge(std::vector<DV>& outer, const std::vector<DV>& hole) {
    std::size_t m = 0;
    for (std::size_t i = 1; i < hole.size(); ++i)
        if (hole[i].x > hole[m].x) m = i;
    const DV M = hole[m];
    // Nearest crossing of the ray M -> +x with the outline.
    double bestX = 1e300;
    std::size_t edge = outer.size();
    for (std::size_t i = 0; i < outer.size(); ++i) {
        const DV& a = outer[i];
        const DV& b = outer[(i + 1) % outer.size()];
        if ((a.y > M.y) == (b.y > M.y) || a.y == b.y) continue;
        const double x = a.x + (M.y - a.y) * (b.x - a.x) / (b.y - a.y);
        if (x >= M.x && x < bestX) { bestX = x; edge = i; }
    }
    if (edge == outer.size()) return false;
    const DV I(bestX, M.y);
    const std::size_t e1 = (edge + 1) % outer.size();
    std::size_t p = outer[edge].x > outer[e1].x ? edge : e1;
    if (outer[edge] == I) p = edge;
    else if (outer[e1] == I) p = e1;
    else {
        // A reflex corner inside the triangle M, I, P would hide P from M:
        // take the one closest in angle to the ray instead.
        const DV P = outer[p];
        double bestAng = 1e300, bestLen = 1e300;
        for (std::size_t i = 0; i < outer.size(); ++i) {
            if (i == p) continue;
            const DV& q = outer[i];
            const DV& a = outer[(i + outer.size() - 1) % outer.size()];
            const DV& c = outer[(i + 1) % outer.size()];
            if (cross(a, q, c) >= 0.0) continue;                 // not reflex
            const double d1 = cross(M, I, q), d2 = cross(I, P, q), d3 = cross(P, M, q);
            const bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
            if (neg && pos) continue;                            // outside the triangle
            const DV d = q - M;
            const double ang = std::atan2(std::abs(d.y), d.x), len = glm::length(d);
            if (ang < bestAng || (ang == bestAng && len < bestLen)) {
                bestAng = ang; bestLen = len; p = i;
            }
        }
    }
    std::vector<DV> out(outer.begin(), outer.begin() + static_cast<std::ptrdiff_t>(p) + 1);
    for (std::size_t k = 0; k <= hole.size(); ++k) out.push_back(hole[(m + k) % hole.size()]);
    out.push_back(outer[p]);
    out.insert(out.end(), outer.begin() + static_cast<std::ptrdiff_t>(p) + 1, outer.end());
    outer = std::move(out);
    return true;
}

bool clip(std::vector<DV> poly, std::vector<std::vector<glm::vec2>>& out) {
    std::vector<std::size_t> idx(poly.size());
    for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::size_t guard = 0;
    while (idx.size() > 3) {
        if (++guard > 4 * poly.size() * poly.size() + 16) return false;
        bool clipped = false;
        const std::size_t n = idx.size();
        for (std::size_t k = 0; k < n; ++k) {
            const DV& a = poly[idx[(k + n - 1) % n]];
            const DV& b = poly[idx[k]];
            const DV& c = poly[idx[(k + 1) % n]];
            if (cross(a, b, c) <= 0.0) continue;                  // reflex or flat
            bool empty = true;
            for (std::size_t j = 0; j < n && empty; ++j) {
                const DV& q = poly[idx[j]];
                if (q == a || q == b || q == c) continue;         // a bridge's twin corners
                empty = !(cross(a, b, q) >= 0.0 && cross(b, c, q) >= 0.0 && cross(c, a, q) >= 0.0);
            }
            if (!empty) continue;
            out.push_back({glm::vec2(a), glm::vec2(b), glm::vec2(c)});
            idx.erase(idx.begin() + static_cast<std::ptrdiff_t>(k));
            clipped = true;
            break;
        }
        if (!clipped) {
            // Only flat corners left to take (a bridge lying on an edge): drop one.
            bool dropped = false;
            for (std::size_t k = 0; k < n && !dropped; ++k) {
                const DV& a = poly[idx[(k + n - 1) % n]];
                const DV& b = poly[idx[k]];
                const DV& c = poly[idx[(k + 1) % n]];
                if (std::abs(cross(a, b, c)) < 1e-9) {
                    idx.erase(idx.begin() + static_cast<std::ptrdiff_t>(k));
                    dropped = true;
                }
            }
            if (!dropped) return false;
        }
    }
    if (idx.size() == 3 && cross(poly[idx[0]], poly[idx[1]], poly[idx[2]]) > 0.0)
        out.push_back({glm::vec2(poly[idx[0]]), glm::vec2(poly[idx[1]]), glm::vec2(poly[idx[2]])});
    return true;
}

// The glyph's outline as triangles; false when it could not be done cleanly
// (the caller then cuts trapezoids, which cope with anything).
bool triangulate(unsigned firstContour, unsigned contourCount,
                 std::vector<std::vector<glm::vec2>>& out) {
    std::vector<std::vector<DV>> cs;
    for (unsigned c = firstContour; c < firstContour + contourCount; ++c) {
        std::vector<DV> p;
        for (unsigned i = font::kContourStart[c]; i < font::kContourStart[c + 1]; ++i)
            p.emplace_back(font::kPoints[2 * i], font::kPoints[2 * i + 1]);
        simplify(p);
        if (p.size() >= 3) cs.push_back(std::move(p));
    }
    // Nesting: a contour inside an odd number of others is a hole.
    const std::size_t n = cs.size();
    std::vector<int> depth(n, 0), parent(n, -1);
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = 0; j < n; ++j)
            if (i != j && insidePoly(cs[j], cs[i][0])) {
                ++depth[i];
                if (parent[i] < 0 || std::abs(signedArea(cs[j])) <
                                         std::abs(signedArea(cs[static_cast<std::size_t>(parent[i])])))
                    parent[i] = static_cast<int>(j);
            }
    for (std::size_t i = 0; i < n; ++i) {
        const bool hole = depth[i] % 2 == 1;
        if ((signedArea(cs[i]) > 0.0) == hole) std::reverse(cs[i].begin(), cs[i].end());
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (depth[i] % 2 == 1) continue;
        std::vector<std::size_t> holes;
        for (std::size_t j = 0; j < n; ++j)
            if (depth[j] % 2 == 1 && parent[j] == static_cast<int>(i)) holes.push_back(j);
        auto maxX = [&](std::size_t h) {
            double x = -1e300;
            for (const DV& v : cs[h]) x = std::max(x, v.x);
            return x;
        };
        std::sort(holes.begin(), holes.end(),
                  [&](std::size_t a, std::size_t b) { return maxX(a) > maxX(b); });
        std::vector<DV> poly = cs[i];
        for (std::size_t h : holes)
            if (!bridge(poly, cs[h])) return false;
        if (!clip(std::move(poly), out)) return false;
    }
    return true;
}

double piecesArea(const std::vector<std::vector<glm::vec2>>& ps) {
    double a = 0.0;
    for (const auto& p : ps)
        for (std::size_t i = 0; i < p.size(); ++i) {
            const glm::vec2& u = p[i];
            const glm::vec2& v = p[(i + 1) % p.size()];
            a += 0.5 * (static_cast<double>(u.x) * v.y - static_cast<double>(v.x) * u.y);
        }
    return a;
}

// All glyphs, cut once on first use (a function-local static: safe if a town
// is derived on a worker thread while the panel previews on the main one).
struct Typeface {
    std::unordered_map<char32_t, GlyphShape> glyphs;
    std::map<std::pair<char32_t, char32_t>, float> kerning;
};
const Typeface& typeface() {
    static const Typeface tf = [] {
        Typeface t;
        for (const font::GlyphDef& g : font::kGlyphs) {
            GlyphShape s;
            s.advance = g.advance;
            // Triangles if they cover exactly what the trapezoids cover (the
            // sweep is the reference: it knows nothing of nesting and cannot
            // get it wrong); otherwise the trapezoids themselves.
            std::vector<std::vector<glm::vec2>> traps;
            for (const Trap& q : cut(g.first, g.count))
                traps.push_back({{q.xl0, q.y0}, {q.xr0, q.y0}, {q.xr1, q.y1}, {q.xl1, q.y1}});
            const double want = piecesArea(traps);
            std::vector<std::vector<glm::vec2>> tris;
            if (triangulate(g.first, g.count, tris) &&
                std::abs(piecesArea(tris) - want) <= 1e-4 * std::max(want, 1.0)) {
                s.pieces = std::move(tris);
            } else {
                s.pieces   = std::move(traps);
                s.fellBack = true;
            }
            t.glyphs.emplace(static_cast<char32_t>(g.cp), std::move(s));
        }
        for (const font::KernDef& k : font::kKerning)
            if (k.adjust != 0)
                t.kerning[{static_cast<char32_t>(k.a), static_cast<char32_t>(k.b)}] = k.adjust;
        return t;
    }();
    return tf;
}

std::u32string decodeUtf8(const std::string& s) {
    std::u32string out;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp = 0;
        int n = 0;
        if (c < 0x80)              { cp = c; n = 1; }
        else if ((c >> 5) == 0x6)  { cp = c & 0x1F; n = 2; }
        else if ((c >> 4) == 0xE)  { cp = c & 0x0F; n = 3; }
        else if ((c >> 3) == 0x1E) { cp = c & 0x07; n = 4; }
        else { ++i; continue; }    // a stray continuation byte
        if (i + n > s.size()) break;
        for (int k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        out += cp;
        i += static_cast<std::size_t>(n);
    }
    return out;
}

// --- Flat shapes ---------------------------------------------------------------

// A closed path around a rectangle of half-extents (a, b), counter-clockwise,
// with its corners square, bowed out (Rounded) or cut in (Notched) by radius r.
std::vector<glm::vec2> framePath(float a, float b, float r, Frame kind) {
    const glm::vec2 corners[4] = {{a, -b}, {a, b}, {-a, b}, {-a, -b}};
    const glm::vec2 dirs[4]    = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};   // edge INTO each corner
    std::vector<glm::vec2> p;
    r = std::min(r, 0.9f * std::min(a, b));
    const bool bent = r > 1e-5f && (kind == Frame::Rounded || kind == Frame::Notched);
    const int  segs = 6;
    for (int i = 0; i < 4; ++i) {
        const glm::vec2 c = corners[i], din = dirs[i], dout = dirs[(i + 1) % 4];
        if (!bent) { p.push_back(c); continue; }
        for (int s = 0; s <= segs; ++s) {
            const float t = 0.5f * kPi * s / segs;
            if (kind == Frame::Rounded)
                p.push_back(c - r * din + r * dout + r * (-dout * std::cos(t) + din * std::sin(t)));
            else
                p.push_back(c + r * (-din * std::cos(t) + dout * std::sin(t)));
        }
    }
    return p;
}

// A line of width `w` along a closed path, as one convex quad per segment with
// mitred joints (the path's corners are at most square, so the mitres stay short).
void stroke(const std::vector<glm::vec2>& path, float w, std::vector<Poly>& out) {
    const std::size_t n = path.size();
    if (n < 3) return;
    std::vector<glm::vec2> inner(n), outer(n);
    for (std::size_t i = 0; i < n; ++i) {
        const glm::vec2 a = path[(i + n - 1) % n], p = path[i], b = path[(i + 1) % n];
        const glm::vec2 t0 = glm::normalize(p - a), t1 = glm::normalize(b - p);
        const glm::vec2 n0(t0.y, -t0.x), n1(t1.y, -t1.x);   // right of travel = outward (CCW)
        glm::vec2 m = n0 + n1;
        const float ml = glm::length(m);
        m = ml > 1e-5f ? m / ml : n0;
        const float len = 0.5f * w / std::max(0.35f, glm::dot(m, n0));
        outer[i] = p + m * len;
        inner[i] = p - m * len;
    }
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t j = (i + 1) % n;
        out.push_back({{inner[i], inner[j], outer[j], outer[i]}});
    }
}

Poly disc(glm::vec2 c, float r) {
    Poly p;
    const int segs = 14;
    for (int i = 0; i < segs; ++i) {
        const float t = 2.0f * kPi * i / segs;
        p.pts.push_back(c + r * glm::vec2(std::cos(t), std::sin(t)));
    }
    return p;
}

std::uint32_t toByte(float v) {
    return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

AssetId ensure(std::vector<MaterialDef>& mats, const std::string& name, glm::vec3 albedo,
               float refl, float rough) {
    for (MaterialDef& m : mats) {
        if (m.name != name) continue;
        m.albedo = albedo; m.reflectivity = refl; m.roughness = rough;
        if (!m.assetId.valid()) m.assetId = AssetId::generate();
        return m.assetId;
    }
    MaterialDef md;
    md.assetId      = AssetId::generate();
    md.name         = name;
    md.albedo       = albedo;
    md.reflectivity = refl;
    md.roughness    = rough;
    mats.push_back(md);
    return md.assetId;
}

// --- The model -----------------------------------------------------------------

// Faces collected in the sign's frame, each wound to face `outward`.
struct Builder {
    std::vector<Poly3> polys;
    bool post = false;   // what the next faces belong to

    void face(std::vector<glm::vec3> p, glm::vec3 outward, AssetId mat) {
        std::vector<glm::vec3> clean;
        for (const glm::vec3& v : p)
            if (clean.empty() || glm::length(v - clean.back()) > 1e-6f) clean.push_back(v);
        while (clean.size() > 1 && glm::length(clean.front() - clean.back()) <= 1e-6f)
            clean.pop_back();
        if (clean.size() < 3) return;
        glm::vec3 n(0.0f);
        for (std::size_t i = 0; i < clean.size(); ++i) {
            const glm::vec3& a = clean[i];
            const glm::vec3& b = clean[(i + 1) % clean.size()];
            n += glm::vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x),
                           (a.x - b.x) * (a.y + b.y));
        }
        if (glm::dot(n, n) < 1e-16f) return;
        if (glm::dot(n, outward) < 0.0f) { std::reverse(clean.begin(), clean.end()); n = -n; }
        polys.push_back({std::move(clean), glm::normalize(n), mat, post});
    }

    // An upright cylinder (no bottom), for the post.
    void cylinder(float r, float y0, float y1, AssetId mat, int segs = 10) {
        std::vector<glm::vec3> top;
        for (int i = 0; i < segs; ++i) {
            const float a0 = 2.0f * kPi * i / segs, a1 = 2.0f * kPi * (i + 1) / segs;
            const glm::vec3 p0(r * std::cos(a0), 0.0f, r * std::sin(a0));
            const glm::vec3 p1(r * std::cos(a1), 0.0f, r * std::sin(a1));
            const float am = 0.5f * (a0 + a1);
            face({p0 + glm::vec3(0, y0, 0), p1 + glm::vec3(0, y0, 0), p1 + glm::vec3(0, y1, 0),
                  p0 + glm::vec3(0, y1, 0)},
                 glm::vec3(std::cos(am), 0.0f, std::sin(am)), mat);
            top.push_back(p0 + glm::vec3(0, y1, 0));
        }
        face(top, {0, 1, 0}, mat);
    }

    void box(glm::vec3 lo, glm::vec3 hi, AssetId mat) {
        const glm::vec3 a = lo, b = hi;
        face({{a.x, a.y, b.z}, {b.x, a.y, b.z}, {b.x, b.y, b.z}, {a.x, b.y, b.z}}, {0, 0, 1}, mat);
        face({{a.x, a.y, a.z}, {b.x, a.y, a.z}, {b.x, b.y, a.z}, {a.x, b.y, a.z}}, {0, 0, -1}, mat);
        face({{b.x, a.y, a.z}, {b.x, a.y, b.z}, {b.x, b.y, b.z}, {b.x, b.y, a.z}}, {1, 0, 0}, mat);
        face({{a.x, a.y, a.z}, {a.x, a.y, b.z}, {a.x, b.y, b.z}, {a.x, b.y, a.z}}, {-1, 0, 0}, mat);
        face({{a.x, b.y, a.z}, {b.x, b.y, a.z}, {b.x, b.y, b.z}, {a.x, b.y, b.z}}, {0, 1, 0}, mat);
        face({{a.x, a.y, a.z}, {b.x, a.y, a.z}, {b.x, a.y, b.z}, {a.x, a.y, b.z}}, {0, -1, 0}, mat);
    }

    // One blade: the plate as a slab `T` thick centred on `at`, running along `u`
    // (horizontal, unit), its front facing w = u x up. The lettering lies a hair
    // proud of the front -- and of the back too when `bothSides`, mirrored so it
    // reads left to right from there as well.
    void blade(const Face& f, glm::vec3 at, glm::vec3 u, bool bothSides, const Palette& P) {
        const float T = 0.02f, lift = 0.0015f;
        const glm::vec3 up(0.0f, 1.0f, 0.0f);
        const glm::vec3 w = glm::normalize(glm::cross(u, up));
        auto P3 = [&](glm::vec2 q, float z) { return at + u * q.x + up * q.y + w * z; };
        std::vector<glm::vec3> front, back;
        for (const glm::vec2& q : f.outline) {
            front.push_back(P3(q, 0.5f * T));
            back.push_back(P3(q, -0.5f * T));
        }
        face(front, w, P.plate);
        face(back, -w, P.plate);
        const std::size_t n = f.outline.size();
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t j = (i + 1) % n;
            const glm::vec2 e = f.outline[j] - f.outline[i];
            const glm::vec2 o = glm::normalize(glm::vec2(e.y, -e.x));
            face({P3(f.outline[i], -0.5f * T), P3(f.outline[j], -0.5f * T),
                  P3(f.outline[j], 0.5f * T), P3(f.outline[i], 0.5f * T)},
                 u * o.x + up * o.y, P.plate);
        }
        for (const Poly& p : f.ink) {
            std::vector<glm::vec3> q;
            for (const glm::vec2& v : p.pts) q.push_back(P3(v, 0.5f * T + lift));
            face(q, w, P.ink);
            if (!bothSides) continue;
            q.clear();
            for (const glm::vec2& v : p.pts) q.push_back(P3({-v.x, v.y}, -0.5f * T - lift));
            face(q, -w, P.ink);
        }
    }
};

} // namespace

// --- Styles --------------------------------------------------------------------

const char* frameName(Frame f) {
    switch (f) {
        case Frame::None:    return "None";
        case Frame::Line:    return "Line";
        case Frame::Rounded: return "Rounded";
        case Frame::Notched: return "Notched";
        default:             return "?";
    }
}

const char* mountName(Mount m) {
    switch (m) {
        case Mount::Post:  return "On a post";
        case Mount::Plate: return "Plate only";
        default:           return "?";
    }
}

const std::vector<Preset>& presets() {
    static const std::vector<Preset> list = [] {
        auto st = [](glm::vec3 plate, glm::vec3 ink, Frame fr, bool dots, bool caps,
                     float corner) {
            Style s;
            s.plate = plate; s.ink = ink; s.frame = fr; s.dots = dots; s.capitals = caps;
            s.corner = corner;
            return s;
        };
        const glm::vec3 white(0.93f, 0.93f, 0.91f), black(0.05f, 0.05f, 0.06f);
        const glm::vec3 blue(0.04f, 0.20f, 0.46f), navy(0.03f, 0.07f, 0.22f);
        return std::vector<Preset>{
            {"Classic",    st(white, black, Frame::Line, false, false, 0.0f)},
            {"Enamel",     st(navy, white, Frame::Line, false, false, 0.25f)},
            {"Munich",     st(blue, white, Frame::None, true, false, 0.0f)},
            {"Capitals",   st(blue, white, Frame::None, true, true, 0.0f)},
            {"Green",      st(glm::vec3(0.02f, 0.30f, 0.15f), white, Frame::None, true, false, 0.0f)},
            {"Ornate",     st(navy, white, Frame::Notched, false, false, 0.0f)},
            {"Duesseldorf", st(black, white, Frame::Line, false, false, 0.0f)},
        };
    }();
    return list;
}

Style presetStyle(int index) {
    const auto& p = presets();
    if (index < 0 || index >= static_cast<int>(p.size())) index = 0;
    return p[static_cast<std::size_t>(index)].style;
}

// --- Layout --------------------------------------------------------------------

bool glyphFellBack(char32_t c) {
    const auto it = typeface().glyphs.find(c);
    return it != typeface().glyphs.end() && it->second.fellBack;
}

std::u32string lettered(const std::string& utf8, bool capitals) {
    const Typeface& tf = typeface();
    std::u32string out;
    for (char32_t c : decodeUtf8(utf8)) {
        if (capitals) {
            if (c == 0xDF) { out += U"SS"; continue; }   // as the older capital signs write it
            if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7)) c -= 0x20;
        }
        if (c == '\t') c = ' ';
        if (tf.glyphs.count(c)) out += c;
    }
    return out;
}

Face layout(const std::string& utf8, const Style& style, float L) {
    const Typeface& tf = typeface();
    L = std::max(L, 0.005f);
    const float s  = L / font::kCapHeight;
    const float sx = s * std::clamp(style.condense, 0.5f, 1.5f);
    // Capitals sit in the middle; mixed case a little low, so the ascenders and
    // the descenders share the space the way they do on a real sign.
    const float base = style.capitals ? -0.5f * L : -0.42f * L;

    Face f;
    std::vector<Poly> text;
    float pen = 0.0f, lo = 1e9f, hi = -1e9f;
    char32_t prev = 0;
    for (char32_t c : lettered(utf8, style.capitals)) {
        const GlyphShape& g = tf.glyphs.at(c);
        if (prev) {
            auto k = tf.kerning.find({prev, c});
            if (k != tf.kerning.end()) pen += k->second * sx;
        }
        for (const std::vector<glm::vec2>& piece : g.pieces) {
            Poly p;
            for (const glm::vec2& v : piece) {
                p.pts.emplace_back(pen + v.x * sx, base + v.y * s);
                lo = std::min(lo, p.pts.back().x);
                hi = std::max(hi, p.pts.back().x);
            }
            text.push_back(std::move(p));
        }
        pen += g.advance * sx;
        prev = c;
    }
    if (text.empty()) { lo = 0.0f; hi = 2.0f * L; }   // an empty name: a short blank plate
    const float shift = -0.5f * (lo + hi);
    for (Poly& p : text)
        for (glm::vec2& v : p.pts) v.x += shift;
    const float textW = hi - lo;

    const bool  framed = style.frame != Frame::None;
    const float inset = 0.20f * L, rule = 0.075f * L;
    const float dotR = 0.15f * L, dotGap = 0.5f * L;
    float pad = 0.55f * L + (framed ? inset + rule : 0.25f * L);
    if (style.dots) pad += dotGap + 2.0f * dotR;
    f.width  = textW + 2.0f * pad;
    f.height = 2.4f * L;
    const float hw = 0.5f * f.width, hh = 0.5f * f.height;

    f.outline = framePath(hw, hh, style.corner * L, Frame::Rounded);
    if (framed) {
        const float a = hw - inset - 0.5f * rule, b = hh - inset - 0.5f * rule;
        const float r = style.frame == Frame::Line ? 0.0f : 0.40f * L;
        stroke(framePath(a, b, r, style.frame), rule, f.ink);
    }
    if (style.dots) {
        const float x = 0.5f * textW + dotGap + dotR, y = base + 0.5f * L;
        f.ink.push_back(disc({-x, y}, dotR));
        f.ink.push_back(disc({x, y}, dotR));
    }
    for (Poly& p : text) f.ink.push_back(std::move(p));
    return f;
}

// --- Materials -----------------------------------------------------------------

Palette ensurePalette(std::vector<MaterialDef>& mats, const Style& st) {
    auto colour = [&](const char* part, glm::vec3 c) {
        char name[64];
        std::snprintf(name, sizeof name, "Street Sign %s#%02X%02X%02X", part, toByte(c.r),
                      toByte(c.g), toByte(c.b));
        // Enamel: a satin gloss, not a mirror.
        return ensure(mats, name, c, 0.04f, 0.38f);
    };
    Palette p;
    p.plate = colour("", st.plate);
    p.ink   = colour("Ink ", st.ink);
    p.post  = ensure(mats, "Street Sign Post", {0.46f, 0.48f, 0.50f}, 0.25f, 0.45f);
    return p;
}

// --- The model -----------------------------------------------------------------

std::vector<Poly3> build(const Params& p, const Palette& pal) {
    Builder b;
    const float L = std::clamp(p.letterHeight, 0.01f, 1.0f);
    const Face a = layout(p.textA, p.style, L);
    if (p.mount == Mount::Plate) {
        b.blade(a, glm::vec3(0.0f), {1.0f, 0.0f, 0.0f}, false, pal);
        return std::move(b.polys);
    }
    // Post: the blades stack ON the post, not through it -- a post running up
    // the middle of a blade would stand in front of the lettering. The lower
    // blade sits on the post's collar, the upper one on a spacer above it.
    const float H = std::max(p.postHeight, 0.3f);
    const bool  two = !p.textB.empty();
    b.post = true;
    b.cylinder(0.035f, 0.0f, H - 0.03f, pal.post);
    b.cylinder(0.048f, H - 0.03f, H, pal.post);
    b.post = false;
    float y = H;
    if (two) {
        const Face bf = layout(p.textB, p.style, L);
        const float r = glm::radians(p.angleB);
        b.blade(bf, {0.0f, y + 0.5f * bf.height, 0.0f}, {std::cos(r), 0.0f, -std::sin(r)}, true, pal);
        y += bf.height;
        b.post = true;
        b.box({-0.03f, y, -0.03f}, {0.03f, y + 0.035f, 0.03f}, pal.post);
        b.post = false;
        y += 0.035f;
    }
    b.blade(a, {0.0f, y + 0.5f * a.height, 0.0f}, {1.0f, 0.0f, 0.0f}, true, pal);
    return std::move(b.polys);
}

std::vector<std::pair<AssetId, fitzel::MeshData>> meshes(const std::vector<Poly3>& polys) {
    // Welded: the pieces of a letter meet corner to corner (the cut evaluates
    // a shared edge at a shared height to the same float), so a corner used by
    // two pieces facing the same way is stored once. Halves a town's signs.
    struct Key {
        glm::vec3 p, n;
        bool operator==(const Key& o) const { return p == o.p && n == o.n; }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const {
            std::size_t h = 1469598103934665603ull;
            const float f[6] = {k.p.x, k.p.y, k.p.z, k.n.x, k.n.y, k.n.z};
            for (float v : f) {
                std::uint32_t b;
                std::memcpy(&b, &v, sizeof b);
                h = (h ^ b) * 1099511628211ull;
            }
            return h;
        }
    };
    struct Part {
        fitzel::MeshData md;
        std::unordered_map<Key, std::uint32_t, KeyHash> index;
    };
    std::map<AssetId, Part> parts;
    std::vector<std::uint32_t> ids;
    for (const Poly3& p : polys) {
        Part& part = parts[p.material];
        fitzel::MeshData& md = part.md;
        const glm::vec3 an = glm::abs(p.normal);
        ids.clear();
        for (const glm::vec3& v : p.pts) {
            const auto [it, fresh] =
                part.index.try_emplace(Key{v, p.normal}, static_cast<std::uint32_t>(md.vertices.size()));
            if (fresh) {
                fitzel::Vertex vx{};
                vx.position = v;
                vx.normal   = p.normal;
                vx.uv = (an.y >= an.x && an.y >= an.z) ? glm::vec2(v.x, v.z)
                      : (an.x >= an.z)                 ? glm::vec2(v.z, v.y)
                                                       : glm::vec2(v.x, v.y);
                md.vertices.push_back(vx);
            }
            ids.push_back(it->second);
        }
        for (std::size_t i = 1; i + 1 < ids.size(); ++i) {
            md.indices.push_back(ids[0]);
            md.indices.push_back(ids[i]);
            md.indices.push_back(ids[i + 1]);
        }
    }
    std::vector<std::pair<AssetId, fitzel::MeshData>> out;
    for (auto& [id, part] : parts)
        if (!part.md.vertices.empty()) out.emplace_back(id, std::move(part.md));
    return out;
}

std::vector<Entity> entities(const Params& p, const Palette& pal, int& counter,
                             const glm::vec3& at) {
    const std::vector<Poly3> polys = build(p, pal);
    std::vector<Entity> out;
    Entity root;
    root.type        = EntityType::Empty;
    root.name        = "Street sign";
    root.id          = counter++;
    root.parent      = -1;
    root.half        = glm::vec3(0.5f);
    root.localCenter = root.center = at;
    {
        auto sc = std::make_unique<StreetSignComponent>();
        sc->params = p;
        root.components.items.push_back(std::move(sc));
    }
    out.push_back(std::move(root));
    const int rootId = out.front().id;

    // One child per part, re-centred on its own bounds (the Modeling panel's
    // invariant: fitScale() is 1, and pick box, gizmo and collider all describe
    // the shape that is there). The object's material is the one most faces
    // wear; only the others are stored per face.
    auto add = [&](const char* name, bool postPart, bool collide) {
        EditMesh m;
        std::map<AssetId, int> tally;
        for (const Poly3& q : polys) {
            if (q.post != postPart) continue;
            std::vector<int> f;
            for (const glm::vec3& v : q.pts) {
                f.push_back(static_cast<int>(m.verts.size()));
                m.verts.push_back(v);
            }
            m.faces.push_back(std::move(f));
            m.faceMat.push_back(q.material);
            ++tally[q.material];
        }
        if (m.faces.empty()) return;
        AssetId own;
        int best = -1;
        for (const auto& [id, n] : tally) if (n > best) { best = n; own = id; }
        for (AssetId& id : m.faceMat) if (id == own) id = AssetId{};
        if (!m.dressed()) m.faceMat.clear();

        const glm::vec3 shift = editmesh::recenter(m);
        glm::vec3 mn, mx;
        m.bounds(mn, mx);
        Entity e;
        e.type        = EntityType::Box;
        e.name        = name;
        e.id          = counter++;
        e.parent      = rootId;
        e.half        = glm::max(0.5f * (mx - mn), glm::vec3(0.005f));
        e.localCenter = shift;
        e.center      = at + shift;
        auto mc = std::make_unique<MeshComponent>();
        mc->mesh = std::move(m);
        mc->touch();
        e.components.items.push_back(std::move(mc));
        if (own.valid()) {
            auto mat = std::make_unique<MaterialComponent>();
            mat->material = own;
            e.components.items.push_back(std::move(mat));
        }
        if (collide) {
            auto pc = std::make_unique<PhysicsComponent>();
            pc->dynamic = false;
            e.components.items.push_back(std::move(pc));
        }
        out.push_back(std::move(e));
    };
    add("Post", true, true);
    add("Sign", false, false);
    return out;
}

// --- Names ------------------------------------------------------------------------

const std::vector<std::string>& frankfurtNames() {
    static const std::vector<std::string> names = {
#include "StreetNamesFrankfurt.inc"
    };
    return names;
}

// --- Persistence ---------------------------------------------------------------

namespace {
nlohmann::json colJson(const glm::vec3& v) { return nlohmann::json::array({v.x, v.y, v.z}); }
glm::vec3 jsonCol(const nlohmann::json& j, const glm::vec3& def) {
    if (!j.is_array() || j.size() != 3) return def;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}
} // namespace

void saveParams(nlohmann::json& j, const Params& p) {
    j["textA"] = p.textA;
    j["textB"] = p.textB;
    j["preset"] = p.preset;
    j["plate"] = colJson(p.style.plate);
    j["ink"] = colJson(p.style.ink);
    j["frame"] = static_cast<int>(p.style.frame);
    j["dots"] = p.style.dots;
    j["capitals"] = p.style.capitals;
    j["condense"] = p.style.condense;
    j["corner"] = p.style.corner;
    j["letterHeight"] = p.letterHeight;
    j["mount"] = static_cast<int>(p.mount);
    j["postHeight"] = p.postHeight;
    j["angleB"] = p.angleB;
}

void loadParams(const nlohmann::json& j, Params& p) {
    const Params d;
    p.textA  = j.value("textA", d.textA);
    p.textB  = j.value("textB", d.textB);
    p.preset = j.value("preset", d.preset);
    const Style ds = presetStyle(p.preset);
    p.style.plate    = jsonCol(j.value("plate", nlohmann::json()), ds.plate);
    p.style.ink      = jsonCol(j.value("ink", nlohmann::json()), ds.ink);
    p.style.frame    = static_cast<Frame>(std::clamp(j.value("frame", static_cast<int>(ds.frame)), 0,
                                                     static_cast<int>(Frame::Count) - 1));
    p.style.dots     = j.value("dots", ds.dots);
    p.style.capitals = j.value("capitals", ds.capitals);
    p.style.condense = j.value("condense", ds.condense);
    p.style.corner   = j.value("corner", ds.corner);
    p.letterHeight   = j.value("letterHeight", d.letterHeight);
    p.mount          = static_cast<Mount>(std::clamp(j.value("mount", 0), 0,
                                                     static_cast<int>(Mount::Count) - 1));
    p.postHeight     = j.value("postHeight", d.postHeight);
    p.angleB         = j.value("angleB", d.angleB);
}

namespace {
// Scene files name components by type id; this lets a saved sign come back as
// a sign, parameters and all.
struct RegisterSign {
    RegisterSign() {
        components::registerType({"streetSign", "Street sign",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<StreetSignComponent>()); },
            /*addable=*/false});
    }
} g_registerSign;
} // namespace

} // namespace streetsign
