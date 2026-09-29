#include "EditMeshModifiers.hpp"
#include "EditMeshDetail.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "MeshSimplify.hpp"

namespace editmesh {

using detail::edgeKey;

namespace {

// Which of the parallel arrays the input carries: an output fills exactly
// those, so a mesh with none of them stays a mesh with none of them.
struct Carried {
    bool paint = false, mat = false, uv = false;
    explicit Carried(const EditMesh& m)
        : paint(!m.paint.empty()), mat(!m.faceMat.empty()), uv(!m.faceUV.empty()) {}
};

// Append face `loop` to `out`, wearing what face `src` of `from` wears.
void addFaceFrom(EditMesh& out, std::vector<int> loop, const EditMesh& from, int src,
                 const Carried& c) {
    out.faces.push_back(std::move(loop));
    if (c.mat) out.faceMat.push_back(from.faceMaterial(src));
    if (c.uv)  out.faceUV.push_back(from.faceUv(src));
}

int addCorner(EditMesh& out, const glm::vec3& p, const glm::vec4& w, const Carried& c) {
    out.verts.push_back(p);
    if (c.paint) out.paint.push_back(w);
    return static_cast<int>(out.verts.size()) - 1;
}

// A mesh with the frame and shading of `src` and nothing in it yet.
EditMesh emptyLike(const EditMesh& src) {
    EditMesh out;
    out.verts.clear();
    out.faces.clear();
    out.hasFrame    = src.hasFrame;
    out.frameMin    = src.frameMin;
    out.frameMax    = src.frameMax;
    out.smoothAngle = src.smoothAngle;
    return out;
}

// Append all of `from` to `out` (corners shifted by `shift`), wearing what it
// wears. Returns the index its first corner landed at.
int appendMesh(EditMesh& out, const EditMesh& from, const glm::vec3& shift, const Carried& c) {
    const int base = static_cast<int>(out.verts.size());
    for (std::size_t v = 0; v < from.verts.size(); ++v)
        addCorner(out, from.verts[v] + shift, from.paintAt(static_cast<int>(v)), c);
    for (std::size_t f = 0; f < from.faces.size(); ++f) {
        std::vector<int> loop = from.faces[f];
        for (int& v : loop) v += base;
        addFaceFrom(out, std::move(loop), from, static_cast<int>(f), c);
    }
    return base;
}

// One level of subdivision (see subdivideSurface).
void subdivideOnce(EditMesh& m, bool smooth) {
    const EditMesh src = m;
    const Carried  c(src);
    const int nv = static_cast<int>(src.verts.size());

    // Every edge once: its ends, and the faces on it (a border has one, a
    // non-manifold edge more than two -- both are held like a crease).
    struct Edge { int a = -1, b = -1, faces = 0, f0 = -1, f1 = -1; };
    std::vector<Edge> edges;
    std::unordered_map<std::uint64_t, int> edgeOf;
    std::vector<std::vector<int>> faceEdges(src.faces.size());
    for (std::size_t f = 0; f < src.faces.size(); ++f) {
        const std::vector<int>& fv = src.faces[f];
        if (fv.size() < 3) continue;
        for (std::size_t i = 0; i < fv.size(); ++i) {
            const int a = fv[i], b = fv[(i + 1) % fv.size()];
            const std::uint64_t k = edgeKey(a, b);
            auto it = edgeOf.find(k);
            if (it == edgeOf.end()) {
                it = edgeOf.emplace(k, static_cast<int>(edges.size())).first;
                Edge e;
                e.a = a;
                e.b = b;
                edges.push_back(e);
            }
            Edge& e = edges[static_cast<std::size_t>(it->second)];
            if (e.faces == 0) e.f0 = static_cast<int>(f);
            else if (e.faces == 1) e.f1 = static_cast<int>(f);
            ++e.faces;
            faceEdges[f].push_back(it->second);   // edge i runs from corner i to i+1
        }
    }

    // Face points: the middle of each face.
    std::vector<glm::vec3> fp(src.faces.size(), glm::vec3(0.0f));
    std::vector<glm::vec4> fw(src.faces.size(), glm::vec4(0.0f));
    for (std::size_t f = 0; f < src.faces.size(); ++f) {
        const std::vector<int>& fv = src.faces[f];
        if (fv.size() < 3) continue;
        for (int v : fv) {
            fp[f] += src.verts[static_cast<std::size_t>(v)];
            fw[f] += src.paintAt(v);
        }
        fp[f] /= static_cast<float>(fv.size());
        fw[f] /= static_cast<float>(fv.size());
    }

    // Edge points: between the ends, pulled towards the middles of the two
    // faces on an inner edge; the plain midpoint on a border (or crease).
    std::vector<glm::vec3> ep(edges.size());
    for (std::size_t e = 0; e < edges.size(); ++e) {
        const Edge& E = edges[e];
        const glm::vec3 mid = 0.5f * (src.verts[static_cast<std::size_t>(E.a)] +
                                      src.verts[static_cast<std::size_t>(E.b)]);
        ep[e] = (smooth && E.faces == 2)
                    ? 0.5f * mid + 0.25f * (fp[static_cast<std::size_t>(E.f0)] +
                                            fp[static_cast<std::size_t>(E.f1)])
                    : mid;
    }

    // Vertex points. Inside: (Q + 2 R + (n - 3) V) / n, with Q the mean of the
    // face middles around the vertex, R the mean of the midpoints of its edges
    // and n its valence. On a border with two border edges: the cubic B-spline
    // rule along the border, (P0 + 6 V + P1) / 8, so a border stays a curve of
    // its own and does not shrink into the surface. Anywhere else a border
    // meets (a corner, a non-manifold edge) the vertex stays put.
    std::vector<glm::vec3> vp(src.verts);
    if (smooth) {
        const std::size_t n = static_cast<std::size_t>(nv);
        std::vector<glm::vec3> sumF(n, glm::vec3(0.0f)), sumE(n, glm::vec3(0.0f)),
            sumB(n, glm::vec3(0.0f));
        std::vector<int> numF(n, 0), numE(n, 0), numB(n, 0);
        for (std::size_t f = 0; f < src.faces.size(); ++f) {
            if (src.faces[f].size() < 3) continue;
            for (int v : src.faces[f]) {
                sumF[static_cast<std::size_t>(v)] += fp[f];
                ++numF[static_cast<std::size_t>(v)];
            }
        }
        for (const Edge& E : edges) {
            const glm::vec3 pa = src.verts[static_cast<std::size_t>(E.a)];
            const glm::vec3 pb = src.verts[static_cast<std::size_t>(E.b)];
            const glm::vec3 mid = 0.5f * (pa + pb);
            const int ends[2] = {E.a, E.b};
            for (int v : ends) {
                sumE[static_cast<std::size_t>(v)] += mid;
                ++numE[static_cast<std::size_t>(v)];
            }
            if (E.faces != 2) {
                sumB[static_cast<std::size_t>(E.a)] += pb;
                ++numB[static_cast<std::size_t>(E.a)];
                sumB[static_cast<std::size_t>(E.b)] += pa;
                ++numB[static_cast<std::size_t>(E.b)];
            }
        }
        for (std::size_t i = 0; i < n; ++i) {
            const glm::vec3 V = src.verts[i];
            if (numB[i] == 2) {
                vp[i] = (sumB[i] + 6.0f * V) / 8.0f;
            } else if (numB[i] == 0 && numE[i] >= 3 && numF[i] > 0) {
                const float k = static_cast<float>(numE[i]);
                const glm::vec3 Q = sumF[i] / static_cast<float>(numF[i]);
                const glm::vec3 R = sumE[i] / k;
                vp[i] = (Q + 2.0f * R + (k - 3.0f) * V) / k;
            }
        }
    }

    // The new mesh: the moved corners keep their indices, then an edge point
    // per edge, then a face point per face; a quad per corner of every face.
    EditMesh out = emptyLike(src);
    for (int v = 0; v < nv; ++v) addCorner(out, vp[static_cast<std::size_t>(v)], src.paintAt(v), c);
    const int firstEdge = static_cast<int>(out.verts.size());
    for (std::size_t e = 0; e < edges.size(); ++e)
        addCorner(out, ep[e], 0.5f * (src.paintAt(edges[e].a) + src.paintAt(edges[e].b)), c);
    const int firstFace = static_cast<int>(out.verts.size());
    for (std::size_t f = 0; f < src.faces.size(); ++f) addCorner(out, fp[f], fw[f], c);
    for (std::size_t f = 0; f < src.faces.size(); ++f) {
        const std::vector<int>& fv = src.faces[f];
        const std::size_t k = fv.size();
        if (k < 3) continue;
        for (std::size_t i = 0; i < k; ++i) {
            // Corner i, the edge on to the next corner, the middle, and the edge
            // back to the previous one: counter-clockwise like the face itself.
            const int next = firstEdge + faceEdges[f][i];
            const int prev = firstEdge + faceEdges[f][(i + k - 1) % k];
            addFaceFrom(out, {fv[i], next, firstFace + static_cast<int>(f), prev}, src,
                        static_cast<int>(f), c);
        }
    }
    detail::dropUnusedVerts(out);
    m = std::move(out);
}

// A normal per corner for thickening: the faces around it, each weighted by
// the angle it makes at that corner (so a corner of a box gets the diagonal,
// however its faces are cut up). `even` also says how much further than 1 the
// corner has to go so every face around it ends up a whole thickness away.
struct CornerNormals {
    std::vector<glm::vec3> n;
    std::vector<float>     reach;
};
CornerNormals cornerNormals(const EditMesh& m, bool even) {
    CornerNormals out;
    out.n.assign(m.verts.size(), glm::vec3(0.0f));
    out.reach.assign(m.verts.size(), 1.0f);
    std::vector<glm::vec3> fn(m.faces.size());
    for (std::size_t f = 0; f < m.faces.size(); ++f) {
        const std::vector<int>& fv = m.faces[f];
        if (fv.size() < 3) continue;
        fn[f] = m.faceNormal(static_cast<int>(f));
        for (std::size_t i = 0; i < fv.size(); ++i) {
            const glm::vec3 p  = m.verts[static_cast<std::size_t>(fv[i])];
            const glm::vec3 a  = m.verts[static_cast<std::size_t>(fv[(i + fv.size() - 1) % fv.size()])] - p;
            const glm::vec3 b  = m.verts[static_cast<std::size_t>(fv[(i + 1) % fv.size()])] - p;
            const float la = glm::length(a), lb = glm::length(b);
            if (la < 1e-9f || lb < 1e-9f) continue;
            const float ang = std::acos(glm::clamp(glm::dot(a, b) / (la * lb), -1.0f, 1.0f));
            out.n[static_cast<std::size_t>(fv[i])] += fn[f] * ang;
        }
    }
    for (glm::vec3& v : out.n) {
        const float l = glm::length(v);
        v = l > 1e-12f ? v / l : glm::vec3(0.0f, 1.0f, 0.0f);
    }
    if (even) {
        std::vector<float> minDot(m.verts.size(), 1.0f);
        for (std::size_t f = 0; f < m.faces.size(); ++f)
            for (int v : m.faces[f])
                if (m.faces[f].size() >= 3)
                    minDot[static_cast<std::size_t>(v)] =
                        std::min(minDot[static_cast<std::size_t>(v)],
                                 glm::dot(out.n[static_cast<std::size_t>(v)], fn[f]));
        // Held at three thicknesses: a corner where faces fold almost flat back
        // on themselves would otherwise shoot off to infinity.
        for (std::size_t v = 0; v < minDot.size(); ++v)
            out.reach[v] = 1.0f / std::max(minDot[v], 0.33f);
    }
    return out;
}

} // namespace

int subdivideSurface(EditMesh& m, int levels, bool smooth, std::size_t maxFaces) {
    int done = 0;
    for (int l = 0; l < levels; ++l) {
        // A level makes one quad per face corner.
        std::size_t next = 0;
        for (const std::vector<int>& f : m.faces) next += f.size() >= 3 ? f.size() : 0;
        if (next == 0 || next > maxFaces) break;
        subdivideOnce(m, smooth);
        ++done;
    }
    return done;
}

void decimate(EditMesh& m, float ratio) {
    if (ratio >= 0.999f || m.faces.empty() || m.verts.size() < 4) return;
    ratio = std::max(ratio, 0.005f);
    const EditMesh src = m;
    const Carried  c(src);

    // Triangles per material, as fans (the way buildGroups draws the faces).
    struct Region {
        fitzel::AssetId            material;
        int                        like = -1;     // a face of it, for what it wears
        std::vector<std::uint32_t> idx;
    };
    std::vector<Region> regions;
    for (std::size_t f = 0; f < src.faces.size(); ++f) {
        const std::vector<int>& fv = src.faces[f];
        if (fv.size() < 3) continue;
        const fitzel::AssetId mat = src.faceMaterial(static_cast<int>(f));
        Region* r = nullptr;
        for (Region& q : regions)
            if (q.material == mat) { r = &q; break; }
        if (!r) {
            regions.push_back(Region{mat, static_cast<int>(f), {}});
            r = &regions.back();
        }
        for (std::size_t k = 1; k + 1 < fv.size(); ++k) {
            r->idx.push_back(static_cast<std::uint32_t>(fv[0]));
            r->idx.push_back(static_cast<std::uint32_t>(fv[k]));
            r->idx.push_back(static_cast<std::uint32_t>(fv[k + 1]));
        }
    }
    std::vector<float> pos;
    pos.reserve(src.verts.size() * 3);
    for (const glm::vec3& v : src.verts) {
        pos.push_back(v.x);
        pos.push_back(v.y);
        pos.push_back(v.z);
    }

    // The collapse moves corners onto neighbours and never makes new ones, so
    // the result indexes the very corners (and paint) it started from.
    EditMesh out = emptyLike(src);
    out.verts = src.verts;
    if (c.paint) out.paint = src.paint;
    for (const Region& r : regions) {
        const std::size_t tris   = r.idx.size() / 3;
        const std::size_t target = std::max<std::size_t>(
            1, static_cast<std::size_t>(std::lround(static_cast<double>(tris) * ratio)));
        const std::vector<std::uint32_t> kept = meshsimplify::simplify(
            pos.data(), 3, src.verts.size(), r.idx.data(), r.idx.size(), target);
        for (std::size_t t = 0; t + 2 < kept.size(); t += 3) {
            const int a = static_cast<int>(kept[t]), b = static_cast<int>(kept[t + 1]),
                      d = static_cast<int>(kept[t + 2]);
            if (a == b || b == d || a == d) continue;
            out.faces.push_back({a, b, d});
            if (c.mat) out.faceMat.push_back(r.material);
            // The texture placement of a triangle that stands for several faces
            // is whatever its face decides again: its own axis, its own plane.
            if (c.uv) out.faceUV.push_back(EditMesh::FaceUV{});
        }
    }
    detail::dropUnusedVerts(out);
    m = std::move(out);
}

void solidify(EditMesh& m, float thickness, float offset, bool rim, bool even) {
    if (std::abs(thickness) < 1e-6f || m.faces.empty()) return;
    const EditMesh src = m;
    const Carried  c(src);
    const int nv = static_cast<int>(src.verts.size());
    const CornerNormals cn = cornerNormals(src, even);
    offset = glm::clamp(offset, -1.0f, 1.0f);
    const float outD = thickness * (offset + 1.0f) * 0.5f;   // how far the outer skin moves
    const float inD  = thickness * (offset - 1.0f) * 0.5f;   // ...and the inner one

    EditMesh out = emptyLike(src);
    for (int v = 0; v < nv; ++v) {
        const std::size_t i = static_cast<std::size_t>(v);
        addCorner(out, src.verts[i] + cn.n[i] * (outD * cn.reach[i]), src.paintAt(v), c);
    }
    for (int v = 0; v < nv; ++v) {
        const std::size_t i = static_cast<std::size_t>(v);
        addCorner(out, src.verts[i] + cn.n[i] * (inD * cn.reach[i]), src.paintAt(v), c);
    }
    // The outer skin as it was wound, the inner one turned round (it looks the
    // other way), then the walls along every open border.
    for (std::size_t f = 0; f < src.faces.size(); ++f) {
        const std::vector<int>& fv = src.faces[f];
        if (fv.size() < 3) continue;
        addFaceFrom(out, fv, src, static_cast<int>(f), c);
        std::vector<int> back(fv.rbegin(), fv.rend());
        for (int& v : back) v += nv;
        addFaceFrom(out, std::move(back), src, static_cast<int>(f), c);
    }
    if (rim) {
        const detail::EdgeMap em = detail::buildEdges(src);
        for (std::size_t f = 0; f < src.faces.size(); ++f) {
            const std::vector<int>& fv = src.faces[f];
            if (fv.size() < 3) continue;
            for (std::size_t i = 0; i < fv.size(); ++i) {
                const int a = fv[i], b = fv[(i + 1) % fv.size()];
                const auto it = em.find(edgeKey(a, b));
                if (it == em.end() || it->second.second >= 0) continue;   // not a border
                // Inner a, inner b, outer b, outer a: facing out across the
                // border, away from the face it closes.
                addFaceFrom(out, {a + nv, b + nv, b, a}, src, static_cast<int>(f), c);
            }
        }
    }
    detail::dropUnusedVerts(out);
    m = std::move(out);
}

void wireframe(EditMesh& m, float thickness, float offset, bool replace) {
    if (thickness <= 1e-6f || m.faces.empty()) return;
    const EditMesh src = m;
    const Carried  c(src);

    // The frames: every face inset by half the thickness, keeping only the rim
    // between its edges and the inset. The outer corners stay the shared ones,
    // so the frames of two faces meet along the edge they share and become one
    // strut the whole thickness wide.
    EditMesh frame = emptyLike(src);
    frame.verts = src.verts;
    if (c.paint) frame.paint = src.paint;
    const float half = thickness * 0.5f;
    for (std::size_t f = 0; f < src.faces.size(); ++f) {
        const std::vector<int>& fv = src.faces[f];
        const std::size_t k = fv.size();
        if (k < 3) continue;
        const glm::vec3 n  = src.faceNormal(static_cast<int>(f));
        const glm::vec3 ce = src.faceCenter(static_cast<int>(f));
        // No deeper than the face can take: short of the distance from its middle
        // to the nearest edge, so the inner rim never folds over itself.
        float room = 1e30f;
        for (std::size_t i = 0; i < k; ++i) {
            const glm::vec3 a = src.verts[static_cast<std::size_t>(fv[i])];
            const glm::vec3 b = src.verts[static_cast<std::size_t>(fv[(i + 1) % k])];
            const glm::vec3 e = b - a;
            const float l2 = glm::dot(e, e);
            const glm::vec3 foot = l2 > 1e-12f ? a + e * (glm::dot(ce - a, e) / l2) : a;
            room = std::min(room, glm::length(ce - foot));
        }
        const float w = std::min(half, room * 0.9f);
        std::vector<int> inner(k);
        for (std::size_t i = 0; i < k; ++i) {
            const glm::vec3 p    = src.verts[static_cast<std::size_t>(fv[i])];
            const glm::vec3 prev = src.verts[static_cast<std::size_t>(fv[(i + k - 1) % k])];
            const glm::vec3 next = src.verts[static_cast<std::size_t>(fv[(i + 1) % k])];
            const glm::vec3 a = prev - p, b = next - p;
            const float la = glm::length(a), lb = glm::length(b);
            glm::vec3 inward = glm::cross(n, next - prev);
            const float li = glm::length(inward);
            inward = li > 1e-12f ? inward / li : glm::vec3(0.0f);
            glm::vec3 d = inward;
            float sinHalf = 1.0f;
            if (la > 1e-9f && lb > 1e-9f) {
                const glm::vec3 ua = a / la, ub = b / lb;
                const glm::vec3 bis = ua + ub;
                if (glm::length(bis) > 1e-5f) {
                    d = glm::normalize(bis);
                    if (glm::dot(d, inward) < 0.0f) d = -d;   // a reflex corner
                }
                sinHalf = std::sqrt(std::max((1.0f - glm::dot(ua, ub)) * 0.5f, 1e-4f));
            }
            // Along the bisector, as far as it takes for BOTH edges to be `w`
            // away -- and never past the middle of the face.
            const float len = std::min(w / sinHalf, 0.95f * glm::length(ce - p));
            inner[i] = addCorner(frame, p + d * len, src.paintAt(fv[i]), c);
        }
        for (std::size_t i = 0; i < k; ++i) {
            const std::size_t j = (i + 1) % k;
            addFaceFrom(frame, {fv[i], fv[j], inner[j], inner[i]}, src, static_cast<int>(f), c);
        }
    }
    detail::dropUnusedVerts(frame);
    // Thickened with walls round every rim: the holes the insets leave and the
    // open borders of the mesh are both borders of the frame surface.
    solidify(frame, thickness, offset, true, true);
    if (!replace) appendMesh(frame, src, glm::vec3(0.0f), c);
    m = std::move(frame);
}

void arrayCopies(EditMesh& m, int count, const glm::vec3& relative, const glm::vec3& constant,
                 bool merge, float mergeDist) {
    count = std::clamp(count, 1, 1000);
    if (count == 1 || m.verts.empty()) return;
    const EditMesh src = m;
    const Carried  c(src);
    glm::vec3 mn, mx;
    src.bounds(mn, mx);
    const glm::vec3 step = relative * (mx - mn) + constant;
    for (int i = 1; i < count; ++i) appendMesh(m, src, step * static_cast<float>(i), c);
    if (merge && mergeDist > 0.0f) {
        std::vector<int> all(m.verts.size());
        for (std::size_t v = 0; v < all.size(); ++v) all[v] = static_cast<int>(v);
        weldVerts(m, all, mergeDist);
    }
}

} // namespace editmesh
