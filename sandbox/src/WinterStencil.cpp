#include "WinterStencil.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "SolidBuild.hpp"

using fitzel::AssetId;

namespace winterstencil {

namespace {

using solidbuild::Builder;
using solidbuild::DV2;
using solidbuild::DV3;
using solidbuild::kPi;
using solidbuild::Ring;

constexpr double kPla  = 1.24;    // g/cm3
constexpr double kFilR = 0.875;   // 1.75 mm filament's radius

// --- Chance and noise, the same on every machine ---------------------------------------

struct Rng {
    std::uint64_t s;
    explicit Rng(int seed) : s(0x9E3779B97F4A7C15ull * static_cast<std::uint64_t>(seed + 1) + 0x7F4A7C159E3779B9ull) {}
    double next() {   // [0, 1), splitmix64
        std::uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        return static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0);
    }
    double pm() { return 2.0 * next() - 1.0; }   // [-1, 1)
    double in(double a, double b) { return a + (b - a) * next(); }
};

double lattice(int x, int seed) {
    std::uint64_t h = static_cast<std::uint64_t>(x) * 0x9E3779B97F4A7C15ull ^ static_cast<std::uint64_t>(seed) * 0xC2B2AE3D27D4EB4Full;
    h = (h ^ (h >> 31)) * 0xBF58476D1CE4E5B9ull;
    h ^= h >> 29;
    return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0);
}

// Smooth noise along a line, two octaves, 0..1.
double noise1(double x, int seed) {
    double s = 0.0, w = 0.65;
    for (int o = 0; o < 2; ++o) {
        const double f = std::floor(x);
        const double t = x - f, u = t * t * (3.0 - 2.0 * t);
        const int i = static_cast<int>(f);
        s += w * (lattice(i, seed + o) + (lattice(i + 1, seed + o) - lattice(i, seed + o)) * u);
        x = x * 2.1 + 5.3;
        w = 0.35;
    }
    return s;
}

// --- Distances (negative inside) ------------------------------------------------------

double sdCircle(DV2 p, DV2 c, double r) { return glm::length(p - c) - r; }

double sdSegment(DV2 p, DV2 a, DV2 b, double r) {
    const double t = std::clamp(glm::dot(p - a, b - a) / std::max(1e-12, glm::dot(b - a, b - a)), 0.0, 1.0);
    return glm::length(p - (a + t * (b - a))) - r;
}

double sdBox(DV2 p, DV2 lo, DV2 hi, double round = 0.0) {
    const DV2 c = 0.5 * (lo + hi), half = 0.5 * (hi - lo) - DV2(round);
    const DV2 d = glm::abs(p - c) - half;
    return glm::length(glm::max(d, DV2(0.0))) + std::min(std::max(d.x, d.y), 0.0) - round;
}

// Any simple polygon (after Inigo Quilez).
double sdPolygon(DV2 p, const std::vector<DV2>& v) {
    double d = glm::dot(p - v[0], p - v[0]), s = 1.0;
    for (std::size_t i = 0, j = v.size() - 1; i < v.size(); j = i, ++i) {
        const DV2 e = v[j] - v[i], w = p - v[i];
        const DV2 b = w - e * std::clamp(glm::dot(w, e) / glm::dot(e, e), 0.0, 1.0);
        d = std::min(d, glm::dot(b, b));
        const bool c1 = p.y >= v[i].y, c2 = p.y < v[j].y, c3 = e.x * w.y > e.y * w.x;
        if ((c1 && c2 && c3) || (!c1 && !c2 && !c3)) s = -s;
    }
    return s * std::sqrt(d);
}

// A piece of the picture: where it lies and how far a point is from it.
struct Shape {
    DV2 lo, hi;
    std::function<double(DV2)> d;
    bool cut = false;   // a hole cut through what is drawn before it
};

Shape polygon(std::vector<DV2> v, bool cut = false) {
    DV2 lo(1e30), hi(-1e30);
    for (const DV2& q : v) {
        lo = glm::min(lo, q);
        hi = glm::max(hi, q);
    }
    return {lo, hi, [v = std::move(v)](DV2 p) { return sdPolygon(p, v); }, cut};
}
Shape circle(DV2 c, double r, bool cut = false) {
    return {c - DV2(r), c + DV2(r), [c, r](DV2 p) { return sdCircle(p, c, r); }, cut};
}
Shape box(DV2 lo, DV2 hi, bool cut = false) {
    return {lo, hi, [lo, hi](DV2 p) { return sdBox(p, lo, hi); }, cut};
}
Shape segment(DV2 a, DV2 b, double r) {
    return {glm::min(a, b) - DV2(r), glm::max(a, b) + DV2(r), [a, b, r](DV2 p) { return sdSegment(p, a, b, r); }};
}

// The picture as distances on a grid.
struct Canvas {
    DV2 lo;
    double h = 0.1;
    int nx = 0, ny = 0;
    std::vector<float> f;

    Canvas(DV2 lo_, DV2 hi, double h_) : lo(lo_), h(h_) {
        nx = static_cast<int>(std::ceil((hi.x - lo.x) / h)) + 1;
        ny = static_cast<int>(std::ceil((hi.y - lo.y) / h)) + 1;
        f.assign(static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny), 1e3f);
    }
    DV2 at(int i, int j) const { return lo + DV2(i, j) * h; }
    template <class Op>
    void over(DV2 a, DV2 b, Op op) {
        const int i0 = std::max(0, static_cast<int>(std::floor((a.x - lo.x) / h)) - 3);
        const int i1 = std::min(nx - 1, static_cast<int>(std::ceil((b.x - lo.x) / h)) + 3);
        const int j0 = std::max(0, static_cast<int>(std::floor((a.y - lo.y) / h)) - 3);
        const int j1 = std::min(ny - 1, static_cast<int>(std::ceil((b.y - lo.y) / h)) + 3);
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) op(f[static_cast<std::size_t>(j) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(i)], at(i, j));
    }
    void draw(const Shape& s) {
        if (s.cut) over(s.lo, s.hi, [&](float& v, DV2 p) { v = std::max(v, static_cast<float>(-s.d(p))); });
        else over(s.lo, s.hi, [&](float& v, DV2 p) { v = std::min(v, static_cast<float>(s.d(p))); });
    }
};

double area(const std::vector<DV2>& l) {
    double a = 0.0;
    for (std::size_t i = 0; i < l.size(); ++i) {
        const DV2 p = l[i], q = l[(i + 1) % l.size()];
        a += 0.5 * (p.x * q.y - q.x * p.y);
    }
    return a;
}

