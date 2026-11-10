#include "WorldGen.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <future>
#include <numeric>
#include <queue>
#include <random>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#include "SandboxMath.hpp"   // sampleSpline -- the curve RiverSystem draws through the points

namespace worldgen {
namespace {

constexpr float kPi = 3.14159265358979f;

// One seeded stream per stage, so a change to a later stage never reshuffles an
// earlier one: moving the farm slider must not move the river. Same discipline
// as LevelGen's phases.
constexpr unsigned kPhaseLand  = 0x9e3779b9u;
constexpr unsigned kPhaseTowns = 0x85ebca6bu;
constexpr unsigned kPhaseRoads = 0x27d4eb2fu;
constexpr unsigned kPhaseFarms = 0x165667b1u;
constexpr unsigned kPhaseRocks = 0xd3a2646cu;

// mt19937 is specified to the bit; the mapping to floats is done by hand so a
// plan is the same plan on every standard library (see LevelGen's Rng).
struct Rng {
    std::mt19937 g;
    explicit Rng(unsigned s) : g(s) {}
    float uni(float a, float b) {
        return a + (b - a) * static_cast<float>(g() >> 8) /
                             static_cast<float>((1u << 24) - 1u);
    }
    int below(int n) { return n > 0 ? static_cast<int>(g() % static_cast<unsigned>(n)) : 0; }
};

std::uint32_t mix(std::uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
float hash01(std::uint32_t a, std::uint32_t b) {
    return static_cast<float>(mix(a * 0x9E3779B1u ^ mix(b + 0x7F4A7C15u)) >> 8) /
           static_cast<float>(1u << 24);
}

float smooth01(float e0, float e1, float x) {
    const float t = glm::clamp((x - e0) / std::max(e1 - e0, 1e-6f), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// A bump that is 1 between `a` and `b` and falls to 0 over a quarter of that
// span on either side -- "near the river, but not in it".
float band(float x, float a, float b) {
    const float w = std::max(0.25f * (b - a), 1.0f);
    return smooth01(a - w, a, x) * (1.0f - smooth01(b, b + w, x));
}

float polyLength(const std::vector<glm::vec2>& l) {
    float s = 0.0f;
    for (std::size_t i = 1; i < l.size(); ++i) s += glm::length(l[i] - l[i - 1]);
    return s;
}

float segDist(glm::vec2 p, glm::vec2 a, glm::vec2 b) {
    const glm::vec2 ab = b - a;
    const float t = glm::clamp(glm::dot(p - a, ab) / std::max(glm::dot(ab, ab), 1e-9f),
                               0.0f, 1.0f);
    return glm::length(p - (a + ab * t));
}

float lineDist(glm::vec2 p, const std::vector<glm::vec2>& l) {
    float d = 1e30f;
    for (std::size_t i = 1; i < l.size(); ++i) d = std::min(d, segDist(p, l[i - 1], l[i]));
    if (l.size() == 1) d = glm::length(p - l[0]);
    return d;
}

// Points every `step` metres along a polyline, both ends kept.
std::vector<glm::vec2> resample(const std::vector<glm::vec2>& l, float step) {
    std::vector<glm::vec2> out;
    if (l.size() < 2) return l;
    const float total = polyLength(l);
    const int   n     = std::max(1, static_cast<int>(std::lround(total / step)));
    const float d     = total / static_cast<float>(n);
    out.push_back(l.front());
    float want = d, run = 0.0f;
    for (std::size_t i = 1; i < l.size() && static_cast<int>(out.size()) < n; ++i) {
        const float seg = glm::length(l[i] - l[i - 1]);
        while (run + seg >= want && static_cast<int>(out.size()) < n) {
            const float t = (want - run) / std::max(seg, 1e-6f);
            out.push_back(glm::mix(l[i - 1], l[i], t));
            want += d;
        }
        run += seg;
    }
    out.push_back(l.back());
    return out;
}

// Three-tap smoothing of the interior, ends pinned.
void relax(std::vector<glm::vec2>& l, int passes) {
    for (int p = 0; p < passes; ++p) {
        std::vector<glm::vec2> o = l;
        for (std::size_t i = 1; i + 1 < l.size(); ++i)
            o[i] = 0.25f * l[i - 1] + 0.5f * l[i] + 0.25f * l[i + 1];
        l.swap(o);
    }
}

// Douglas-Peucker: the corners a grid path actually turns at, without the
// staircase between them.
void simplifyInto(const std::vector<glm::vec2>& l, std::size_t a, std::size_t b, float tol,
                  std::vector<char>& keep) {
    if (b <= a + 1) return;
    float worst = -1.0f;
    std::size_t at = a;
    for (std::size_t i = a + 1; i < b; ++i) {
        const float d = segDist(l[i], l[a], l[b]);
        if (d > worst) { worst = d; at = i; }
    }
    if (worst > tol) {
        keep[at] = 1;
        simplifyInto(l, a, at, tol, keep);
        simplifyInto(l, at, b, tol, keep);
    }
}
// ...and the same in three dimensions for a road: a point stays if dropping it
// would move the line sideways OR change the ground it runs over. On a hillside
// the two are the same thing seen twice -- a chord cut four metres across a 40%
// slope runs a metre and a half higher or lower than the route that was found,
// which over thirty metres is another five percent of gradient.
void simplify3Into(const std::vector<glm::vec2>& l, const std::vector<float>& h, std::size_t a,
                   std::size_t b, float tol, std::vector<char>& keep) {
    if (b <= a + 1) return;
    const float len = glm::length(l[b] - l[a]);
    float worst = -1.0f;
    std::size_t at = a;
    for (std::size_t i = a + 1; i < b; ++i) {
        const glm::vec2 ab = l[b] - l[a];
        const float t = glm::clamp(glm::dot(l[i] - l[a], ab) / std::max(glm::dot(ab, ab), 1e-9f), 0.0f, 1.0f);
        const float side = glm::length(l[i] - (l[a] + ab * t));
        const float up   = std::fabs(h[i] - glm::mix(h[a], h[b], t)) * 4.0f;
        const float d    = std::max(side, up);
        if (d > worst) { worst = d; at = i; }
    }
    (void)len;
    if (worst > tol) {
        keep[at] = 1;
        simplify3Into(l, h, a, at, tol, keep);
        simplify3Into(l, h, at, b, tol, keep);
    }
}

std::vector<glm::vec2> simplify(const std::vector<glm::vec2>& l, float tol) {
    if (l.size() < 3) return l;
    std::vector<char> keep(l.size(), 0);
    keep.front() = keep.back() = 1;
    simplifyInto(l, 0, l.size() - 1, tol, keep);
    std::vector<glm::vec2> out;
    for (std::size_t i = 0; i < l.size(); ++i)
        if (keep[i]) out.push_back(l[i]);
    return out;
}

// Summed-area table over the grid, for "the mean of X over a square" in O(1).
struct Sat {
    int n = 0;
    std::vector<double> s;
    void build(const std::vector<float>& v, int n_) {
        n = n_;
        s.assign(static_cast<std::size_t>((n + 1) * (n + 1)), 0.0);
        for (int j = 0; j < n; ++j) {
            double row = 0.0;
            for (int i = 0; i < n; ++i) {
                row += v[static_cast<std::size_t>(j * n + i)];
                s[static_cast<std::size_t>((j + 1) * (n + 1) + i + 1)] =
                    s[static_cast<std::size_t>(j * (n + 1) + i + 1)] + row;
            }
        }
    }
    // Sum over cells [i0, i1] x [j0, j1], clamped to the grid.
    double sum(int i0, int j0, int i1, int j1) const {
        i0 = std::max(i0, 0); j0 = std::max(j0, 0);
        i1 = std::min(i1, n - 1); j1 = std::min(j1, n - 1);
        if (i1 < i0 || j1 < j0) return 0.0;
        auto at = [&](int i, int j) { return s[static_cast<std::size_t>(j * (n + 1) + i)]; };
        return at(i1 + 1, j1 + 1) - at(i0, j1 + 1) - at(i1 + 1, j0) + at(i0, j0);
    }
    double mean(int ci, int cj, int r) const {
        const int i0 = std::max(ci - r, 0), j0 = std::max(cj - r, 0);
        const int i1 = std::min(ci + r, n - 1), j1 = std::min(cj + r, n - 1);
        const double cnt = static_cast<double>((i1 - i0 + 1) * (j1 - j0 + 1));
        return cnt > 0.0 ? sum(i0, j0, i1, j1) / cnt : 0.0;
    }
};

// Distance in metres from every cell to the nearest cell where `src` is set,
// by the two-pass 3-4 chamfer -- within a few percent of Euclidean, which is all
// "how far is the river" needs.
std::vector<float> distanceTo(const Grid& g, const std::function<bool(int)>& src) {
    const int n = g.n;
    const float big = 1e9f;
    std::vector<float> d(static_cast<std::size_t>(n * n), big);
    for (int k = 0; k < n * n; ++k)
        if (src(k)) d[static_cast<std::size_t>(k)] = 0.0f;
    const float a = g.cell, b = g.cell * 1.41421356f;
    auto at = [&](int i, int j) -> float& { return d[static_cast<std::size_t>(j * n + i)]; };
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            float& v = at(i, j);
            if (i > 0)              v = std::min(v, at(i - 1, j) + a);
            if (j > 0)              v = std::min(v, at(i, j - 1) + a);
            if (i > 0 && j > 0)     v = std::min(v, at(i - 1, j - 1) + b);
            if (i < n - 1 && j > 0) v = std::min(v, at(i + 1, j - 1) + b);
        }
    for (int j = n - 1; j >= 0; --j)
        for (int i = n - 1; i >= 0; --i) {
            float& v = at(i, j);
            if (i < n - 1)              v = std::min(v, at(i + 1, j) + a);
            if (j < n - 1)              v = std::min(v, at(i, j + 1) + a);
            if (i < n - 1 && j < n - 1) v = std::min(v, at(i + 1, j + 1) + b);
            if (i > 0 && j < n - 1)     v = std::min(v, at(i - 1, j + 1) + b);
        }
    return d;
}

// Claim every cell within `r` of the polyline for `use`, unless it is already
// claimed by something that outranks it (a lower use::Kind, Free aside).
void claimAlong(Grid& g, const std::vector<glm::vec2>& l, float r, use::Kind kind) {
    if (l.empty()) return;
    for (std::size_t s = 0; s < l.size(); ++s) {
        const glm::vec2 a = l[s], b = l[s + 1 < l.size() ? s + 1 : s];
        const glm::vec2 lo = glm::min(a, b) - glm::vec2(r), hi = glm::max(a, b) + glm::vec2(r);
        const int i0 = std::max(0, static_cast<int>(std::floor((lo.x - g.origin.x) / g.cell)));
        const int j0 = std::max(0, static_cast<int>(std::floor((lo.y - g.origin.y) / g.cell)));
        const int i1 = std::min(g.n - 1, static_cast<int>(std::ceil((hi.x - g.origin.x) / g.cell)));
        const int j1 = std::min(g.n - 1, static_cast<int>(std::ceil((hi.y - g.origin.y) / g.cell)));
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) {
                std::uint8_t& u = g.use[static_cast<std::size_t>(g.index(i, j))];
                if (u != use::Free && u <= kind) continue;
                if (segDist(g.at(i, j), a, b) <= r) u = kind;
            }
    }
}

int cellOf(const Grid& g, glm::vec2 p, int& i, int& j) {
    i = glm::clamp(static_cast<int>(std::lround((p.x - g.origin.x) / g.cell)), 0, g.n - 1);
    j = glm::clamp(static_cast<int>(std::lround((p.y - g.origin.y) / g.cell)), 0, g.n - 1);
    return g.index(i, j);
}

use::Kind useAt(const Grid& g, glm::vec2 p) {
    int i, j;
    return static_cast<use::Kind>(g.use[static_cast<std::size_t>(cellOf(g, p, i, j))]);
}

float slopeAt(const Grid& g, glm::vec2 p) {
    int i, j;
    return g.slope[static_cast<std::size_t>(cellOf(g, p, i, j))];
}

// --- Stage 1: the ground --------------------------------------------------------

void sampleGround(Grid& g, const fitzel::TerrainSettings& ts, const GroundFn& groundAt) {
    const int n = g.n;
    g.height.assign(static_cast<std::size_t>(n * n), 0.0f);
    // Rows in parallel: the base sampler is a pure function of its arguments
    // (levelgen leans on the same contract), and a 4 km plan is sixty thousand
    // of them -- the difference between a slider that answers and one that does
    // not.
    const int workers = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 16);
    std::vector<std::future<void>> jobs;
    for (int w = 0; w < workers; ++w)
        jobs.push_back(std::async(std::launch::async, [&, w] {
            for (int j = w; j < n; j += workers)
                for (int i = 0; i < n; ++i) {
                    const glm::vec2 p = g.at(i, j);
                    g.height[static_cast<std::size_t>(j * n + i)] = groundAt(ts, p.x, p.y);
                }
        }));
    for (auto& f : jobs) f.get();
    g.slope.assign(g.height.size(), 0.0f);
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const int il = std::max(i - 1, 0), ir = std::min(i + 1, n - 1);
            const int jd = std::max(j - 1, 0), ju = std::min(j + 1, n - 1);
            const float dx = (g.height[static_cast<std::size_t>(j * n + ir)] -
                              g.height[static_cast<std::size_t>(j * n + il)]) /
                             (static_cast<float>(ir - il) * g.cell);
            const float dz = (g.height[static_cast<std::size_t>(ju * n + i)] -
                              g.height[static_cast<std::size_t>(jd * n + i)]) /
                             (static_cast<float>(ju - jd) * g.cell);
            g.slope[static_cast<std::size_t>(j * n + i)] = std::sqrt(dx * dx + dz * dz);
        }
    g.use.assign(g.height.size(), use::Free);
}

// The ground well past the plan's edge, coarsely: what throws its shadow in.
void sampleHorizon(Horizon& h, const Grid& g, float size, const fitzel::TerrainSettings& ts,
                   const GroundFn& groundAt) {
    const float margin = glm::clamp(0.8f * size, 1500.0f, 4000.0f);
    h.cell   = 50.0f;
    h.n      = static_cast<int>(std::ceil((size + 2.0f * margin) / h.cell)) + 1;
    h.origin = g.origin - glm::vec2(margin);
    h.height.assign(static_cast<std::size_t>(h.n * h.n), 0.0f);
    const int workers = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 16);
    std::vector<std::future<void>> jobs;
    for (int w = 0; w < workers; ++w)
        jobs.push_back(std::async(std::launch::async, [&, w] {
            for (int j = w; j < h.n; j += workers)
                for (int i = 0; i < h.n; ++i) {
                    const glm::vec2 p = h.origin + glm::vec2(static_cast<float>(i), static_cast<float>(j)) * h.cell;
                    h.height[static_cast<std::size_t>(j * h.n + i)] = groundAt(ts, p.x, p.y);
                }
        }));
    for (auto& f : jobs) f.get();
    h.top = h.height.empty() ? 0.0f : *std::max_element(h.height.begin(), h.height.end());
}

