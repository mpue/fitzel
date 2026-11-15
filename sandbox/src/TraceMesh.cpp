#include "TraceMesh.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <thread>
#include <unordered_map>

namespace pathtrace {
namespace {

// The seven edges a grid point starts: towards every corner of the cube above
// it, as a bit mask (1 x, 2 y, 4 z) less one. 0..2 stay in the point's layer,
// 3..6 climb to the next.
constexpr int kOffsets = 7;

// Where along an edge a crossing may sit: never on a grid point itself, so two
// crossings on edges from the same point are never one position (a triangle
// of zero area). A hundredth of a cell is far below anything a print shows.
constexpr double kEdgeMin = 0.01;
constexpr int    kBisect  = 14;

void parallelFor(int n, int threads, const std::function<void(int, int)>& body) {
    if (n <= 0) return;
    if (threads <= 1 || n < 64) { body(0, n); return; }
    const int chunk = std::max(16, n / (threads * 8));
    std::atomic<int> next{0};
    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(threads));
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([&] {
            for (;;) {
                const int a = next.fetch_add(chunk);
                if (a >= n) return;
                body(a, std::min(n, a + chunk));
            }
        });
    for (std::thread& th : pool) th.join();
}

struct Grid {
    glm::dvec3 origin{0.0};
    double     cell = 1.0;
    int nx = 0, ny = 0, nz = 0;   // points per axis