// The outlines where the distance crosses 0 (marching squares; a saddle joined
// across the cell when its middle is inside), the plastic on their left: outer
// outlines counter-clockwise, holes clockwise. Beyond the grid is outside, so
// every outline closes.
std::vector<std::vector<DV2>> trace(const Canvas& c) {
    const int NX = c.nx + 2, NY = c.ny + 2;   // with a ring outside
    auto val = [&](int I, int J) -> double {
        const int i = I - 1, j = J - 1;
        if (i < 0 || j < 0 || i >= c.nx || j >= c.ny) return 1.0;
        const double v = c.f[static_cast<std::size_t>(j) * static_cast<std::size_t>(c.nx) + static_cast<std::size_t>(i)];
        return std::abs(v) < 1e-5 ? -1e-5 : v;   // never on a corner: two shapes meeting there are one
    };
    auto pos = [&](int I, int J) { return c.lo + DV2(I - 1, J - 1) * c.h; };
    struct Seg {
        long long to;
        DV2 p;
    };
    std::unordered_map<long long, Seg> next;
    for (int J = 0; J + 1 < NY; ++J)
        for (int I = 0; I + 1 < NX; ++I) {
            const int ci[4] = {I, I + 1, I + 1, I}, cj[4] = {J, J, J + 1, J + 1};
            double v[4];
            int inside = 0;
            for (int k = 0; k < 4; ++k) {
                v[k] = val(ci[k], cj[k]);
                inside += v[k] < 0.0;
            }
            if (inside == 0 || inside == 4) continue;
            // The cell's edges counter-clockwise, each its own key: bottom,
            // right, top, left.
            const long long key[4] = {2LL * (static_cast<long long>(J) * NX + I), 2LL * (static_cast<long long>(J) * NX + I + 1) + 1,
                                      2LL * (static_cast<long long>(J + 1) * NX + I), 2LL * (static_cast<long long>(J) * NX + I) + 1};
            struct X {
                long long key;
                DV2 p;
                bool leaving;   // inside to outside, going round
            };
            std::vector<X> xs;
            for (int k = 0; k < 4; ++k) {
                const int n = (k + 1) % 4;
                if ((v[k] < 0.0) == (v[n] < 0.0)) continue;
                const double t = v[k] / (v[k] - v[n]);
                const DV2 a = pos(ci[k], cj[k]), b = pos(ci[n], cj[n]);
                xs.push_back({key[k], a + t * (b - a), v[k] < 0.0});
            }
            const bool middleIn = (v[0] + v[1] + v[2] + v[3]) < 0.0;
            for (std::size_t k = 0; k < xs.size(); ++k) {
                if (!xs[k].leaving) continue;
                const std::size_t to = xs.size() == 2 || middleIn ? (k + 1) % xs.size() : (k + xs.size() - 1) % xs.size();
                next[xs[k].key] = {xs[to].key, xs[k].p};
            }
        }
    std::vector<std::vector<DV2>> loops;
    std::unordered_map<long long, bool> done;
    for (const auto& [start, s] : next) {
        if (done[start]) continue;
        std::vector<DV2> loop;
        long long k = start;
        while (!done[k]) {
            done[k] = true;
            const auto it = next.find(k);
            if (it == next.end()) break;
            // A crossing a hair from the last (both by one grid corner) is the same.
            if (loop.empty() || glm::length(it->second.p - loop.back()) > 0.01) loop.push_back(it->second.p);
            k = it->second.to;
        }
        while (loop.size() > 3 && glm::length(loop.front() - loop.back()) <= 0.01) loop.pop_back();
        if (loop.size() >= 3 && std::abs(area(loop)) > 1e-3) loops.push_back(std::move(loop));
    }
    return loops;
}

// Corners dropped that lie within `tol` of the line on from the last one kept
// (a run no longer than `run`), so a straight or gently curved stretch is a
// few corners, not one a grid cell.
std::vector<DV2> simplify(const std::vector<DV2>& in, double tol, double run) {
    const std::size_t n = in.size();
    if (n < 8) return in;
    std::vector<DV2> out{in[0]};
    std::size_t anchor = 0;
    for (std::size_t i = 2; i <= n; ++i) {
        const DV2 end = in[i % n];
        bool fits = glm::length(end - in[anchor]) <= run;
        for (std::size_t k = anchor + 1; k < i && fits; ++k)
            fits = sdSegment(in[k], in[anchor], end, 0.0) <= tol;
        if (!fits) {
            anchor = i - 1;
            out.push_back(in[anchor]);
        }
    }
    return out.size() >= 3 ? out : in;
}


// How wide a hole is: the widest round that fits in it, found on a grid over
// it and refined round the best twice.
double holeWidth(const std::vector<DV2>& loop, DV2& where) {
    DV2 lo(1e30), hi(-1e30);
    for (const DV2& q : loop) {
        lo = glm::min(lo, q);
        hi = glm::max(hi, q);
    }
    auto clearance = [&](DV2 q) {   // to the outline, negative outside the hole
        double d = 1e30;
        bool in = false;
        for (std::size_t i = 0; i < loop.size(); ++i) {
            const DV2 A = loop[i], B = loop[(i + 1) % loop.size()];
            d = std::min(d, sdSegment(q, A, B, 0.0));
            if ((A.y > q.y) != (B.y > q.y) && q.x < A.x + (B.x - A.x) * (q.y - A.y) / (B.y - A.y)) in = !in;
        }
        return in ? d : -d;
    };
    DV2 best = 0.5 * (lo + hi), span = hi - lo;
    double r = clearance(best);
    for (int round = 0; round < 3; ++round) {
        const DV2 c = best;
        for (int i = 0; i <= 12; ++i)
            for (int j = 0; j <= 12; ++j) {
                const DV2 q = c + span * DV2(i / 12.0 - 0.5, j / 12.0 - 0.5);
                const double d = clearance(q);
                if (d > r) {
                    r = d;
                    best = q;
                }
            }
        span *= 0.25;
    }
    where = best;
    return 2.0 * r;
}

// Squared distances along one row or column to the nearest marked cell
// (Felzenszwalb and Huttenlocher), in place; `inf` where none is.
void edt1(std::vector<double>& f, std::vector<double>& d, std::vector<int>& v, std::vector<double>& z, int n) {
    int k = 0;
    v[0] = 0;
    z[0] = -1e30;
    z[1] = 1e30;
    for (int q = 1; q < n; ++q) {
        double s;
        for (;;) {
            const int r = v[static_cast<std::size_t>(k)];
            s = ((f[static_cast<std::size_t>(q)] + q * q) - (f[static_cast<std::size_t>(r)] + r * r)) / (2.0 * q - 2.0 * r);
            if (s > z[static_cast<std::size_t>(k)] || k == 0) break;
            --k;
        }
        if (s <= z[static_cast<std::size_t>(k)]) {   // k == 0 and still not past: replace
            v[0] = q;
            z[0] = -1e30;
            z[1] = 1e30;
            k = 0;
            continue;
        }
        ++k;
        v[static_cast<std::size_t>(k)] = q;
        z[static_cast<std::size_t>(k)] = s;
        z[static_cast<std::size_t>(k + 1)] = 1e30;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[static_cast<std::size_t>(k + 1)] < q) ++k;
        const int r = v[static_cast<std::size_t>(k)];
        d[static_cast<std::size_t>(q)] = (q - r) * (q - r) + f[static_cast<std::size_t>(r)];
    }
}

// For every cell, the squared distance (in cells) to the nearest cell where
// `mark` is set.
std::vector<double> edt(const std::vector<char>& mark, int nx, int ny) {
    const double inf = 1e20;
    std::vector<double> g(mark.size());
    for (std::size_t i = 0; i < mark.size(); ++i) g[i] = mark[i] ? 0.0 : inf;
    const int n = std::max(nx, ny);
    std::vector<double> f(static_cast<std::size_t>(n)), d(static_cast<std::size_t>(n)), z(static_cast<std::size_t>(n) + 1);
    std::vector<int> v(static_cast<std::size_t>(n));
    for (int x = 0; x < nx; ++x) {
        for (int y = 0; y < ny; ++y) f[static_cast<std::size_t>(y)] = g[static_cast<std::size_t>(y) * nx + x];
        edt1(f, d, v, z, ny);
        for (int y = 0; y < ny; ++y) g[static_cast<std::size_t>(y) * nx + x] = d[static_cast<std::size_t>(y)];
    }
    for (int y = 0; y < ny; ++y) {
        for (int x = 0; x < nx; ++x) f[static_cast<std::size_t>(x)] = g[static_cast<std::size_t>(y) * nx + x];
        edt1(f, d, v, z, nx);
        for (int x = 0; x < nx; ++x) g[static_cast<std::size_t>(y) * nx + x] = d[static_cast<std::size_t>(x)];
    }
    return g;
}