// --- Stage 2: where the water goes ------------------------------------------------
//
// Priority-flood (Barnes, Lehman & Mulla 2014): flood the land inwards from its
// outlets -- the map's edge and the sea -- always from the lowest cell reached so
// far. Every cell is reached first from the neighbour it would drain into, so the
// flood IS the drainage tree, pits included: a hollow is filled to its spill
// point and drains across it rather than trapping the water. The rivers then
// follow the cells that drain the most land, which is where valleys are --
// found, not drawn.
struct Drainage {
    std::vector<int>   to;     // the cell this one drains into; -1 = an outlet
    std::vector<float> area;   // m^2 drained through this cell
    // How far the flood had to fill this cell to drain it: > 0 in a hollow. Water
    // there stands as a pond up to its spill point -- it does not flow across
    // the hollow floor and back UP to the rim, which is the only course the
    // drainage tree can describe for it.
    std::vector<float> pond;
};

Drainage drain(const Grid& g, bool sea, float seaLevel) {
    const int n = g.n, N = n * n;
    Drainage d;
    d.to.assign(static_cast<std::size_t>(N), -1);
    d.area.assign(static_cast<std::size_t>(N), g.cell * g.cell);
    std::vector<char>  seen(static_cast<std::size_t>(N), 0);
    std::vector<float> fill(g.height);
    std::vector<int>   order;
    order.reserve(static_cast<std::size_t>(N));
    using Item = std::pair<float, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const int k = j * n + i;
            const bool edge  = (i == 0 || j == 0 || i == n - 1 || j == n - 1);
            const bool water = sea && g.height[static_cast<std::size_t>(k)] < seaLevel;
            if (!edge && !water) continue;
            seen[static_cast<std::size_t>(k)] = 1;
            if (water) fill[static_cast<std::size_t>(k)] = seaLevel;
            pq.push({fill[static_cast<std::size_t>(k)], k});
        }
    static const int di[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    static const int dj[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    while (!pq.empty()) {
        const auto [f, k] = pq.top();
        pq.pop();
        order.push_back(k);
        const int i = k % n, j = k / n;
        for (int e = 0; e < 8; ++e) {
            const int ni = i + di[e], nj = j + dj[e];
            if (ni < 0 || nj < 0 || ni >= n || nj >= n) continue;
            const int nk = nj * n + ni;
            if (seen[static_cast<std::size_t>(nk)]) continue;
            seen[static_cast<std::size_t>(nk)] = 1;
            // The epsilon makes a filled hollow drain in a definite direction
            // instead of being a lake of equal cells nobody flows out of.
            fill[static_cast<std::size_t>(nk)] =
                std::max(g.height[static_cast<std::size_t>(nk)], f + 1e-3f);
            d.to[static_cast<std::size_t>(nk)] = k;
            pq.push({fill[static_cast<std::size_t>(nk)], nk});
        }
    }
    d.pond.resize(static_cast<std::size_t>(N));
    for (int k = 0; k < N; ++k)
        d.pond[static_cast<std::size_t>(k)] = fill[static_cast<std::size_t>(k)] - g.height[static_cast<std::size_t>(k)];
    for (int q = N - 1; q >= 0; --q) {
        const int k = order[static_cast<std::size_t>(q)];
        const int t = d.to[static_cast<std::size_t>(k)];
        if (t >= 0) d.area[static_cast<std::size_t>(t)] += d.area[static_cast<std::size_t>(k)];
    }
    return d;
}

// Follow the biggest tributary upstream from `from` while it still drains at
// least `minArea`. Returned SOURCE first.
//
// It runs through hollows: the flood filled them, so the tree crosses each one
// down its far side, over the floor and back up to the spill point. The river
// solver keeps water falling by cutting through the rim (a gorge, which is what
// a real river does to a hollow it drains) -- up to its maxCut. trimToCut()
// then moves the source downstream until no rim needs more than that.
std::vector<int> upstream(const Grid& g, const Drainage& d, int from, float minArea,
                          const std::vector<char>& taken) {
    const int n = g.n;
    std::vector<int> path{from};
    int k = from;
    for (int guard = 0; guard < n * n; ++guard) {
        const int i = k % n, j = k / n;
        int best = -1;
        float bestA = minArea;
        for (int dj = -1; dj <= 1; ++dj)
            for (int di = -1; di <= 1; ++di) {
                if (!di && !dj) continue;
                const int ni = i + di, nj = j + dj;
                if (ni < 0 || nj < 0 || ni >= n || nj >= n) continue;
                const int nk = nj * n + ni;
                if (d.to[static_cast<std::size_t>(nk)] != k || taken[static_cast<std::size_t>(nk)]) continue;
                if (d.area[static_cast<std::size_t>(nk)] >= bestA) {
                    bestA = d.area[static_cast<std::size_t>(nk)];
                    best = nk;
                }
            }
        if (best < 0) break;
        path.push_back(best);
        k = best;
    }
    std::reverse(path.begin(), path.end());
    return path;
}

// The deepest the water would have to cut into the ground for its surface to
// keep falling from cell `from` of the path to its mouth: the largest rise of the
// ground above the lowest point upstream of it.
float cutNeeded(const Grid& g, const std::vector<int>& cells, std::size_t from) {
    float low = 1e30f, worst = 0.0f;
    for (std::size_t q = from; q < cells.size(); ++q) {
        const float h = g.height[static_cast<std::size_t>(cells[q])];
        low = std::min(low, h);
        worst = std::max(worst, h - low);
    }
    return worst;
}

// Drop the upstream end of a course until the rest needs no cut deeper than
// `limit` -- the source moves down to below the last hollow it could not drain.
void trimToCut(const Grid& g, std::vector<int>& cells, float limit) {
    std::size_t from = 0;
    while (from + 2 < cells.size() && cutNeeded(g, cells, from) > limit) ++from;
    cells.erase(cells.begin(), cells.begin() + static_cast<std::ptrdiff_t>(from));
}

std::vector<glm::vec2> cellsToLine(const Grid& g, const std::vector<int>& cells) {
    std::vector<glm::vec2> l;
    l.reserve(cells.size());
    for (int k : cells) l.push_back(g.at(k % g.n, k / g.n));
    return l;
}

// The half-width a channel claims from the land: the water at its widest, its
// banks, and a margin nothing else may build in.
float claimHalf(rivergen::Preset p) {
    const rivergen::Style st = rivergen::preset(p);
    return 0.5f * st.width * std::max(st.widen, 1.0f) + std::max(st.bankWidth, 2.0f) + 8.0f;
}

// The cut the plan allows a course, against the solver's own limit (kMaxCut,
// which the apply step gives every generated river): the grid sees the ground
// at its cell size, the solver at a metre, and the spline between the control
// points does not run exactly through the cell centres.
constexpr float kPlanCut = 0.6f * kMaxCut;

// Solve the course the way RiverSystem will -- the same spline through the same
// points, the same solver, the same ground -- and say whether the water can keep
// falling all the way without cutting deeper than it is allowed to.
bool courseHolds(const River& r, const Plan& out, const GroundFn& groundAt) {
    std::vector<int> ptSample;
    const std::vector<glm::vec2> flat = sampleSpline(r.points, false, &ptSample);
    if (flat.size() < 2) return false;
    rivergen::Style st = rivergen::preset(r.preset);
    st.maxCut = r.maxCut;
    const rivergen::Course c = rivergen::solve(
        rivergen::presetKind(r.preset), st, flat, std::vector<float>(flat.size(), 0.0f), ptSample,
        [&](float x, float z) { return groundAt(out.terrain, x, z); });
    return !c.empty() && !c.uphill && !c.reversed;
}

void planRivers(Plan& out, const Params& p, const Character& c, const GroundFn& groundAt) {
    Grid& g = out.grid;
    const Drainage d = drain(g, out.sea, out.seaLevel);
    for (std::size_t k = 0; k < d.area.size(); ++k) g.flow[k] = d.area[k];
    // Relative to the LAND, not the map: a coast that is a third sea drains a
    // third less, and its river is no less a river for that.
    const float mapArea = p.size * p.size * (out.sea ? std::max(1.0f - c.seaShare, 0.3f) : 1.0f);
    const float mainMin   = 0.025f * mapArea;   // a river drains a fortieth of the map
    const float mainSrc   = 0.004f * mapArea;
    const float brookMin  = 0.004f * mapArea;
    const float brookSrc  = 0.0012f * mapArea;
    std::vector<char> taken(d.area.size(), 0);

    // Make the course, prove it with the solver, and move the source downstream
    // until it holds. False if nothing long enough is left.
    auto finish = [&](std::vector<int> cells, bool brook, int joins, float minLen) {
        River r;
        r.brook  = brook;
        r.joins  = joins;
        r.preset = brook ? c.brook : c.river;
        r.maxCut = kMaxCut;
        for (int attempt = 0; attempt < 10; ++attempt) {
            std::vector<glm::vec2> l = cellsToLine(g, cells);
            // Close enough together that the spline through them cannot cut a
            // bend across the valley side: the solver would have to dig the water
            // through the spur it swung over.
            relax(l, 2);
            r.points = resample(l, brook ? 28.0f : 34.0f);
            r.length = polyLength(r.points);
            if (r.length < minLen || r.points.size() < 3) return false;
            if (courseHolds(r, out, groundAt)) break;
            if (attempt == 9) return false;
            cells.erase(cells.begin(), cells.begin() + static_cast<std::ptrdiff_t>(std::max<std::size_t>(cells.size() / 8, 2)));
        }
        r.drop = g.heightAt(r.points.front()) - g.heightAt(r.points.back());
        if (r.drop < 2.0f) return false;
        for (int k : cells) taken[static_cast<std::size_t>(k)] = 1;
        claimAlong(g, r.points, claimHalf(r.preset), use::River);
        out.rivers.push_back(std::move(r));
        return true;
    };

    // Outlets, biggest catchment first.
    std::vector<int> roots;
    for (int k = 0; k < g.n * g.n; ++k)
        if (d.to[static_cast<std::size_t>(k)] < 0) roots.push_back(k);
    std::sort(roots.begin(), roots.end(), [&](int a, int b) {
        return d.area[static_cast<std::size_t>(a)] > d.area[static_cast<std::size_t>(b)];
    });

    if (p.stages.rivers && c.rivers > 0) {
        for (int root : roots) {
            if (static_cast<int>(out.rivers.size()) >= c.rivers) break;
            if (d.area[static_cast<std::size_t>(root)] < mainMin) break;
            std::vector<int> cells = upstream(g, d, root, mainSrc, taken);
            trimToCut(g, cells, kPlanCut);
            if (cells.size() < 4) continue;
            const float len = static_cast<float>(cells.size()) * g.cell;
            if (len < 0.2f * p.size) continue;
            // Not a second river down the same valley: two outlets side by side on
            // the map's edge are often one valley seen twice.
            int near = 0;
            for (int k : cells)
                for (const River& o : out.rivers)
                    if (lineDist(g.at(k % g.n, k / g.n), o.points) < 160.0f) { ++near; break; }
            if (near * 4 > static_cast<int>(cells.size())) continue;
            finish(cells, false, -1, 0.2f * p.size);
        }
    }

    // Brooks: the biggest tributaries of the rivers, then lesser outlets of their own.
    if (p.stages.rivers && c.brooks > 0) {
        struct Cand { float area; int cell; int river; };
        std::vector<Cand> cands;
        const int mains = static_cast<int>(out.rivers.size());
        for (int ri = 0; ri < mains; ++ri) {
            for (const glm::vec2& pt : out.rivers[static_cast<std::size_t>(ri)].points) {
                int i, j;
                const int k = cellOf(g, pt, i, j);
                for (int dj = -2; dj <= 2; ++dj)
                    for (int di = -2; di <= 2; ++di) {
                        const int ni = i + di, nj = j + dj;
                        if (ni < 0 || nj < 0 || ni >= g.n || nj >= g.n) continue;
                        const int nk = g.index(ni, nj);
                        if (taken[static_cast<std::size_t>(nk)]) continue;
                        const int t = d.to[static_cast<std::size_t>(nk)];
                        if (t < 0 || !taken[static_cast<std::size_t>(t)]) continue;
                        if (d.area[static_cast<std::size_t>(nk)] < brookMin) continue;
                        cands.push_back({d.area[static_cast<std::size_t>(nk)], nk, ri});
                    }
                (void)k;
            }
        }
        for (int root : roots)
            if (d.area[static_cast<std::size_t>(root)] >= brookMin && !taken[static_cast<std::size_t>(root)])
                cands.push_back({d.area[static_cast<std::size_t>(root)] * 0.7f, root, -1});
        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.area > b.area; });
        std::vector<glm::vec2> mouths;
        int made = 0;
        for (const Cand& cd : cands) {
            if (made >= c.brooks) break;
            if (taken[static_cast<std::size_t>(cd.cell)]) continue;
            const glm::vec2 m = g.at(cd.cell % g.n, cd.cell / g.n);
            bool crowded = false;
            for (const glm::vec2& o : mouths) crowded = crowded || glm::length(o - m) < 260.0f;
            if (crowded) continue;
            std::vector<int> cells = upstream(g, d, cd.cell, brookSrc, taken);
            trimToCut(g, cells, kPlanCut);
            if (static_cast<float>(cells.size()) * g.cell < 280.0f) continue;
            // Run it into the river it joins, so the two waters meet.
            if (cd.river >= 0) {
                const int t = d.to[static_cast<std::size_t>(cd.cell)];
                if (t >= 0) cells.push_back(t);
            }
            if (!finish(cells, true, cd.river, 280.0f)) continue;
            mouths.push_back(m);
            ++made;
        }
    }
}