    glm::dvec3 at(int i, int j, int k) const {
        return origin + glm::dvec3(i, j, k) * cell;
    }
};

bool gridOf(const Shapes& shapes, int node, double cell, Grid& g) {
    if (node < 0 || node >= static_cast<int>(shapes.nodes.size()) || !(cell > 0.0)) return false;
    const ShapeNode& s = shapes.nodes[static_cast<std::size_t>(node)];
    if (!s.bounded || !(s.hi.x >= s.lo.x && s.hi.y >= s.lo.y && s.hi.z >= s.lo.z)) return false;
    const glm::dvec3 lo(s.lo), hi(s.hi);
    const glm::dvec3 size = hi - lo;
    g.cell = cell;
    // A cell and a half of nothing on every side: the outermost points are
    // outside whatever the bounds said, so the surface always closes.
    const glm::ivec3 n(glm::ceil(size / cell));
    g.nx = n.x + 4;
    g.ny = n.y + 4;
    g.nz = n.z + 4;
    const glm::dvec3 span = glm::dvec3(g.nx - 1, g.ny - 1, g.nz - 1) * cell;
    g.origin = 0.5 * (lo + hi) - 0.5 * span;
    return true;
}

// The creases put back (TraceMesh.hpp): every edge across a crease split at
// a corner on it, and those corners joined within each triangle.
void sharpen(const Shapes& shapes, int node, double cell, int threads, SolidMesh& m) {
    const int nv = static_cast<int>(m.verts.size());
    const int nt = static_cast<int>(m.tris.size());
    std::vector<glm::dvec3>& P = m.verts;
    const auto V = [&](int i) -> glm::dvec3& { return P[static_cast<std::size_t>(i)]; };

    // Which triangles each corner is in.
    std::vector<int> start(static_cast<std::size_t>(nv) + 1, 0), list(static_cast<std::size_t>(nt) * 3);
    for (const glm::ivec3& t : m.tris)
        for (int k = 0; k < 3; ++k) ++start[static_cast<std::size_t>(t[k]) + 1];
    for (int i = 0; i < nv; ++i) start[static_cast<std::size_t>(i) + 1] += start[static_cast<std::size_t>(i)];
    {
        std::vector<int> fill(start.begin(), start.end() - 1);
        for (int t = 0; t < nt; ++t)
            for (int k = 0; k < 3; ++k) list[static_cast<std::size_t>(fill[static_cast<std::size_t>(m.tris[static_cast<std::size_t>(t)][k])]++)] = t;
    }
    const auto faceN = [&](const glm::ivec3& t) { return glm::cross(V(t.y) - V(t.x), V(t.z) - V(t.x)); };

    // Every corner's exact tangent plane: a short ray at the solid along the
    // mesh's own normal there.
    std::vector<glm::dvec3> pp(static_cast<std::size_t>(nv)), pn(static_cast<std::size_t>(nv));
    std::vector<std::uint8_t> has(static_cast<std::size_t>(nv), 0);
    // Short: from further out the ray could meet the other face of a crease
    // first, and the corner would be given that face's plane.
    const float h = static_cast<float>(cell * 0.1);
    parallelFor(nv, threads, [&](int a, int b) {
        for (int v = a; v < b; ++v) {
            glm::dvec3 n(0.0);
            for (int k = start[static_cast<std::size_t>(v)]; k < start[static_cast<std::size_t>(v) + 1]; ++k)
                n += faceN(m.tris[static_cast<std::size_t>(list[static_cast<std::size_t>(k)])]);
            const double l = glm::length(n);
            if (!(l > 0.0)) continue;
            const glm::vec3 nf(n / l), c(V(v));
            ShapeHit hit;
            if (shapes.closest(node, c + nf * h, -nf, 0.0f, 2.0f * h, hit)) {
                pp[static_cast<std::size_t>(v)] = glm::dvec3(c + nf * h - nf * hit.t);
            } else if (shapes.closest(node, c - nf * h, nf, 0.0f, 2.0f * h, hit)) {
                pp[static_cast<std::size_t>(v)] = glm::dvec3(c - nf * h + nf * hit.t);
            } else {
                continue;
            }
            pn[static_cast<std::size_t>(v)] = glm::dvec3(hit.n);
            has[static_cast<std::size_t>(v)] = 1;
        }
    });

    // Every edge that crosses a crease -- its two ends' planes disagree -- is
    // split at a point ON the crease: nearest its two planes, moved from the
    // edge's middle only in the directions they disagree in, and kept only
    // when it is near and on the surface. In a triangle with two such edges
    // the two new corners are then joined, and that edge is the crease.
    constexpr double kAcross = 0.85;   // the ends' normals more than ~30 degrees apart
    const auto key = [](int a, int b) {
        if (a > b) std::swap(a, b);
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(a)) << 32) | static_cast<std::uint32_t>(b);
    };
    std::vector<std::uint64_t> across;
    for (const glm::ivec3& tr : m.tris)
        for (int k = 0; k < 3; ++k) {
            const int a = tr[k], b = tr[(k + 1) % 3];
            if (a > b || !has[static_cast<std::size_t>(a)] || !has[static_cast<std::size_t>(b)]) continue;
            const double d = glm::dot(pn[static_cast<std::size_t>(a)], pn[static_cast<std::size_t>(b)]);
            // Below -0.5 the two are a thin wall's two sides, not a crease.
            if (d < kAcross && d > -0.5) across.push_back(key(a, b));
        }
    std::vector<glm::dvec3> at(across.size());
    std::vector<std::uint8_t> good(across.size(), 0);
    parallelFor(static_cast<int>(across.size()), threads, [&](int lo, int hi) {
        for (int e = lo; e < hi; ++e) {
            const int a = static_cast<int>(across[static_cast<std::size_t>(e)] >> 32);
            const int b = static_cast<int>(across[static_cast<std::size_t>(e)] & 0xffffffffu);
            const glm::dvec3 &na = pn[static_cast<std::size_t>(a)], &nb = pn[static_cast<std::size_t>(b)];
            const glm::dvec3 mid = 0.5 * (V(a) + V(b));
            // Two planes: the point of their line nearest the middle, by the
            // 2x2 system in span(na, nb).
            const double g = glm::dot(na, nb);
            const double det = 1.0 - g * g;
            if (det < 1e-6) continue;
            const double ra = glm::dot(na, pp[static_cast<std::size_t>(a)] - mid);
            const double rb = glm::dot(nb, pp[static_cast<std::size_t>(b)] - mid);
            const double ca = (ra - g * rb) / det, cb = (rb - g * ra) / det;
            const glm::dvec3 x = mid + na * ca + nb * cb;
            if (!(glm::length(x - mid) < cell)) continue;   // the planes meet far off
            // On the surface: in a little way in, out a little way out.
            const glm::dvec3 nAvg = na + nb;
            const double la = glm::length(nAvg);
            if (!(la > 0.0)) continue;
            const glm::dvec3 step = nAvg / la * (0.15 * cell);
            if (!shapes.inside(node, glm::vec3(x - step)) || shapes.inside(node, glm::vec3(x + step))) continue;
            at[static_cast<std::size_t>(e)] = x;
            good[static_cast<std::size_t>(e)] = 1;
        }
    });
    std::unordered_map<std::uint64_t, int> splitAt;   // edge -> its new corner
    splitAt.reserve(across.size() * 2 + 16);
    for (std::size_t e = 0; e < across.size(); ++e)
        if (good[e]) splitAt.emplace(across[e], -1);
    if (splitAt.empty()) return;

    // A triangle's pieces for the edges of it that are split (e0 = v0 v1,
    // e1 = v1 v2, e2 = v2 v0), with the new corners as given.
    const auto pieces = [&](const glm::ivec3& t, const int f[3], const auto& pos, glm::ivec3 out[4]) {
        const int n = (f[0] >= 0) + (f[1] >= 0) + (f[2] >= 0);
        if (n == 0) { out[0] = t; return 1; }
        if (n == 3) {
            out[0] = {t.x, f[0], f[2]};
            out[1] = {f[0], t.y, f[1]};
            out[2] = {f[2], f[1], t.z};
            out[3] = {f[0], f[1], f[2]};
            return 4;
        }
        if (n == 1) {
            const int k = f[0] >= 0 ? 0 : f[1] >= 0 ? 1 : 2;   // split edge k: t[k] t[k+1]
            const int a = t[k], b = t[(k + 1) % 3], c = t[(k + 2) % 3];
            out[0] = {a, f[k], c};
            out[1] = {f[k], b, c};
            return 2;
        }
        // Two: the corner they share gets a triangle of its own; the quad left
        // is cut along its shorter diagonal.
        const int k = f[0] < 0 ? 1 : f[1] < 0 ? 2 : 0;   // edges k and k+1 split, they share t[k+1]
        const int a = t[k], s = t[(k + 1) % 3], c = t[(k + 2) % 3];
        const int fa = f[k], fc = f[(k + 1) % 3];
        out[0] = {fa, s, fc};
        if (glm::length(pos(a) - pos(fc)) <= glm::length(pos(fa) - pos(c))) {
            out[1] = {a, fa, fc};
            out[2] = {a, fc, c};
        } else {
            out[1] = {a, fa, c};
            out[2] = {fa, fc, c};
        }
        return 3;
    };

    // Splits that would fold a piece over are taken back, the whole edge (so
    // its other triangle agrees), until none does.
    std::unordered_map<std::uint64_t, glm::dvec3> where;
    where.reserve(splitAt.size() * 2 + 16);
    for (std::size_t e = 0; e < across.size(); ++e)
        if (good[e]) where.emplace(across[e], at[e]);
    const int nt0 = nt;
    for (int round = 0; round < 6; ++round) {
        std::vector<std::uint64_t> undo;
        for (int t = 0; t < nt0; ++t) {
            const glm::ivec3& tr = m.tris[static_cast<std::size_t>(t)];
            int f[3];
            std::uint64_t ek[3];
            int any = 0;
            for (int k = 0; k < 3; ++k) {
                ek[k] = key(tr[k], tr[(k + 1) % 3]);
                f[k] = -1;
                if (splitAt.count(ek[k])) { f[k] = nv + k; ++any; }
            }
            if (!any) continue;
            const auto pos = [&](int i) -> glm::dvec3 {
                if (i >= nv) return where[ek[i - nv]];
                return V(i);
            };
            glm::ivec3 out[4];
            const int n = pieces(tr, f, pos, out);
            // Each piece must face the way the surface does where its old
            // corners are -- not the way the old triangle did: across a
            // sharper crease than a right angle that one faced neither side.
            const glm::dvec3 n0 = faceN(tr);
            const double l0 = glm::length(n0);
            bool folds = false;
            for (int i = 0; i < n && !folds; ++i) {
                const glm::dvec3 ni = glm::cross(pos(out[i].y) - pos(out[i].x), pos(out[i].z) - pos(out[i].x));
                glm::dvec3 ref(0.0);
                for (int j = 0; j < 3; ++j)
                    if (out[i][j] < nv) ref += pn[static_cast<std::size_t>(out[i][j])];
                if (glm::dot(ref, ref) < 1e-12) ref = n0 / l0;
                if (!(glm::dot(ni, ref) > 1e-6 * l0) ) folds = true;
            }
            if (folds)
                for (int k = 0; k < 3; ++k) if (f[k] >= 0) undo.push_back(ek[k]);
        }
        if (undo.empty()) break;
        for (std::uint64_t k : undo) splitAt.erase(k);
    }
    if (splitAt.empty()) return;

    // Make them: the corners, then every triangle in its pieces.
    for (auto& [k, idx] : splitAt) {
        idx = static_cast<int>(P.size());
        P.push_back(where[k]);
    }
    for (int t = 0; t < nt0; ++t) {
        const glm::ivec3 tr = m.tris[static_cast<std::size_t>(t)];
        int f[3];
        int any = 0;
        for (int k = 0; k < 3; ++k) {
            const auto it = splitAt.find(key(tr[k], tr[(k + 1) % 3]));
            f[k] = it == splitAt.end() ? -1 : it->second;
            any += f[k] >= 0;
        }
        if (!any) continue;
        glm::ivec3 out[4];
        const int n = pieces(tr, f, [&](int i) -> glm::dvec3 { return V(i); }, out);
        m.tris[static_cast<std::size_t>(t)] = out[0];
        for (int i = 1; i < n; ++i) m.tris.push_back(out[i]);
    }
}

} // namespace