// The picture made printable where shapes come too close: a gap narrower
// than 2 r closed (the plastic grown by r and shrunk back), an opening that
// holds no round `minHole` across filled, and plastic that holds on to
// nothing dropped (counted: a stencil is one piece). Cells that change sign
// get half a cell's distance, so the outline runs between them.
int tidy(Canvas& c, double r, double minHole, DV2& speck) {
    const int nx = c.nx, ny = c.ny;
    const std::size_t N = c.f.size();
    std::vector<char> in(N), grown(N);
    for (std::size_t i = 0; i < N; ++i) in[i] = c.f[i] < 0.0f;
    const double rc = r / c.h;
    {
        const std::vector<double> d = edt(in, nx, ny);
        for (std::size_t i = 0; i < N; ++i) grown[i] = d[i] <= rc * rc;
    }
    std::vector<char> out(N);
    for (std::size_t i = 0; i < N; ++i) out[i] = !grown[i];
    std::vector<char> closed(N);
    {
        const std::vector<double> d = edt(out, nx, ny);
        for (std::size_t i = 0; i < N; ++i) closed[i] = in[i] || (grown[i] && d[i] > rc * rc);
    }
    // Openings 4-connected, plastic 8-connected (a diagonal step of plastic
    // holds, as the outline traced through it does).
    auto regions = [&](const std::vector<char>& set, std::vector<int>& label, bool diagonal) {
        label.assign(N, -1);
        int n = 0;
        std::vector<std::size_t> stack;
        for (std::size_t s = 0; s < N; ++s) {
            if (!set[s] || label[s] >= 0) continue;
            label[s] = n;
            stack.push_back(s);
            while (!stack.empty()) {
                const std::size_t i = stack.back();
                stack.pop_back();
                const int x = static_cast<int>(i % static_cast<std::size_t>(nx)), y = static_cast<int>(i / static_cast<std::size_t>(nx));
                const int nb[8][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1},
                                      {x - 1, y - 1}, {x + 1, y - 1}, {x - 1, y + 1}, {x + 1, y + 1}};
                for (int k = 0; k < (diagonal ? 8 : 4); ++k) {
                    const int* q = nb[k];
                    if (q[0] < 0 || q[1] < 0 || q[0] >= nx || q[1] >= ny) continue;
                    const std::size_t j = static_cast<std::size_t>(q[1]) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(q[0]);
                    if (set[j] && label[j] < 0) {
                        label[j] = n;
                        stack.push_back(j);
                    }
                }
            }
            ++n;
        }
        return n;
    };
    std::vector<int> label;
    {
        std::vector<char> open(N);
        for (std::size_t i = 0; i < N; ++i) open[i] = !closed[i];
        const int n = regions(open, label, false);
        const std::vector<double> d = edt(closed, nx, ny);
        std::vector<double> widest(static_cast<std::size_t>(n), 0.0);
        for (std::size_t i = 0; i < N; ++i)
            if (label[i] >= 0) widest[static_cast<std::size_t>(label[i])] = std::max(widest[static_cast<std::size_t>(label[i])], d[i]);
        const double need = 0.5 * minHole / c.h;
        for (std::size_t i = 0; i < N; ++i)
            if (label[i] >= 0 && widest[static_cast<std::size_t>(label[i])] < need * need) closed[i] = 1;
    }
    // Plastic touching plastic only across a corner joined properly, so the
    // outline does not pinch there.
    for (int y = 0; y + 1 < ny; ++y)
        for (int x = 0; x + 1 < nx; ++x) {
            const std::size_t a = static_cast<std::size_t>(y) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(x);
            const std::size_t b = a + 1, c2 = a + static_cast<std::size_t>(nx), d = c2 + 1;
            if (closed[a] && closed[d] && !closed[b] && !closed[c2]) closed[b] = closed[c2] = 1;
            if (closed[b] && closed[c2] && !closed[a] && !closed[d]) closed[a] = closed[d] = 1;
        }
    int dropped = 0;
    {
        const int n = regions(closed, label, true);
        std::vector<std::size_t> size(static_cast<std::size_t>(n), 0);
        for (std::size_t i = 0; i < N; ++i)
            if (label[i] >= 0) ++size[static_cast<std::size_t>(label[i])];
        const int keep = n ? static_cast<int>(std::max_element(size.begin(), size.end()) - size.begin()) : -1;
        // A few cells off a sharp tip are the grid's, not the picture's: only
        // what would be a piece of its own (a quarter of a square millimetre
        // and more) counts.
        const std::size_t piece = static_cast<std::size_t>(std::ceil(0.25 / (c.h * c.h)));
        bool first = true;
        for (int k = 0; k < n; ++k)
            if (k != keep && size[static_cast<std::size_t>(k)] >= piece) ++dropped;
        for (std::size_t i = 0; i < N; ++i)
            if (label[i] >= 0 && label[i] != keep) {
                if (first && size[static_cast<std::size_t>(label[i])] >= piece) {
                    speck = c.at(static_cast<int>(i % static_cast<std::size_t>(nx)), static_cast<int>(i / static_cast<std::size_t>(nx)));
                    first = false;
                }
                closed[i] = 0;
            }
    }
    for (std::size_t i = 0; i < N; ++i)
        if (closed[i] != in[i]) c.f[i] = closed[i] ? static_cast<float>(-0.5 * c.h) : static_cast<float>(0.5 * c.h);
    return dropped;
}