// --- Stage 3: where people settle ---------------------------------------------------

std::string nameFrom(unsigned seed, int index) {
    static const char* head[] = {
        "Wald", "Ober", "Unter", "Alt", "Neu", "Stein", "Rosen", "Lichten", "Hohen",
        "Kirch", "Mühl", "Eich", "Linden", "Birken", "Tann", "Wolfs", "Hasel", "Kalt",
        "Schön", "Gold", "Erlen", "Buchen", "Fichten", "Hain", "Weiden", "Rot",
        "Grün", "Schwarz", "Klein", "Groß", "Mittel", "Frei", "Brunn", "Ahorn"};
    static const char* tail[] = {
        "bach", "dorf", "hausen", "heim", "feld", "berg", "au", "ingen", "stadt",
        "rode", "tal", "brück", "furt", "weiler", "hofen", "stein", "born", "kirchen",
        "wald", "eck", "stetten", "burg", "hagen", "beuren"};
    constexpr int nh = static_cast<int>(sizeof(head) / sizeof(head[0]));
    constexpr int nt = static_cast<int>(sizeof(tail) / sizeof(tail[0]));
    const std::uint32_t h = mix(seed * 0x2545F491u ^ mix(static_cast<std::uint32_t>(index) + 77u));
    return std::string(head[h % nh]) + tail[(h / nh) % nt];
}

// The middle of a block on each edge street, and which way is out.
void gatesOf(Town& t, const cityplan::Layout& L) {
    t.gates.clear();
    t.gateDir.clear();
    // A village says where a road may come in itself: the forks at its ends.
    if (L.organic) {
        t.gates   = L.gates;
        t.gateDir = L.gateDirs;
        return;
    }
    if (L.nodes.empty()) return;
    auto node = [&](int i, int j) {
        return L.nodes[static_cast<std::size_t>(j * (L.nx + 1) + i)];
    };
    const int bz = std::max((L.nz - 1) / 2, 0), bx = std::max((L.nx - 1) / 2, 0);
    t.gates.push_back(0.5f * (node(L.nx, bz) + node(L.nx, bz + 1)));  t.gateDir.push_back(L.axisX);
    t.gates.push_back(0.5f * (node(0, bz) + node(0, bz + 1)));        t.gateDir.push_back(-L.axisX);
    t.gates.push_back(0.5f * (node(bx, L.nz) + node(bx + 1, L.nz)));  t.gateDir.push_back(L.axisZ);
    t.gates.push_back(0.5f * (node(bx, 0) + node(bx + 1, 0)));        t.gateDir.push_back(-L.axisZ);
}

void planTowns(Plan& out, const Params& p, const Character& c) {
    Grid& g = out.grid;
    if (!p.stages.towns || (c.towns <= 0 && c.villages <= 0)) return;
    const int n = g.n;
    Sat hs, h2s, ss, wet;
    std::vector<float> h2(g.height.size()), w(g.height.size());
    for (std::size_t k = 0; k < g.height.size(); ++k) {
        h2[k] = g.height[k] * g.height[k];
        const bool low = out.sea && g.height[k] < out.seaLevel + 1.5f;
        w[k] = (g.use[k] == use::River || g.use[k] == use::Sea || low) ? 1.0f : 0.0f;
    }
    hs.build(g.height, n); h2s.build(h2, n); ss.build(g.slope, n); wet.build(w, n);
    const std::vector<float> toRiver = distanceTo(g, [&](int k) { return g.use[static_cast<std::size_t>(k)] == use::River; });
    const std::vector<float> toSea   = distanceTo(g, [&](int k) { return g.use[static_cast<std::size_t>(k)] == use::Sea; });

    struct Slot { cityplan::Preset preset; bool village; };
    std::vector<Slot> slots;
    for (int i = 0; i < c.towns; ++i)    slots.push_back({c.town, false});
    for (int i = 0; i < c.villages; ++i) slots.push_back({c.village, true});

    Rng rng(p.seed ^ kPhaseTowns);
    int named = 0;
    for (const Slot& s : slots) {
        cityplan::Rule r;
        cityplan::applyPreset(r, s.preset);
        Town t;
        t.preset  = s.preset;
        t.village = s.village;
        const float scale = s.village ? glm::clamp(c.villageScale, 0.4f, 1.5f) : 1.0f;
        t.sizeX   = std::min(r.grid.sizeX * scale, 0.42f * p.size);
        t.sizeZ   = std::min(r.grid.sizeZ * scale, 0.42f * p.size);
        const float R  = t.radius();
        const int   rw = std::max(1, static_cast<int>(std::ceil(R / g.cell)));
        const int   margin = rw + static_cast<int>(std::ceil(120.0f / g.cell));
        float best = -1e30f;
        int   bi = -1, bj = -1;
        const int stride = std::max(1, rw / 6);
        for (int j = margin; j < n - margin; j += stride)
            for (int i = margin; i < n - margin; i += stride) {
                const int k = g.index(i, j);
                if (wet.sum(i - rw, j - rw, i + rw, j + rw) > 0.5) continue;
                const float meanSlope = static_cast<float>(ss.mean(i, j, rw));
                if (meanSlope > c.siteSlope) continue;
                const double m  = hs.mean(i, j, rw);
                const double v  = std::max(h2s.mean(i, j, rw) - m * m, 0.0);
                const float  sd = static_cast<float>(std::sqrt(v));
                if (out.treeLine > 0.0f && m > out.treeLine - 40.0f) continue;
                const glm::vec2 at = g.at(i, j);
                bool crowded = false;
                for (const Town& o : out.towns)
                    crowded = crowded || glm::length(o.center - at) < R + o.radius() + 260.0f;
                if (crowded) continue;
                const float dR = toRiver[static_cast<std::size_t>(k)];
                const float dS = toSea[static_cast<std::size_t>(k)];
                float score = -1.2f * meanSlope / std::max(c.siteSlope, 1e-3f)
                              - sd / std::max(0.04f * R, 1.0f)
                              + (out.rivers.empty() ? 0.0f : 1.0f * band(dR, 1.0f * R, 2.6f * R))
                              + (out.sea ? 0.7f * band(dS, 1.0f * R, 3.0f * R) : 0.0f)
                              + 0.45f * hash01(p.seed ^ kPhaseTowns, static_cast<std::uint32_t>(k));
                // The main town wants the middle of the map; villages go out.
                const float off = glm::length(at - p.center) / (0.5f * p.size);
                score += s.village ? 0.25f * band(off, 0.35f, 0.85f) : -0.8f * off;
                if (score > best) { best = score; bi = i; bj = j; }
            }
        if (bi < 0) {
            if (out.report.why.empty())
                out.report.why = std::string("No flat, dry site left for a ") +
                                 (s.village ? "village" : "town") + " -- fewer, or a gentler region.";
            continue;
        }
        const int k = g.index(bi, bj);
        t.center = g.at(bi, bj);
        t.ground = static_cast<float>(hs.mean(bi, bj, rw));
        {
            float lo = 1e30f, hi = -1e30f;
            for (int j = bj - rw; j <= bj + rw; ++j)
                for (int i = bi - rw; i <= bi + rw; ++i) {
                    if (i < 0 || j < 0 || i >= n || j >= n) continue;
                    const float hv = g.height[static_cast<std::size_t>(g.index(i, j))];
                    lo = std::min(lo, hv); hi = std::max(hi, hv);
                }
            t.relief = hi - lo;
        }
        // The grid follows the land: along the river when there is one close by
        // (a high street runs WITH the water), else along the contours.
        glm::vec2 along(1.0f, 0.0f);
        if (toRiver[static_cast<std::size_t>(k)] < 3.0f * R) {
            float bestD = 1e30f;
            for (const River& rv : out.rivers)
                for (std::size_t q = 1; q < rv.points.size(); ++q) {
                    const float dd = segDist(t.center, rv.points[q - 1], rv.points[q]);
                    if (dd < bestD) { bestD = dd; along = rv.points[q] - rv.points[q - 1]; }
                }
        } else {
            const int il = std::max(bi - 1, 0), ir = std::min(bi + 1, n - 1);
            const int jd = std::max(bj - 1, 0), ju = std::min(bj + 1, n - 1);
            const glm::vec2 grad(g.height[static_cast<std::size_t>(g.index(ir, bj))] - g.height[static_cast<std::size_t>(g.index(il, bj))],
                                 g.height[static_cast<std::size_t>(g.index(bi, ju))] - g.height[static_cast<std::size_t>(g.index(bi, jd))]);
            if (glm::dot(grad, grad) > 1e-6f) along = glm::vec2(-grad.y, grad.x);
        }
        if (glm::dot(along, along) < 1e-9f) along = glm::vec2(1.0f, 0.0f);
        t.rotation = glm::degrees(std::atan2(along.y, along.x)) + rng.uni(-6.0f, 6.0f);
        t.name = nameFrom(p.seed, named++);
        for (int guard = 0; guard < 8; ++guard) {
            bool dup = false;
            for (const Town& o : out.towns) dup = dup || o.name == t.name;
            if (!dup) break;
            t.name = nameFrom(p.seed, 100 + named++);
        }
        t.seed = mix(p.seed ^ mix(static_cast<std::uint32_t>(out.towns.size()) + 0x51u)) | 1u;

        cityplan::Rule lr = r;
        lr.grid.center = t.center;
        lr.grid.sizeX = t.sizeX; lr.grid.sizeZ = t.sizeZ;
        lr.grid.rotation = t.rotation;
        lr.grid.seed = t.seed;
        gatesOf(t, cityplan::layout(lr, lr.grid));

        // Claim the town (and its stubs) as a rotated rectangle.
        const float a = glm::radians(t.rotation);
        const glm::vec2 ax(std::cos(a), std::sin(a)), az(-std::sin(a), std::cos(a));
        const float hx = 0.5f * t.sizeX + 40.0f, hz = 0.5f * t.sizeZ + 40.0f;
        for (int j = bj - 2 * rw; j <= bj + 2 * rw; ++j)
            for (int i = bi - 2 * rw; i <= bi + 2 * rw; ++i) {
                if (i < 0 || j < 0 || i >= n || j >= n) continue;
                const glm::vec2 d = g.at(i, j) - t.center;
                if (std::fabs(glm::dot(d, ax)) <= hx && std::fabs(glm::dot(d, az)) <= hz) {
                    std::uint8_t& u = g.use[static_cast<std::size_t>(g.index(i, j))];
                    if (u == use::Free || u > use::Town) u = use::Town;
                }
            }
        out.towns.push_back(std::move(t));
    }
}

// --- Stage 4: the roads between them -------------------------------------------------
//
// A* over the analysis grid with sixteen moves (the eight neighbours and the
// eight knight's moves): with only eight, every road is a staircase of 45-degree
// legs, and on a slope the cheapest climb it can find is straight up. The knight's
// moves give it the shallow diagonals a road actually takes across a hillside, and
// with the grade penalty below they are what turns a climb into hairpins.

struct Router {
    const Grid&      g;
    const Character& c;
    // The shore: a cell whose centre is dry can still have the sea in its
    // corner, and a road skimming along it is a road in the surf. Below this,
    // land counts as sea for the route (-1e30 = no sea).
    float             shore;
    std::vector<char> nearRoad;   // within a road's width of a road already planned

    Router(const Grid& g_, const Character& c_, float shore_)
        : g(g_), c(c_), shore(shore_), nearRoad(g_.height.size(), 0) {}

    float stepCost(int a, int b, float len) const {
        const std::uint8_t ub = g.use[static_cast<std::size_t>(b)];
        if (ub == use::Sea || g.height[static_cast<std::size_t>(b)] < shore) return -1.0f;
        // The grade over the step's halves, not end to end: a knight's move is
        // thirty metres, and a gully between two cells of the same height is a
        // climb the road would have to make twice.
        const glm::vec2 pa = g.at(a % g.n, a / g.n), pb = g.at(b % g.n, b / g.n);
        const float ha = g.height[static_cast<std::size_t>(a)], hb = g.height[static_cast<std::size_t>(b)];
        float grade = std::fabs(hb - ha) / len;
        if (len > 1.2f * g.cell) {
            const float hm = g.heightAt(0.5f * (pa + pb));
            grade = std::max({grade, std::fabs(hm - ha) / (0.5f * len), std::fabs(hb - hm) / (0.5f * len)});
        }
        const float q = grade / std::max(c.maxGrade, 0.01f);
        float cost = len * (1.0f + 2.0f * q * q);
        if (q > 1.0f) cost *= 1.0f + 14.0f * (q - 1.0f);
        // Sidelong ground costs a road its cuttings even where it runs level.
        const float side = g.slope[static_cast<std::size_t>(b)] / 0.35f;
        cost += len * 1.5f * side * side;
        if (ub == use::River) {
            cost += len * 10.0f;
            if (g.use[static_cast<std::size_t>(a)] != use::River) cost += 260.0f;  // a bridge
        }
        if (ub == use::Town) cost *= 6.0f;
        if (nearRoad[static_cast<std::size_t>(b)]) cost *= 2.5f;
        return cost;
    }

