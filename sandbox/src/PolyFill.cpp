#include "PolyFill.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <utility>

namespace polyfill {

namespace {

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

// A corner on its way through the clipper: where it is, and which corner of
// `Fill::points` it is (a bridge visits two corners twice; both visits are the
// one corner).
struct P {
    DV  p;
    int id;
};

// Splice `hole` (clockwise) into `outer` (counter-clockwise, holes already
// joined) through a bridge from the hole's rightmost corner to a corner of the
// outline that sees it.
bool bridge(std::vector<P>& outer, const std::vector<P>& hole) {
    std::size_t m = 0;
    for (std::size_t i = 1; i < hole.size(); ++i)
        if (hole[i].p.x > hole[m].p.x) m = i;
    const DV M = hole[m].p;
    // Nearest crossing of the ray M -> +x with the outline.
    double bestX = 1e300;
    std::size_t edge = outer.size();
    for (std::size_t i = 0; i < outer.size(); ++i) {
        const DV& a = outer[i].p;
        const DV& b = outer[(i + 1) % outer.size()].p;
        if ((a.y > M.y) == (b.y > M.y) || a.y == b.y) continue;
        const double x = a.x + (M.y - a.y) * (b.x - a.x) / (b.y - a.y);
        if (x >= M.x && x < bestX) { bestX = x; edge = i; }
    }
    if (edge == outer.size()) return false;
    const DV I(bestX, M.y);
    const std::size_t e1 = (edge + 1) % outer.size();
    std::size_t p = outer[edge].p.x > outer[e1].p.x ? edge : e1;
    if (outer[edge].p == I) p = edge;
    else if (outer[e1].p == I) p = e1;
    else {
        // A reflex corner inside the triangle M, I, P would hide P from M:
        // take the one closest in angle to the ray instead.
        const DV P0 = outer[p].p;
        double bestAng = 1e300, bestLen = 1e300;
        for (std::size_t i = 0; i < outer.size(); ++i) {
            if (i == p) continue;
            const DV& q = outer[i].p;
            const DV& a = outer[(i + outer.size() - 1) % outer.size()].p;
            const DV& c = outer[(i + 1) % outer.size()].p;
            if (cross(a, q, c) >= 0.0) continue;                 // not reflex
            const double d1 = cross(M, I, q), d2 = cross(I, P0, q), d3 = cross(P0, M, q);
            const bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
            if (neg && pos) continue;                            // outside the triangle
            const DV d = q - M;
            const double ang = std::atan2(std::abs(d.y), d.x), len = glm::length(d);
            if (ang < bestAng || (ang == bestAng && len < bestLen)) {
                bestAng = ang; bestLen = len; p = i;
            }
        }
    }
    std::vector<P> out(outer.begin(), outer.begin() + static_cast<std::ptrdiff_t>(p) + 1);
    for (std::size_t k = 0; k <= hole.size(); ++k) out.push_back(hole[(m + k) % hole.size()]);
    out.push_back(outer[p]);
    out.insert(out.end(), outer.begin() + static_cast<std::ptrdiff_t>(p) + 1, outer.end());
    outer = std::move(out);
    return true;
}

bool clip(const std::vector<P>& poly, std::vector<glm::ivec3>& out) {
    std::vector<std::size_t> idx(poly.size());
    for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::size_t guard = 0;
    while (idx.size() > 3) {
        if (++guard > 4 * poly.size() * poly.size() + 16) return false;
        bool clipped = false;
        const std::size_t n = idx.size();
        for (std::size_t k = 0; k < n; ++k) {
            const P& a = poly[idx[(k + n - 1) % n]];
            const P& b = poly[idx[k]];
            const P& c = poly[idx[(k + 1) % n]];
            if (cross(a.p, b.p, c.p) <= 0.0) continue;            // reflex or flat
            bool empty = true;
            for (std::size_t j = 0; j < n && empty; ++j) {
                const DV& q = poly[idx[j]].p;
                if (q == a.p || q == b.p || q == c.p) continue;   // a bridge's twin corners
                empty = !(cross(a.p, b.p, q) >= 0.0 && cross(b.p, c.p, q) >= 0.0 && cross(c.p, a.p, q) >= 0.0);
            }
            if (!empty) continue;
            out.emplace_back(a.id, b.id, c.id);
            idx.erase(idx.begin() + static_cast<std::ptrdiff_t>(k));
            clipped = true;
            break;
        }
        if (!clipped) {
            // Only flat corners left to take (a bridge lying on an edge): drop one.
            bool dropped = false;
            for (std::size_t k = 0; k < n && !dropped; ++k) {
                const DV& a = poly[idx[(k + n - 1) % n]].p;
                const DV& b = poly[idx[k]].p;
                const DV& c = poly[idx[(k + 1) % n]].p;
                if (std::abs(cross(a, b, c)) < 1e-9) {
                    idx.erase(idx.begin() + static_cast<std::ptrdiff_t>(k));
                    dropped = true;
                }
            }
            if (!dropped) return false;
        }
    }
    if (idx.size() == 3 && cross(poly[idx[0]].p, poly[idx[1]].p, poly[idx[2]].p) > 0.0)
        out.emplace_back(poly[idx[0]].id, poly[idx[1]].id, poly[idx[2]].id);
    return true;
}

} // namespace

namespace {

bool fillOnce(const std::vector<std::vector<DV>>& outlines, Fill& out) {
    out = Fill{};
    std::vector<std::vector<DV>> cs;
    for (std::vector<DV> p : outlines) {
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
    std::vector<std::vector<P>> ps(n);
    for (std::size_t i = 0; i < n; ++i) {
        const bool hole = depth[i] % 2 == 1;
        if ((signedArea(cs[i]) > 0.0) == hole) std::reverse(cs[i].begin(), cs[i].end());
        out.loops.emplace_back();
        for (const DV& v : cs[i]) {
            ps[i].push_back({v, static_cast<int>(out.points.size())});
            out.loops.back().push_back(static_cast<int>(out.points.size()));
            out.points.push_back(v);
        }
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
        std::vector<P> poly = ps[i];
        for (std::size_t h : holes)
            if (!bridge(poly, ps[h])) return false;
        if (!clip(poly, out.tris)) return false;
    }
    return true;
}

} // namespace

bool fill(const std::vector<std::vector<DV>>& outlines, Fill& out, bool seal) {
    const bool ok = fillOnce(outlines, out);
    if (!seal) return ok;
    if (ok && sealed(out)) return true;
    for (const double a : {0.0007, -0.0013, 0.0029}) {
        const double c = std::cos(a), s = std::sin(a);
        std::vector<std::vector<DV>> turned = outlines;
        for (std::vector<DV>& l : turned)
            for (DV& v : l) v = DV(c * v.x - s * v.y, s * v.x + c * v.y);
        Fill f;
        if (fillOnce(turned, f) && sealed(f)) {
            for (DV& v : f.points) v = DV(c * v.x + s * v.y, -s * v.x + c * v.y);
            out = std::move(f);
            return true;
        }
    }
    return false;
}

bool sealed(const Fill& f) {
    std::map<std::pair<int, int>, int> inner, outline;
    for (const glm::ivec3& t : f.tris)
        for (int k = 0; k < 3; ++k) ++inner[{t[k], t[(k + 1) % 3]}];
    for (const std::vector<int>& l : f.loops)
        for (std::size_t k = 0; k < l.size(); ++k) ++outline[{l[k], l[(k + 1) % l.size()]}];
    for (const auto& [e, n] : inner) {
        if (n != 1) return false;
        const bool back = inner.count({e.second, e.first}) != 0;
        if (back == (outline.count(e) != 0)) return false;   // an outline edge has no twin, an inner one has
    }
    for (const auto& [e, n] : outline)
        if (n != 1 || !inner.count(e)) return false;          // every outline edge drawn, once
    return !f.tris.empty();
}

std::vector<Trap> trapezoids(const std::vector<std::vector<DV>>& outlines) {
    struct Edge { double xa, ya, xb, yb; int dir; };
    std::vector<Edge> edges;
    std::vector<double> ys;
    for (const std::vector<DV>& c : outlines)
        for (std::size_t i = 0; i < c.size(); ++i) {
            const DV& a = c[i];
            const DV& b = c[(i + 1) % c.size()];
            ys.push_back(a.y);
            if (a.y == b.y) continue;
            if (a.y < b.y) edges.push_back({a.x, a.y, b.x, b.y, 1});
            else           edges.push_back({b.x, b.y, a.x, a.y, -1});
        }
    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
    auto xAt = [&](int e, double y) {
        const Edge& E = edges[static_cast<std::size_t>(e)];
        // Exact at the ends, so the piece below a corner and the piece above it
        // name that corner with the same number.
        if (y <= E.ya) return E.xa;
        if (y >= E.yb) return E.xb;
        return E.xa + (y - E.ya) * (E.xb - E.xa) / (E.yb - E.ya);
    };

    struct Open { int l, r; double y0; };
    std::vector<Trap> out;
    std::vector<Open> open, next;
    auto emit = [&](const Open& o, double y1) {
        out.push_back({o.y0, y1, xAt(o.l, o.y0), xAt(o.r, o.y0), xAt(o.l, y1), xAt(o.r, y1)});
    };
    std::vector<std::pair<double, int>> act;
    for (std::size_t k = 0; k + 1 < ys.size(); ++k) {
        const double y0 = ys[k], y1 = ys[k + 1], ym = 0.5 * (y0 + y1);
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
        for (Open& nx : next)
            for (Open& o : open)
                if (o.l == nx.l && o.r == nx.r && o.l >= 0) { nx.y0 = o.y0; o.l = -1; break; }
        for (const Open& o : open)
            if (o.l >= 0) emit(o, y0);
        std::swap(open, next);
    }
    if (!ys.empty())
        for (const Open& o : open) emit(o, ys.back());
    return out;
}

double area(const Fill& f) {
    double a = 0.0;
    for (const glm::ivec3& t : f.tris) {
        const DV& p = f.points[static_cast<std::size_t>(t.x)];
        const DV& q = f.points[static_cast<std::size_t>(t.y)];
        const DV& r = f.points[static_cast<std::size_t>(t.z)];
        a += 0.5 * cross(p, q, r);
    }
    return a;
}

double area(const std::vector<Trap>& ts) {
    double a = 0.0;
    for (const Trap& t : ts) a += 0.5 * ((t.xr0 - t.xl0) + (t.xr1 - t.xl1)) * (t.y1 - t.y0);
    return a;
}

} // namespace polyfill