// Triangles of a flat face with a corner (all but) on their longest edge --
// as a fill leaves where many corners stand on one line -- turned into two
// with the triangle across that edge: the four corners split the other way.
void unsliver(Builder& b) {
    for (int pass = 0; pass < 20; ++pass) {
        std::unordered_map<long long, std::size_t> owner;
        auto key = [](int u, int v) { return static_cast<long long>(u) << 32 | static_cast<unsigned>(v); };
        for (std::size_t f = 0; f < b.faces.size(); ++f)
            for (std::size_t k = 0; k < b.faces[f].size(); ++k)
                owner[key(b.faces[f][k], b.faces[f][(k + 1) % b.faces[f].size()])] = f;
        bool changed = false;
        for (std::size_t f = 0; f < b.faces.size(); ++f) {
            if (b.faces[f].size() != 3) continue;
            const std::vector<int> t = b.faces[f];
            std::size_t k = 0;
            double best = -1.0;
            for (std::size_t e = 0; e < 3; ++e) {
                const double l = glm::length(b.verts[static_cast<std::size_t>(t[(e + 1) % 3])] - b.verts[static_cast<std::size_t>(t[e])]);
                if (l > best) {
                    best = l;
                    k = e;
                }
            }
            const int A = t[k], B = t[(k + 1) % 3], C = t[(k + 2) % 3];
            const DV3 pa = b.verts[static_cast<std::size_t>(A)], pb = b.verts[static_cast<std::size_t>(B)], pc = b.verts[static_cast<std::size_t>(C)];
            const double area2 = glm::length(glm::cross(pb - pa, pc - pa));
            if (area2 / best > 1e-4) continue;   // C stands off AB by more than a twenty-thousandth of a mm
            const auto it = owner.find(key(B, A));
            if (it == owner.end() || it->second == f || b.faces[it->second].size() != 3) continue;
            std::vector<int>& g = b.faces[it->second];
            int D = -1;
            for (int v : g)
                if (v != A && v != B) D = v;
            const DV3 pd = b.verts[static_cast<std::size_t>(D)];
            if (std::abs(pd.z - pa.z) > 1e-9 || std::abs(pc.z - pa.z) > 1e-9) continue;   // flat faces only
            const DV3 n = glm::cross(pb - pa, pd - pa);
            auto up = [&](const DV3& u, const DV3& v, const DV3& w) { return glm::dot(glm::cross(v - u, w - u), n) < 0.0; };
            // (A, D, C) and (D, B, C), turned as the two were.
            if (!up(pa, pd, pc) || !up(pd, pb, pc)) continue;
            const std::size_t gi = it->second;
            for (const std::vector<int>* face : {&b.faces[f], &g})
                for (std::size_t e = 0; e < 3; ++e) owner.erase(key((*face)[e], (*face)[(e + 1) % 3]));
            b.faces[f] = {A, D, C};
            g = {D, B, C};
            for (std::size_t fi : {f, gi})
                for (std::size_t e = 0; e < 3; ++e) owner[key(b.faces[fi][e], b.faces[fi][(e + 1) % 3])] = fi;
            changed = true;
        }
        if (!changed) break;
    }
}

std::uint32_t toByte(float v) {
    return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

// --- The picture ----------------------------------------------------------------------

// Everything drawn, in the order it is drawn (a hole cuts what is drawn before
// it), and what the sky has to stay clear of.
struct Scene {
    const Params& p;
    Rng rng;
    double a = 0.0, b = 0.0;   // the frame's inside, half across and half up
    double S = 0.0;            // the smaller of the two: what sizes are measured in
    double archY = 0.0;        // an arch: its round's middle
    std::vector<Shape> shapes;
    std::vector<double> sky;   // the highest thing drawn, every millimetre across
    int trees = 0, stars = 0, flakes = 0;

    Scene(const Params& p_) : p(p_), rng(p_.seed) {}

    // The frame's inside: negative inside.
    double inner(DV2 q) const {
        switch (p.frame) {
        case Frame::Round: return glm::length(q) - a;
        case Frame::Rect:  return sdBox(q, DV2(-a, -b), DV2(a, b), std::min(6.0, 0.1 * S));
        default:           return std::min(sdBox(q, DV2(-a, -b), DV2(a, archY)), sdCircle(q, DV2(0.0, archY), a));
        }
    }
    double ground(double x) const {
        const double y0 = -0.5 * b;
        return y0 + p.hills * 0.12 * b * (2.0 * noise1(x / (0.45 * S), p.seed) - 1.0);
    }
    double groundMin(double x0, double x1) const {
        double g = 1e30;
        for (double x = x0; x <= x1 + 1e-9; x += 0.5) g = std::min(g, ground(x));
        return g;
    }
    int col(double x) const { return std::clamp(static_cast<int>(std::lround(x + a)), 0, static_cast<int>(sky.size()) - 1); }
    void raiseSky(double x0, double x1, double top) {
        for (int i = col(x0); i <= col(x1); ++i) sky[static_cast<std::size_t>(i)] = std::max(sky[static_cast<std::size_t>(i)], top);
    }
    double skyOver(double x0, double x1) const {
        double s = -1e30;
        for (int i = col(x0); i <= col(x1); ++i) s = std::max(s, sky[static_cast<std::size_t>(i)]);
        return s;
    }
    void add(Shape s) {
        if (!s.cut) raiseSky(s.lo.x, s.hi.x, s.hi.y);
        shapes.push_back(std::move(s));
    }
    // A thread from q straight up into the frame.
    void hang(DV2 q) { shapes.push_back(segment(q, DV2(q.x, b + p.border + 20.0), 0.5 * p.line)); }

    // A fir of drooping tiers standing on the snow at x.
    void fir(double x, double H) {
        const double base = ground(x) - 1.0, trunkH = 0.0, w = 0.56 * H;
        const int T = H > 0.5 * S ? 4 : 3;
        const double tierH = 0.46 * (H - trunkH);
        std::vector<std::vector<DV2>> tiers;
        for (int k = 0; k < T; ++k) {
            const double yb = base + 0.6 + trunkH + k * (H - trunkH - tierH) / (T - 1);
            const double hw = 0.5 * w * (1.0 - 0.55 * k / (T - 1)), h = tierH;
            // Branches drooping to points along each tier's lower edge -- but
            // the lowest stands on the snow, its edge straight, so no row of
            // specks of light runs along the ground.
            if (k == 0) tiers.push_back({{x + hw, yb}, {x, yb + h}, {x - hw, yb}});
            else tiers.push_back({{x + hw, yb}, {x, yb + h}, {x - hw, yb}, {x - 0.5 * hw, yb + 0.16 * h}, {x, yb + 0.07 * h},
                                  {x + 0.5 * hw, yb + 0.16 * h}});
        }
        const double tw = std::max(static_cast<double>(p.line), 0.035 * H);
        const DV2 tlo(x - tw, base - 2.0), thi(x + tw, base + trunkH + 2.0);
        add({DV2(x - 0.5 * w, base - 2.0), DV2(x + 0.5 * w, base + 1.0 + H), [tiers, tlo, thi](DV2 q) {
                 double d = sdBox(q, tlo, thi);
                 for (const std::vector<DV2>& t : tiers) d = std::min(d, sdPolygon(q, t));
                 return d;
             }});
        ++trees;
    }
};

} // namespace

const char* frameName(Frame f) {
    switch (f) {
    case Frame::Round: return "Round";
    case Frame::Rect:  return "Rectangle";
    case Frame::Arch:  return "Arch";
    default:           return "?";
    }
}

Params sane(const Params& in) {
    Params p = in;
    p.frame  = static_cast<Frame>(std::clamp(static_cast<int>(in.frame), 0, static_cast<int>(Frame::Count) - 1));
    p.width  = std::clamp(in.width, 60.0f, 250.0f);
    // An arch needs its round on top of a rectangle at least a fifth as high.
    p.height = std::clamp(in.height, p.frame == Frame::Arch ? 0.7f * p.width : 50.0f, 250.0f);
    p.line   = std::clamp(in.line, 0.8f, 3.0f);
    p.border = std::clamp(in.border, std::max(2.0f, p.line), 12.0f);
    p.thickness = std::clamp(in.thickness, 0.6f, 5.0f);
    p.hills  = std::clamp(in.hills, 0.0f, 1.0f);
    p.trees  = std::clamp(in.trees, 0, 16);
    p.stars  = std::clamp(in.stars, 0, 16);
    p.flakes = std::clamp(in.flakes, 0, 12);
    return p;
}

