#include "UvOps.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>

namespace uvops {

namespace {

constexpr float kPi = 3.14159265358979f;

std::uint64_t edgeKey(int a, int b) {
    const std::uint32_t lo = static_cast<std::uint32_t>(std::min(a, b));
    const std::uint32_t hi = static_cast<std::uint32_t>(std::max(a, b));
    return (static_cast<std::uint64_t>(lo) << 32) | hi;
}

struct UnionFind {
    std::vector<int> p;
    explicit UnionFind(std::size_t n) : p(n) { std::iota(p.begin(), p.end(), 0); }
    int find(int x) {
        while (p[static_cast<std::size_t>(x)] != x) x = p[static_cast<std::size_t>(x)] = p[static_cast<std::size_t>(p[static_cast<std::size_t>(x)])];
        return x;
    }
    void unite(int a, int b) { a = find(a); b = find(b); if (a != b) p[static_cast<std::size_t>(a)] = b; }
};

int next(const std::vector<int>& fv, int k) { return (k + 1) % static_cast<int>(fv.size()); }

// The shown faces' edges: edge key -> (face, k) for edge k of face f (from
// corner k to corner k + 1).
std::unordered_map<std::uint64_t, std::vector<Loop>> edgeMap(const EditMesh& m, const std::vector<int>& faces) {
    std::unordered_map<std::uint64_t, std::vector<Loop>> out;
    for (int f : faces) {
        const std::vector<int>& fv = m.faces[static_cast<std::size_t>(f)];
        for (int k = 0; k < static_cast<int>(fv.size()); ++k) {
            const int a = fv[static_cast<std::size_t>(k)], b = fv[static_cast<std::size_t>(next(fv, k))];
            if (a != b) out[edgeKey(a, b)].push_back({f, k});
        }
    }
    return out;
}

// Corner of face `f` that is mesh corner `vert`, -1 if none.
int cornerOf(const EditMesh& m, int f, int vert) {
    const std::vector<int>& fv = m.faces[static_cast<std::size_t>(f)];
    for (int k = 0; k < static_cast<int>(fv.size()); ++k)
        if (fv[static_cast<std::size_t>(k)] == vert) return k;
    return -1;
}

float polyArea(const std::vector<glm::vec2>& p) {
    float a = 0.0f;
    for (std::size_t i = 0; i < p.size(); ++i) {
        const glm::vec2& u = p[i];
        const glm::vec2& w = p[(i + 1) % p.size()];
        a += u.x * w.y - w.x * u.y;
    }
    return 0.5f * a;
}

// A plane basis for looking along `d`, the same the planar projection uses.
void basis(glm::vec3 d, glm::vec3& tx, glm::vec3& ty) {
    if (glm::dot(d, d) < 1e-12f) d = glm::vec3(0, 1, 0);
    d = glm::normalize(d);
    const glm::vec3 ad = glm::abs(d);
    const glm::vec3 up = (ad.y > ad.x && ad.y > ad.z) ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
    tx = glm::normalize(glm::cross(up, d));
    ty = glm::cross(d, tx);
}

std::vector<int> validFaces(const EditMesh& m, const std::vector<int>& faces) {
    std::vector<int> out;
    std::vector<char> seen(m.faces.size(), 0);
    for (int f : faces)
        if (m.validFace(f) && !seen[static_cast<std::size_t>(f)]) {
            seen[static_cast<std::size_t>(f)] = 1;
            out.push_back(f);
        }
    return out;
}

using Corner = EditMesh::FaceUV::Corner;
Corner& corner(EditMesh& m, int f, int k) { return m.faceUV[static_cast<std::size_t>(f)].corners[static_cast<std::size_t>(k)]; }

// Every corner of these groups of faces: rotated to its tightest box (when
// `rotate`), then all of them scaled alike and laid out in rows in the tile.
void packGroups(EditMesh& m, const std::vector<std::vector<int>>& groups, float margin, bool rotate) {
    struct Piece { std::vector<Loop> loops; glm::vec2 lo{0}, hi{0}; };
    std::vector<Piece> pieces;
    for (const std::vector<int>& g : groups) {
        ensureCorners(m, g);
        Piece pc;
        for (int f : g)
            for (int k = 0; k < static_cast<int>(m.faces[static_cast<std::size_t>(f)].size()); ++k) pc.loops.push_back({f, k});
        if (pc.loops.empty()) continue;
        auto pos = [&](const Loop& l) -> glm::vec2& { return corner(m, l.f, l.k).uv; };
        if (rotate) {
            // The edge directions are where a tightest box can stand.
            glm::vec2 c(0.0f);
            for (const Loop& l : pc.loops) c += pos(l);
            c /= static_cast<float>(pc.loops.size());
            std::vector<float> angles{0.0f};
            for (int f : g) {
                const std::vector<int>& fv = m.faces[static_cast<std::size_t>(f)];
                for (int k = 0; k < static_cast<int>(fv.size()); ++k) {
                    const glm::vec2 d = corner(m, f, next(fv, k)).uv - corner(m, f, k).uv;
                    if (glm::dot(d, d) > 1e-12f) angles.push_back(std::fmod(std::atan2(d.y, d.x) + 2.0f * kPi, 0.5f * kPi));
                }
            }
            if (angles.size() > 64) {
                std::vector<float> few;
                for (std::size_t i = 0; i < 64; ++i) few.push_back(angles[i * angles.size() / 64]);
                angles = std::move(few);
            }
            float best = 1e30f, bestA = 0.0f;
            for (float a : angles) {
                const float ca = std::cos(-a), sa = std::sin(-a);
                glm::vec2 lo(1e30f), hi(-1e30f);
                for (const Loop& l : pc.loops) {
                    const glm::vec2 q = pos(l) - c;
                    const glm::vec2 r(q.x * ca - q.y * sa, q.x * sa + q.y * ca);
                    lo = glm::min(lo, r);
                    hi = glm::max(hi, r);
                }
                const float area = (hi.x - lo.x) * (hi.y - lo.y);
                if (area < best - 1e-9f) { best = area; bestA = a; }
            }
            const float ca = std::cos(-bestA), sa = std::sin(-bestA);
            for (const Loop& l : pc.loops) {
                const glm::vec2 q = pos(l) - c;
                pos(l) = c + glm::vec2(q.x * ca - q.y * sa, q.x * sa + q.y * ca);
            }
        }
        glm::vec2 lo(1e30f), hi(-1e30f);
        for (const Loop& l : pc.loops) { lo = glm::min(lo, pos(l)); hi = glm::max(hi, pos(l)); }
        // Lying down packs better in rows.
        if (rotate && hi.y - lo.y > hi.x - lo.x) {
            const glm::vec2 c = 0.5f * (lo + hi);
            for (const Loop& l : pc.loops) {
                const glm::vec2 q = pos(l) - c;
                pos(l) = c + glm::vec2(-q.y, q.x);
            }
            lo = glm::vec2(1e30f);
            hi = glm::vec2(-1e30f);
            for (const Loop& l : pc.loops) { lo = glm::min(lo, pos(l)); hi = glm::max(hi, pos(l)); }
        }
        pc.lo = lo;
        pc.hi = glm::max(hi, lo + glm::vec2(1e-5f));
        pieces.push_back(std::move(pc));
    }
    if (pieces.empty()) return;
    std::vector<std::size_t> order(pieces.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return pieces[a].hi.y - pieces[a].lo.y > pieces[b].hi.y - pieces[b].lo.y;
    });
    double area = 0.0;
    float widest = 0.0f;
    for (const Piece& p : pieces) {
        area += double(p.hi.x - p.lo.x) * double(p.hi.y - p.lo.y);
        widest = std::max({widest, p.hi.x - p.lo.x, p.hi.y - p.lo.y});
    }
    margin = std::clamp(margin, 0.0f, 0.2f);
    float s = std::min(float(1.0 / std::sqrt(std::max(area, 1e-12))), (1.0f - 2.0f * margin) / widest);
    std::vector<glm::vec2> at(pieces.size());
    for (int tries = 0; tries < 400; ++tries, s *= 0.97f) {
        float x = margin, y = margin, rowH = 0.0f;
        bool fits = true;
        for (std::size_t i : order) {
            const float w = (pieces[i].hi.x - pieces[i].lo.x) * s, h = (pieces[i].hi.y - pieces[i].lo.y) * s;
            if (x + w + margin > 1.0f + 1e-6f && x > margin) {
                y += rowH + margin;
                x = margin;
                rowH = 0.0f;
            }
            at[i] = {x, y};
            x += w + margin;
            rowH = std::max(rowH, h);
            if (y + h + margin > 1.0f + 1e-6f || w + 2.0f * margin > 1.0f + 1e-6f) { fits = false; break; }
        }
        if (fits) break;
    }
    for (std::size_t i = 0; i < pieces.size(); ++i)
        for (const Loop& l : pieces[i].loops) {
            glm::vec2& u = corner(m, l.f, l.k).uv;
            u = (u - pieces[i].lo) * s + at[i];
        }
}

// Least squares on a sparse system, ||A x - b||, by conjugate gradients on the
// normal equations (CGLS). Each row has at most six entries: one triangle's
// real or imaginary part.
struct Row {
    int    n = 0;
    int    col[6] = {};
    double val[6] = {};
    double rhs = 0.0;
};

void cgls(const std::vector<Row>& rows, std::vector<double>& x, int iters) {
    const std::size_t nr = rows.size(), nx = x.size();
    if (nr == 0 || nx == 0) return;
    std::vector<double> r(nr), s(nx), p(nx), q(nr);
    for (std::size_t i = 0; i < nr; ++i) {
        double ax = 0.0;
        for (int j = 0; j < rows[i].n; ++j) ax += rows[i].val[j] * x[static_cast<std::size_t>(rows[i].col[j])];
        r[i] = rows[i].rhs - ax;
    }
    auto atimes = [&](const std::vector<double>& in, std::vector<double>& out) {   // out = A^T in
        std::fill(out.begin(), out.end(), 0.0);
        for (std::size_t i = 0; i < nr; ++i)
            for (int j = 0; j < rows[i].n; ++j) out[static_cast<std::size_t>(rows[i].col[j])] += rows[i].val[j] * in[i];
    };
    atimes(r, s);
    p = s;
    double gamma = 0.0;
    for (double v : s) gamma += v * v;
    const double gamma0 = gamma;
    for (int it = 0; it < iters && gamma > 1e-24 * std::max(gamma0, 1e-30); ++it) {
        double qq = 0.0;
        for (std::size_t i = 0; i < nr; ++i) {
            double v = 0.0;
            for (int j = 0; j < rows[i].n; ++j) v += rows[i].val[j] * p[static_cast<std::size_t>(rows[i].col[j])];
            q[i] = v;
            qq += v * v;
        }
        if (qq < 1e-300) break;
        const double alpha = gamma / qq;
        for (std::size_t j = 0; j < nx; ++j) x[j] += alpha * p[j];
        for (std::size_t i = 0; i < nr; ++i) r[i] -= alpha * q[i];
        atimes(r, s);
        double gn = 0.0;
        for (double v : s) gn += v * v;
        const double beta = gn / gamma;
        for (std::size_t j = 0; j < nx; ++j) p[j] = s[j] + beta * p[j];
        gamma = gn;
    }
}

} // namespace