    // A viaduct: one straight span from `a` to `b` over ground at least ten
    // metres below both ends. Dearer per metre than a road on the ground, and a
    // fixed price on top -- so it wins only where the alternative is the climb
    // down a canyon wall and back up the other side, which on the ground is
    // either a kilometre of hairpins or a gradient no road has.
    float deckCost(int a, int b, float len) const {
        const std::uint8_t ub = g.use[static_cast<std::size_t>(b)];
        if (ub == use::Sea || ub == use::River || ub == use::Town) return -1.0f;
        const float ha = g.height[static_cast<std::size_t>(a)], hb = g.height[static_cast<std::size_t>(b)];
        if (std::fabs(hb - ha) / len > c.maxGrade) return -1.0f;
        const glm::vec2 pa = g.at(a % g.n, a / g.n), pb = g.at(b % g.n, b / g.n);
        const int samples = std::max(3, static_cast<int>(len / (0.5f * g.cell)));
        float lowest = 1e30f;
        for (int q = 1; q < samples; ++q) {
            const glm::vec2 x = glm::mix(pa, pb, static_cast<float>(q) / static_cast<float>(samples));
            int i, j;
            const std::uint8_t u = g.use[static_cast<std::size_t>(cellOf(g, x, i, j))];
            if (u == use::Town) return -1.0f;
            const float h = g.heightAt(x);
            // It has to leave the rim at once; a deck that first runs along level
            // ground is a road someone forgot to build.
            if (q == 1 && h > std::min(ha, hb) - 2.0f) return -1.0f;
            lowest = std::min(lowest, h);
        }
        if (std::min(ha, hb) - lowest < 10.0f) return -1.0f;
        return len * 2.5f + 250.0f;
    }

    struct Route {
        std::vector<int>  cells;
        std::vector<char> deck;   // deck[k]: the step from cells[k-1] to cells[k] is a span
    };

    // `to` is the goal; with `exitReach` > 0 any cell on the map's edge within
    // that many metres of it will do -- a road leaves the map wherever along that
    // stretch the ground lets it, not at one cell that may be a cliff.
    Route route(int from, int to, float exitReach = 0.0f) const {
        const int n = g.n, N = n * n;
        static const int mi[16] = {1, -1, 0, 0, 1, 1, -1, -1, 2, 2, -2, -2, 1, 1, -1, -1};
        static const int mj[16] = {0, 0, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 2, -2, 2, -2};
        static const int span[6] = {3, 4, 6, 8, 10, 13};
        constexpr float kMaxDeck = 320.0f;
        std::vector<float> best(static_cast<std::size_t>(N), 1e30f);
        std::vector<int>   prev(static_cast<std::size_t>(N), -1);
        std::vector<char>  done(static_cast<std::size_t>(N), 0);
        std::vector<char>  viaDeck(static_cast<std::size_t>(N), 0);
        const glm::vec2 goal = g.at(to % n, to / n);
        auto isGoal = [&](int k) {
            if (k == to) return true;
            if (exitReach <= 0.0f) return false;
            const int i = k % n, j = k / n;
            // The outermost row itself: from anywhere further in, the stretch run
            // off the edge would cross ground the route never looked at.
            if (i > 0 && j > 0 && i < n - 1 && j < n - 1) return false;
            return glm::length(g.at(i, j) - goal) <= exitReach;
        };
        auto h = [&](glm::vec2 q) { return std::max(glm::length(q - goal) - exitReach, 0.0f); };
        using Item = std::pair<float, int>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
        best[static_cast<std::size_t>(from)] = 0.0f;
        open.push({h(g.at(from % n, from / n)), from});
        int reached = -1;
        while (!open.empty()) {
            const int k = open.top().second;
            open.pop();
            if (done[static_cast<std::size_t>(k)]) continue;
            done[static_cast<std::size_t>(k)] = 1;
            if (isGoal(k)) { reached = k; break; }
            const int i = k % n, j = k / n;
            for (int e = 0; e < 16; ++e) {
                const int ni = i + mi[e], nj = j + mj[e];
                if (ni < 0 || nj < 0 || ni >= n || nj >= n) continue;
                const int nk = nj * n + ni;
                if (done[static_cast<std::size_t>(nk)]) continue;
                // A knight's move must not jump over the sea.
                if (e >= 8) {
                    const int hi = i + mi[e] / 2, hj = j + mj[e] / 2;
                    const int mk = glm::clamp(hj, 0, n - 1) * n + glm::clamp(hi, 0, n - 1);
                    if (g.use[static_cast<std::size_t>(mk)] == use::Sea) continue;
                }
                const float len = g.cell * std::sqrt(static_cast<float>(mi[e] * mi[e] + mj[e] * mj[e]));
                const float sc = stepCost(k, nk, len);
                if (sc < 0.0f) continue;
                const float nb = best[static_cast<std::size_t>(k)] + sc;
                if (nb < best[static_cast<std::size_t>(nk)]) {
                    best[static_cast<std::size_t>(nk)] = nb;
                    prev[static_cast<std::size_t>(nk)] = k;
                    viaDeck[static_cast<std::size_t>(nk)] = 0;
                    open.push({nb + h(g.at(ni, nj)), nk});
                }
            }
            // Spans only off a rim: where the ground falls away. Everywhere else
            // the check would cost a hundred samples to learn there is no valley.
            if (g.slope[static_cast<std::size_t>(k)] < 0.12f) continue;
            for (int e = 0; e < 16; ++e)
                for (int m : span) {
                    const int ni = i + mi[e] * m, nj = j + mj[e] * m;
                    if (ni < 0 || nj < 0 || ni >= n || nj >= n) break;
                    const float len = g.cell * static_cast<float>(m) *
                                      std::sqrt(static_cast<float>(mi[e] * mi[e] + mj[e] * mj[e]));
                    if (len > kMaxDeck) break;
                    const int nk = nj * n + ni;
                    if (done[static_cast<std::size_t>(nk)]) continue;
                    const float sc = deckCost(k, nk, len);
                    if (sc < 0.0f) continue;
                    const float nb = best[static_cast<std::size_t>(k)] + sc;
                    if (nb < best[static_cast<std::size_t>(nk)]) {
                        best[static_cast<std::size_t>(nk)] = nb;
                        prev[static_cast<std::size_t>(nk)] = k;
                        viaDeck[static_cast<std::size_t>(nk)] = 1;
                        open.push({nb + h(g.at(ni, nj)), nk});
                    }
                }
        }
        Route r;
        if (reached < 0) return r;
        for (int k = reached; k >= 0; k = prev[static_cast<std::size_t>(k)]) {
            r.cells.push_back(k);
            r.deck.push_back(viaDeck[static_cast<std::size_t>(k)]);
        }
        std::reverse(r.cells.begin(), r.cells.end());
        std::reverse(r.deck.begin(), r.deck.end());
        if (!r.deck.empty()) r.deck.front() = 0;
        return r;
    }
};

// Where the road meets water, as stretches of arc length along the line.
struct Wet { float s0, s1; };

std::vector<Wet> wetStretches(const std::vector<glm::vec2>& line, const Plan& plan) {
    std::vector<Wet> out;
    const float step = 2.0f;
    float s = 0.0f;
    bool in = false;
    Wet cur{0.0f, 0.0f};
    for (std::size_t i = 1; i < line.size(); ++i) {
        const glm::vec2 a = line[i - 1], b = line[i];
        const float seg = glm::length(b - a);
        for (float t = 0.0f; t < seg; t += step) {
            const glm::vec2 p = a + (b - a) * (t / std::max(seg, 1e-6f));
            bool wet = plan.sea && plan.grid.heightAt(p) < plan.seaLevel + 0.5f;
            for (const River& r : plan.rivers) {
                if (wet) break;
                const rivergen::Style st = rivergen::preset(r.preset);
                wet = lineDist(p, r.points) < 0.5f * st.width * std::max(st.widen, 1.0f) +
                                                  std::max(st.bankWidth, 2.0f) + 3.0f;
            }
            if (wet && !in) { cur.s0 = s + t; in = true; }
            if (!wet && in) { cur.s1 = s + t; out.push_back(cur); in = false; }
        }
        s += seg;
    }
    if (in) { cur.s1 = s; out.push_back(cur); }
    // Two crossings closer than their abutments are one bridge.
    std::vector<Wet> merged;
    for (const Wet& w : out) {
        if (!merged.empty() && w.s0 - merged.back().s1 < 24.0f) merged.back().s1 = w.s1;
        else merged.push_back(w);
    }
    return merged;
}

glm::vec2 pointAt(const std::vector<glm::vec2>& l, float s) {
    float run = 0.0f;
    for (std::size_t i = 1; i < l.size(); ++i) {
        const float seg = glm::length(l[i] - l[i - 1]);
        if (run + seg >= s) return glm::mix(l[i - 1], l[i], (s - run) / std::max(seg, 1e-6f));
        run += seg;
    }
    return l.back();
}

// Rebuild the control points so every water crossing is one span between two
// points standing on dry ground either side -- the form RoadSystem's BridgeSpec
// wants, with nothing in between for the profile to dip to. The spans the route
// already has (viaducts) are kept as they are, and a crossing under one of them
// is already bridged.
void bridgeCrossings(Road& road, const Plan& plan) {
    std::vector<float> arc(road.points.size(), 0.0f);
    for (std::size_t i = 1; i < road.points.size(); ++i)
        arc[i] = arc[i - 1] + glm::length(road.points[i] - road.points[i - 1]);
    const float total = arc.back();
    std::vector<Wet> decks;
    for (const Span& sp : road.bridges)
        decks.push_back({arc[static_cast<std::size_t>(sp.a)], arc[static_cast<std::size_t>(sp.b)]});
    std::vector<Wet> wet;
    for (const Wet& w : wetStretches(road.points, plan)) {
        bool under = false;
        for (const Wet& d : decks) under = under || (w.s0 >= d.s0 - 1.0f && w.s1 <= d.s1 + 1.0f);
        if (!under) wet.push_back({std::max(w.s0 - 10.0f, 0.0f), std::min(w.s1 + 10.0f, total)});
    }
    if (wet.empty()) return;
    for (const Wet& d : decks) wet.push_back(d);
    std::sort(wet.begin(), wet.end(), [](const Wet& x, const Wet& y) { return x.s0 < y.s0; });
    std::vector<Wet> all;
    for (const Wet& w : wet) {
        if (!all.empty() && w.s0 <= all.back().s1 + 4.0f) all.back().s1 = std::max(all.back().s1, w.s1);
        else all.push_back(w);
    }
    std::vector<glm::vec2> pts;
    std::vector<Span> spans;
    std::size_t next = 0;
    for (const Wet& w : all) {
        const float a = w.s0, b = w.s1;
        while (next < road.points.size() && arc[next] < a) pts.push_back(road.points[next++]);
        while (next < road.points.size() && arc[next] <= b) ++next;
        if (!pts.empty() && glm::length(pts.back() - pointAt(road.points, a)) < 4.0f) pts.pop_back();
        pts.push_back(pointAt(road.points, a));
        const int ia = static_cast<int>(pts.size()) - 1;
        pts.push_back(pointAt(road.points, b));
        spans.push_back({ia, ia + 1});
    }
    while (next < road.points.size()) {
        if (glm::length(road.points[next] - pts.back()) > 4.0f) pts.push_back(road.points[next]);
        ++next;
    }
    road.points  = std::move(pts);
    road.bridges = std::move(spans);
}