Model build(const Params& in) {
    const Params p = sane(in);
    Model out;
    Scene sc(p);
    const bool round = p.frame == Frame::Round;
    sc.a = 0.5 * p.width - p.border;
    sc.b = round ? sc.a : 0.5 * p.height - p.border;
    sc.S = std::min(sc.a, sc.b);
    sc.archY = sc.b - sc.a;
    const double a = sc.a, b = sc.b, S = sc.S;
    sc.sky.assign(static_cast<std::size_t>(std::ceil(2.0 * a)) + 1, -1e30);
    for (int i = 0; i < static_cast<int>(sc.sky.size()); ++i) sc.sky[static_cast<std::size_t>(i)] = sc.ground(i - a);
    Rng& rng = sc.rng;
    const double line = p.line, hl = 0.5 * line;

    // --- The cabin, a little off the middle; the snowman and the fence beside it.
    double hx = 0.25 * S * rng.pm();
    const double W = 0.42 * S, Hh = 0.55 * W, roofH = 0.5 * W, eave = 0.08 * W;
    int side = rng.next() < 0.5 ? -1 : 1;   // where the snowman stands
    struct Span { double x0, x1; };
    std::vector<Span> taken;
    std::vector<Shape> smoke;
    DV2 smokeLo(1e30), smokeHi(-1e30);
    if (p.house) {
        const double yb = sc.ground(hx), base = sc.groundMin(hx - 0.5 * W, hx + 0.5 * W) - 1.5;
        const double top = yb + Hh;
        sc.add(box(DV2(hx - 0.5 * W, base), DV2(hx + 0.5 * W, top)));
        sc.add(polygon({{hx + 0.5 * W + eave, top - 0.6 * eave}, {hx, top + roofH}, {hx - 0.5 * W - eave, top - 0.6 * eave}}));
        // The chimney on the side away from the snowman, the smoke drifting off that way.
        const double cs = -side;
        const double cx0 = hx + cs * 0.2 * W, cx1 = hx + cs * 0.32 * W;
        const double chimTop = top + roofH * 0.82;
        sc.add(box(DV2(std::min(cx0, cx1), top), DV2(std::max(cx0, cx1), chimTop)));
        // Lit window panes behind a cross of bars, a lit door, a round window in the gable.
        const double ww = 0.22 * W, wh = 0.3 * Hh, m = std::max(line, 1.0);
        for (int sx : {-1, 1}) {
            const DV2 c(hx + sx * 0.28 * W, yb + 0.56 * Hh);
            const double pw = 0.5 * (ww - m), ph = 0.5 * (wh - m);
            for (int ix : {-1, 1})
                for (int iy : {-1, 1}) {
                    const DV2 pc = c + DV2(ix * 0.5 * (pw + m), iy * 0.5 * (ph + m));
                    sc.add(box(pc - 0.5 * DV2(pw, ph), pc + 0.5 * DV2(pw, ph), true));
                }
        }
        sc.add(box(DV2(hx - 0.09 * W, yb + 0.8), DV2(hx + 0.09 * W, yb + 0.6 * Hh), true));
        sc.add(circle(DV2(hx, top + 0.36 * roofH), 0.075 * W, true));
        if (p.smoke) {
            double r = std::max(1.6, 0.055 * W), ang = 75.0 * kPi / 180.0;
            DV2 c(0.5 * (cx0 + cx1), chimTop + 0.5 * r);
            for (int k = 0; k < 5; ++k) {
                smoke.push_back(circle(c, r));
                smokeLo = glm::min(smokeLo, c - DV2(r));
                smokeHi = glm::max(smokeHi, c + DV2(r));
                const double r2 = r * 1.22;
                ang -= (10.0 + 6.0 * rng.next()) * kPi / 180.0;
                c += 0.78 * (r + r2) * DV2(cs * std::cos(ang), std::sin(ang));
                r = r2;
                if (sc.inner(c) > -(r + 2.0)) break;   // no further than the frame
            }
        }
        taken.push_back({hx - 0.5 * W - eave, hx + 0.5 * W + eave});
    } else {
        hx = 0.0;
    }
    const double r1 = 0.085 * S;
    double snowX = hx + side * ((p.house ? 0.5 * W + eave : 0.0) + r1 + 0.1 * S);
    if (std::abs(snowX) + r1 > 0.8 * a) {
        side = -side;
        snowX = hx + side * ((p.house ? 0.5 * W + eave : 0.0) + r1 + 0.1 * S);
    }
    double fenceX0 = 0.0, fenceX1 = 0.0;
    if (p.fence) {
        const double from = hx - side * ((p.house ? 0.5 * W + eave : 0.0) + 3.0);
        const double to = from - side * 0.36 * S;
        fenceX0 = std::max(-0.88 * a, std::min(from, to));
        fenceX1 = std::min(0.88 * a, std::max(from, to));
    }

    // --- Firs, wherever there is room on the snow: none in front of the cabin or
    // the snowman, none in the smoke, none taller than the frame lets them.
    {
        std::vector<double> heights;
        for (int i = 0; i < p.trees; ++i) heights.push_back(S * rng.in(0.5, 0.95));
        std::sort(heights.rbegin(), heights.rend());
        std::vector<std::pair<double, double>> placed;   // x, half width
        for (double H : heights) {
            for (int tries = 0; tries < 300; ++tries) {
                const double h = H * (1.0 - 0.5 * tries / 300.0), hw = 0.28 * h;
                const double x = 0.95 * a * rng.pm();
                bool ok = true;
                for (const Span& s : taken) ok &= x + 0.6 * hw < s.x0 || x - 0.6 * hw > s.x1;
                if (p.snowman) ok &= std::abs(x - snowX) > r1 + 0.7 * hw;
                for (const auto& [px, phw] : placed) ok &= std::abs(x - px) > 0.38 * (hw + phw);
                const double top = sc.ground(x) + h;
                ok &= sc.inner(DV2(x, top)) < -4.0;
                if (!smoke.empty())
                    ok &= x + hw < smokeLo.x - 3.0 || x - hw > smokeHi.x + 3.0 || top < smokeLo.y - 3.0;
                if (!ok) continue;
                placed.push_back({x, hw});
                break;
            }
        }
        // Drawn back to front: the smaller ones last, as if nearer.
        for (const auto& [x, hw] : placed) sc.fir(x, hw / 0.28);
    }
    for (const Shape& s : smoke) sc.add(s);

    // --- The snowman: three balls, a hat, twig arms; eyes and buttons cut out,
    // a carrot sticking out of the face.
    if (p.snowman) {
        const double r2 = 0.72 * r1, r3 = 0.52 * r1, x = snowX;
        const double c1 = sc.ground(x) + 0.68 * r1, c2 = c1 + 0.8 * (r1 + r2), c3 = c2 + 0.8 * (r2 + r3);
        sc.add(circle(DV2(x, c1), r1));
        sc.add(circle(DV2(x, c2), r2));
        sc.add(circle(DV2(x, c3), r3));
        const double brim = std::max(1.2, 0.18 * r3);
        sc.add(box(DV2(x - 0.78 * r3, c3 + 0.72 * r3), DV2(x + 0.78 * r3, c3 + 0.72 * r3 + brim)));
        sc.add(box(DV2(x - 0.5 * r3, c3 + 0.72 * r3), DV2(x + 0.5 * r3, c3 + 0.72 * r3 + brim + 0.95 * r3)));
        for (int sx : {-1, 1}) {
            const DV2 from(x + sx * 0.7 * r2, c2 + 0.1 * r2), to(x + sx * (r2 + 0.85 * r1), c2 + 0.75 * r1);
            sc.add(segment(from, to, hl));
            const DV2 dir = glm::normalize(to - from);
            for (double turn : {0.6, -0.5}) {
                const double c = std::cos(turn), s = std::sin(turn);
                sc.add(segment(to, to + 0.3 * r1 * DV2(c * dir.x - s * dir.y, s * dir.x + c * dir.y), hl));
            }
        }
        const double eye = std::max(0.7, 0.12 * r3);
        sc.add(circle(DV2(x - 0.33 * r3, c3 + 0.2 * r3), eye, true));
        sc.add(circle(DV2(x + 0.33 * r3, c3 + 0.2 * r3), eye, true));
        const double button = std::max(0.7, 0.1 * r2);
        for (double y : {c2 + 0.35 * r2, c2 - 0.1 * r2, c1 + 0.25 * r1}) sc.add(circle(DV2(x, y), button, true));
        const double nose = side < 0 ? 1.0 : -1.0;   // towards the cabin
        sc.add(polygon({{x + nose * 0.2 * r3, c3 + 0.08 * r3}, {x + nose * 1.45 * r3, c3 - 0.12 * r3},
                        {x + nose * 0.2 * r3, c3 - 0.3 * r3}}));
    }

    // --- The fence: pointed pickets, two rails following the snow.
    if (p.fence && fenceX1 - fenceX0 > 10.0) {
        const double ph = 0.16 * S, pw = std::max(1.6, line + 0.4);
        const int n = std::max(2, static_cast<int>(std::floor((fenceX1 - fenceX0) / (0.07 * S + pw))) + 1);
        std::vector<DV2> feet;
        for (int k = 0; k < n; ++k) {
            const double x = fenceX0 + (fenceX1 - fenceX0) * k / (n - 1), g = sc.ground(x);
            sc.add(polygon({{x + 0.5 * pw, g - 1.5}, {x + 0.5 * pw, g + ph}, {x, g + ph + 0.6 * pw},
                            {x - 0.5 * pw, g + ph}, {x - 0.5 * pw, g - 1.5}}));
            feet.push_back({x, g});
        }
        for (std::size_t k = 0; k + 1 < feet.size(); ++k)
            for (double f : {0.38, 0.75})
                sc.add(segment(feet[k] + DV2(0.0, f * ph), feet[k + 1] + DV2(0.0, f * ph), hl));
    }

    // --- The snow: hills along the bottom, down into the frame.
    sc.add({DV2(-a - 2.0, -b - p.border - 2.0), DV2(a + 2.0, sc.groundMin(-a, a) + 0.24 * b + 2.0), [&sc](DV2 q) {
                const double g = sc.ground(q.x), d = (sc.ground(q.x + 0.05) - sc.ground(q.x - 0.05)) / 0.1;
                return (q.y - g) / std::sqrt(1.0 + d * d);
            }});

    // --- The sky: the moon, stars and snowflakes, each on a thread from the
    // frame's top, none above another (their threads would cross it).
    struct Hung {
        DV2 c;
        double r;
    };
    std::vector<Hung> hung;
    auto spot = [&](double r, DV2& at) {
        for (int tries = 0; tries < 400; ++tries) {
            const DV2 q(0.9 * a * rng.pm(), rng.in(-0.2 * b, b));
            if (sc.inner(q) > -(r + 3.0)) continue;
            if (q.y - r < sc.skyOver(q.x - r - 2.0, q.x + r + 2.0) + 4.0) continue;
            // Apart, and the lower one's thread past the upper one.
            bool clear = true;
            for (const Hung& o : hung) {
                clear &= glm::length(q - o.c) > r + o.r + 4.0;
                const double above = q.y > o.c.y ? r : o.r;
                clear &= std::abs(q.x - o.c.x) > above + p.line + 2.0;
            }
            if (!clear) continue;
            at = q;
            hung.push_back({q, r});
            return true;
        }
        return false;
    };
    if (p.moon) {
        const double R = 0.13 * S;
        DV2 c;
        if (spot(R, c)) {
            const DV2 off(0.38 * R, 0.24 * R);
            sc.shapes.push_back({c - DV2(R), c + DV2(R), [c, R, off](DV2 q) {
                                     return std::max(sdCircle(q, c, R), -sdCircle(q, c + off, 0.86 * R));
                                 }});
            sc.hang(c + R * DV2(std::cos(2.1), std::sin(2.1)) - DV2(0.0, 0.5));
        }
    }
    for (int i = 0; i < p.flakes; ++i) {
        const double r = S * rng.in(0.055, 0.075);
        DV2 c;
        if (!spot(r, c)) continue;
        for (int k = 0; k < 6; ++k) {
            const double ang = 0.5 * kPi + k * kPi / 3.0;
            const DV2 d(std::cos(ang), std::sin(ang)), tip = c + r * d;
            sc.shapes.push_back(segment(c, tip, hl));
            for (double side2 : {-1.0, 1.0}) {
                const double ba = ang + side2 * kPi / 3.0;
                const DV2 at = c + 0.55 * r * d;
                sc.shapes.push_back(segment(at, at + 0.3 * r * DV2(std::cos(ba), std::sin(ba)), hl));
            }
        }
        sc.shapes.push_back(circle(c, std::max(line, 0.14 * r)));
        sc.hang(c + DV2(0.0, r - 0.3));
        ++sc.flakes;
    }

    for (int i = 0; i < p.stars; ++i) {
        const double r = S * rng.in(0.045, 0.07);
        DV2 c;
        if (!spot(r, c)) continue;
        std::vector<DV2> star;
        const double turn = 0.15 * rng.pm();
        for (int k = 0; k < 10; ++k) {
            const double ang = 0.5 * kPi + turn + k * kPi / 5.0, rr = k % 2 ? 0.42 * r : r;
            star.push_back(c + rr * DV2(std::cos(ang), std::sin(ang)));
        }
        sc.shapes.push_back(polygon(star));
        sc.hang(star[0] - DV2(0.0, 0.5));
        ++sc.stars;
    }
    // --- Drawn, held inside the frame, the frame and the loop round it.
    const double topY = round ? a + p.border : (p.frame == Frame::Arch ? sc.archY + a + p.border : b + p.border);
    const double loopR = std::max(3.5, 2.0 * line + 2.0), loopW = std::max(2.0, 0.8 * p.border);
    const DV2 loopC(0.0, topY + 0.55 * loopR);
    const double h = std::clamp(p.width / 1500.0, 0.06, 0.15);
    Canvas cv(DV2(-a - p.border - 2.0, -b - p.border - 2.0),
              DV2(a + p.border + 2.0, (p.eyelet ? loopC.y + loopR + 0.5 * loopW : topY) + 2.0), h);
    for (const Shape& s : sc.shapes) cv.draw(s);
    const double bw = p.border;
    for (int j = 0; j < cv.ny; ++j)
        for (int i = 0; i < cv.nx; ++i) {
            const DV2 q = cv.at(i, j);
            const double fi = sc.inner(q);
            float& v = cv.f[static_cast<std::size_t>(j) * static_cast<std::size_t>(cv.nx) + static_cast<std::size_t>(i)];
            v = std::max(v, static_cast<float>(fi - 0.5 * bw));   // into the band, so the two overlap
            v = std::min(v, static_cast<float>(std::abs(fi - 0.5 * bw) - 0.5 * bw));
            if (p.eyelet) v = std::min(v, static_cast<float>(std::abs(glm::length(q - loopC) - loopR) - 0.5 * loopW));
        }
    {
        DV2 speck(0.0);
        out.dropped = tidy(cv, 0.45, 1.0, speck);
        out.droppedAt = glm::vec2(speck);
    }
    out.trees = sc.trees;
    out.stars = sc.stars;
    out.flakes = sc.flakes;

    // --- The outlines, and the plate made of them.
    std::vector<std::vector<DV2>> loops;
    for (std::vector<DV2>& l : trace(cv)) loops.push_back(simplify(l, 0.02, 30.0));
    DV2 lo(1e30), hi(-1e30);
    for (const auto& l : loops)
        for (const DV2& q : l) {
            lo = glm::min(lo, q);
            hi = glm::max(hi, q);
        }
    const DV2 mid = 0.5 * (lo + hi);
    for (auto& l : loops)
        for (DV2& q : l) q -= mid;
    lo -= mid;
    hi -= mid;
    out.smallestHole = 1e30;
    for (const std::vector<DV2>& l : loops) {
        if (area(l) > 0.0) {
            ++out.pieces;
            continue;
        }
        ++out.holes;
        DV2 where;
        const double w = holeWidth(l, where);
        if (w < out.smallestHole) {
            out.smallestHole = w;
            out.smallestHoleAt = glm::vec2(where);
        }
    }
    if (out.holes == 0) out.smallestHole = 0.0;

    // The top and the bottom are filled band by band between level cuts: one
    // every 5 mm and one through every opening no other cut crosses, so no band
    // holds a whole opening and each piece of a band is an outline without
    // holes. Every cut is kept off the outlines' corners.
    std::vector<double> cornerYs;
    for (const auto& l : loops)
        for (const DV2& q : l) cornerYs.push_back(q.y);
    std::sort(cornerYs.begin(), cornerYs.end());
    auto offCorners = [&](double c) {
        for (int k = 0; k < 400; ++k) {
            const auto it = std::lower_bound(cornerYs.begin(), cornerYs.end(), c - 0.004);
            if (it == cornerYs.end() || *it > c + 0.004) break;
            c += 0.0091;
        }
        return c;
    };
    std::vector<double> cuts;
    for (double c = std::floor(lo.y / 5.0) * 5.0 + 5.0; c < hi.y; c += 5.0) cuts.push_back(c);
    for (const std::vector<DV2>& l : loops) {
        if (area(l) > 0.0) continue;
        double y0 = 1e30, y1 = -1e30;
        for (const DV2& q : l) {
            y0 = std::min(y0, q.y);
            y1 = std::max(y1, q.y);
        }
        if (std::none_of(cuts.begin(), cuts.end(), [&](double c) { return c > y0 + 0.01 && c < y1 - 0.01; }))
            cuts.push_back(0.5 * (y0 + y1));
    }
    for (double& c : cuts) c = offCorners(c);
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end(), [](double x, double y) { return y - x < 0.02; }), cuts.end());

    // The outlines with a corner wherever they cross a cut.
    std::vector<DV2> pts;
    std::vector<std::vector<int>> rings;
    std::vector<std::vector<std::pair<double, int>>> onCut(cuts.size());
    for (const std::vector<DV2>& l : loops) {
        std::vector<int> ring;
        const std::size_t n = l.size();
        for (std::size_t i = 0; i < n; ++i) {
            const DV2 A = l[i], B = l[(i + 1) % n];
            ring.push_back(static_cast<int>(pts.size()));
            pts.push_back(A);
            const double ylo = std::min(A.y, B.y), yhi = std::max(A.y, B.y);
            auto k0 = std::upper_bound(cuts.begin(), cuts.end(), ylo) - cuts.begin();
            auto k1 = std::lower_bound(cuts.begin(), cuts.end(), yhi) - cuts.begin();
            std::vector<std::size_t> ks;
            for (auto k = k0; k < k1; ++k) ks.push_back(static_cast<std::size_t>(k));
            if (B.y < A.y) std::reverse(ks.begin(), ks.end());
            for (std::size_t k : ks) {
                const double t = (cuts[k] - A.y) / (B.y - A.y);
                ring.push_back(static_cast<int>(pts.size()));
                onCut[k].push_back({A.x + t * (B.x - A.x), static_cast<int>(pts.size())});
                pts.push_back({A.x + t * (B.x - A.x), cuts[k]});
            }
        }
        rings.push_back(std::move(ring));
    }
    const double T = p.thickness;
    Builder bld;
    std::vector<int> top(pts.size()), bot(pts.size());
    for (std::size_t i = 0; i < pts.size(); ++i) {
        top[i] = bld.vid({pts[i].x, pts[i].y, T});
        bot[i] = bld.vid({pts[i].x, pts[i].y, 0.0});
    }
    // Walls along every outline.
    for (const std::vector<int>& r : rings)
        for (std::size_t i = 0; i < r.size(); ++i) {
            const auto u = static_cast<std::size_t>(r[i]), v = static_cast<std::size_t>(r[(i + 1) % r.size()]);
            bld.face({bot[u], bot[v], top[v], top[u]});
        }
    // Each band's boundary: the outlines' pieces in it, and along its cuts the
    // stretches inside the plate (between a crossing in and the next out).
    std::vector<std::vector<std::pair<int, int>>> band(cuts.size() + 1);
    for (const std::vector<int>& r : rings)
        for (std::size_t i = 0; i < r.size(); ++i) {
            const int u = r[i], v = r[(i + 1) % r.size()];
            const double y = 0.5 * (pts[static_cast<std::size_t>(u)].y + pts[static_cast<std::size_t>(v)].y);
            band[static_cast<std::size_t>(std::upper_bound(cuts.begin(), cuts.end(), y) - cuts.begin())].push_back({u, v});
        }
    for (std::size_t k = 0; k < cuts.size(); ++k) {
        std::vector<std::pair<double, int>>& c = onCut[k];
        std::sort(c.begin(), c.end());
        if (c.size() % 2 && out.ok) {
            out.ok = false;
            out.error = "an outline crossing a cut an odd number of times";
        }
        for (std::size_t i = 0; i + 1 < c.size(); i += 2) {
            band[k + 1].push_back({c[i].second, c[i + 1].second});   // the band above: plastic on the left going +x
            band[k].push_back({c[i + 1].second, c[i].second});
        }
    }
    for (const auto& edges : band) {
        if (edges.empty()) continue;
        std::vector<Ring> tops, bots;
        std::unordered_map<int, std::vector<int>> next;
        for (const auto& [u, v] : edges) next[u].push_back(v);
        for (const auto& [u0, v0] : edges) {
            if (next[u0].empty()) continue;
            Ring rt, rb;
            for (int u = u0; !next[u].empty();) {
                const int v = next[u].back();
                next[u].pop_back();
                rt.push_back(top[static_cast<std::size_t>(u)]);
                rb.push_back(bot[static_cast<std::size_t>(u)]);
                u = v;
                if (u == u0) break;
            }
            if (rt.size() >= 3) {
                tops.push_back(std::move(rt));
                bots.push_back(std::move(rb));
            }
        }
        bld.region(tops, {}, true);
        bld.region(bots, {}, false);
    }
    unsliver(bld);
    if (!bld.error.empty()) {
        out.ok = false;
        out.error = bld.error;
    }
    for (const std::vector<int>& f : bld.faces) {
        if (f.size() != 3) continue;
        bool up = true;
        for (int v : f) up &= bld.verts[static_cast<std::size_t>(v)].z > T - 1e-9;
        if (up)
            for (int v : f) out.tris.emplace_back(DV2(bld.verts[static_cast<std::size_t>(v)]));
    }
    out.size   = glm::vec3(static_cast<float>(hi.x - lo.x), static_cast<float>(hi.y - lo.y), static_cast<float>(T));
    out.volume = solidbuild::volume(bld);
    out.grams  = out.volume / 1000.0 * kPla;
    out.metres = out.volume / (kPi * kFilR * kFilR) / 1000.0;
    out.body   = solidbuild::toEditMesh(bld);

    char hint[320];
    std::snprintf(hint, sizeof hint,
                  "Prints flat as it lies, without support: one plate %.2g mm thick (%d layers of 0.2 mm). The "
                  "thinnest lines are %.2g mm, %d lines of a 0.4 mm nozzle. Light from behind draws the picture.",
                  p.thickness, std::max(1, static_cast<int>(std::lround(p.thickness / 0.2f))), p.line,
                  std::max(1, static_cast<int>(std::floor(p.line / 0.42f))));
    out.printHint = hint;
    return out;
}