// --- The view -------------------------------------------------------------------------

View view(const EditMesh& m, const std::vector<int>& faces) {
    View v;
    v.mesh  = &m;
    v.faces = validFaces(m, faces);
    v.slot.assign(m.faces.size(), -1);
    v.atVert.assign(m.verts.size(), {});
    v.uv.reserve(v.faces.size());
    for (std::size_t i = 0; i < v.faces.size(); ++i) {
        const int f = v.faces[i];
        v.slot[static_cast<std::size_t>(f)] = static_cast<int>(i);
        v.uv.push_back(editmesh::faceUvs(m, f));
        const std::vector<int>& fv = m.faces[static_cast<std::size_t>(f)];
        for (int k = 0; k < static_cast<int>(fv.size()); ++k)
            v.atVert[static_cast<std::size_t>(fv[static_cast<std::size_t>(k)])].push_back({f, k});
    }
    return v;
}

std::vector<Loop> coincident(const View& v, const Loop& l) {
    std::vector<Loop> out;
    if (!v.shown(l.f)) return out;
    const glm::vec2 p = v.at(l);
    for (const Loop& q : v.atVert[static_cast<std::size_t>(v.vert(l))])
        if (glm::length(v.at(q) - p) < kWeld) out.push_back(q);
    return out;
}