void planRoads(Plan& out, const Params& p, const Character& c) {
    if (!p.stages.roads) return;
    Grid& g = out.grid;
    Router router(g, c, out.sea ? out.seaLevel + 2.0f : -1e30f);
    Rng rng(p.seed ^ kPhaseRoads);

    // --- Where roads leave the map -------------------------------------------
    std::vector<glm::vec2> exits;
    {
        struct Cand { float score; glm::vec2 at; glm::vec2 in; };
        std::vector<Cand> cands;
        const int n = g.n, edge = std::max(2, n / 10);
        for (int side = 0; side < 4; ++side)
            for (int t = edge; t < n - edge; t += 3) {
                int i = 0, j = 0;
                glm::vec2 in(0.0f);
                switch (side) {
                case 0: i = 1;     j = t; in = { 1, 0}; break;
                case 1: i = n - 2; j = t; in = {-1, 0}; break;
                case 2: i = t; j = 1;     in = {0,  1}; break;
                default: i = t; j = n - 2; in = {0, -1}; break;
                }
                const int k = g.index(i, j);
                const std::uint8_t u = g.use[static_cast<std::size_t>(k)];
                if (u == use::Sea || u == use::River || u == use::Town) continue;
                if (out.sea && g.height[static_cast<std::size_t>(k)] < out.seaLevel + 2.0f) continue;
                // Low and level: a road leaves the map the way a valley does, not
                // over the mountains round it.
                const float rise = (g.height[static_cast<std::size_t>(k)] - out.report.lowest) /
                                   std::max(out.report.highest - out.report.lowest, 1.0f);
                const float score = -g.slope[static_cast<std::size_t>(k)] * 4.0f - 3.0f * rise +
                                    hash01(p.seed ^ kPhaseRoads, static_cast<std::uint32_t>(k));
                cands.push_back({score, g.at(i, j), in});
            }
        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.score > b.score; });
        for (const Cand& cd : cands) {
            if (static_cast<int>(exits.size()) >= c.exits) break;
            bool crowded = false;
            for (const glm::vec2& e : exits) crowded = crowded || glm::length(e - cd.at) < 0.45f * p.size;
            if (!crowded) exits.push_back(cd.at);
        }
    }

    // --- Which places are joined ----------------------------------------------
    const int nt = static_cast<int>(out.towns.size());
    std::vector<std::pair<int, int>> links;      // town indices; -1-e = exit e
    if (nt > 0) {
        std::vector<char> in(static_cast<std::size_t>(nt), 0);
        in[0] = 1;
        for (int added = 1; added < nt; ++added) {
            float bd = 1e30f; int ba = -1, bb = -1;
            for (int a = 0; a < nt; ++a) {
                if (!in[static_cast<std::size_t>(a)]) continue;
                for (int b = 0; b < nt; ++b) {
                    if (in[static_cast<std::size_t>(b)]) continue;
                    const float d = glm::length(out.towns[static_cast<std::size_t>(a)].center -
                                                out.towns[static_cast<std::size_t>(b)].center);
                    if (d < bd) { bd = d; ba = a; bb = b; }
                }
            }
            in[static_cast<std::size_t>(bb)] = 1;
            links.push_back({ba, bb});
        }
        // Loops: the pairs the tree makes the longest detour between.
        auto graphDist = [&](int a, int b) {
            std::vector<float> dist(static_cast<std::size_t>(nt), 1e30f);
            dist[static_cast<std::size_t>(a)] = 0.0f;
            for (int it = 0; it < nt; ++it)
                for (const auto& [u, v] : links) {
                    if (u < 0 || v < 0) continue;
                    const float w = glm::length(out.towns[static_cast<std::size_t>(u)].center -
                                                out.towns[static_cast<std::size_t>(v)].center);
                    dist[static_cast<std::size_t>(v)] = std::min(dist[static_cast<std::size_t>(v)], dist[static_cast<std::size_t>(u)] + w);
                    dist[static_cast<std::size_t>(u)] = std::min(dist[static_cast<std::size_t>(u)], dist[static_cast<std::size_t>(v)] + w);
                }
            return dist[static_cast<std::size_t>(b)];
        };
        int extra = static_cast<int>(std::lround(c.extraRoads * static_cast<float>(nt)));
        while (extra-- > 0) {
            float bestRatio = 1.6f; int ba = -1, bb = -1;
            for (int a = 0; a < nt; ++a)
                for (int b = a + 1; b < nt; ++b) {
                    const float e = glm::length(out.towns[static_cast<std::size_t>(a)].center -
                                                out.towns[static_cast<std::size_t>(b)].center);
                    if (e > 0.75f * p.size) continue;
                    const float r = graphDist(a, b) / std::max(e, 1.0f);
                    if (r > bestRatio) { bestRatio = r; ba = a; bb = b; }
                }
            if (ba < 0) break;
            links.push_back({ba, bb});
        }
        for (int e = 0; e < static_cast<int>(exits.size()); ++e) {
            float bd = 1e30f; int bt = 0;
            for (int t = 0; t < nt; ++t) {
                const float d = glm::length(out.towns[static_cast<std::size_t>(t)].center - exits[static_cast<std::size_t>(e)]);
                if (d < bd) { bd = d; bt = t; }
            }
            links.push_back({bt, -1 - e});
        }
    } else {
        for (int e = 1; e < static_cast<int>(exits.size()); ++e) links.push_back({-1 - (e - 1), -1 - e});
    }

    // --- Lay each one -----------------------------------------------------------
    std::vector<std::vector<char>> gateUsed(static_cast<std::size_t>(nt), std::vector<char>(4, 0));
    auto endOf = [&](int node, glm::vec2 toward, glm::vec2& at, glm::vec2& out_) {
        if (node < 0) {
            at = exits[static_cast<std::size_t>(-1 - node)];
            // Run the last stretch straight off the map edge.
            glm::vec2 e = at;
            const glm::vec2 lo = g.origin, hi = g.origin + glm::vec2(g.cell * static_cast<float>(g.n - 1));
            const float dl = at.x - lo.x, dr = hi.x - at.x, dd = at.y - lo.y, du = hi.y - at.y;
            const float m = std::min({dl, dr, dd, du});
            if (m == dl) e.x = lo.x - 20.0f; else if (m == dr) e.x = hi.x + 20.0f;
            else if (m == dd) e.y = lo.y - 20.0f; else e.y = hi.y + 20.0f;
            out_ = at;
            at = e;
            return;
        }
        Town& t = out.towns[static_cast<std::size_t>(node)];
        int best = 0; float bs = -1e30f;
        for (int q = 0; q < static_cast<int>(t.gates.size()); ++q) {
            const glm::vec2 gt = t.gates[static_cast<std::size_t>(q)], gd = t.gateDir[static_cast<std::size_t>(q)];
            const glm::vec2 d = glm::normalize(toward - gt + glm::vec2(1e-4f));
            // The straight lead out of the gate cannot dodge the ground, so a gate
            // whose lead runs down a bank is a worse gate however well it faces.
            const float leadLen = t.village ? 20.0f : 40.0f;
            const float lead = std::fabs(g.heightAt(gt + gd * leadLen) - g.heightAt(gt)) / leadLen;
            const float s = glm::dot(d, gd) - 3.0f * std::max(lead - 0.5f * c.maxGrade, 0.0f) / std::max(c.maxGrade, 0.01f)
                          - (gateUsed[static_cast<std::size_t>(node)][static_cast<std::size_t>(q)] ? 1.2f : 0.0f)
                          - (out.sea && g.heightAt(gt + gd * leadLen) < out.seaLevel + 1.0f ? 5.0f : 0.0f);
            if (s > bs) { bs = s; best = q; }
        }
        gateUsed[static_cast<std::size_t>(node)][static_cast<std::size_t>(best)] = 1;
        at   = t.gates[static_cast<std::size_t>(best)];
        // Square off the edge street for a stub's length (every town street runs on
        // ~30 m past the edge) before the route may turn: a T, not a graze. A
        // village's fork has no stubs to clear, and its gate stands outside the
        // houses already: a short lead, so it does not climb the slope beyond.
        out_ = at + t.gateDir[static_cast<std::size_t>(best)] * (t.village ? 20.0f : 40.0f);
    };

    int roadNo = 0;
    for (const auto& [a, b] : links) {
        const glm::vec2 ca = a >= 0 ? out.towns[static_cast<std::size_t>(a)].center : exits[static_cast<std::size_t>(-1 - a)];
        const glm::vec2 cb = b >= 0 ? out.towns[static_cast<std::size_t>(b)].center : exits[static_cast<std::size_t>(-1 - b)];
        glm::vec2 endA, leadA, endB, leadB;
        endOf(a, cb, endA, leadA);
        endOf(b, ca, endB, leadB);
        int i0, j0, i1, j1;
        const int from = cellOf(g, leadA, i0, j0), to = cellOf(g, leadB, i1, j1);
        const Router::Route route = router.route(from, to, b < 0 ? 150.0f : 0.0f);
        const std::vector<int>& cells = route.cells;
        Road road;
        road.width = c.roadWidth;
        road.from = a; road.to = b;
        if (cells.empty()) {
            if (a >= 0) ++out.report.unreached;
            continue;
        }
        if (b < 0) {
            // Off the edge from the cell it actually reached.
            const int k = cells.back();
            leadB = g.at(k % g.n, k / g.n);
            glm::vec2 e = leadB;
            const glm::vec2 lo = g.origin, hi = g.origin + glm::vec2(g.cell * static_cast<float>(g.n - 1));
            const float dl = e.x - lo.x, dr = hi.x - e.x, dd = e.y - lo.y, du = hi.y - e.y;
            const float m = std::min({dl, dr, dd, du});
            if (m == dl) e.x = lo.x - 20.0f; else if (m == dr) e.x = hi.x + 20.0f;
            else if (m == dd) e.y = lo.y - 20.0f; else e.y = hi.y + 20.0f;
            endB = e;
        }
        // The route as a line, each point flagged when the stretch leading to it
        // is a span -- kept whole, never simplified away or subdivided.
        std::vector<glm::vec2> line;
        std::vector<char>      deck;
        auto add = [&](glm::vec2 q, bool d) {
            if (!line.empty() && glm::length(q - line.back()) < 1.0f) return;
            line.push_back(q);
            deck.push_back(d ? 1 : 0);
        };
        add(endA, false);
        add(leadA, false);
        {
            const std::vector<glm::vec2> raw = cellsToLine(g, cells);
            std::vector<float> rh;
            for (int k : cells) rh.push_back(g.height[static_cast<std::size_t>(k)]);
            std::vector<char> keep(raw.size(), 0);
            // Simplify each stretch on the ground on its own; a span's ends stay.
            std::size_t run0 = 0;
            for (std::size_t q = 1; q <= raw.size(); ++q) {
                if (q < raw.size() && !route.deck[q]) continue;
                const std::size_t run1 = q - 1;
                keep[run0] = keep[run1] = 1;
                if (run1 > run0 + 1) simplify3Into(raw, rh, run0, run1, 0.5f * g.cell, keep);
                run0 = q;
            }
            for (std::size_t q = 0; q < raw.size(); ++q)
                if (keep[q]) {
                    if (!route.deck[q] && !line.empty() && glm::length(raw[q] - line.back()) < 0.6f * g.cell) continue;
                    add(raw[q], route.deck[q] != 0);
                }
        }
        add(leadB, false);
        add(endB, false);
        // Control points every 30..45 m: dense enough that the spline cannot
        // swing out into the river or up the bank between two of them, sparse
        // enough that the road's own smoothing still has a say. A span is one
        // stretch between two points, as RoadSystem's BridgeSpec wants it.
        std::vector<glm::vec2> pts{line.front()};
        for (std::size_t q = 1; q < line.size(); ++q) {
            const glm::vec2 a0 = pts.back(), b0 = line[q];
            if (deck[q]) {
                road.bridges.push_back({static_cast<int>(pts.size()) - 1, static_cast<int>(pts.size())});
                pts.push_back(b0);
                continue;
            }
            const float seg = glm::length(b0 - a0);
            const int parts = std::max(1, static_cast<int>(std::ceil(seg / 45.0f)));
            for (int s = 1; s <= parts; ++s) pts.push_back(glm::mix(a0, b0, static_cast<float>(s) / parts));
        }
        road.points = std::move(pts);
        bridgeCrossings(road, out);
        road.length   = polyLength(road.points);
        road.maxGrade = steepest(g, road);
        const std::string na = a >= 0 ? out.towns[static_cast<std::size_t>(a)].name : std::string();
        const std::string nb = b >= 0 ? out.towns[static_cast<std::size_t>(b)].name : std::string();
        road.name = (!na.empty() && !nb.empty()) ? na + " - " + nb
                  : !na.empty() ? na + " - Land" : !nb.empty() ? nb + " - Land"
                  : "Landstrasse " + std::to_string(++roadNo);
        claimAlong(g, road.points, 0.5f * road.width + 5.0f, use::Road);
        for (const glm::vec2& q : road.points) {
            int ri, rj;
            cellOf(g, q, ri, rj);
            const int rr = std::max(1, static_cast<int>(std::ceil((road.width + 20.0f) / g.cell)));
            for (int dj = -rr; dj <= rr; ++dj)
                for (int di = -rr; di <= rr; ++di) {
                    const int ni = ri + di, nj = rj + dj;
                    if (ni >= 0 && nj >= 0 && ni < g.n && nj < g.n)
                        router.nearRoad[static_cast<std::size_t>(g.index(ni, nj))] = 1;
                }
        }
        out.roads.push_back(std::move(road));
        (void)rng;
    }
}

// A place no road reaches is left out rather than built as an island of its own
// making -- a village across a fjord the router found no bridge for. Its ground
// goes back to the land, and the roads' town indices follow the ones that stay.
void dropUnreached(Plan& out, const Params& p) {
    if (!p.stages.roads || out.towns.size() < 2) return;
    std::vector<char> reached(out.towns.size(), 0);
    for (const Road& rd : out.roads) {
        if (rd.from >= 0) reached[static_cast<std::size_t>(rd.from)] = 1;
        if (rd.to >= 0)   reached[static_cast<std::size_t>(rd.to)] = 1;
    }
    std::vector<int> remap(out.towns.size(), -1);
    std::vector<Town> kept;
    for (std::size_t i = 0; i < out.towns.size(); ++i) {
        if (reached[i]) {
            remap[i] = static_cast<int>(kept.size());
            kept.push_back(std::move(out.towns[i]));
            continue;
        }
        const Town& t = out.towns[i];
        const float a = glm::radians(t.rotation);
        const glm::vec2 ax(std::cos(a), std::sin(a)), az(-std::sin(a), std::cos(a));
        Grid& g = out.grid;
        for (int k = 0; k < g.n * g.n; ++k) {
            if (g.use[static_cast<std::size_t>(k)] != use::Town) continue;
            const glm::vec2 d = g.at(k % g.n, k / g.n) - t.center;
            if (std::fabs(glm::dot(d, ax)) <= 0.5f * t.sizeX + 40.0f &&
                std::fabs(glm::dot(d, az)) <= 0.5f * t.sizeZ + 40.0f)
                g.use[static_cast<std::size_t>(k)] = use::Free;
        }
        ++out.report.dropped;
    }
    out.towns = std::move(kept);
    for (Road& rd : out.roads) {
        if (rd.from >= 0) rd.from = remap[static_cast<std::size_t>(rd.from)];
        if (rd.to >= 0)   rd.to   = remap[static_cast<std::size_t>(rd.to)];
    }
    out.report.unreached = 0;
}

// --- Stage 5: farmsteads along the country roads ---------------------------------

