// Spin and copies: the EditMesh operations that repeat something under a
// transform -- a profile turned round an axis, faces copied into a row or along
// a spline path.
//
// Same rules as the rest of the toolbox (EditMeshDetail.hpp): corners go in
// through addVert so `paint` stays parallel to `verts`, faces through
// addFaceLike so the material and texture placement stay parallel to `faces`,
// and nothing that exists is renumbered -- the new geometry is appended, which is
// what keeps the host's selection pointing at the same faces afterwards.

#include "EditMesh.hpp"
#include "EditMeshDetail.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

#include <glm/gtc/matrix_transform.hpp>

using namespace editmesh::detail;

namespace {

constexpr float kTwoPi = 6.28318530718f;

// Does face `f` run the edge x -> y (rather than y -> x, or not at all)?
bool runs(const EditMesh& m, int f, int x, int y) {
    const std::vector<int>& fv = m.faces[f];
    for (std::size_t i = 0; i < fv.size(); ++i)
        if (fv[i] == x && fv[(i + 1) % fv.size()] == y) return true;
    return false;
}

// A loop with the corners that repeat next to each other squeezed out (a quad
// with one end on the spin axis names that corner twice).
std::vector<int> squeeze(std::vector<int> loop) {
    std::vector<int> out;
    for (int v : loop)
        if (out.empty() || out.back() != v) out.push_back(v);
    while (out.size() > 1 && out.front() == out.back()) out.pop_back();
    return out;
}

} // namespace