glm::ivec3 solidGrid(const Shapes& shapes, int node, double cell) {
    Grid g;
    if (!gridOf(shapes, node, cell, g)) return glm::ivec3(0);
    return {g.nx - 1, g.ny - 1, g.nz - 1};
}

bool meshSolid(const Shapes& shapes, int node, const SolidMeshOptions& opt, SolidMesh& out,
               std::string& err) {
    out = SolidMesh{};
    Grid g;
    if (!gridOf(shapes, node, opt.cell, g)) {
        err = "it has no bounds (a plane, or a CSG an unbounded shape makes infinite)";
        return false;
    }
    if (std::max({g.nx, g.ny, g.nz}) - 1 > opt.maxCells) {
        err = "too fine: " + std::to_string(std::max({g.nx, g.ny, g.nz}) - 1) + " cells across, at most " +
              std::to_string(opt.maxCells);
        return false;
    }
    const int threads = opt.threads > 0 ? opt.threads
                                        : std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 1);
    const int nx = g.nx, ny = g.ny, nz = g.nz;
    const std::size_t layer = static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny);
    const auto idx = [nx](int i, int j) { return static_cast<std::size_t>(j) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(i); };

    const auto inside = [&](const glm::dvec3& p) { return shapes.inside(node, glm::vec3(p)); };

    // In or out at every point of a layer; the outermost ring and the first
    // and last layers are out by construction.
    const auto sample = [&](int k, std::vector<std::uint8_t>& occ) {
        std::fill(occ.begin(), occ.end(), std::uint8_t(0));
        if (k == 0 || k == nz - 1) return;
        parallelFor(ny - 2, threads, [&](int a, int b) {
            for (int j = a + 1; j < b + 1; ++j)
                for (int i = 1; i < nx - 1; ++i) occ[idx(i, j)] = inside(g.at(i, j, k)) ? 1 : 0;
        });
    };

    std::vector<std::uint8_t> occLo(layer), occHi(layer);
    // Corner index per edge, per point of the layer: [point * 7 + offset].
    std::vector<int> eLo(layer * kOffsets, -1), eHi(layer * kOffsets, -1);

    // The crossings on a set of edges: found in parallel, numbered in order.
    struct Pending { int i, j, m; bool hiLayer; };
    std::vector<Pending> pend;
    std::vector<glm::dvec3> pos;
    const auto solveEdges = [&](int k) {
        // k is the lower layer: edges of layer k climbing (m & 4), and the
        // flat edges of layer k + 1.
        pend.clear();
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) {
                const std::uint8_t lo = occLo[idx(i, j)], hi = occHi[idx(i, j)];
                for (int m = 1; m <= 7; ++m) {
                    const int di = m & 1, dj = (m >> 1) & 1;
                    if (i + di >= nx || j + dj >= ny) continue;
                    if (m & 4) {
                        if (lo != occHi[idx(i + di, j + dj)]) pend.push_back({i, j, m, false});
                    } else if (hi != occHi[idx(i + di, j + dj)]) {
                        pend.push_back({i, j, m, true});
                    }
                }
            }
        pos.resize(pend.size());
        parallelFor(static_cast<int>(pend.size()), threads, [&](int a, int b) {
            for (int e = a; e < b; ++e) {
                const Pending& p = pend[static_cast<std::size_t>(e)];
                const int ka = p.hiLayer ? k + 1 : k;
                const glm::dvec3 A = g.at(p.i, p.j, ka);
                const glm::dvec3 B = g.at(p.i + (p.m & 1), p.j + ((p.m >> 1) & 1), ka + ((p.m >> 2) & 1));
                const bool inA = (p.hiLayer ? occHi : occLo)[idx(p.i, p.j)] != 0;
                double lo = 0.0, hi = 1.0;
                for (int it = 0; it < kBisect; ++it) {
                    const double mid = 0.5 * (lo + hi);
                    if (inside(A + (B - A) * mid) == inA) lo = mid;
                    else                                  hi = mid;
                }
                const double t = std::clamp(0.5 * (lo + hi), kEdgeMin, 1.0 - kEdgeMin);
                pos[static_cast<std::size_t>(e)] = A + (B - A) * t;
            }
        });
        for (std::size_t e = 0; e < pend.size(); ++e) {
            const Pending& p = pend[e];
            std::vector<int>& ev = p.hiLayer ? eHi : eLo;
            ev[idx(p.i, p.j) * kOffsets + static_cast<std::size_t>(p.m - 1)] = static_cast<int>(out.verts.size());
            out.verts.push_back(pos[e]);
        }
    };

    // The six tetrahedra of a cube, as corner chains (corner = bit mask
    // 1 x, 2 y, 4 z): every chain runs from 0 to 7 and each corner is a
    // subset of the next.
    static constexpr int kTets[6][4] = {
        {0, 1, 3, 7}, {0, 1, 5, 7}, {0, 2, 3, 7}, {0, 2, 6, 7}, {0, 4, 5, 7}, {0, 4, 6, 7},
    };
    const auto cornerVec = [](int c) { return glm::dvec3(c & 1, (c >> 1) & 1, (c >> 2) & 1); };

    sample(0, occLo);
    for (int k = 0; k + 1 < nz; ++k) {
        if (opt.progress && !opt.progress(static_cast<float>(k) / static_cast<float>(nz - 1))) {
            err = "cancelled";
            out = SolidMesh{};
            return false;
        }
        sample(k + 1, occHi);
        solveEdges(k);

        // The corner of the edge between cube corners a and b of cube (i, j).
        const auto edgeVert = [&](int i, int j, int a, int b) {
            if ((a & b) != a) std::swap(a, b);   // a is the lower end
            const int m = a ^ b;
            const int pi = i + (a & 1), pj = j + ((a >> 1) & 1);
            const std::vector<int>& ev = (a & 4) ? eHi : eLo;
            return ev[idx(pi, pj) * kOffsets + static_cast<std::size_t>(m - 1)];
        };

        for (int j = 0; j + 1 < ny; ++j)
            for (int i = 0; i + 1 < nx; ++i) {
                int any = 0, all = 1;
                std::uint8_t in[8];
                for (int c = 0; c < 8; ++c) {
                    const std::vector<std::uint8_t>& occ = (c & 4) ? occHi : occLo;
                    in[c] = occ[idx(i + (c & 1), j + ((c >> 1) & 1))];
                    any |= in[c];
                    all &= in[c];
                }
                if (!any || all) continue;
                for (const auto& tet : kTets) {
                    int ins[4], outs[4], ni = 0, no = 0;
                    for (int v = 0; v < 4; ++v) (in[tet[v]] ? ins[ni++] : outs[no++]) = tet[v];
                    if (ni == 0 || no == 0) continue;
                    if (ni == 1 || no == 1) {
                        // One corner apart: a triangle across its three edges.
                        const bool loneIn = ni == 1;
                        const int L = loneIn ? ins[0] : outs[0];
                        const int* o = loneIn ? outs : ins;
                        int v0 = edgeVert(i, j, L, o[0]), v1 = edgeVert(i, j, L, o[1]),
                            v2 = edgeVert(i, j, L, o[2]);
                        // Wound from the edges' midpoints, which are never
                        // degenerate: facing out means facing away from an
                        // inside corner.
                        const glm::dvec3 pL = cornerVec(L);
                        const glm::dvec3 m0 = 0.5 * (pL + cornerVec(o[0])), m1 = 0.5 * (pL + cornerVec(o[1])),
                                         m2 = 0.5 * (pL + cornerVec(o[2]));
                        const glm::dvec3 n = glm::cross(m1 - m0, m2 - m0);
                        const double side = glm::dot(n, m0 - pL);
                        if ((side > 0.0) != loneIn) std::swap(v1, v2);
                        out.tris.emplace_back(v0, v1, v2);
                    } else {
                        // Two and two: a quad, the edges in order round it.
                        const int I0 = ins[0], I1 = ins[1], O0 = outs[0], O1 = outs[1];
                        int q[4] = {edgeVert(i, j, I0, O0), edgeVert(i, j, I0, O1), edgeVert(i, j, I1, O1),
                                    edgeVert(i, j, I1, O0)};
                        const glm::dvec3 a = 0.5 * (cornerVec(I0) + cornerVec(O0)),
                                         b = 0.5 * (cornerVec(I0) + cornerVec(O1)),
                                         d = 0.5 * (cornerVec(I1) + cornerVec(O0));
                        const glm::dvec3 n = glm::cross(b - a, d - a);
                        const glm::dvec3 outward = 0.5 * (cornerVec(O0) + cornerVec(O1)) -
                                                   0.5 * (cornerVec(I0) + cornerVec(I1));
                        if (glm::dot(n, outward) < 0.0) std::swap(q[1], q[3]);
                        // Split along the shorter diagonal: the better pair of triangles.
                        const auto& P = out.verts;
                        const double d02 = glm::length(P[static_cast<std::size_t>(q[0])] - P[static_cast<std::size_t>(q[2])]);
                        const double d13 = glm::length(P[static_cast<std::size_t>(q[1])] - P[static_cast<std::size_t>(q[3])]);
                        if (d02 <= d13) {
                            out.tris.emplace_back(q[0], q[1], q[2]);
                            out.tris.emplace_back(q[0], q[2], q[3]);
                        } else {
                            out.tris.emplace_back(q[0], q[1], q[3]);
                            out.tris.emplace_back(q[1], q[2], q[3]);
                        }
                    }
                }
            }

        std::swap(occLo, occHi);
        std::swap(eLo, eHi);
        std::fill(eHi.begin(), eHi.end(), -1);
    }

    if (out.tris.empty()) {
        err = "nothing inside it at this spacing (finer, or is it thinner than a cell?)";
        return false;
    }

    if (opt.sharpen) sharpen(shapes, node, opt.cell, threads, out);

    // Whose surface each triangle is: a short ray at it from just outside,
    // and from just inside when that misses (a sliver of a cell).
    if (opt.leaves) {
        out.triLeaf.assign(out.tris.size(), -1);
        const float h = static_cast<float>(opt.cell * 0.75);
        parallelFor(static_cast<int>(out.tris.size()), threads, [&](int a, int b) {
            for (int t = a; t < b; ++t) {
                const glm::ivec3& tr = out.tris[static_cast<std::size_t>(t)];
                const glm::dvec3& A = out.verts[static_cast<std::size_t>(tr.x)];
                const glm::dvec3& B = out.verts[static_cast<std::size_t>(tr.y)];
                const glm::dvec3& C = out.verts[static_cast<std::size_t>(tr.z)];
                const glm::dvec3 nd = glm::cross(B - A, C - A);
                const double nl = glm::length(nd);
                if (!(nl > 0.0)) continue;
                const glm::vec3 c((A + B + C) / 3.0);
                const glm::vec3 n(nd / nl);
                ShapeHit hit;
                if (shapes.closest(node, c + n * h, -n, 0.0f, 2.0f * h, hit) ||
                    shapes.closest(node, c - n * h, n, 0.0f, 2.0f * h, hit))
                    out.triLeaf[static_cast<std::size_t>(t)] = hit.leaf;
            }
        });
    }
    if (opt.progress) opt.progress(1.0f);
    return true;
}

} // namespace pathtrace