void planFarms(Plan& out, const Params& p, const Character& c, const GroundFn& groundAt) {
    if (!p.stages.farms || c.farms <= 0 || out.roads.empty()) return;
    Grid& g = out.grid;
    struct Cand { float score; glm::vec2 at; float yaw; };
    std::vector<Cand> cands;
    for (std::size_t ri = 0; ri < out.roads.size(); ++ri) {
        const Road& r = out.roads[ri];
        const float total = polyLength(r.points);
        for (float s = 60.0f; s < total - 60.0f; s += 45.0f) {
            const glm::vec2 a = pointAt(r.points, s), b = pointAt(r.points, s + 2.0f);
            glm::vec2 dir = b - a;
            if (glm::dot(dir, dir) < 1e-6f) continue;
            dir = glm::normalize(dir);
            const glm::vec2 side(-dir.y, dir.x);
            for (int sgn = -1; sgn <= 1; sgn += 2) {
                const std::uint32_t h = mix(p.seed ^ kPhaseFarms ^ mix(static_cast<std::uint32_t>(ri) * 977u +
                                                                        static_cast<std::uint32_t>(s)));
                const float off = 0.5f * r.width + 24.0f + 10.0f * hash01(h, 1u);
                const glm::vec2 at = a + side * (static_cast<float>(sgn) * off);
                bool ok = true;
                for (int q = 0; q < 9 && ok; ++q) {
                    const glm::vec2 pp = at + glm::vec2(static_cast<float>(q % 3 - 1), static_cast<float>(q / 3 - 1)) * 14.0f;
                    const use::Kind u = useAt(g, pp);
                    ok = (u == use::Free || u == use::Crag) && slopeAt(g, pp) < 0.14f;
                }
                if (!ok) continue;
                if (out.sea && g.heightAt(at) < out.seaLevel + 2.0f) continue;
                if (out.treeLine > 0.0f && g.heightAt(at) > out.treeLine - 20.0f) continue;
                // The house turns its street side (local -Z) to the road.
                const glm::vec2 toRoad = -side * static_cast<float>(sgn);
                const float yaw = glm::degrees(std::atan2(-toRoad.x, -toRoad.y));
                cands.push_back({hash01(h, 2u) - slopeAt(g, at) * 4.0f, at, yaw});
            }
        }
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.score > b.score; });
    static const housegen::Preset alts[] = {housegen::Preset::Classic, housegen::Preset::Cottage,
                                            housegen::Preset::LargeFamily};
    for (const Cand& cd : cands) {
        if (static_cast<int>(out.farms.size()) >= c.farms) break;
        bool crowded = false;
        for (const Farm& f : out.farms) crowded = crowded || glm::length(f.pos - cd.at) < 240.0f;
        for (const Town& t : out.towns) crowded = crowded || glm::length(t.center - cd.at) < t.radius() + 150.0f;
        if (crowded) continue;
        Farm f;
        f.pos    = cd.at;
        f.yaw    = cd.yaw;
        f.ground = groundAt(out.terrain, cd.at.x, cd.at.y);
        f.seed   = mix(p.seed ^ kPhaseFarms ^ static_cast<std::uint32_t>(out.farms.size() * 131u)) | 1u;
        const float roll = hash01(f.seed, 3u);
        f.preset = roll < 0.6f ? c.farmHouse : alts[static_cast<int>(roll * 10.0f) % 3];
        out.farms.push_back(f);
        claimAlong(g, {f.pos}, 26.0f, use::Farm);
    }
}

// --- Stage 6: rock where the land is steep or high -----------------------------------

void planRocks(Plan& out, const Params& p, const Character& c, const GroundFn& groundAt) {
    Grid& g = out.grid;
    for (std::size_t k = 0; k < g.use.size(); ++k)
        if (g.use[k] == use::Free && g.slope[k] > c.rockSlope) g.use[k] = use::Crag;
    if (!p.stages.rocks || c.rocks <= 0.0f) return;

    // Where rock lies: on the crags and in the scree at their foot, and out on
    // the open ground above the tree line -- nowhere else. Not a chance per
    // square metre of map: a few percent of a meadow is still hundreds of
    // boulders on a map this size, and a landscape strewn with them evenly reads
    // as debris. A land with no crag has no rocks.
    std::vector<int> rocky;
    for (int k = 0; k < g.n * g.n; ++k) {
        const std::uint8_t u = g.use[static_cast<std::size_t>(k)];
        if (u != use::Free && u != use::Crag) continue;
        const float sl = g.slope[static_cast<std::size_t>(k)];
        const bool crag = sl > c.rockSlope;
        const bool high = out.treeLine > 0.0f && g.height[static_cast<std::size_t>(k)] > out.treeLine &&
                          sl > 0.35f * c.rockSlope;
        if (crag || high) rocky.push_back(k);
    }
    if (rocky.empty()) return;

    // A budget all the same: every rock is an object of its own in the scene (an
    // editable procedural boulder). And no more than the crag can carry -- a
    // cluster for every few rocky cells.
    const int budget = std::min(std::clamp(static_cast<int>(std::lround(c.rocks * 110.0f)), 0, 140),
                                static_cast<int>(rocky.size()) / 2);
    Rng rng(p.seed ^ kPhaseRocks);
    for (std::size_t i = rocky.size(); i > 1; --i)
        std::swap(rocky[i - 1], rocky[static_cast<std::size_t>(rng.below(static_cast<int>(i)))]);

    auto clear = [&](glm::vec2 at, float size) {
        const use::Kind u = useAt(g, at);
        if (u != use::Free && u != use::Crag) return false;
        if (out.sea && g.heightAt(at) < out.seaLevel + 0.5f) return false;
        for (const Town& t : out.towns)
            if (glm::length(at - t.center) < t.radius() + 60.0f) return false;
        for (const Farm& f : out.farms)
            if (glm::length(at - f.pos) < 40.0f) return false;
        for (const Road& rd : out.roads)
            if (lineDist(at, rd.points) < 0.5f * rd.width + 8.0f) return false;
        for (const Rock& r : out.rocks)
            if (glm::length(glm::vec2(r.pos.x, r.pos.z) - at) < 2.5f + 0.5f * (r.size + size)) return false;
        return true;
    };

    std::vector<glm::vec2> centres;
    for (std::size_t q = 0; q < rocky.size() && static_cast<int>(out.rocks.size()) < budget; ++q) {
        const int k = rocky[q];
        const int i = k % g.n, j = k / g.n;
        const glm::vec2 c0 = g.at(i, j);
        bool near = false;
        for (const glm::vec2& o : centres) near = near || glm::length(o - c0) < 45.0f;
        if (near) continue;
        // Downhill: scree comes to rest at the foot of what it broke off.
        const int il = std::max(i - 1, 0), ir = std::min(i + 1, g.n - 1);
        const int jd = std::max(j - 1, 0), ju = std::min(j + 1, g.n - 1);
        glm::vec2 down(g.height[static_cast<std::size_t>(g.index(il, j))] - g.height[static_cast<std::size_t>(g.index(ir, j))],
                       g.height[static_cast<std::size_t>(g.index(i, jd))] - g.height[static_cast<std::size_t>(g.index(i, ju))]);
        down = glm::dot(down, down) > 1e-6f ? glm::normalize(down) : glm::vec2(1.0f, 0.0f);
        const glm::vec2 side(-down.y, down.x);
        const bool crag = g.slope[static_cast<std::size_t>(k)] > c.rockSlope;
        const int count = 1 + rng.below(4);
        int placed = 0;
        for (int m = 0; m < count * 3 && placed < count && static_cast<int>(out.rocks.size()) < budget; ++m) {
            const float size = placed == 0 ? rng.uni(2.0f, 4.0f) * (crag ? 1.3f : 1.0f) : rng.uni(1.0f, 2.6f);
            const glm::vec2 at = c0 + down * rng.uni(0.0f, crag ? 1.2f * g.cell : 0.5f * g.cell) +
                                 side * rng.uni(-8.0f, 8.0f);
            if (!clear(at, size)) continue;
            Rock r;
            r.size = size;
            r.yaw  = rng.uni(0.0f, 360.0f);
            r.seed = mix(p.seed ^ kPhaseRocks ^ static_cast<std::uint32_t>(q * 16 + static_cast<std::size_t>(m))) | 1u;
            // Half buried: a boulder resting on the ground reads as dropped there.
            r.pos  = glm::vec3(at.x, groundAt(out.terrain, at.x, at.y) - 0.3f * r.size, at.y);
            out.rocks.push_back(r);
            ++placed;
        }
        if (placed > 0) centres.push_back(c0);
    }
}

} // namespace

// --- Public -----------------------------------------------------------------------------

const char* regionName(Region r) {
    switch (r) {
    case Region::Uplands: return "Mittelgebirge";
    case Region::Alpine:  return "Alpental";
    case Region::Lowland: return "Flussniederung";
    case Region::Coast:   return "Küste";
    case Region::Fjord:   return "Fjordland";
    case Region::Canyon:  return "Canyon";
    case Region::Island:  return "Insel";
    case Region::Count:   break;
    }
    return "?";
}

const char* regionBlurb(Region r) {
    switch (r) {
    case Region::Uplands: return "Wooded hills, a river in the valley, a small town and villages.";
    case Region::Alpine:  return "A valley under high mountains: a mountain stream, villages, hairpins, scree.";
    case Region::Lowland: return "Flat farmland and a wide meandering river; towns, villages, many farms.";
    case Region::Coast:   return "A coastal plain into the sea; a town near the shore, cliffs and rock.";
    case Region::Fjord:   return "Steep land falling into sea inlets; few villages, bare rock above the trees.";
    case Region::Canyon:  return "Dry plateaus cut by a river canyon; sparse villages, a lot of rock.";
    case Region::Island:  return "An island in open sea: a village or two, a brook, a road round it.";
    case Region::Count:   break;
    }
    return "";
}

namespace {
// A place from sunpos::cities() by name, on the 21st of June.
sunpos::Place cityPlace(const char* name) {
    sunpos::Place p;
    for (const sunpos::City& c : sunpos::cities())
        if (std::string(c.name) == name) {
            p.latitude = c.latitude; p.longitude = c.longitude;
            p.utcOffset = c.utcOffset; p.summerTime = c.summerTime;
        }
    p.day = 172;
    return p;
}
} // namespace

Character character(Region r) {
    Character c;
    // The land of each, then everything else. `land(...)` takes the terrain
    // generator's knobs in the order the terrain panel lists them.
    auto land = [&c](float hs, float cont, float bf, float f, float ridge, float peak,
                     float valley, float terrace, float warp, float relief, int oct) {
        c.heightScale = hs; c.continentAmp = cont; c.biomeFreq = bf; c.frequency = f;
        c.ridgeScale = ridge; c.peakSharpness = peak; c.valleyDepth = valley;
        c.terrace = terrace; c.warpStrength = warp; c.warpFrequency = 0.0012f;
        c.reliefGain = relief; c.octaves = oct;
    };
    switch (r) {
    case Region::Uplands:
        // No valley channels: the terrain's are a ridged-noise network whose
        // depth wanders along its own lines, and every dip in it is a hollow the
        // river would have to cut its way out of. The hills make valleys enough.
        land(70.0f, 1.6f, 0.00035f, 0.0016f, 60.0f, 0.9f, 0.0f, 0.0f, 120.0f, 1.0f, 6);
        c.rivers = 1; c.brooks = 3; c.river = rivergen::Preset::River; c.brook = rivergen::Preset::Brook;
        c.towns = 1; c.villages = 3; c.town = cityplan::Preset::SmallTown; c.village = cityplan::Preset::Village;
        c.farms = 8; c.farmHouse = housegen::Preset::CountryHouse; c.siteSlope = 0.08f;
        c.roadWidth = 7.0f; c.maxGrade = 0.08f; c.extraRoads = 0.35f; c.exits = 2;
        c.forest = 0.6f; c.treeLine = 0.0f; c.rocks = 0.2f; c.rockSlope = 0.6f;
        // Slate country: slate on the crags, slate on the roofs.
        c.facade = {0.92f, 0.90f, 0.85f}; c.roof = {0.22f, 0.23f, 0.25f}; c.rockType = 4;
        c.place = cityPlace("Kassel");
        break;
    case Region::Alpine:
        land(110.0f, 1.5f, 0.0003f, 0.0018f, 220.0f, 1.6f, 0.0f, 0.0f, 150.0f, 1.2f, 7);
        c.backdrop = 1200.0f;
        c.rivers = 1; c.brooks = 3; c.river = rivergen::Preset::MountainStream; c.brook = rivergen::Preset::Brook;
        c.towns = 0; c.villages = 3; c.village = cityplan::Preset::Village;
        c.farms = 5; c.farmHouse = housegen::Preset::CountryHouse; c.siteSlope = 0.14f;
        c.villageScale = 0.85f;
        c.roadWidth = 6.0f; c.maxGrade = 0.11f; c.extraRoads = 0.2f; c.exits = 1;
        c.forest = 0.75f; c.treeLine = 260.0f; c.rocks = 0.8f; c.rockSlope = 0.45f;
        c.facade = {0.93f, 0.91f, 0.86f}; c.roof = {0.30f, 0.27f, 0.24f}; c.rockType = 2;
        c.trees = {{"Spruce", 0.6f}, {"Scots pine", 0.25f}, {"Birch", 0.15f}};
        c.place = cityPlace("Innsbruck");
        break;
    case Region::Lowland:
        land(25.0f, 1.0f, 0.0004f, 0.0012f, 4.0f, 0.7f, 8.0f, 0.0f, 100.0f, 1.0f, 5);
        c.rivers = 1; c.brooks = 3; c.river = rivergen::Preset::WideRiver; c.brook = rivergen::Preset::Creek;
        c.towns = 1; c.villages = 4; c.town = cityplan::Preset::SmallTown; c.village = cityplan::Preset::Village;
        c.farms = 12; c.farmHouse = housegen::Preset::Classic; c.siteSlope = 0.05f;
        c.roadWidth = 7.5f; c.maxGrade = 0.06f; c.extraRoads = 0.5f; c.exits = 3;
        c.forest = 0.3f; c.treeLine = 0.0f; c.rocks = 0.0f; c.rockSlope = 0.8f;
        // Brick and clay: the plain has no stone of its own but what the ice left.
        c.facade = {0.60f, 0.30f, 0.22f}; c.roof = {0.55f, 0.22f, 0.14f}; c.rockType = 0;
        c.trees = {{"Oak", 0.35f}, {"Birch", 0.25f}, {"Lombardy poplar", 0.2f}, {"Maple", 0.2f}};
        c.place = cityPlace("Münster");
        break;
    case Region::Coast:
        land(60.0f, 2.4f, 0.0004f, 0.0016f, 30.0f, 1.0f, 12.0f, 0.0f, 150.0f, 1.0f, 6);
        c.seaShare = 0.3f;
        c.rivers = 1; c.brooks = 2; c.river = rivergen::Preset::River; c.brook = rivergen::Preset::Brook;
        c.towns = 1; c.villages = 2; c.town = cityplan::Preset::SmallTown; c.village = cityplan::Preset::Village;
        c.farms = 6; c.farmHouse = housegen::Preset::Cottage; c.siteSlope = 0.08f;
        c.roadWidth = 7.0f; c.maxGrade = 0.08f; c.extraRoads = 0.3f; c.exits = 2;
        c.forest = 0.35f; c.treeLine = 0.0f; c.rocks = 0.35f; c.rockSlope = 0.5f;
        c.facade = {0.95f, 0.95f, 0.93f}; c.roof = {0.58f, 0.24f, 0.15f}; c.rockType = 0;
        c.trees = {{"Scots pine", 0.5f}, {"Birch", 0.3f}, {"Shrub", 0.2f}};
        c.place = cityPlace("Kiel");
        break;
    case Region::Fjord:
        land(120.0f, 2.0f, 0.00035f, 0.0016f, 200.0f, 1.5f, 80.0f, 0.0f, 150.0f, 1.2f, 7);
        c.seaShare = 0.25f;
        c.rivers = 1; c.brooks = 2; c.river = rivergen::Preset::MountainStream; c.brook = rivergen::Preset::Brook;
        c.towns = 0; c.villages = 2; c.village = cityplan::Preset::Village;
        c.farms = 3; c.farmHouse = housegen::Preset::Cottage; c.siteSlope = 0.16f;
        c.villageScale = 0.65f;
        c.roadWidth = 6.0f; c.maxGrade = 0.11f; c.extraRoads = 0.0f; c.exits = 1;
        c.forest = 0.6f; c.treeLine = 220.0f; c.rocks = 0.9f; c.rockSlope = 0.42f;
        // Falu red timber under dark roofs, on granite.
        c.facade = {0.52f, 0.14f, 0.10f}; c.roof = {0.18f, 0.18f, 0.19f}; c.rockType = 0;
        c.trees = {{"Spruce", 0.5f}, {"Birch", 0.4f}, {"Shrub", 0.1f}};
        c.place = cityPlace("Bergen");
        break;
    case Region::Canyon:
        // Terraces at half strength: the terrain steps them every five metres,
        // and at full strength each step is a short cliff that a road crossing
        // the mesa has to climb in fifteen metres.
        land(60.0f, 1.0f, 0.0004f, 0.0015f, 25.0f, 1.0f, 90.0f, 0.5f, 120.0f, 1.0f, 6);
        c.rivers = 1; c.brooks = 0; c.river = rivergen::Preset::River; c.brook = rivergen::Preset::Creek;
        c.towns = 0; c.villages = 2; c.village = cityplan::Preset::Village;
        c.farms = 2; c.farmHouse = housegen::Preset::Bungalow; c.siteSlope = 0.09f;
        c.roadWidth = 7.0f; c.maxGrade = 0.09f; c.extraRoads = 0.0f; c.exits = 2;
        c.forest = 0.08f; c.treeLine = 0.0f; c.rocks = 1.0f; c.rockSlope = 0.38f;
        c.facade = {0.80f, 0.60f, 0.44f}; c.roof = {0.66f, 0.34f, 0.20f}; c.rockType = 1;
        c.trees = {{"Shrub", 0.6f}, {"Scots pine", 0.4f}};
        c.place = cityPlace("Grand Canyon");
        break;
    case Region::Island:
        land(70.0f, 0.6f, 0.0005f, 0.0018f, 90.0f, 1.3f, 15.0f, 0.0f, 120.0f, 1.0f, 6);
        c.island = 0.8f;
        c.seaShare = 0.5f;
        c.rivers = 1; c.brooks = 1; c.river = rivergen::Preset::Creek; c.brook = rivergen::Preset::Brook;
        c.towns = 0; c.villages = 2; c.village = cityplan::Preset::Village;
        c.farms = 4; c.farmHouse = housegen::Preset::Cottage; c.siteSlope = 0.1f;
        c.roadWidth = 6.0f; c.maxGrade = 0.09f; c.extraRoads = 0.5f; c.exits = 0;
        c.forest = 0.45f; c.treeLine = 0.0f; c.rocks = 0.45f; c.rockSlope = 0.48f;
        c.facade = {0.95f, 0.94f, 0.90f}; c.roof = {0.62f, 0.30f, 0.18f}; c.rockType = 3;
        c.trees = {{"Scots pine", 0.4f}, {"Maple", 0.3f}, {"Shrub", 0.3f}};
        c.place = cityPlace("Palma");
        break;
    case Region::Count:
        break;
    }
    return c;
}