namespace editmesh {

// =============================================================================
// Spin
// =============================================================================

std::vector<std::pair<int, int>> spinEdges(EditMesh& m,
                                           const std::vector<std::pair<int, int>>& sel,
                                           const glm::mat4& step, int steps, bool close) {
    std::vector<std::pair<int, int>> out;
    steps = std::clamp(steps, 1, 512);
    // A closed turn needs three steps at least: with two, the way there and the
    // way back are the same quads, and with one there is no way anywhere.
    if (close) steps = std::max(steps, 3);
    const int nv = static_cast<int>(m.verts.size());
    const EdgeMap em = buildEdges(m);

    // The edges that exist, once each. `natural` is the direction the face
    // beside it runs it -- on a border edge, the ONLY face, whose surface the
    // sweep should continue.
    struct E { int a, b, src; bool border, natural; };
    std::vector<E> es;
    std::unordered_set<std::uint64_t> seen;
    for (const auto& e : sel) {
        const int a = e.first, b = e.second;
        if (a < 0 || b < 0 || a >= nv || b >= nv || a == b) continue;
        const std::uint64_t key = edgeKey(a, b);
        if (!seen.insert(key).second) continue;
        auto it = em.find(key);
        if (it == em.end()) continue;
        const int f0 = it->second.first;
        es.push_back({a, b, f0, it->second.second < 0, runs(m, f0, a, b)});
    }
    if (es.empty()) return out;
    const int n = static_cast<int>(es.size());

    // Orient each connected strip ONE way along it. A quad for the edge t -> h
    // runs the ring edge at t forwards and the one at h backwards, so two edges
    // meeting at a corner agree when that corner is the head of one and the tail
    // of the other. Strips with a border edge are seeded from it; then, if most
    // of their border edges came out against their faces, the whole strip turns.
    std::unordered_map<int, std::vector<int>> at;
    for (int i = 0; i < n; ++i) { at[es[i].a].push_back(i); at[es[i].b].push_back(i); }
    std::vector<int> fwd(n, 1), comp(n, -1);
    int nc = 0;
    for (int pass = 0; pass < 2; ++pass)
        for (int seed = 0; seed < n; ++seed) {
            if (comp[seed] >= 0 || (pass == 0 && !es[seed].border)) continue;
            comp[seed] = nc;
            fwd[seed]  = es[seed].natural ? 1 : 0;
            std::vector<int> queue{seed};
            for (std::size_t q = 0; q < queue.size(); ++q) {
                const int e    = queue[q];
                const int tail = fwd[e] ? es[e].a : es[e].b;
                const int head = fwd[e] ? es[e].b : es[e].a;
                for (int e2 : at[head])
                    if (comp[e2] < 0) { comp[e2] = nc; fwd[e2] = es[e2].a == head; queue.push_back(e2); }
                for (int e2 : at[tail])
                    if (comp[e2] < 0) { comp[e2] = nc; fwd[e2] = es[e2].b == tail; queue.push_back(e2); }
            }
            ++nc;
        }
    std::vector<int> agree(nc, 0), clash(nc, 0);
    for (int i = 0; i < n; ++i) {
        if (!es[i].border) continue;
        if ((fwd[i] != 0) == es[i].natural) ++agree[comp[i]];
        else                                ++clash[comp[i]];
    }
    for (int i = 0; i < n; ++i)
        if (clash[comp[i]] > agree[comp[i]]) fwd[i] ^= 1;

    // The rings: every corner of the profile, stepped round. A corner the step
    // leaves where it is lies on the axis and stays one corner.
    std::vector<int> corners;
    for (const E& e : es) { corners.push_back(e.a); corners.push_back(e.b); }
    corners = uniqueVerts(m, corners);
    glm::vec3 mn, mx;
    m.bounds(mn, mx);
    const float eps = 1e-5f * std::max(1.0f, glm::length(mx - mn));
    std::unordered_set<int> fixedOnAxis;
    for (int v : corners) {
        const glm::vec3 p = m.verts[v];
        if (glm::length(glm::vec3(step * glm::vec4(p, 1.0f)) - p) < eps) fixedOnAxis.insert(v);
    }

    const int last = close ? steps - 1 : steps;   // the last ring that gets corners
    std::vector<std::unordered_map<int, int>> ring(last + 1);
    {
        // Powers of the step in double: two hundred steps of float round-off is
        // a visible seam on a closed turn.
        const glm::dmat4 S(step);
        glm::dmat4 acc(1.0);
        const std::vector<glm::vec3> P(m.verts.begin(), m.verts.end());
        for (int k = 1; k <= last; ++k) {
            acc = S * acc;
            for (int v : corners) {
                if (fixedOnAxis.count(v)) continue;
                const glm::dvec4 q = acc * glm::dvec4(glm::dvec3(P[v]), 1.0);
                ring[k][v] = addVert(m, glm::vec3(q), m.paintAt(v));
            }
        }
    }
    auto R = [&](int k, int v) {
        if (k == 0 || (close && k == steps) || fixedOnAxis.count(v)) return v;
        return ring[k].at(v);
    };

    std::vector<std::vector<int>> made(nc);
    for (int i = 0; i < n; ++i) {
        const int t = fwd[i] ? es[i].a : es[i].b;
        const int h = fwd[i] ? es[i].b : es[i].a;
        if (fixedOnAxis.count(t) && fixedOnAxis.count(h)) continue;   // the axis itself
        for (int k = 0; k < steps; ++k) {
            std::vector<int> loop = squeeze({R(k, h), R(k, t), R(k + 1, t), R(k + 1, h)});
            if (loop.size() < 3) continue;
            made[comp[i]].push_back(addFaceLike(m, std::move(loop), es[i].src));
        }
        const int kEnd = close ? 0 : steps;
        const int ea = R(kEnd, es[i].a), eb = R(kEnd, es[i].b);
        out.push_back({std::min(ea, eb), std::max(ea, eb)});
    }

    // A strip with no border had no face to agree with: let it look out, by the
    // volume it sweeps measured from its own middle -- the same test "recalculate
    // outside" uses.
    for (int c = 0; c < nc; ++c) {
        if (agree[c] + clash[c] > 0 || made[c].empty()) continue;
        glm::vec3 mid(0.0f);
        int cnt = 0;
        for (int f : made[c])
            for (int v : m.faces[f]) { mid += m.verts[v]; ++cnt; }
        mid /= static_cast<float>(std::max(cnt, 1));
        float vol = 0.0f;
        for (int f : made[c]) {
            const std::vector<int>& fv = m.faces[f];
            for (std::size_t i = 1; i + 1 < fv.size(); ++i)
                vol += glm::dot(m.verts[fv[0]] - mid,
                                glm::cross(m.verts[fv[i]] - mid, m.verts[fv[i + 1]] - mid));
        }
        if (vol < 0.0f)
            for (int f : made[c]) std::reverse(m.faces[f].begin() + 1, m.faces[f].end());
    }
    return out;
}

glm::mat4 spinStep(const glm::mat4& model, const glm::vec3& centre, const glm::vec3& axis,
                   float angle, float rise, int steps) {
    steps = std::max(1, steps);
    const glm::vec3 ax = glm::length(axis) > 1e-6f ? glm::normalize(axis) : glm::vec3(0, 1, 0);
    const float     k  = 1.0f / static_cast<float>(steps);
    const glm::mat4 W  = glm::translate(glm::mat4(1.0f), centre + ax * (rise * k)) *
                         glm::rotate(glm::mat4(1.0f), angle * k, ax) *
                         glm::translate(glm::mat4(1.0f), -centre);
    return glm::inverse(model) * W * model;
}

bool spinCloses(float angle, float rise) {
    return std::fabs(std::fabs(angle) - kTwoPi) < 1e-3f && std::fabs(rise) < 1e-6f;
}

// =============================================================================
// Copies
// =============================================================================

std::vector<int> duplicateFacesAt(EditMesh& m, const std::vector<int>& faces,
                                  const std::vector<glm::mat4>& xforms) {
    std::vector<int> fs;
    for (int f : faces)
        if (okFace(m, f) && std::find(fs.begin(), fs.end(), f) == fs.end()) fs.push_back(f);
    std::vector<int> out;
    for (const glm::mat4& X : xforms) {
        const bool mirror = glm::determinant(glm::mat3(X)) < 0.0f;
        std::unordered_map<int, int> copy;   // per set: the sets are separate pieces
        for (int f : fs) {
            const std::vector<int> fv = m.faces[f];   // by value: m.faces grows
            std::vector<int> loop;
            for (int v : fv) {
                auto it = copy.find(v);
                if (it == copy.end()) {
                    const glm::vec3 p(X * glm::vec4(m.verts[v], 1.0f));
                    it = copy.emplace(v, addVert(m, p, m.paintAt(v))).first;
                }
                loop.push_back(it->second);
            }
            if (mirror) std::reverse(loop.begin() + 1, loop.end());
            out.push_back(addFaceLike(m, std::move(loop), f));
        }
    }
    return out;
}

float lineLength(const std::vector<glm::vec3>& line, bool closed) {
    float len = 0.0f;
    for (std::size_t i = 1; i < line.size(); ++i) len += glm::length(line[i] - line[i - 1]);
    if (closed && line.size() > 2) len += glm::length(line.front() - line.back());
    return len;
}

std::vector<glm::mat4> framesAlong(const std::vector<glm::vec3>& lineIn, bool closed,
                                   int count, bool align) {
    std::vector<glm::mat4> out;
    std::vector<glm::vec3> line = lineIn;
    // A closed line may or may not repeat its first point at the end; the
    // closing segment is added here either way, so drop the repeat.
    if (closed && line.size() > 2 && glm::length(line.front() - line.back()) < 1e-4f)
        line.pop_back();
    if (line.size() < 2 || count < 1) return out;
    if (closed) line.push_back(line.front());

    std::vector<float> cum(line.size(), 0.0f);
    for (std::size_t i = 1; i < line.size(); ++i)
        cum[i] = cum[i - 1] + glm::length(line[i] - line[i - 1]);
    const float total = cum.back();
    if (total <= 1e-6f) return out;

    std::size_t seg = 1;
    for (int i = 0; i < count; ++i) {
        const float s = closed      ? total * static_cast<float>(i) / static_cast<float>(count)
                        : count > 1 ? total * static_cast<float>(i) / static_cast<float>(count - 1)
                                    : 0.0f;
        // `<=`: a place exactly on a corner of the line belongs to the segment
        // that LEAVES it, so it faces where the line goes on to, not where it
        // came from.
        while (seg + 1 < line.size() && cum[seg] <= s) ++seg;
        const float     segLen = cum[seg] - cum[seg - 1];
        const float     u      = segLen > 1e-9f ? (s - cum[seg - 1]) / segLen : 0.0f;
        const glm::vec3 p      = glm::mix(line[seg - 1], line[seg], std::clamp(u, 0.0f, 1.0f));

        glm::mat4 F(1.0f);
        if (align) {
            glm::vec3 X = line[seg] - line[seg - 1];
            X = glm::length(X) > 1e-9f ? glm::normalize(X) : glm::vec3(1, 0, 0);
            // Up, tipped only as far as the slope makes it: a copy on a ramp leans
            // with the ramp, one on the flat stands straight.
            glm::vec3 Y = glm::vec3(0, 1, 0) - X * X.y;
            if (glm::length(Y) < 1e-4f) Y = glm::vec3(1, 0, 0) - X * X.x;   // straight up the line
            Y = glm::normalize(Y);
            const glm::vec3 Z = glm::cross(X, Y);
            F[0] = glm::vec4(X, 0.0f);
            F[1] = glm::vec4(Y, 0.0f);
            F[2] = glm::vec4(Z, 0.0f);
        }
        F[3] = glm::vec4(p, 1.0f);
        out.push_back(F);
    }
    return out;
}

} // namespace editmesh
