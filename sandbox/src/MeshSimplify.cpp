#include "MeshSimplify.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <queue>
#include <unordered_map>

namespace meshsimplify {

namespace {

// A symmetric 4x4 quadric, upper triangle: the summed squared distance to a set
// of planes, as a function of position.
struct Quadric {
    double q[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    void addPlane(double a, double b, double c, double d, double w) {
        q[0] += w * a * a; q[1] += w * a * b; q[2] += w * a * c; q[3] += w * a * d;
        q[4] += w * b * b; q[5] += w * b * c; q[6] += w * b * d;
        q[7] += w * c * c; q[8] += w * c * d;
        q[9] += w * d * d;
    }
    void add(const Quadric& o) { for (int i = 0; i < 10; ++i) q[i] += o.q[i]; }
    double eval(double x, double y, double z) const {
        return q[0] * x * x + 2 * q[1] * x * y + 2 * q[2] * x * z + 2 * q[3] * x
             + q[4] * y * y + 2 * q[5] * y * z + 2 * q[6] * y
             + q[7] * z * z + 2 * q[8] * z + q[9];
    }
};

struct V3 { double x, y, z; };
V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double len(V3 a) { return std::sqrt(dot(a, a)); }

struct PosKey {
    float v[3];
    bool operator==(const PosKey& o) const { return std::memcmp(v, o.v, sizeof v) == 0; }
};
struct PosKeyHash {
    std::size_t operator()(const PosKey& k) const noexcept {
        std::uint32_t b[3];
        std::memcpy(b, k.v, sizeof b);
        std::size_t h = 1469598103934665603ull;
        for (std::uint32_t x : b) { h ^= x; h *= 1099511628211ull; }
        return h;
    }
};

struct Candidate {
    double cost;
    int from, to;               // position ids: `from` moves onto `to`
    std::uint32_t vFrom, vTo;   // their versions when this was costed
    bool operator<(const Candidate& o) const { return cost > o.cost; } // min-heap
};

} // namespace

std::vector<std::uint32_t> simplify(const float* verts, std::size_t stride,
                                    std::size_t vertexCount,
                                    const std::uint32_t* idx, std::size_t indexCount,
                                    std::size_t targetTris) {
    const std::size_t triCount0 = indexCount / 3;
    std::vector<std::uint32_t> out(idx, idx + triCount0 * 3);
    if (triCount0 <= targetTris || vertexCount == 0) return out;

    // --- Topology over POSITIONS. Two corners at the same point are the same
    // point of the surface even when a UV seam gives them different vertices;
    // collapsing over vertex indices would tear every seam open.
    std::vector<int> pidOf(vertexCount);
    std::vector<V3>  pos;
    std::vector<std::vector<std::uint32_t>> attrsOf;   // pid -> vertices at it
    {
        std::unordered_map<PosKey, int, PosKeyHash> seen;
        seen.reserve(vertexCount);
        for (std::size_t i = 0; i < vertexCount; ++i) {
            PosKey k{{verts[i * stride], verts[i * stride + 1], verts[i * stride + 2]}};
            auto it = seen.find(k);
            if (it == seen.end()) {
                it = seen.emplace(k, static_cast<int>(pos.size())).first;
                pos.push_back({k.v[0], k.v[1], k.v[2]});
                attrsOf.emplace_back();
            }
            pidOf[i] = it->second;
            attrsOf[it->second].push_back(static_cast<std::uint32_t>(i));
        }
    }
    const int nP = static_cast<int>(pos.size());

    // Triangles: attribute corners + position corners.
    struct Tri { std::uint32_t c[3]; int p[3]; bool dead; };
    std::vector<Tri> tris;
    tris.reserve(triCount0);
    for (std::size_t t = 0; t < triCount0; ++t) {
        Tri tr;
        for (int k = 0; k < 3; ++k) { tr.c[k] = idx[t * 3 + k]; tr.p[k] = pidOf[tr.c[k]]; }
        tr.dead = (tr.p[0] == tr.p[1] || tr.p[1] == tr.p[2] || tr.p[0] == tr.p[2]);
        tris.push_back(tr);
    }
    std::vector<std::vector<int>> vt(nP);                // pid -> triangles
    std::size_t live = 0;
    for (int t = 0; t < static_cast<int>(tris.size()); ++t) {
        if (tris[t].dead) continue;
        ++live;
        for (int k = 0; k < 3; ++k) vt[tris[t].p[k]].push_back(t);
    }

    // --- Quadrics: every face's plane, weighted by its area.
    std::vector<Quadric> Q(nP);
    auto faceNormal = [&](const Tri& tr, V3& n, double& area) {
        const V3 e1 = sub(pos[tr.p[1]], pos[tr.p[0]]);
        const V3 e2 = sub(pos[tr.p[2]], pos[tr.p[0]]);
        n = cross(e1, e2);
        const double l = len(n);
        area = 0.5 * l;
        if (l > 0) { n.x /= l; n.y /= l; n.z /= l; }
    };
    for (const Tri& tr : tris) {
        if (tr.dead) continue;
        V3 n; double area;
        faceNormal(tr, n, area);
        if (area <= 0) continue;
        const double d = -dot(n, pos[tr.p[0]]);
        for (int k = 0; k < 3; ++k) Q[tr.p[k]].addPlane(n.x, n.y, n.z, d, area);
    }

    // --- Borders: an edge with one face is an open end (a cut branch, the rim
    // of a leaf). A plane through it, perpendicular to its face, heavily
    // weighted, keeps it where it is. Measured over positions, so a UV seam is
    // NOT a border and simplifies like the surface around it.
    {
        std::unordered_map<std::uint64_t, int> edgeFaces;
        std::unordered_map<std::uint64_t, int> edgeFace;   // one face using it
        edgeFaces.reserve(live * 3);
        auto ekey = [](int a, int b) {
            if (a > b) std::swap(a, b);
            return (static_cast<std::uint64_t>(a) << 32) | static_cast<std::uint32_t>(b);
        };
        for (int t = 0; t < static_cast<int>(tris.size()); ++t) {
            if (tris[t].dead) continue;
            for (int k = 0; k < 3; ++k) {
                const std::uint64_t e = ekey(tris[t].p[k], tris[t].p[(k + 1) % 3]);
                ++edgeFaces[e];
                edgeFace[e] = t;
            }
        }
        for (const auto& [e, count] : edgeFaces) {
            if (count != 1) continue;
            const int a = static_cast<int>(e >> 32), b = static_cast<int>(e & 0xffffffffu);
            V3 fn; double area;
            faceNormal(tris[edgeFace[e]], fn, area);
            const V3 ed = sub(pos[b], pos[a]);
            V3 n = cross(ed, fn);
            const double l = len(n);
            if (l <= 0) continue;
            n.x /= l; n.y /= l; n.z /= l;
            const double d = -dot(n, pos[a]);
            const double w = 50.0 * dot(ed, ed);
            Q[a].addPlane(n.x, n.y, n.z, d, w);
            Q[b].addPlane(n.x, n.y, n.z, d, w);
        }
    }

    std::vector<std::uint32_t> version(nP, 0);
    std::vector<char> removed(nP, 0);

    // Neighbour pids of p, through its live triangles.
    auto neighbours = [&](int p, std::vector<int>& outN) {
        outN.clear();
        for (int t : vt[p]) {
            if (tris[t].dead) continue;
            for (int k = 0; k < 3; ++k)
                if (tris[t].p[k] != p) outN.push_back(tris[t].p[k]);
        }
        std::sort(outN.begin(), outN.end());
        outN.erase(std::unique(outN.begin(), outN.end()), outN.end());
    };

    std::priority_queue<Candidate> heap;
    auto push = [&](int a, int b) {
        Quadric q = Q[a]; q.add(Q[b]);
        const double cab = q.eval(pos[b].x, pos[b].y, pos[b].z);   // a -> b
        const double cba = q.eval(pos[a].x, pos[a].y, pos[a].z);   // b -> a
        if (cab <= cba) heap.push({cab, a, b, version[a], version[b]});
        else            heap.push({cba, b, a, version[b], version[a]});
    };
    {
        std::vector<int> nb;
        for (int p = 0; p < nP; ++p) {
            neighbours(p, nb);
            for (int q : nb) if (q > p) push(p, q);
        }
    }

    std::vector<int> nu, nv;
    while (live > targetTris && !heap.empty()) {
        const Candidate c = heap.top();
        heap.pop();
        const int u = c.from, v = c.to;
        if (removed[u] || removed[v]) continue;
        if (version[u] != c.vFrom || version[v] != c.vTo) continue;

        // Link condition: an interior edge shares exactly two neighbours with
        // its ends; more, and the collapse pinches the surface into a
        // non-manifold fin.
        neighbours(u, nu);
        neighbours(v, nv);
        int common = 0;
        {
            std::size_t i = 0, j = 0;
            while (i < nu.size() && j < nv.size()) {
                if (nu[i] < nv[j]) ++i;
                else if (nu[i] > nv[j]) ++j;
                else { ++common; ++i; ++j; }
            }
        }
        if (common > 2) continue;

        // No face around u may flip or collapse to a sliver when u moves onto v.
        bool ok = true;
        for (int t : vt[u]) {
            const Tri& tr = tris[t];
            if (tr.dead) continue;
            if (tr.p[0] == v || tr.p[1] == v || tr.p[2] == v) continue;   // dies
            V3 before, after; double ab, aa;
            faceNormal(tr, before, ab);
            Tri moved = tr;
            for (int k = 0; k < 3; ++k) if (moved.p[k] == u) moved.p[k] = v;
            faceNormal(moved, after, aa);
            if (aa < 1e-12 || dot(before, after) < 0.25) { ok = false; break; }
        }
        if (!ok) continue;

        // Collapse. Faces on the edge die; the rest take v -- and for each
        // corner, v's vertex whose UV and normal are nearest the one it had,
        // so a face on one side of a seam keeps that side's texture.
        for (int t : vt[u]) {
            Tri& tr = tris[t];
            if (tr.dead) continue;
            if (tr.p[0] == v || tr.p[1] == v || tr.p[2] == v) {
                tr.dead = true;
                --live;
                continue;
            }
            for (int k = 0; k < 3; ++k) {
                if (tr.p[k] != u) continue;
                const float* a = verts + static_cast<std::size_t>(tr.c[k]) * stride;
                std::uint32_t best = attrsOf[v].front();
                double bestD = 1e300;
                for (std::uint32_t cand : attrsOf[v]) {
                    const float* b = verts + static_cast<std::size_t>(cand) * stride;
                    double d = 0;
                    if (stride >= 8) {
                        d += (a[6] - b[6]) * (a[6] - b[6]) + (a[7] - b[7]) * (a[7] - b[7]);
                        d += 0.05 * ((a[3] - b[3]) * (a[3] - b[3]) + (a[4] - b[4]) * (a[4] - b[4])
                                   + (a[5] - b[5]) * (a[5] - b[5]));
                    }
                    if (d < bestD) { bestD = d; best = cand; }
                }
                tr.p[k] = v;
                tr.c[k] = best;
            }
            vt[v].push_back(t);
        }
        vt[u].clear();
        removed[u] = 1;
        Q[v].add(Q[u]);
        ++version[v];
        // Drop dead faces from v's list now and then, so it does not grow
        // without bound around a vertex that swallows many neighbours.
        if (vt[v].size() > 64) {
            std::vector<int> keepT;
            for (int t : vt[v]) if (!tris[t].dead) keepT.push_back(t);
            std::sort(keepT.begin(), keepT.end());
            keepT.erase(std::unique(keepT.begin(), keepT.end()), keepT.end());
            vt[v].swap(keepT);
        }
        neighbours(v, nv);
        for (int w : nv) push(v, w);
    }

    out.clear();
    out.reserve(live * 3);
    for (const Tri& tr : tris)
        if (!tr.dead) out.insert(out.end(), {tr.c[0], tr.c[1], tr.c[2]});
    return out;
}

void pruneCards(std::vector<float>& verts, std::size_t stride,
                std::vector<std::uint32_t>& idx, float keep, std::uint32_t seed) {
    keep = std::clamp(keep, 0.02f, 1.0f);
    if (keep >= 0.999f || idx.empty()) return;
    const std::size_t nV = verts.size() / stride;

    // Pieces: triangles joined through shared vertices (a card is two).
    std::vector<std::uint32_t> parent(nV);
    for (std::size_t i = 0; i < nV; ++i) parent[i] = static_cast<std::uint32_t>(i);
    auto find = [&](std::uint32_t x) {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    };
    for (std::size_t t = 0; t + 2 < idx.size(); t += 3) {
        const std::uint32_t a = find(idx[t]), b = find(idx[t + 1]), c = find(idx[t + 2]);
        parent[b] = a;
        parent[find(c)] = a;
    }
    // Each piece: its centre, and whether it stays.
    struct Piece { double x = 0, y = 0, z = 0; int n = 0; bool keep = false; };
    std::unordered_map<std::uint32_t, Piece> pieces;
    std::vector<char> used(nV, 0);
    for (std::uint32_t i : idx) used[i] = 1;
    for (std::size_t i = 0; i < nV; ++i) {
        if (!used[i]) continue;
        Piece& p = pieces[find(static_cast<std::uint32_t>(i))];
        p.x += verts[i * stride]; p.y += verts[i * stride + 1]; p.z += verts[i * stride + 2];
        ++p.n;
    }
    for (auto& [root, p] : pieces) {
        std::uint32_t h = root * 2654435761u ^ seed;
        h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
        p.keep = (static_cast<float>(h & 0xffffff) / 16777216.0f) < keep;
        p.x /= p.n; p.y /= p.n; p.z /= p.n;
    }
    // Survivors grow to carry the area of the ones that went.
    const float s = std::min(1.0f / std::sqrt(keep), 2.5f);
    for (std::size_t i = 0; i < nV; ++i) {
        if (!used[i]) continue;
        const Piece& p = pieces[find(static_cast<std::uint32_t>(i))];
        if (!p.keep) continue;
        float* v = &verts[i * stride];
        v[0] = static_cast<float>(p.x + (v[0] - p.x) * s);
        v[1] = static_cast<float>(p.y + (v[1] - p.y) * s);
        v[2] = static_cast<float>(p.z + (v[2] - p.z) * s);
    }
    std::vector<std::uint32_t> kept;
    kept.reserve(idx.size());
    for (std::size_t t = 0; t + 2 < idx.size(); t += 3)
        if (pieces[find(idx[t])].keep)
            kept.insert(kept.end(), {idx[t], idx[t + 1], idx[t + 2]});
    idx.swap(kept);
}

} // namespace meshsimplify