float Grid::heightAt(glm::vec2 p) const {
    if (n <= 0 || height.empty()) return 0.0f;
    const float fx = glm::clamp((p.x - origin.x) / cell, 0.0f, static_cast<float>(n - 1));
    const float fz = glm::clamp((p.y - origin.y) / cell, 0.0f, static_cast<float>(n - 1));
    const int i0 = std::min(static_cast<int>(fx), n - 2), j0 = std::min(static_cast<int>(fz), n - 2);
    const float tx = fx - static_cast<float>(i0), tz = fz - static_cast<float>(j0);
    auto h = [&](int i, int j) { return height[static_cast<std::size_t>(j * n + i)]; };
    return glm::mix(glm::mix(h(i0, j0), h(i0 + 1, j0), tx),
                    glm::mix(h(i0, j0 + 1), h(i0 + 1, j0 + 1), tx), tz);
}

bool Horizon::covers(glm::vec2 p) const {
    const float span = cell * static_cast<float>(n - 1);
    return n > 1 && p.x >= origin.x && p.y >= origin.y && p.x <= origin.x + span && p.y <= origin.y + span;
}

float Horizon::heightAt(glm::vec2 p) const {
    if (n <= 1 || height.empty()) return 0.0f;
    const float fx = glm::clamp((p.x - origin.x) / cell, 0.0f, static_cast<float>(n - 1));
    const float fz = glm::clamp((p.y - origin.y) / cell, 0.0f, static_cast<float>(n - 1));
    const int i0 = std::min(static_cast<int>(fx), n - 2), j0 = std::min(static_cast<int>(fz), n - 2);
    const float tx = fx - static_cast<float>(i0), tz = fz - static_cast<float>(j0);
    auto at = [&](int i, int j) { return height[static_cast<std::size_t>(j * n + i)]; };
    return glm::mix(glm::mix(at(i0, j0), at(i0 + 1, j0), tx), glm::mix(at(i0, j0 + 1), at(i0 + 1, j0 + 1), tx), tz);
}

std::vector<std::uint8_t> shade(const Plan& pl, glm::vec3 sunDir) {
    const Grid& g = pl.grid;
    std::vector<std::uint8_t> out(g.height.size(), 1);
    const float flat = std::sqrt(sunDir.x * sunDir.x + sunDir.z * sunDir.z);
    if (g.n <= 0 || sunDir.y <= 0.002f) return out;
    if (flat < 1e-4f) { std::fill(out.begin(), out.end(), 0); return out; }
    const glm::vec2 toward(sunDir.x / flat, sunDir.z / flat);
    const float rise = sunDir.y / flat;              // metres up per metre towards the sun
    const float top  = std::max(pl.horizon.top, pl.report.highest);
    const float span = g.cell * static_cast<float>(g.n - 1);
    const int workers = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 16);
    std::vector<std::future<void>> jobs;
    for (int w = 0; w < workers; ++w)
        jobs.push_back(std::async(std::launch::async, [&, w] {
            for (int j = w; j < g.n; j += workers)
                for (int i = 0; i < g.n; ++i) {
                    const glm::vec2 p0 = g.at(i, j);
                    // A little above the ground: a cell must not shade itself.
                    const float h0 = g.height[static_cast<std::size_t>(g.index(i, j))] + 2.0f;
                    bool dark = false;
                    for (float d = 0.6f * g.cell;; d += (d < 400.0f ? 0.6f * g.cell : 40.0f)) {
                        const float ray = h0 + rise * d;
                        if (ray > top) break;
                        const glm::vec2 q = p0 + toward * d;
                        const bool inside = q.x >= g.origin.x && q.y >= g.origin.y &&
                                            q.x <= g.origin.x + span && q.y <= g.origin.y + span;
                        if (!inside && !pl.horizon.covers(q)) break;
                        const float ground = inside ? g.heightAt(q) : pl.horizon.heightAt(q);
                        if (ground > ray) { dark = true; break; }
                    }
                    out[static_cast<std::size_t>(g.index(i, j))] = dark ? 1 : 0;
                }
        }));
    for (auto& f : jobs) f.get();
    return out;
}

fitzel::TerrainSettings terrainFor(const Params& p, const fitzel::TerrainSettings& current) {
    if (!p.stages.terrain) return current;
    const Character& c = p.look;
    fitzel::TerrainSettings s = current;
    s.heightScale = c.heightScale;  s.ridgeScale = c.ridgeScale;
    s.continentAmp = c.continentAmp; s.biomeFreq = c.biomeFreq;
    s.terrace = c.terrace;          s.warpStrength = c.warpStrength;
    s.frequency = c.frequency;      s.valleyDepth = c.valleyDepth;
    s.warpFrequency = c.warpFrequency;
    s.peakSharpness = c.peakSharpness; s.reliefGain = c.reliefGain;
    s.octaves = c.octaves;
    // A noise-coordinate OFFSET, not RNG state. Held under 256 -- the period of
    // the Perlin lattice, so nothing above it is a new landscape -- because it is
    // added to coordinates that at this scale are a fraction of a unit per
    // hundred metres: at an offset of 700 000 (what levelgen's thousands-step
    // gives) a float resolves 1/16 of a unit, which is forty metres of ground,
    // and the land comes out in terraces with a grid on them.
    s.seed = static_cast<float>(mix(p.seed ^ kPhaseLand) % 25600u) * 0.01f;
    s.islandRadius = c.island > 0.0f ? c.island * 0.5f * p.size : 0.0f;
    s.islandCenterX = p.center.x; s.islandCenterZ = p.center.y;
    s.islandShape = 0.0f;
    s.backdropHeight = c.backdrop;
    s.backdropCenterX = p.center.x; s.backdropCenterZ = p.center.y;
    s.backdropRadius = 0.5f * p.size;
    s.backdropWidth = 0.8f * p.size;
    s.backdropScale = std::max(1500.0f, p.size);
    s.backdropStretch = 1.0f;
    s.backdropAngle = static_cast<float>(mix(p.seed) % 360u);
    s.backdropOutlet = c.backdrop > 0.0f ? 0.6f * p.size : 0.0f;
    s.backdropLake = 0.0f;
    return fitzel::sanitized(s);
}

Plan plan(const Params& p, const fitzel::TerrainSettings& current, float currentWater,
          const GroundFn& groundAt) {
    Plan out;
    const Character& c = p.look;
    out.terrain = terrainFor(p, current);

    Grid& g = out.grid;
    const float size = glm::clamp(p.size, 500.0f, 12000.0f);
    g.cell   = glm::clamp(size / 200.0f, 8.0f, 32.0f);
    g.n      = static_cast<int>(std::ceil(size / g.cell)) + 1;
    g.origin = p.center - glm::vec2(0.5f * size);
    sampleGround(g, out.terrain, groundAt);
    g.flow.assign(g.height.size(), 0.0f);
    sampleHorizon(out.horizon, g, size, out.terrain, groundAt);

    auto [lo, hi] = std::minmax_element(g.height.begin(), g.height.end());
    out.report.lowest  = *lo;
    out.report.highest = *hi;

    // --- The sea, as a share of the map ---------------------------------------
    if (p.stages.terrain) {
        if (c.seaShare > 0.0f) {
            std::vector<float> sorted(g.height);
            const std::size_t at = std::min(sorted.size() - 1,
                static_cast<std::size_t>(glm::clamp(c.seaShare, 0.0f, 0.9f) * static_cast<float>(sorted.size())));
            std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(at), sorted.end());
            out.sea = true;
            out.seaLevel = sorted[at];
            // An island's mask lays the sea floor flat at -30 m, so a quantile of
            // the whole map lands on that floor and drowns nothing. The sea is set
            // from the island's own heart instead: all but its lowest eighth dry,
            // whatever this seed's noise does inside the mask.
            if (c.island > 0.0f) {
                const float r = 0.75f * c.island * 0.5f * p.size;
                std::vector<float> inner;
                for (int k = 0; k < g.n * g.n; ++k)
                    if (glm::length(g.at(k % g.n, k / g.n) - p.center) < r)
                        inner.push_back(g.height[static_cast<std::size_t>(k)]);
                if (!inner.empty()) {
                    const std::size_t q = inner.size() / 8;
                    std::nth_element(inner.begin(), inner.begin() + static_cast<std::ptrdiff_t>(q), inner.end());
                    out.seaLevel = glm::clamp(inner[q], -26.0f, 0.0f);
                }
            }
        } else {
            out.sea = false;
            out.seaLevel = -1000.0f;
        }
    } else {
        out.sea = currentWater > -500.0f;
        out.seaLevel = currentWater;
    }
    if (out.sea)
        for (std::size_t k = 0; k < g.height.size(); ++k)
            if (g.height[k] < out.seaLevel) g.use[k] = use::Sea;
    out.treeLine = c.treeLine > 0.0f ? out.report.lowest + c.treeLine : 0.0f;
    if (out.sea && c.treeLine > 0.0f) out.treeLine = out.seaLevel + c.treeLine;

    planRivers(out, p, c, groundAt);
    planTowns(out, p, c);
    planRoads(out, p, c);
    dropUnreached(out, p);
    planFarms(out, p, c, groundAt);
    planRocks(out, p, c, groundAt);

    // --- What came out -----------------------------------------------------------
    Report& r = out.report;
    r.seaLevel = out.seaLevel;
    for (const River& rv : out.rivers) {
        (rv.brook ? r.brooks : r.rivers)++;
        r.riverKm += rv.length / 1000.0f;
    }
    for (const Town& t : out.towns) (t.village ? r.villages : r.towns)++;
    for (const Road& rd : out.roads) {
        ++r.roads;
        r.bridges += static_cast<int>(rd.bridges.size());
        r.roadKm += rd.length / 1000.0f;
        r.steepestRoad = std::max(r.steepestRoad, rd.maxGrade);
    }
    r.farms = static_cast<int>(out.farms.size());
    r.rocks = static_cast<int>(out.rocks.size());
    // Every town must be reachable: a town no road leads to is a town on an island
    // of its own making.
    if (!out.towns.empty() && p.stages.roads) {
        std::vector<char> reached(out.towns.size(), 0);
        for (const Road& rd : out.roads) {
            if (rd.from >= 0) reached[static_cast<std::size_t>(rd.from)] = 1;
            if (rd.to >= 0)   reached[static_cast<std::size_t>(rd.to)] = 1;
        }
        r.unreached = out.towns.size() > 1
            ? static_cast<int>(std::count(reached.begin(), reached.end(), 0)) : 0;
    }
    if (p.stages.rivers && c.rivers > 0 && r.rivers == 0 && r.why.empty())
        r.why = "This land drains nowhere big enough for a river -- more relief, or a larger map.";
    if (r.unreached > 0) {
        r.ok = false;
        if (r.why.empty()) r.why = std::to_string(r.unreached) + " place(s) no road could reach.";
    }
    if (r.dropped > 0 && r.why.empty())
        r.why = std::to_string(r.dropped) + " place(s) left out: no road could reach the site.";
    if (r.steepestRoad > 1.8f * c.maxGrade && r.why.empty())
        r.why = "A road climbs steeper than its limit somewhere -- the land leaves it no gentler way.";
    return out;
}