std::vector<std::vector<int>> islands(const View& v) {
    const EditMesh& m = *v.mesh;
    UnionFind uf(v.faces.size());
    for (const auto& [key, es] : edgeMap(m, v.faces)) {
        for (std::size_t i = 0; i + 1 < es.size(); ++i)
            for (std::size_t j = i + 1; j < es.size(); ++j) {
                const Loop& e = es[i];
                const Loop& o = es[j];
                const std::vector<int>& fv = m.faces[static_cast<std::size_t>(e.f)];
                const int a = fv[static_cast<std::size_t>(e.k)], b = fv[static_cast<std::size_t>(next(fv, e.k))];
                const int oa = cornerOf(m, o.f, a), ob = cornerOf(m, o.f, b);
                if (oa < 0 || ob < 0) continue;
                const bool welded = glm::length(v.at(e) - v.at({o.f, oa})) < kWeld &&
                                    glm::length(v.at({e.f, next(fv, e.k)}) - v.at({o.f, ob})) < kWeld;
                if (welded) uf.unite(v.slot[static_cast<std::size_t>(e.f)], v.slot[static_cast<std::size_t>(o.f)]);
            }
    }
    std::unordered_map<int, std::size_t> idx;
    std::vector<std::vector<int>> out;
    for (std::size_t i = 0; i < v.faces.size(); ++i) {
        const int r = uf.find(static_cast<int>(i));
        auto [it, fresh] = idx.try_emplace(r, out.size());
        if (fresh) out.emplace_back();
        out[it->second].push_back(v.faces[i]);
    }
    return out;
}

// --- Picking --------------------------------------------------------------------------

Loop nearestLoop(const View& v, glm::vec2 p, float maxDist) {
    Loop best;
    float bd = maxDist;
    for (std::size_t i = 0; i < v.faces.size(); ++i)
        for (std::size_t k = 0; k < v.uv[i].size(); ++k) {
            const float d = glm::length(v.uv[i][k] - p);
            if (d < bd) { bd = d; best = {v.faces[i], static_cast<int>(k)}; }
        }
    return best;
}

Loop nearestEdge(const View& v, glm::vec2 p, float maxDist) {
    Loop best;
    float bd = maxDist;
    for (std::size_t i = 0; i < v.faces.size(); ++i) {
        const std::vector<glm::vec2>& u = v.uv[i];
        for (std::size_t k = 0; k < u.size(); ++k) {
            const glm::vec2 a = u[k], b = u[(k + 1) % u.size()];
            const glm::vec2 ab = b - a;
            const float len2 = glm::dot(ab, ab);
            const float t = len2 > 1e-12f ? std::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
            const float d = glm::length(a + ab * t - p);
            if (d < bd) { bd = d; best = {v.faces[i], static_cast<int>(k)}; }
        }
    }
    return best;
}

int faceAt(const View& v, glm::vec2 p, float maxDist) {
    int best = -1;
    float bestArea = 1e30f;
    for (std::size_t i = 0; i < v.faces.size(); ++i) {
        const std::vector<glm::vec2>& u = v.uv[i];
        bool in = false;
        for (std::size_t a = 0, b = u.size() - 1; a < u.size(); b = a++)
            if ((u[a].y > p.y) != (u[b].y > p.y) &&
                p.x < (u[b].x - u[a].x) * (p.y - u[a].y) / (u[b].y - u[a].y) + u[a].x)
                in = !in;
        if (!in) continue;
        // Overlapping faces: the smallest is the one that was aimed at.
        const float area = std::abs(polyArea(u));
        if (area < bestArea) { bestArea = area; best = v.faces[i]; }
    }
    if (best >= 0) return best;
    float bd = maxDist;
    for (std::size_t i = 0; i < v.faces.size(); ++i) {
        glm::vec2 c(0.0f);
        for (const glm::vec2& q : v.uv[i]) c += q;
        c /= static_cast<float>(v.uv[i].size());
        const float d = glm::length(c - p);
        if (d < bd) { bd = d; best = v.faces[i]; }
    }
    return best;
}

// --- Selecting ------------------------------------------------------------------------

namespace {
void addFace(const View& v, std::vector<Loop>& out, int f, bool sticky) {
    const int n = static_cast<int>(v.mesh->faces[static_cast<std::size_t>(f)].size());
    for (int k = 0; k < n; ++k) {
        if (sticky) for (const Loop& q : coincident(v, {f, k})) out.push_back(q);
        else out.push_back({f, k});
    }
}
} // namespace

bool click(const View& v, Selection& s, SelectMode mode, glm::vec2 p, float maxDist, bool toggle, bool sticky) {
    std::vector<Loop> hit;
    auto take = [&](const Loop& l) {
        if (sticky) for (const Loop& q : coincident(v, l)) hit.push_back(q);
        else hit.push_back(l);
    };
    if (mode == SelectMode::Vertex) {
        const Loop l = nearestLoop(v, p, maxDist);
        if (l.valid()) take(l);
    } else if (mode == SelectMode::Edge) {
        const Loop e = nearestEdge(v, p, maxDist);
        if (e.valid()) {
            const int n = static_cast<int>(v.mesh->faces[static_cast<std::size_t>(e.f)].size());
            take(e);
            take({e.f, (e.k + 1) % n});
        }
    } else {
        const int f = faceAt(v, p, maxDist);
        if (f >= 0) {
            if (mode == SelectMode::Face) {
                addFace(v, hit, f, sticky);
            } else {
                for (const std::vector<int>& isl : islands(v))
                    if (std::find(isl.begin(), isl.end(), f) != isl.end())
                        for (int g : isl) addFace(v, hit, g, false);
            }
        }
    }
    if (hit.empty()) return false;
    if (!toggle) {
        s.clear();
        s.insert(hit.begin(), hit.end());
        return true;
    }
    const bool all = std::all_of(hit.begin(), hit.end(), [&](const Loop& l) { return s.count(l) > 0; });
    for (const Loop& l : hit) {
        if (all) s.erase(l);
        else s.insert(l);
    }
    return true;
}

void selectAll(const View& v, Selection& s) {
    for (int f : v.faces) {
        const int n = static_cast<int>(v.mesh->faces[static_cast<std::size_t>(f)].size());
        for (int k = 0; k < n; ++k) s.insert({f, k});
    }
}

void invert(const View& v, Selection& s) {
    Selection out;
    for (int f : v.faces) {
        const int n = static_cast<int>(v.mesh->faces[static_cast<std::size_t>(f)].size());
        for (int k = 0; k < n; ++k)
            if (!s.count({f, k})) out.insert({f, k});
    }
    s = std::move(out);
}