AssetId ensureMaterial(std::vector<MaterialDef>& mats, const Params& p) {
    char name[64];
    std::snprintf(name, sizeof name, "Stencil#%02X%02X%02X", toByte(p.color.r), toByte(p.color.g), toByte(p.color.b));
    for (MaterialDef& m : mats) {
        if (m.name != name) continue;
        m.albedo = p.color;
        if (!m.assetId.valid()) m.assetId = AssetId::generate();
        return m.assetId;
    }
    MaterialDef md;
    md.assetId      = AssetId::generate();
    md.name         = name;
    md.albedo       = p.color;
    md.reflectivity = 0.0f;
    md.roughness    = 0.55f;   // printed plastic: a dull sheen
    mats.push_back(md);
    return md.assetId;
}

std::string autoName(const Params& in) {
    const Params p = sane(in);
    char b[96];
    if (p.frame == Frame::Round) std::snprintf(b, sizeof b, "Winter stencil round %.4g mm", p.width);
    else std::snprintf(b, sizeof b, "Winter stencil %s %.4g x %.4g mm", p.frame == Frame::Rect ? "rectangle" : "arch",
                       p.width, p.height);
    return b;
}

std::vector<Entity> entities(const Params& in, const AssetId& material, int& counter, const glm::vec3& at) {
    const Params p = sane(in);
    Model mdl = build(p);
    if (mdl.body.faces.empty()) return {};
    std::vector<Entity> out;
    Entity root;
    root.type = EntityType::Empty;
    root.name = autoName(p);
    root.id = counter++;
    root.parent = -1;
    // The root in the middle of the plate, so its pick box is the plate's: `at`
    // is where its bottom lies.
    glm::vec3 lo(1e9f), hi(-1e9f);
    for (const glm::vec3& v : mdl.body.verts) {
        lo = glm::min(lo, v);
        hi = glm::max(hi, v);
    }
    const glm::vec3 mid = 0.5f * (lo + hi);
    root.half = glm::max(0.5f * (hi - lo), glm::vec3(1e-4f));
    root.localCenter = root.center = at + glm::vec3(0.0f, root.half.y, 0.0f);
    {
        auto wc = std::make_unique<WinterStencilComponent>();
        wc->params = p;
        root.components.items.push_back(std::move(wc));
    }
    out.push_back(std::move(root));
    const int rootId = out.front().id;
    const glm::vec3 rootC = out.front().center;

    EditMesh m = std::move(mdl.body);
    for (glm::vec3& v : m.verts) v -= mid;
    const glm::vec3 shift = editmesh::recenter(m);
    glm::vec3 mn, mx;
    m.bounds(mn, mx);
    Entity e;
    e.type = EntityType::Box;
    e.name = "Stencil";
    e.id = counter++;
    e.parent = rootId;
    e.half = glm::max(0.5f * (mx - mn), glm::vec3(1e-4f));
    e.localCenter = shift;
    e.center = rootC + shift;
    auto mc = std::make_unique<MeshComponent>();
    mc->mesh = std::move(m);
    mc->touch();
    e.components.items.push_back(std::move(mc));
    auto mt = std::make_unique<MaterialComponent>();
    mt->material = material;
    e.components.items.push_back(std::move(mt));
    auto ph = std::make_unique<PhysicsComponent>();
    ph->dynamic = false;
    e.components.items.push_back(std::move(ph));
    out.push_back(std::move(e));
    return out;
}