float steepest(const Grid& g, const std::vector<glm::vec2>& line, float window) {
    if (line.size() < 2) return 0.0f;
    std::vector<float> h;
    const float step = 5.0f;
    for (std::size_t i = 1; i < line.size(); ++i) {
        const glm::vec2 a = line[i - 1], b = line[i];
        const float seg = glm::length(b - a);
        for (float t = 0.0f; t < seg; t += step) h.push_back(g.heightAt(a + (b - a) * (t / std::max(seg, 1e-6f))));
    }
    h.push_back(g.heightAt(line.back()));
    const int w = std::max(1, static_cast<int>(std::lround(window / step)));
    float worst = 0.0f;
    for (std::size_t i = 0; i + static_cast<std::size_t>(w) < h.size(); ++i)
        worst = std::max(worst, std::fabs(h[i + static_cast<std::size_t>(w)] - h[i]) / (static_cast<float>(w) * step));
    return worst;
}

float steepest(const Grid& g, const Road& road, float window, glm::vec2* where) {
    const std::vector<glm::vec2>& pts = road.points;
    if (pts.size() < 2) return 0.0f;
    std::vector<float> h;
    std::vector<glm::vec2> at;
    const float step = 5.0f;
    for (std::size_t i = 1; i < pts.size(); ++i) {
        const glm::vec2 a = pts[i - 1], b = pts[i];
        bool deck = false;
        for (const Span& sp : road.bridges)
            deck = deck || (static_cast<int>(i) > sp.a && static_cast<int>(i) <= sp.b);
        const float seg = glm::length(b - a);
        const float ha = g.heightAt(a), hb = g.heightAt(b);
        for (float t = 0.0f; t < seg; t += step) {
            const float u = t / std::max(seg, 1e-6f);
            h.push_back(deck ? glm::mix(ha, hb, u) : g.heightAt(a + (b - a) * u));
            at.push_back(a + (b - a) * u);
        }
    }
    h.push_back(g.heightAt(pts.back()));
    at.push_back(pts.back());
    const int w = std::max(1, static_cast<int>(std::lround(window / step)));
    float worst = 0.0f;
    for (std::size_t i = 0; i + static_cast<std::size_t>(w) < h.size(); ++i) {
        const float gr = std::fabs(h[i + static_cast<std::size_t>(w)] - h[i]) / (static_cast<float>(w) * step);
        if (gr > worst) {
            worst = gr;
            if (where) *where = at[i];
        }
    }
    return worst;
}

std::string placeName(unsigned seed, int index) { return nameFrom(seed, index); }

std::vector<unsigned char> image(const Plan& pl, int px, int& width,
                                 const std::vector<std::uint8_t>* shadeMap) {
    const Grid& g = pl.grid;
    px = std::max(px, 1);
    width = g.n * px;
    std::vector<unsigned char> img(static_cast<std::size_t>(width * width * 4), 255);
    if (g.n <= 0 || g.height.empty()) { width = 0; return {}; }
    const float lo = pl.report.lowest, hi = std::max(pl.report.highest, lo + 1.0f);
    for (int j = 0; j < g.n; ++j)
        for (int i = 0; i < g.n; ++i) {
            const std::size_t k = static_cast<std::size_t>(g.index(i, j));
            const float t = (g.height[k] - lo) / (hi - lo);
            // Lit from the north-west, so the relief reads at a glance.
            const float hl = g.height[static_cast<std::size_t>(g.index(std::max(i - 1, 0), std::max(j - 1, 0)))];
            const float shade = glm::clamp(0.78f + (hl - g.height[k]) / g.cell * 1.4f, 0.4f, 1.25f);
            glm::vec3 c = glm::mix(glm::vec3(0.36f, 0.50f, 0.27f), glm::vec3(0.63f, 0.56f, 0.42f), t);
            if (pl.treeLine > 0.0f && g.height[k] > pl.treeLine) c = glm::vec3(0.60f, 0.58f, 0.55f);
            c *= shade;
            switch (g.use[k]) {
            case use::Sea:  c = glm::mix(glm::vec3(0.12f, 0.24f, 0.42f), glm::vec3(0.20f, 0.36f, 0.56f),
                                         glm::clamp(1.0f + (g.height[k] - pl.seaLevel) / 40.0f, 0.0f, 1.0f)); break;
            case use::Town: c = glm::mix(c, glm::vec3(0.78f, 0.50f, 0.42f), 0.35f); break;
            case use::Crag: c = glm::mix(c, glm::vec3(0.46f, 0.44f, 0.43f), 0.55f); break;
            default: break;
            }
            // In the shade: darker and bluer, as the sky lights it.
            if (shadeMap && k < shadeMap->size() && (*shadeMap)[k] && g.use[k] != use::Sea)
                c = c * glm::vec3(0.50f, 0.54f, 0.66f);
            const unsigned char r = static_cast<unsigned char>(glm::clamp(c.r, 0.0f, 1.0f) * 255.0f);
            const unsigned char gg = static_cast<unsigned char>(glm::clamp(c.g, 0.0f, 1.0f) * 255.0f);
            const unsigned char b = static_cast<unsigned char>(glm::clamp(c.b, 0.0f, 1.0f) * 255.0f);
            for (int y = 0; y < px; ++y)
                for (int x = 0; x < px; ++x) {
                    unsigned char* d = &img[static_cast<std::size_t>(((j * px + y) * width + i * px + x) * 4)];
                    d[0] = r; d[1] = gg; d[2] = b; d[3] = 255;
                }
        }
    return img;
}

namespace {
void putVec(nlohmann::json& j, const char* k, glm::vec3 v) { j[k] = {v.x, v.y, v.z}; }
void getVec(const nlohmann::json& j, const char* k, glm::vec3& v) {
    const auto it = j.find(k);
    if (it != j.end() && it->is_array() && it->size() == 3)
        v = {(*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>()};
}
template <class T> void get(const nlohmann::json& j, const char* k, T& v) {
    const auto it = j.find(k);
    if (it != j.end() && !it->is_null()) v = it->get<T>();
}
} // namespace

void save(nlohmann::json& j, const Params& p) {
    j["seed"] = p.seed;
    j["region"] = static_cast<int>(p.region);
    j["size"] = p.size;
    j["center"] = {p.center.x, p.center.y};
    const Stages& st = p.stages;
    j["stages"] = {{"terrain", st.terrain}, {"rivers", st.rivers}, {"towns", st.towns},
                   {"roads", st.roads}, {"farms", st.farms}, {"forest", st.forest},
                   {"rocks", st.rocks}};
    const Character& c = p.look;
    nlohmann::json& l = j["look"];
    l["heightScale"] = c.heightScale; l["ridgeScale"] = c.ridgeScale; l["continentAmp"] = c.continentAmp;
    l["biomeFreq"] = c.biomeFreq; l["terrace"] = c.terrace; l["warpStrength"] = c.warpStrength;
    l["warpFrequency"] = c.warpFrequency; l["frequency"] = c.frequency; l["valleyDepth"] = c.valleyDepth;
    l["peakSharpness"] = c.peakSharpness; l["reliefGain"] = c.reliefGain; l["octaves"] = c.octaves;
    l["backdrop"] = c.backdrop; l["island"] = c.island;
    l["seaShare"] = c.seaShare; l["rivers"] = c.rivers; l["brooks"] = c.brooks;
    l["river"] = static_cast<int>(c.river); l["brook"] = static_cast<int>(c.brook);
    l["towns"] = c.towns; l["villages"] = c.villages;
    l["town"] = static_cast<int>(c.town); l["village"] = static_cast<int>(c.village);
    l["farms"] = c.farms; l["farmHouse"] = static_cast<int>(c.farmHouse);
    l["siteSlope"] = c.siteSlope; l["villageScale"] = c.villageScale;
    l["roadWidth"] = c.roadWidth; l["maxGrade"] = c.maxGrade; l["extraRoads"] = c.extraRoads; l["exits"] = c.exits;
    l["forest"] = c.forest; l["treeLine"] = c.treeLine; l["rocks"] = c.rocks; l["rockSlope"] = c.rockSlope;
    putVec(l, "facade", c.facade); putVec(l, "roof", c.roof); l["rockType"] = c.rockType;
    nlohmann::json trees = nlohmann::json::array();
    for (const Character::Tree& t : c.trees)
        trees.push_back({{"preset", t.preset}, {"share", t.share}, {"file", t.file}});
    l["trees"] = trees;
    const sunpos::Place& pc = c.place;
    l["place"] = {{"lat", pc.latitude}, {"lon", pc.longitude}, {"utc", pc.utcOffset},
                  {"summer", pc.summerTime}, {"day", pc.day}};
    j["hour"] = p.hour;
}

void load(const nlohmann::json& j, Params& p) {
    get(j, "seed", p.seed);
    int region = static_cast<int>(p.region);
    get(j, "region", region);
    p.region = static_cast<Region>(std::clamp(region, 0, static_cast<int>(Region::Count) - 1));
    p.look = character(p.region);
    get(j, "size", p.size);
    get(j, "hour", p.hour);
    if (const auto it = j.find("center"); it != j.end() && it->is_array() && it->size() == 2)
        p.center = {(*it)[0].get<float>(), (*it)[1].get<float>()};
    if (const auto it = j.find("stages"); it != j.end() && it->is_object()) {
        get(*it, "terrain", p.stages.terrain); get(*it, "rivers", p.stages.rivers);
        get(*it, "towns", p.stages.towns);     get(*it, "roads", p.stages.roads);
        get(*it, "farms", p.stages.farms);     get(*it, "forest", p.stages.forest);
        get(*it, "rocks", p.stages.rocks);
    }
    const auto lt = j.find("look");
    if (lt == j.end() || !lt->is_object()) return;
    const nlohmann::json& l = *lt;
    Character& c = p.look;
    get(l, "heightScale", c.heightScale); get(l, "ridgeScale", c.ridgeScale); get(l, "continentAmp", c.continentAmp);
    get(l, "biomeFreq", c.biomeFreq); get(l, "terrace", c.terrace); get(l, "warpStrength", c.warpStrength);
    get(l, "warpFrequency", c.warpFrequency); get(l, "frequency", c.frequency); get(l, "valleyDepth", c.valleyDepth);
    get(l, "peakSharpness", c.peakSharpness); get(l, "reliefGain", c.reliefGain); get(l, "octaves", c.octaves);
    get(l, "backdrop", c.backdrop); get(l, "island", c.island);
    get(l, "seaShare", c.seaShare); get(l, "rivers", c.rivers); get(l, "brooks", c.brooks);
    auto getEnum = [&](const char* k, auto& e, int count) {
        int v = static_cast<int>(e);
        get(l, k, v);
        e = static_cast<std::remove_reference_t<decltype(e)>>(std::clamp(v, 0, count - 1));
    };
    getEnum("river", c.river, static_cast<int>(rivergen::Preset::Count));
    getEnum("brook", c.brook, static_cast<int>(rivergen::Preset::Count));
    get(l, "towns", c.towns); get(l, "villages", c.villages);
    getEnum("town", c.town, static_cast<int>(cityplan::Preset::Count));
    getEnum("village", c.village, static_cast<int>(cityplan::Preset::Count));
    get(l, "farms", c.farms);
    getEnum("farmHouse", c.farmHouse, static_cast<int>(housegen::Preset::Count));
    get(l, "siteSlope", c.siteSlope); get(l, "villageScale", c.villageScale);
    get(l, "roadWidth", c.roadWidth); get(l, "maxGrade", c.maxGrade); get(l, "extraRoads", c.extraRoads);
    get(l, "exits", c.exits);
    get(l, "forest", c.forest); get(l, "treeLine", c.treeLine); get(l, "rocks", c.rocks); get(l, "rockSlope", c.rockSlope);
    getVec(l, "facade", c.facade); getVec(l, "roof", c.roof); get(l, "rockType", c.rockType);
    if (const auto it = l.find("place"); it != l.end() && it->is_object()) {
        get(*it, "lat", c.place.latitude);  get(*it, "lon", c.place.longitude);
        get(*it, "utc", c.place.utcOffset); get(*it, "summer", c.place.summerTime);
        get(*it, "day", c.place.day);
        c.place.legacy = false;
    }
    if (const auto it = l.find("trees"); it != l.end() && it->is_array()) {
        c.trees.clear();
        for (const nlohmann::json& t : *it)
            if (t.is_object())
                c.trees.push_back({t.value("preset", std::string()), t.value("share", 1.0f),
                                   t.value("file", std::string())});
    }
}

} // namespace worldgen