void selectBox(const View& v, Selection& s, SelectMode mode, glm::vec2 lo, glm::vec2 hi, bool remove) {
    const glm::vec2 a = glm::min(lo, hi), b = glm::max(lo, hi);
    auto inside = [&](glm::vec2 p) { return p.x >= a.x && p.x <= b.x && p.y >= a.y && p.y <= b.y; };
    std::vector<Loop> hit;
    if (mode == SelectMode::Vertex || mode == SelectMode::Edge) {
        for (std::size_t i = 0; i < v.faces.size(); ++i)
            for (std::size_t k = 0; k < v.uv[i].size(); ++k)
                if (inside(v.uv[i][k])) hit.push_back({v.faces[i], static_cast<int>(k)});
    } else {
        std::vector<int> picked;
        for (std::size_t i = 0; i < v.faces.size(); ++i) {
            glm::vec2 c(0.0f);
            for (const glm::vec2& q : v.uv[i]) c += q;
            if (inside(c / static_cast<float>(v.uv[i].size()))) picked.push_back(v.faces[i]);
        }
        if (mode == SelectMode::Island) {
            std::vector<int> whole;
            for (const std::vector<int>& isl : islands(v))
                for (int f : isl)
                    if (std::find(picked.begin(), picked.end(), f) != picked.end()) {
                        whole.insert(whole.end(), isl.begin(), isl.end());
                        break;
                    }
            picked = std::move(whole);
        }
        for (int f : picked) addFace(v, hit, f, mode == SelectMode::Face);
    }
    for (const Loop& l : hit) {
        if (remove) s.erase(l);
        else s.insert(l);
    }
}

void selectLinked(const View& v, Selection& s) {
    for (const std::vector<int>& isl : islands(v)) {
        bool any = false;
        for (int f : isl) {
            const int n = static_cast<int>(v.mesh->faces[static_cast<std::size_t>(f)].size());
            for (int k = 0; k < n && !any; ++k) any = s.count({f, k}) > 0;
            if (any) break;
        }
        if (!any) continue;
        for (int f : isl) {
            const int n = static_cast<int>(v.mesh->faces[static_cast<std::size_t>(f)].size());
            for (int k = 0; k < n; ++k) s.insert({f, k});
        }
    }
}

void selectPinned(const View& v, Selection& s) {
    const EditMesh& m = *v.mesh;
    for (int f : v.faces) {
        if (!m.hasCornerUvs(f)) continue;
        const auto& c = m.faceUV[static_cast<std::size_t>(f)].corners;
        for (int k = 0; k < static_cast<int>(c.size()); ++k)
            if (c[static_cast<std::size_t>(k)].pin) s.insert({f, k});
    }
}

void validate(const View& v, Selection& s) {
    for (auto it = s.begin(); it != s.end();) {
        const bool ok = v.shown(it->f) && it->k >= 0 &&
                        it->k < static_cast<int>(v.mesh->faces[static_cast<std::size_t>(it->f)].size());
        it = ok ? std::next(it) : s.erase(it);
    }
}

bool edgeSelected(const Selection& s, const EditMesh& m, int f, int k) {
    const int n = static_cast<int>(m.faces[static_cast<std::size_t>(f)].size());
    return s.count({f, k}) && s.count({f, (k + 1) % n});
}

bool faceSelected(const Selection& s, const EditMesh& m, int f) {
    const int n = static_cast<int>(m.faces[static_cast<std::size_t>(f)].size());
    for (int k = 0; k < n; ++k)
        if (!s.count({f, k})) return false;
    return n > 0;
}