// --- Persistence -------------------------------------------------------------------

void saveParams(nlohmann::json& j, const Params& p) {
    j["frame"]     = static_cast<int>(p.frame);
    j["width"]     = p.width;
    j["height"]    = p.height;
    j["border"]    = p.border;
    j["thickness"] = p.thickness;
    j["eyelet"]    = p.eyelet;
    j["seed"]      = p.seed;
    j["hills"]     = p.hills;
    j["trees"]     = p.trees;
    j["house"]     = p.house;
    j["smoke"]     = p.smoke;
    j["snowman"]   = p.snowman;
    j["fence"]     = p.fence;
    j["moon"]      = p.moon;
    j["stars"]     = p.stars;
    j["flakes"]    = p.flakes;
    j["line"]      = p.line;
    j["color"]     = nlohmann::json::array({p.color.r, p.color.g, p.color.b});
}

void loadParams(const nlohmann::json& j, Params& p) {
    const Params d;
    p.frame     = static_cast<Frame>(j.value("frame", static_cast<int>(d.frame)));
    p.width     = j.value("width", d.width);
    p.height    = j.value("height", d.height);
    p.border    = j.value("border", d.border);
    p.thickness = j.value("thickness", d.thickness);
    p.eyelet    = j.value("eyelet", d.eyelet);
    p.seed      = j.value("seed", d.seed);
    p.hills     = j.value("hills", d.hills);
    p.trees     = j.value("trees", d.trees);
    p.house     = j.value("house", d.house);
    p.smoke     = j.value("smoke", d.smoke);
    p.snowman   = j.value("snowman", d.snowman);
    p.fence     = j.value("fence", d.fence);
    p.moon      = j.value("moon", d.moon);
    p.stars     = j.value("stars", d.stars);
    p.flakes    = j.value("flakes", d.flakes);
    p.line      = j.value("line", d.line);
    p.color     = d.color;
    if (const auto c = j.find("color"); c != j.end() && c->is_array() && c->size() == 3)
        p.color = {(*c)[0].get<float>(), (*c)[1].get<float>(), (*c)[2].get<float>()};
    p = sane(p);
}

namespace {
// Scene files name components by type id; this lets a saved stencil come back
// as one, parameters and all.
struct RegisterWinterStencil {
    RegisterWinterStencil() {
        components::registerType({"winterStencil", "Winter stencil",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<WinterStencilComponent>()); },
            /*addable=*/false});
    }
} g_registerWinterStencil;
} // namespace

} // namespace winterstencil