std::vector<int> facesOf(const Selection& s) {
    std::vector<int> out;
    for (const Loop& l : s)
        if (out.empty() || out.back() != l.f) out.push_back(l.f);
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// --- Moving coordinates -----------------------------------------------------------------

void ensureCorners(EditMesh& m, const std::vector<int>& faces) {
    m.syncFaceUv();
    for (int f : faces) {
        if (!m.validFace(f) || m.hasCornerUvs(f)) continue;
        const std::vector<glm::vec2> uv = editmesh::faceUvs(m, f);
        std::vector<Corner> c;
        c.reserve(uv.size());
        for (const glm::vec2& q : uv) c.push_back({q, false});
        m.faceUV[static_cast<std::size_t>(f)].corners = std::move(c);
    }
}

bool bounds(const View& v, const Selection& s, glm::vec2& lo, glm::vec2& hi) {
    lo = glm::vec2(1e30f);
    hi = glm::vec2(-1e30f);
    bool any = false;
    for (const Loop& l : s) {
        if (!v.shown(l.f)) continue;
        const glm::vec2 p = v.at(l);
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
        any = true;
    }
    return any;
}

void transform(EditMesh& m, const Selection& s, const glm::mat2& a, glm::vec2 pivot, glm::vec2 t) {
    ensureCorners(m, facesOf(s));
    for (const Loop& l : s) {
        if (!m.hasCornerUvs(l.f)) continue;
        glm::vec2& u = corner(m, l.f, l.k).uv;
        u = a * (u - pivot) + pivot + t;
    }
}

void transformEach(EditMesh& m, const View& v, const Selection& s, const glm::mat2& a, glm::vec2 t) {
    for (const std::vector<int>& isl : islands(v)) {
        Selection part;
        for (int f : isl) {
            const int n = static_cast<int>(m.faces[static_cast<std::size_t>(f)].size());
            for (int k = 0; k < n; ++k)
                if (s.count({f, k})) part.insert({f, k});
        }
        glm::vec2 lo, hi;
        if (!bounds(v, part, lo, hi)) continue;
        transform(m, part, a, 0.5f * (lo + hi), t);
    }
}

void align(EditMesh& m, const View& v, const Selection& s, int axis) {
    if (s.empty()) return;
    float sum = 0.0f;
    int n = 0;
    for (const Loop& l : s)
        if (v.shown(l.f)) { sum += v.at(l)[axis]; ++n; }
    if (!n) return;
    ensureCorners(m, facesOf(s));
    for (const Loop& l : s)
        if (m.hasCornerUvs(l.f)) corner(m, l.f, l.k).uv[axis] = sum / static_cast<float>(n);
}

void mergeAtCenter(EditMesh& m, const View& v, const Selection& s) {
    glm::vec2 c(0.0f);
    int n = 0;
    for (const Loop& l : s)
        if (v.shown(l.f)) { c += v.at(l); ++n; }
    if (!n) return;
    c /= static_cast<float>(n);
    ensureCorners(m, facesOf(s));
    for (const Loop& l : s)
        if (m.hasCornerUvs(l.f)) corner(m, l.f, l.k).uv = c;
}

int mergeByDistance(EditMesh& m, const View& v, const Selection& s, float dist) {
    std::vector<Loop> ls;
    for (const Loop& l : s)
        if (v.shown(l.f)) ls.push_back(l);
    UnionFind uf(ls.size());
    for (std::size_t i = 0; i < ls.size(); ++i)
        for (std::size_t j = i + 1; j < ls.size(); ++j)
            if (glm::length(v.at(ls[i]) - v.at(ls[j])) <= dist) uf.unite(static_cast<int>(i), static_cast<int>(j));
    std::unordered_map<int, std::pair<glm::vec2, int>> sum;
    for (std::size_t i = 0; i < ls.size(); ++i) {
        auto& e = sum[uf.find(static_cast<int>(i))];
        e.first += v.at(ls[i]);
        ++e.second;
    }
    ensureCorners(m, facesOf(s));
    int moved = 0;
    for (std::size_t i = 0; i < ls.size(); ++i) {
        const auto& e = sum[uf.find(static_cast<int>(i))];
        const glm::vec2 c = e.first / static_cast<float>(e.second);
        glm::vec2& u = corner(m, ls[i].f, ls[i].k).uv;
        if (glm::length(u - c) > 1e-7f) ++moved;
        u = c;
    }
    return moved;
}

void snapToPixels(EditMesh& m, const Selection& s, int w, int h) {
    if (w <= 0 || h <= 0) return;
    ensureCorners(m, facesOf(s));
    for (const Loop& l : s) {
        if (!m.hasCornerUvs(l.f)) continue;
        glm::vec2& u = corner(m, l.f, l.k).uv;
        u = glm::vec2(std::round(u.x * float(w)) / float(w), std::round(u.y * float(h)) / float(h));
    }
}

void setPin(EditMesh& m, const Selection& s, bool on) {
    ensureCorners(m, facesOf(s));
    for (const Loop& l : s)
        if (m.hasCornerUvs(l.f)) corner(m, l.f, l.k).pin = on;
}

// --- Projections ------------------------------------------------------------------------

void cubeProject(EditMesh& m, const std::vector<int>& faces, float size) {
    m.syncFaceUv();
    size = std::max(size, 1e-3f);
    for (int f : validFaces(m, faces)) {
        const glm::vec3 n = glm::abs(m.faceNormal(f));
        EditMesh::FaceUV u;
        u.axis = (n.x >= n.y && n.x >= n.z) ? 1 : (n.y >= n.z ? 2 : 3);
        u.size = glm::vec2(size);
        const std::vector<glm::vec2> uv = editmesh::faceUvs(m, f, u);
        for (const glm::vec2& q : uv) u.corners.push_back({q, false});
        m.faceUV[static_cast<std::size_t>(f)] = std::move(u);
    }
}

namespace {
// Around the vertical axis through the middle of these faces: U from the
// angle, V from `vOf`. A face that straddles the back (U jumping from near 1
// to near 0) is taken round once more, so it does not stretch back across.
template <class VOf>
void roundProject(EditMesh& m, const std::vector<int>& fs, VOf vOf) {
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (int f : fs)
        for (int i : m.faces[static_cast<std::size_t>(f)]) {
            lo = glm::min(lo, m.verts[static_cast<std::size_t>(i)]);
            hi = glm::max(hi, m.verts[static_cast<std::size_t>(i)]);
        }
    const glm::vec3 c = 0.5f * (lo + hi);
    m.syncFaceUv();
    for (int f : fs) {
        const std::vector<int>& fv = m.faces[static_cast<std::size_t>(f)];
        const glm::vec3 fc = m.faceCenter(f);
        std::vector<Corner> out;
        for (int i : fv) {
            const glm::vec3 p = m.verts[static_cast<std::size_t>(i)];
            glm::vec2 d(p.x - c.x, p.z - c.z);
            // On the axis itself there is no angle: the face's own middle has one.
            if (glm::dot(d, d) < 1e-10f) d = glm::vec2(fc.x - c.x, fc.z - c.z);
            const float u = std::atan2(d.y, d.x) / (2.0f * kPi) + 0.5f;
            out.push_back({glm::vec2(u, vOf(p, c, lo, hi)), false});
        }
        float umin = 1e30f, umax = -1e30f;
        for (const Corner& k : out) { umin = std::min(umin, k.uv.x); umax = std::max(umax, k.uv.x); }
        if (umax - umin > 0.5f)
            for (Corner& k : out)
                if (k.uv.x < 0.5f) k.uv.x += 1.0f;
        m.faceUV[static_cast<std::size_t>(f)].corners = std::move(out);
    }
}
} // namespace

void cylinderProject(EditMesh& m, const std::vector<int>& faces) {
    roundProject(m, validFaces(m, faces), [](glm::vec3 p, glm::vec3, glm::vec3 lo, glm::vec3 hi) {
        return hi.y - lo.y > 1e-6f ? (p.y - lo.y) / (hi.y - lo.y) : 0.5f;
    });
}

void sphereProject(EditMesh& m, const std::vector<int>& faces) {
    roundProject(m, validFaces(m, faces), [](glm::vec3 p, glm::vec3 c, glm::vec3, glm::vec3) {
        const glm::vec3 d = p - c;
        const float len = glm::length(d);
        return len > 1e-8f ? std::asin(std::clamp(d.y / len, -1.0f, 1.0f)) / kPi + 0.5f : 0.5f;
    });
}

void resetUvs(EditMesh& m, const std::vector<int>& faces) {
    m.syncFaceUv();
    for (int f : validFaces(m, faces)) {
        EditMesh::FaceUV& u = m.faceUV[static_cast<std::size_t>(f)];
        u.corners.clear();
        const std::size_t n = m.faces[static_cast<std::size_t>(f)].size();
        std::vector<Corner> out;
        if (n == 4) {
            out = {{{0, 0}, false}, {{1, 0}, false}, {{1, 1}, false}, {{0, 1}, false}};
        } else {
            // Its own shape, fitted into the tile.
            EditMesh::FaceUV plain;
            const std::vector<glm::vec2> q = editmesh::faceUvs(m, f, plain);
            glm::vec2 lo(1e30f), hi(-1e30f);
            for (const glm::vec2& p : q) { lo = glm::min(lo, p); hi = glm::max(hi, p); }
            const float ext = std::max({hi.x - lo.x, hi.y - lo.y, 1e-6f});
            const glm::vec2 off = 0.5f * (glm::vec2(1.0f) - (hi - lo) / ext);
            for (const glm::vec2& p : q) out.push_back({(p - lo) / ext + off, false});
        }
        u.corners = std::move(out);
    }
}

void projectWorld(EditMesh& m, const std::vector<int>& faces, const EditMesh::FaceUV& placement) {
    m.syncFaceUv();
    EditMesh::FaceUV u = placement;
    u.corners.clear();
    for (int f : validFaces(m, faces)) m.faceUV[static_cast<std::size_t>(f)] = u;
}

int smartProject(EditMesh& m, const std::vector<int>& faces, float angleLimit, float margin) {
    const std::vector<int> fs = validFaces(m, faces);
    if (fs.empty()) return 0;
    const float cosLimit = std::cos(glm::radians(std::clamp(angleLimit, 1.0f, 89.0f)));
    // The directions: the biggest face's first, then every face that faces
    // away from all of them by more than the limit brings its own.
    std::vector<int> bySize = fs;
    std::sort(bySize.begin(), bySize.end(), [&](int a, int b) { return m.faceArea(a) > m.faceArea(b); });
    std::vector<glm::vec3> dirs;
    for (int f : bySize) {
        const glm::vec3 n = m.faceNormal(f);
        bool near = false;
        for (const glm::vec3& d : dirs) near = near || glm::dot(n, d) >= cosLimit;
        if (!near) dirs.push_back(n);
    }
    auto closest = [&](const glm::vec3& n) {
        int best = 0;
        for (int i = 1; i < static_cast<int>(dirs.size()); ++i)
            if (glm::dot(n, dirs[static_cast<std::size_t>(i)]) > glm::dot(n, dirs[static_cast<std::size_t>(best)])) best = i;
        return best;
    };
    // Each direction the area-weighted mean of its faces': the plane they lie
    // flattest on.
    std::vector<glm::vec3> mean(dirs.size(), glm::vec3(0.0f));
    for (int f : fs) mean[static_cast<std::size_t>(closest(m.faceNormal(f)))] += m.faceNormal(f) * m.faceArea(f);
    for (std::size_t i = 0; i < dirs.size(); ++i)
        if (glm::length(mean[i]) > 1e-9f) dirs[i] = glm::normalize(mean[i]);
    std::unordered_map<int, int> dirOf;
    for (int f : fs) dirOf[f] = closest(m.faceNormal(f));
    // Pieces: faces of one direction joined across their shared edges.
    std::unordered_map<int, int> slotOf;
    for (std::size_t i = 0; i < fs.size(); ++i) slotOf[fs[i]] = static_cast<int>(i);
    UnionFind uf(fs.size());
    for (const auto& [key, es] : edgeMap(m, fs))
        for (std::size_t i = 0; i + 1 < es.size(); ++i)
            for (std::size_t j = i + 1; j < es.size(); ++j)
                if (dirOf[es[i].f] == dirOf[es[j].f]) uf.unite(slotOf[es[i].f], slotOf[es[j].f]);
    std::unordered_map<int, std::size_t> idx;
    std::vector<std::vector<int>> groups;
    for (std::size_t i = 0; i < fs.size(); ++i) {
        auto [it, fresh] = idx.try_emplace(uf.find(static_cast<int>(i)), groups.size());
        if (fresh) groups.emplace_back();
        groups[it->second].push_back(fs[i]);
    }
    m.syncFaceUv();
    for (const std::vector<int>& g : groups) {
        glm::vec3 tx, ty;
        basis(dirs[static_cast<std::size_t>(dirOf[g.front()])], tx, ty);
        for (int f : g) {
            std::vector<Corner> c;
            for (int i : m.faces[static_cast<std::size_t>(f)]) {
                const glm::vec3& p = m.verts[static_cast<std::size_t>(i)];
                c.push_back({glm::vec2(glm::dot(p, tx), glm::dot(p, ty)), false});
            }
            m.faceUV[static_cast<std::size_t>(f)].corners = std::move(c);
        }
    }
    packGroups(m, groups, margin, true);
    return static_cast<int>(groups.size());
}

int unwrap(EditMesh& m, const std::vector<int>& faces, float margin, int* closed) {
    if (closed) *closed = 0;
    const std::vector<int> fs = validFaces(m, faces);
    if (fs.empty()) return 0;
    m.syncFaceUv();
    // Every loop of these faces numbered: base[slot] + k.
    std::unordered_map<int, int> slotOf;
    std::vector<int> base(fs.size() + 1, 0);
    for (std::size_t i = 0; i < fs.size(); ++i) {
        slotOf[fs[i]] = static_cast<int>(i);
        base[i + 1] = base[i] + static_cast<int>(m.faces[static_cast<std::size_t>(fs[i])].size());
    }
    auto loopId = [&](int f, int k) { return base[static_cast<std::size_t>(slotOf[f])] + k; };
    // Faces join across every edge that is not a seam and borders exactly two
    // of them; their loops at the edge's two ends become one chart corner.
    UnionFind faceUf(fs.size()), loopUf(static_cast<std::size_t>(base.back()));
    const auto em = edgeMap(m, fs);
    for (const auto& [key, es] : em) {
        if (es.size() != 2) continue;
        const std::vector<int>& fv = m.faces[static_cast<std::size_t>(es[0].f)];
        const int a = fv[static_cast<std::size_t>(es[0].k)], b = fv[static_cast<std::size_t>(next(fv, es[0].k))];
        if (m.isSeam(a, b)) continue;
        const int ga = cornerOf(m, es[1].f, a), gb = cornerOf(m, es[1].f, b);
        if (ga < 0 || gb < 0) continue;
        faceUf.unite(slotOf[es[0].f], slotOf[es[1].f]);
        loopUf.unite(loopId(es[0].f, es[0].k), loopId(es[1].f, ga));
        loopUf.unite(loopId(es[0].f, next(fv, es[0].k)), loopId(es[1].f, gb));
    }
    std::unordered_map<int, std::size_t> chartIdx;
    std::vector<std::vector<int>> charts;
    for (std::size_t i = 0; i < fs.size(); ++i) {
        auto [it, fresh] = chartIdx.try_emplace(faceUf.find(static_cast<int>(i)), charts.size());
        if (fresh) charts.emplace_back();
        charts[it->second].push_back(fs[i]);
    }

    std::vector<std::vector<int>> toPack;
    for (const std::vector<int>& chart : charts) {
        // Open somewhere: an edge with one face of these, or a seam.
        bool open = false;
        for (int f : chart) {
            const std::vector<int>& fv = m.faces[static_cast<std::size_t>(f)];
            for (int k = 0; k < static_cast<int>(fv.size()) && !open; ++k) {
                const int a = fv[static_cast<std::size_t>(k)], b = fv[static_cast<std::size_t>(next(fv, k))];
                const auto it = em.find(edgeKey(a, b));
                open = it == em.end() || it->second.size() != 2 || m.isSeam(a, b);
            }
            if (open) break;
        }
        if (!open && closed) ++*closed;
        // The chart's corners: one per joined group of loops.
        std::unordered_map<int, int> vertOf;   // loop-UF root -> chart corner
        std::vector<glm::vec3> pos;
        std::vector<int> fixedSrc;             // a pinned loop there, or -1
        std::vector<glm::vec2> fixedUv;
        auto cv = [&](int f, int k) {
            const int r = loopUf.find(loopId(f, k));
            auto [it, fresh] = vertOf.try_emplace(r, static_cast<int>(pos.size()));
            if (fresh) {
                pos.push_back(m.verts[static_cast<std::size_t>(m.faces[static_cast<std::size_t>(f)][static_cast<std::size_t>(k)])]);
                fixedSrc.push_back(-1);
                fixedUv.emplace_back(0.0f);
            }
            return it->second;
        };
        for (int f : chart)
            for (int k = 0; k < static_cast<int>(m.faces[static_cast<std::size_t>(f)].size()); ++k) {
                const int c = cv(f, k);
                if (m.hasCornerUvs(f) && corner(m, f, k).pin && fixedSrc[static_cast<std::size_t>(c)] < 0) {
                    fixedSrc[static_cast<std::size_t>(c)] = 1;
                    fixedUv[static_cast<std::size_t>(c)] = corner(m, f, k).uv;
                }
            }
        const int n = static_cast<int>(pos.size());
        int pinned = 0;
        for (int c : fixedSrc) pinned += c >= 0;
        // A start that is already close: the chart flattened along its mean normal.
        glm::vec3 nrm(0.0f);
        for (int f : chart) nrm += m.faceNormal(f) * m.faceArea(f);
        glm::vec3 tx, ty;
        basis(nrm, tx, ty);
        std::vector<glm::vec2> init(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) init[static_cast<std::size_t>(i)] = {glm::dot(pos[static_cast<std::size_t>(i)], tx), glm::dot(pos[static_cast<std::size_t>(i)], ty)};
        // Fewer than two pins: the two corners furthest apart hold it.
        bool autoPins = false;
        if (pinned < 2 && n >= 2) {
            autoPins = true;
            auto far = [&](int from) {
                int best = from;
                float bd = -1.0f;
                for (int i = 0; i < n; ++i) {
                    const float d = glm::length(pos[static_cast<std::size_t>(i)] - pos[static_cast<std::size_t>(from)]);
                    if (d > bd) { bd = d; best = i; }
                }
                return best;
            };
            const int a = far(0), b = far(a);
            for (int i = 0; i < n; ++i) fixedSrc[static_cast<std::size_t>(i)] = -1;
            fixedSrc[static_cast<std::size_t>(a)] = 1;
            fixedSrc[static_cast<std::size_t>(b)] = 1;
            fixedUv[static_cast<std::size_t>(a)] = init[static_cast<std::size_t>(a)];
            fixedUv[static_cast<std::size_t>(b)] = init[static_cast<std::size_t>(b)];
        }
        // The unknowns: (u, v) of every corner not held.
        std::vector<int> var(static_cast<std::size_t>(n), -1);
        int nv = 0;
        for (int i = 0; i < n; ++i)
            if (fixedSrc[static_cast<std::size_t>(i)] < 0) var[static_cast<std::size_t>(i)] = nv++;
        std::vector<double> x(static_cast<std::size_t>(nv) * 2);
        for (int i = 0; i < n; ++i)
            if (var[static_cast<std::size_t>(i)] >= 0) {
                x[static_cast<std::size_t>(var[static_cast<std::size_t>(i)]) * 2]     = init[static_cast<std::size_t>(i)].x;
                x[static_cast<std::size_t>(var[static_cast<std::size_t>(i)]) * 2 + 1] = init[static_cast<std::size_t>(i)].y;
            }
        // Two rows per triangle: the real and imaginary part of
        // sum_j W_j (u_j + i v_j), which is zero exactly where the map keeps
        // the triangle's angles (Levy et al., LSCM).
        std::vector<Row> rows;
        double area3 = 0.0;
        for (int f : chart) {
            const int nc = static_cast<int>(m.faces[static_cast<std::size_t>(f)].size());
            for (int t = 1; t + 1 < nc; ++t) {
                const int id[3] = {cv(f, 0), cv(f, t), cv(f, t + 1)};
                if (id[0] == id[1] || id[1] == id[2] || id[0] == id[2]) continue;
                const glm::vec3 p0 = pos[static_cast<std::size_t>(id[0])], p1 = pos[static_cast<std::size_t>(id[1])], p2 = pos[static_cast<std::size_t>(id[2])];
                const glm::vec3 e1 = p1 - p0, e2 = p2 - p0;
                const glm::vec3 cr = glm::cross(e1, e2);
                const float twiceA = glm::length(cr);
                if (twiceA < 1e-12f || glm::length(e1) < 1e-9f) continue;
                area3 += 0.5 * twiceA;
                const glm::vec3 X = glm::normalize(e1), Y = glm::normalize(glm::cross(cr, X));
                const double lx[3] = {0.0, glm::length(e1), glm::dot(e2, X)};
                const double ly[3] = {0.0, 0.0, glm::dot(e2, Y)};
                const double w = 1.0 / std::sqrt(double(twiceA));
                Row re, im;
                for (int j = 0; j < 3; ++j) {
                    const int j1 = (j + 1) % 3, j2 = (j + 2) % 3;
                    const double a = (lx[j2] - lx[j1]) * w, b = (ly[j2] - ly[j1]) * w;
                    const int c = id[j];
                    if (var[static_cast<std::size_t>(c)] >= 0) {
                        const int q = var[static_cast<std::size_t>(c)];
                        re.col[re.n] = 2 * q;     re.val[re.n++] = a;
                        re.col[re.n] = 2 * q + 1; re.val[re.n++] = -b;
                        im.col[im.n] = 2 * q;     im.val[im.n++] = b;
                        im.col[im.n] = 2 * q + 1; im.val[im.n++] = a;
                    } else {
                        const glm::vec2 u = fixedUv[static_cast<std::size_t>(c)];
                        re.rhs -= a * u.x - b * u.y;
                        im.rhs -= b * u.x + a * u.y;
                    }
                }
                rows.push_back(re);
                rows.push_back(im);
            }
        }
        cgls(rows, x, std::clamp(nv * 8, 400, 8000));
        std::vector<glm::vec2> uv(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            const int q = var[static_cast<std::size_t>(i)];
            uv[static_cast<std::size_t>(i)] = q >= 0 ? glm::vec2(float(x[static_cast<std::size_t>(q) * 2]), float(x[static_cast<std::size_t>(q) * 2 + 1]))
                                                     : fixedUv[static_cast<std::size_t>(i)];
        }
        // Held by its own pins: it stays where they are. Otherwise its size in
        // metres, so the charts pack at one density.
        if (autoPins) {
            double areaUv = 0.0;
            for (int f : chart) {
                std::vector<glm::vec2> poly;
                for (int k = 0; k < static_cast<int>(m.faces[static_cast<std::size_t>(f)].size()); ++k)
                    poly.push_back(uv[static_cast<std::size_t>(cv(f, k))]);
                areaUv += std::abs(polyArea(poly));
            }
            if (areaUv > 1e-12 && area3 > 1e-12) {
                const float k = float(std::sqrt(area3 / areaUv));
                for (glm::vec2& u : uv) u *= k;
            }
        }
        for (int f : chart) {
            const int nc = static_cast<int>(m.faces[static_cast<std::size_t>(f)].size());
            std::vector<Corner> c(static_cast<std::size_t>(nc));
            for (int k = 0; k < nc; ++k) {
                c[static_cast<std::size_t>(k)].uv  = uv[static_cast<std::size_t>(cv(f, k))];
                c[static_cast<std::size_t>(k)].pin = m.hasCornerUvs(f) && corner(m, f, k).pin;
            }
            m.faceUV[static_cast<std::size_t>(f)].corners = std::move(c);
        }
        if (autoPins) toPack.push_back(chart);
    }
    packGroups(m, toPack, margin, true);
    return static_cast<int>(charts.size());
}

// --- Islands ----------------------------------------------------------------------------

void pack(EditMesh& m, const std::vector<int>& faces, float margin, bool rotate) {
    const std::vector<std::vector<int>> isl = islands(view(m, faces));
    packGroups(m, isl, margin, rotate);
}

void averageScale(EditMesh& m, const std::vector<int>& faces) {
    const View v = view(m, faces);
    const std::vector<std::vector<int>> isl = islands(v);
    std::vector<double> a3(isl.size(), 0.0), au(isl.size(), 0.0);
    for (std::size_t i = 0; i < isl.size(); ++i)
        for (int f : isl[i]) {
            a3[i] += m.faceArea(f);
            au[i] += std::abs(polyArea(v.uv[static_cast<std::size_t>(v.slot[static_cast<std::size_t>(f)])]));
        }
    double num = 0.0, den = 0.0;
    for (std::size_t i = 0; i < isl.size(); ++i)
        if (a3[i] > 1e-12 && au[i] > 1e-12) { num += std::sqrt(au[i] / a3[i]) * a3[i]; den += a3[i]; }
    if (den <= 0.0) return;
    const double target = num / den;
    for (std::size_t i = 0; i < isl.size(); ++i) {
        if (a3[i] <= 1e-12 || au[i] <= 1e-12) continue;
        const float k = float(target / std::sqrt(au[i] / a3[i]));
        Selection s;
        for (int f : isl[i])
            for (int c = 0; c < static_cast<int>(m.faces[static_cast<std::size_t>(f)].size()); ++c) s.insert({f, c});
        glm::vec2 lo, hi;
        if (!bounds(v, s, lo, hi)) continue;
        transform(m, s, glm::mat2(k), 0.5f * (lo + hi), glm::vec2(0.0f));
    }
}

// --- Seams ------------------------------------------------------------------------------

int seamsFromIslands(EditMesh& m, const std::vector<int>& faces) {
    const View v = view(m, faces);
    int marked = 0;
    for (const auto& [key, es] : edgeMap(m, v.faces)) {
        if (es.size() != 2) continue;
        const std::vector<int>& fv = m.faces[static_cast<std::size_t>(es[0].f)];
        const int a = fv[static_cast<std::size_t>(es[0].k)], b = fv[static_cast<std::size_t>(next(fv, es[0].k))];
        const int oa = cornerOf(m, es[1].f, a), ob = cornerOf(m, es[1].f, b);
        if (oa < 0 || ob < 0) continue;
        const bool welded = glm::length(v.at(es[0]) - v.at({es[1].f, oa})) < kWeld &&
                            glm::length(v.at({es[0].f, next(fv, es[0].k)}) - v.at({es[1].f, ob})) < kWeld;
        if (!welded && !m.isSeam(a, b)) {
            m.setSeam(a, b, true);
            ++marked;
        }
    }
    return marked;
}

int seamsFromSharp(EditMesh& m, const std::vector<int>& faces, float angle) {
    const float cosA = std::cos(glm::radians(std::clamp(angle, 0.0f, 180.0f)));
    int marked = 0;
    for (const auto& [key, es] : edgeMap(m, validFaces(m, faces))) {
        if (es.size() != 2) continue;
        if (glm::dot(m.faceNormal(es[0].f), m.faceNormal(es[1].f)) >= cosA) continue;
        const std::vector<int>& fv = m.faces[static_cast<std::size_t>(es[0].f)];
        const int a = fv[static_cast<std::size_t>(es[0].k)], b = fv[static_cast<std::size_t>(next(fv, es[0].k))];
        if (!m.isSeam(a, b)) {
            m.setSeam(a, b, true);
            ++marked;
        }
    }
    return marked;
}

// --- Measuring --------------------------------------------------------------------------

std::vector<float> stretch(const View& v) {
    const EditMesh& m = *v.mesh;
    std::vector<float> ratio(v.faces.size(), 0.0f);
    double num = 0.0, den = 0.0;
    for (std::size_t i = 0; i < v.faces.size(); ++i) {
        const float a3 = m.faceArea(v.faces[i]);
        const float au = std::abs(polyArea(v.uv[i]));
        ratio[i] = a3 > 1e-12f ? au / a3 : 0.0f;
        num += double(au);
        den += double(a3);
    }
    const double mean = den > 0.0 ? num / den : 1.0;
    for (float& r : ratio) r = r > 0.0f && mean > 0.0 ? std::clamp(float(std::log2(r / mean)), -4.0f, 4.0f) : -4.0f;
    return ratio;
}

} // namespace uvops
