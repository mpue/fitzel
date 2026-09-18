// The second half of the EditMesh toolbox: the face operations for several faces
// at once, the ways to make a face where there is none (from picked corners, by
// filling a hole, by cutting one face in two), welding, and the bevel.
//
// Same rules as EditMesh.cpp, and the same helpers (EditMeshDetail.hpp): every
// face that is added goes through addFaceLike so the material and texture
// placement arrays stay parallel to `faces`, every corner through addVert so
// `paint` stays parallel to `verts`, and whatever can leave a face collapsed ends
// in dropCollapsedFaces.

#include "EditMesh.hpp"
#include "EditMeshDetail.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

using namespace editmesh::detail;

namespace {

// A half-edge -- edge (a, b) in the direction a face runs it. Two faces that
// meet properly run their shared edge opposite ways, so "is b -> a in the mesh
// too?" is the whole of "has this edge a face on the other side".
std::uint64_t halfKey(int a, int b) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(a)) << 32) |
           static_cast<std::uint32_t>(b);
}

std::unordered_set<std::uint64_t> halfEdges(const EditMesh& m) {
    std::unordered_set<std::uint64_t> h;
    for (const std::vector<int>& fv : m.faces)
        for (std::size_t i = 0; i < fv.size(); ++i)
            h.insert(halfKey(fv[i], fv[(i + 1) % fv.size()]));
    return h;
}

// The usable, distinct faces of a selection, in the order given -- the first is
// the one the caller treats as active, so it has to stay first.
std::vector<int> uniqueFaces(const EditMesh& m, const std::vector<int>& faces) {
    std::vector<int> out;
    for (int f : faces)
        if (okFace(m, f) && std::find(out.begin(), out.end(), f) == out.end())
            out.push_back(f);
    return out;
}

// Which way, and how far per metre of `dist`, each corner of a face region moves
// when the region goes "out". The average of the picked normals at the corner,
// lengthened by 1 / (its dot with the least aligned of them): for faces at right
// angles that makes every one of them travel exactly `dist` along its own
// normal, which is what keeps a box's top and front, pulled out together, a box.
// The floor on the divisor stops a knife-edge (two faces folded almost flat onto
// each other) from throwing its corner to infinity.
struct Offsets {
    std::vector<int>          order;   // corners in the order the faces name them
    std::unordered_map<int, glm::vec3> dir;
};
Offsets regionOffsets(const EditMesh& m, const std::vector<int>& faces) {
    std::unordered_map<int, std::vector<glm::vec3>> ns;
    Offsets o;
    for (int f : faces) {
        const glm::vec3 n = m.faceNormal(f);
        for (int v : m.faces[f]) {
            auto& list = ns[v];
            if (list.empty()) o.order.push_back(v);
            // A corner listed twice by one face is still one vote.
            if (list.empty() || list.back() != n) list.push_back(n);
        }
    }
    for (int v : o.order) {
        const std::vector<glm::vec3>& list = ns[v];
        glm::vec3 sum(0.0f);
        for (const glm::vec3& n : list) sum += n;
        const glm::vec3 avg = glm::length(sum) > 1e-6f ? glm::normalize(sum) : list[0];
        float least = 1.0f;
        for (const glm::vec3& n : list) least = std::min(least, glm::dot(avg, n));
        o.dir[v] = avg / std::max(least, 0.25f);
    }
    return o;
}

// Groups of faces that touch (share a corner), for operations that act on each
// group about its own centre.
std::vector<std::vector<int>> faceGroups(const EditMesh& m, const std::vector<int>& faces) {
    std::vector<int> parent(faces.size());
    std::iota(parent.begin(), parent.end(), 0);
    auto find = [&](int i) {
        while (parent[i] != i) i = parent[i] = parent[parent[i]];
        return i;
    };
    std::unordered_map<int, int> firstFaceOf;   // corner -> first face (slot) using it
    for (std::size_t k = 0; k < faces.size(); ++k)
        for (int v : m.faces[faces[k]]) {
            auto [it, fresh] = firstFaceOf.try_emplace(v, static_cast<int>(k));
            if (!fresh) parent[find(static_cast<int>(k))] = find(it->second);
        }
    std::vector<std::vector<int>> groups;
    std::unordered_map<int, int> slot;
    for (std::size_t k = 0; k < faces.size(); ++k) {
        auto [it, fresh] = slot.try_emplace(find(static_cast<int>(k)),
                                            static_cast<int>(groups.size()));
        if (fresh) groups.emplace_back();
        groups[it->second].push_back(faces[k]);
    }
    return groups;
}

// Newell's normal of a loop of corners -- EditMesh::faceNormal for a face that
// is not in the mesh yet.
glm::vec3 loopNormal(const EditMesh& m, const std::vector<int>& loop) {
    glm::vec3 n(0.0f);
    for (std::size_t i = 0; i < loop.size(); ++i) {
        const glm::vec3& a = m.verts[loop[i]];
        const glm::vec3& b = m.verts[loop[(i + 1) % loop.size()]];
        n.x += (a.y - b.y) * (a.z + b.z);
        n.y += (a.z - b.z) * (a.x + b.x);
        n.z += (a.x - b.x) * (a.y + b.y);
    }
    return n;
}

} // namespace

namespace editmesh {

// =============================================================================
// Several faces at once
// =============================================================================

int extrudeFaces(EditMesh& m, const std::vector<int>& faces, float dist) {
    const std::vector<int> fs = uniqueFaces(m, faces);
    if (fs.empty()) return -1;
    // One face is the old operation exactly -- the region version would agree,
    // but "exactly" is what the loop-cut and material checks are written against.
    if (fs.size() == 1) return extrude(m, fs[0], dist);

    const Offsets off = regionOffsets(m, fs);
    // Every half-edge the region runs. A wall only grows along an edge whose
    // other side is NOT in the region: that is the region's rim.
    std::unordered_set<std::uint64_t> inRegion;
    for (int f : fs) {
        const std::vector<int>& fv = m.faces[f];
        for (std::size_t i = 0; i < fv.size(); ++i)
            inRegion.insert(halfKey(fv[i], fv[(i + 1) % fv.size()]));
    }
    // The region's own copy of each corner, moved out. A corner two picked faces
    // share is copied once, which is what keeps the region in one piece.
    std::unordered_map<int, int> cap;
    for (int v : off.order) {
        const glm::vec3 p = m.verts[v] + off.dir.at(v) * dist;
        cap[v] = addVert(m, p, m.paintAt(v));
    }
    for (int f : fs) {
        const std::vector<int> loop = m.faces[f];   // by value: m.faces grows
        for (std::size_t i = 0; i < loop.size(); ++i) {
            const int a = loop[i], b = loop[(i + 1) % loop.size()];
            if (inRegion.count(halfKey(b, a))) continue;   // inside the region
            // Wound like extrude's walls, and for the same reason: the sign of
            // the distance turns them the right way round by itself.
            addFaceLike(m, {a, b, cap[b], cap[a]}, f);
        }
    }
    for (int f : fs)
        for (int& v : m.faces[f]) v = cap[v];
    return fs[0];
}

int moveFaces(EditMesh& m, const std::vector<int>& faces, float dist) {
    const std::vector<int> fs = uniqueFaces(m, faces);
    if (fs.empty()) return -1;
    const Offsets off = regionOffsets(m, fs);
    for (int v : off.order) m.verts[v] += off.dir.at(v) * dist;
    return fs[0];
}

int scaleFaces(EditMesh& m, const std::vector<int>& faces, float factor) {
    const std::vector<int> fs = uniqueFaces(m, faces);
    if (fs.empty()) return -1;
    for (const std::vector<int>& g : faceGroups(m, fs)) {
        std::vector<int> corners;
        for (int f : g) corners.insert(corners.end(), m.faces[f].begin(), m.faces[f].end());
        corners = uniqueVerts(m, corners);
        glm::vec3 c(0.0f);
        for (int v : corners) c += m.verts[v];
        c /= static_cast<float>(corners.size());
        for (int v : corners) m.verts[v] = c + (m.verts[v] - c) * factor;
    }
    return fs[0];
}

int insetFaces(EditMesh& m, const std::vector<int>& faces, float amount) {
    const std::vector<int> fs = uniqueFaces(m, faces);
    for (int f : fs) inset(m, f, amount);   // appends the rims; indices stay
    return fs.empty() ? -1 : fs[0];
}

int subdivideFaces(EditMesh& m, const std::vector<int>& faces) {
    const std::vector<int> fs = uniqueFaces(m, faces);
    for (int f : fs) subdivide(m, f);       // non-quads say no and are skipped
    return fs.empty() ? -1 : fs[0];
}

void deleteFaces(EditMesh& m, const std::vector<int>& faces) {
    std::vector<int> fs = uniqueFaces(m, faces);
    // Highest first: deleting a face shifts every face after it down by one,
    // and none of the ones still to go come after it.
    std::sort(fs.begin(), fs.end(), std::greater<int>());
    for (int f : fs) deleteFace(m, f);
}

int flipFaces(EditMesh& m, const std::vector<int>& faces) {
    const std::vector<int> fs = uniqueFaces(m, faces);
    // The first corner stays where it is, so the face's texture placement (which
    // is measured from the face's own centre) does not move.
    for (int f : fs) std::reverse(m.faces[f].begin() + 1, m.faces[f].end());
    return fs.empty() ? -1 : fs[0];
}

// =============================================================================
// Making faces
// =============================================================================

int makeFace(EditMesh& m, const std::vector<int>& verts) {
    const std::vector<int> u = uniqueVerts(m, verts);
    if (u.size() < 3) return -1;
    glm::vec3 c(0.0f);
    for (int v : u) c += m.verts[v];
    c /= static_cast<float>(u.size());

    // The plane: the widest pair of spokes from the centre spans it best. (Newell
    // needs an order, and the order is what this is trying to find.)
    glm::vec3 n(0.0f);
    for (std::size_t i = 0; i < u.size(); ++i)
        for (std::size_t k = i + 1; k < u.size(); ++k) {
            const glm::vec3 x = glm::cross(m.verts[u[i]] - c, m.verts[u[k]] - c);
            if (glm::dot(x, x) > glm::dot(n, n)) n = x;
        }
    if (glm::dot(n, n) < 1e-12f) return -1;   // all in a line
    n = glm::normalize(n);
    glm::vec3 tx = m.verts[u[0]] - c;
    tx -= n * glm::dot(tx, n);
    if (glm::dot(tx, tx) < 1e-12f) tx = glm::cross(n, glm::vec3(0.37f, 0.61f, 0.7f));
    tx = glm::normalize(tx);
    const glm::vec3 ty = glm::cross(n, tx);

    // Round the centre, by angle: the picking order does not matter.
    std::vector<std::pair<float, int>> ang;
    for (int v : u) {
        const glm::vec3 d = m.verts[v] - c;
        ang.push_back({std::atan2(glm::dot(d, ty), glm::dot(d, tx)), v});
    }
    std::sort(ang.begin(), ang.end());
    std::vector<int> loop;
    for (const auto& a : ang) loop.push_back(a.second);

    // Not twice: the same corners as a face that is already there.
    for (const std::vector<int>& f : m.faces) {
        if (f.size() != loop.size()) continue;
        std::vector<int> s = f;
        std::sort(s.begin(), s.end());
        if (s == u) return -1;
    }

    // Winding. A neighbour that runs an edge of the new face b -> a says the new
    // face runs it a -> b; one that runs it a -> b says the opposite. With no
    // neighbour to ask, the face looks away from the middle of the mesh.
    const std::unordered_set<std::uint64_t> half = halfEdges(m);
    int agree = 0, clash = 0, src = -1;
    for (std::size_t i = 0; i < loop.size(); ++i) {
        const int a = loop[i], b = loop[(i + 1) % loop.size()];
        if (half.count(halfKey(b, a))) ++agree;
        if (half.count(halfKey(a, b))) ++clash;
    }
    bool flip = clash > agree;
    if (agree == 0 && clash == 0) {
        glm::vec3 mn, mx;
        m.bounds(mn, mx);
        flip = glm::dot(loopNormal(m, loop), c - 0.5f * (mn + mx)) < 0.0f;
    }
    if (flip) std::reverse(loop.begin() + 1, loop.end());

    // Dressed like the first face it borders, if it borders one.
    for (std::size_t i = 0; i < loop.size() && src < 0; ++i) {
        const int a = loop[i], b = loop[(i + 1) % loop.size()];
        for (int f = 0; f < static_cast<int>(m.faces.size()) && src < 0; ++f)
            if (edgeIndexIn(m, f, a, b) >= 0) src = f;
    }
    return addFaceLike(m, std::move(loop), src);
}

int fillHole(EditMesh& m, int a, int b) {
    const int nv = static_cast<int>(m.verts.size());
    if (a < 0 || b < 0 || a >= nv || b >= nv || a == b) return -1;
    const std::unordered_set<std::uint64_t> half = halfEdges(m);
    // The rim runs x -> y wherever a face runs x -> y and none runs y -> x; the
    // patch runs every one of those the other way, y -> x.
    std::unordered_map<int, int> next;   // along the patch
    std::unordered_map<int, int> srcOf;  // the face beside that step
    for (int f = 0; f < static_cast<int>(m.faces.size()); ++f) {
        const std::vector<int>& fv = m.faces[f];
        for (std::size_t i = 0; i < fv.size(); ++i) {
            const int x = fv[i], y = fv[(i + 1) % fv.size()];
            if (half.count(halfKey(y, x))) continue;
            next.try_emplace(y, x);
            srcOf.try_emplace(y, f);
        }
    }
    int start = -1;
    if (half.count(halfKey(a, b)) && !half.count(halfKey(b, a))) start = b;
    else if (half.count(halfKey(b, a)) && !half.count(halfKey(a, b))) start = a;
    if (start < 0) return -1;   // not a border edge

    std::vector<int> loop;
    int cur = start;
    do {
        loop.push_back(cur);
        auto it = next.find(cur);
        if (it == next.end() || loop.size() > m.verts.size()) return -1;
        cur = it->second;
    } while (cur != start);
    if (loop.size() < 3) return -1;
    return addFaceLike(m, std::move(loop), srcOf[start]);
}

int connectVerts(EditMesh& m, int a, int b) {
    if (a == b) return -1;
    for (int f = 0; f < static_cast<int>(m.faces.size()); ++f) {
        const std::vector<int> fv = m.faces[f];   // by value: m.faces may grow
        const int n  = static_cast<int>(fv.size());
        const auto pa = std::find(fv.begin(), fv.end(), a);
        const auto pb = std::find(fv.begin(), fv.end(), b);
        if (pa == fv.end() || pb == fv.end()) continue;
        const int ia = static_cast<int>(pa - fv.begin());
        const int ib = static_cast<int>(pb - fv.begin());
        const int gap = (ib - ia + n) % n;
        if (gap == 1 || gap == n - 1) continue;   // neighbours: already an edge
        std::vector<int> first, second;
        for (int k = ia;; k = (k + 1) % n) { first.push_back(fv[k]);  if (k == ib) break; }
        for (int k = ib;; k = (k + 1) % n) { second.push_back(fv[k]); if (k == ia) break; }
        m.faces[f] = std::move(first);
        addFaceLike(m, std::move(second), f);
        return f;
    }
    return -1;
}

int weldVerts(EditMesh& m, const std::vector<int>& idx, float dist) {
    const std::vector<int> u = uniqueVerts(m, idx);
    if (u.size() < 2) return 0;
    dist = std::max(dist, 1e-6f);

    // Close pairs through a grid of cells one weld distance wide: a corner can
    // only be within reach of corners in its own cell and the 26 around it.
    std::vector<int> parent(u.size());
    std::iota(parent.begin(), parent.end(), 0);
    auto find = [&](int i) {
        while (parent[i] != i) i = parent[i] = parent[parent[i]];
        return i;
    };
    auto cellKey = [](int x, int y, int z) {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x) & 0x1FFFFFu) << 42) |
               (static_cast<std::uint64_t>(static_cast<std::uint32_t>(y) & 0x1FFFFFu) << 21) |
               (static_cast<std::uint64_t>(static_cast<std::uint32_t>(z) & 0x1FFFFFu));
    };
    std::unordered_map<std::uint64_t, std::vector<int>> grid;
    for (int k = 0; k < static_cast<int>(u.size()); ++k) {
        const glm::vec3& p = m.verts[u[k]];
        const int cx = static_cast<int>(std::floor(p.x / dist));
        const int cy = static_cast<int>(std::floor(p.y / dist));
        const int cz = static_cast<int>(std::floor(p.z / dist));
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -1; dz <= 1; ++dz) {
                    auto it = grid.find(cellKey(cx + dx, cy + dy, cz + dz));
                    if (it == grid.end()) continue;
                    for (int j : it->second)
                        if (glm::length(m.verts[u[j]] - p) <= dist) parent[find(k)] = find(j);
                }
        grid[cellKey(cx, cy, cz)].push_back(k);
    }

    m.syncPaint();
    std::vector<int> into(m.verts.size());
    std::iota(into.begin(), into.end(), 0);
    std::unordered_map<int, std::vector<int>> groups;
    for (int k = 0; k < static_cast<int>(u.size()); ++k) groups[find(k)].push_back(u[k]);
    int removed = 0;
    for (const auto& g : groups) {
        const std::vector<int>& vs = g.second;
        if (vs.size() < 2) continue;
        glm::vec3 c(0.0f);
        glm::vec4 w(0.0f);
        for (int v : vs) { c += m.verts[v]; w += m.paint[v]; }
        const int keep = *std::min_element(vs.begin(), vs.end());
        m.verts[keep] = c / static_cast<float>(vs.size());
        m.paint[keep] = w / static_cast<float>(vs.size());
        for (int v : vs) into[v] = keep;
        removed += static_cast<int>(vs.size()) - 1;
    }
    if (removed == 0) return 0;
    for (std::vector<int>& f : m.faces)
        for (int& v : f) v = into[v];
    dropCollapsedFaces(m);
    return removed;
}

// =============================================================================
// Bevel
// =============================================================================
//
// Every face keeps its index and loses its corners at the bevelled edges; each
// edge becomes a strip; whatever is left open where strips meet at a corner is
// closed with a patch. Per face corner v (prev -> v -> next in the face):
//
//   both edges at v bevelled  -> one new point, set in from both by the width
//   only prev -> v bevelled   -> the point `width` along v -> next
//   only v -> next bevelled   -> the point `width` along v -> prev
//   neither, but v is on a bevelled edge elsewhere
//                             -> v is cut off: the points along v -> prev and
//                                v -> next, and between them the strip's
//                                rounding when a strip ends there
//
// The "along an edge" points are shared by every face that asks for the same one
// (keyed by the directed pair v -> u), which is what keeps neighbours meeting
// without cracks. On the box-modelling case -- three edges at every corner --
// that is the whole story; at busier corners the patch step closes the rest.
int bevelEdges(EditMesh& m, const std::vector<std::pair<int, int>>& sel,
               float width, int segments) {
    segments = std::clamp(segments, 1, 12);
    if (!(width > 0.0f)) return 0;
    const int nv = static_cast<int>(m.verts.size());
    const int nf = static_cast<int>(m.faces.size());
    const EdgeMap em = buildEdges(m);

    auto runs = [&](int f, int x, int y) {
        const std::vector<int>& fv = m.faces[f];
        for (std::size_t i = 0; i < fv.size(); ++i)
            if (fv[i] == x && fv[(i + 1) % fv.size()] == y) return true;
        return false;
    };
    // The edges that can go: a face on either side, wound consistently, once each.
    struct Bev { int a, b, f0, f1; };   // f0 runs a -> b, f1 runs b -> a
    std::vector<Bev> bev;
    std::unordered_set<std::uint64_t> isBev;
    for (const auto& e : sel) {
        const int a = e.first, b = e.second;
        if (a < 0 || b < 0 || a >= nv || b >= nv || a == b) continue;
        const std::uint64_t key = edgeKey(a, b);
        if (isBev.count(key)) continue;
        auto it = em.find(key);
        if (it == em.end() || it->second.second < 0) continue;
        int f0 = it->second.first, f1 = it->second.second;
        if (!runs(f0, a, b)) std::swap(f0, f1);
        if (!runs(f0, a, b) || !runs(f1, b, a)) continue;
        isBev.insert(key);
        bev.push_back({a, b, f0, f1});
    }
    if (bev.empty()) return 0;

    std::vector<char> touched(nv, 0);
    for (const Bev& e : bev) touched[e.a] = touched[e.b] = 1;

    // The original positions (addVert grows the array under us) and the width
    // each touched corner can afford: under half its shortest edge, so the cuts
    // from both ends of an edge never pass each other.
    const std::vector<glm::vec3> P(m.verts.begin(), m.verts.end());
    std::vector<float> w(nv, width);
    for (const std::vector<int>& fv : m.faces)
        for (std::size_t i = 0; i < fv.size(); ++i) {
            const int a = fv[i], b = fv[(i + 1) % fv.size()];
            const float len = glm::length(P[a] - P[b]);
            if (touched[a]) w[a] = std::min(w[a], 0.45f * len);
            if (touched[b]) w[b] = std::min(w[b], 0.45f * len);
        }

    auto dirTo = [&](int v, int u) {
        const glm::vec3 d = P[u] - P[v];
        const float     l = glm::length(d);
        return l > 1e-9f ? d / l : glm::vec3(0.0f);
    };
    std::unordered_map<int, int> owner;   // new point -> the corner it replaces
    auto newVert = [&](int v, const glm::vec3& p) {
        const int i = addVert(m, p, m.paintAt(v));
        owner[i] = v;
        return i;
    };
    std::unordered_map<std::uint64_t, int> slide;   // directed v -> u
    auto along = [&](int v, int u) {
        const std::uint64_t k = halfKey(v, u);
        auto it = slide.find(k);
        if (it != slide.end()) return it->second;
        const int i = newVert(v, P[v] + dirTo(v, u) * w[v]);
        slide.emplace(k, i);
        return i;
    };
    auto fvKey = [](int f, int v) { return halfKey(f, v); };

    // 1) The corners that become ONE point.
    std::unordered_map<std::uint64_t, int> corner;
    for (int f = 0; f < nf; ++f) {
        const std::vector<int> fv = m.faces[f];
        const int n = static_cast<int>(fv.size());
        for (int i = 0; i < n; ++i) {
            const int v = fv[i];
            if (!touched[v]) continue;
            const int prev = fv[(i + n - 1) % n], next = fv[(i + 1) % n];
            const bool bin  = isBev.count(edgeKey(prev, v)) != 0;
            const bool bout = isBev.count(edgeKey(v, next)) != 0;
            if (bin && bout)
                corner[fvKey(f, v)] =
                    newVert(v, P[v] + (dirTo(v, prev) + dirTo(v, next)) * w[v]);
            else if (bin)  corner[fvKey(f, v)] = along(v, next);
            else if (bout) corner[fvKey(f, v)] = along(v, prev);
        }
    }

    // 2) The strips, and the profile across each end of one. A profile is the
    //    row of points from one side of the strip to the other: just its two
    //    ends when flat, a curve bulging towards the old edge when rounded.
    //    Stored once per pair of ends, because two strips meeting at a corner
    //    share the end they meet along -- and must share its points, too.
    struct Profile { int from = -1; std::vector<int> mid; };
    std::unordered_map<std::uint64_t, Profile> prof;
    auto between = [&](int x, int y) {
        std::vector<int> mid;
        auto it = prof.find(edgeKey(x, y));
        if (it == prof.end()) return mid;
        mid = it->second.mid;
        if (it->second.from != x) std::reverse(mid.begin(), mid.end());
        return mid;
    };
    auto profile = [&](int v, int x, int y, const glm::vec3& edgeDir) {
        if (x != y && segments > 1 && !prof.count(edgeKey(x, y))) {
            Profile p;
            p.from = x;
            const glm::vec3 X = m.verts[x], Y = m.verts[y];
            // The curve's control point: the old corner, slid along the edge to
            // the cross-section the two ends lie in.
            const float     k = 0.5f * (glm::dot(X - P[v], edgeDir) + glm::dot(Y - P[v], edgeDir));
            const glm::vec3 C = P[v] + edgeDir * k;
            for (int s = 1; s < segments; ++s) {
                const float t = static_cast<float>(s) / static_cast<float>(segments);
                p.mid.push_back(newVert(v, (1 - t) * (1 - t) * X + 2 * t * (1 - t) * C + t * t * Y));
            }
            prof.emplace(edgeKey(x, y), std::move(p));
        }
        std::vector<int> out{x};
        for (int i : between(x, y)) out.push_back(i);
        if (y != x) out.push_back(y);
        return out;
    };
    int made = 0;
    for (const Bev& e : bev) {
        const auto a0 = corner.find(fvKey(e.f0, e.a)), a1 = corner.find(fvKey(e.f1, e.a));
        const auto b0 = corner.find(fvKey(e.f0, e.b)), b1 = corner.find(fvKey(e.f1, e.b));
        if (a0 == corner.end() || a1 == corner.end() || b0 == corner.end() || b1 == corner.end())
            continue;
        const glm::vec3 d = dirTo(e.a, e.b);
        const std::vector<int> A = profile(e.a, a0->second, a1->second, d);
        const std::vector<int> B = profile(e.b, b0->second, b1->second, -d);
        if (A.size() != B.size()) continue;
        // f0 runs A0 -> B0 along its side of the strip, so the strip runs it back.
        for (std::size_t k = 0; k + 1 < A.size(); ++k)
            addFaceLike(m, {B[k], A[k], A[k + 1], B[k + 1]}, e.f0);
        ++made;
    }

    // 3) Every original face, with its touched corners replaced. Indices stay.
    for (int f = 0; f < nf; ++f) {
        const std::vector<int> fv = m.faces[f];
        const int n = static_cast<int>(fv.size());
        std::vector<int> loop;
        for (int i = 0; i < n; ++i) {
            const int v = fv[i];
            if (!touched[v]) { loop.push_back(v); continue; }
            auto it = corner.find(fvKey(f, v));
            if (it != corner.end()) { loop.push_back(it->second); continue; }
            const int s1 = along(v, fv[(i + n - 1) % n]);
            const int s2 = along(v, fv[(i + 1) % n]);
            loop.push_back(s1);
            for (int k : between(s1, s2)) loop.push_back(k);
            loop.push_back(s2);
        }
        m.faces[f] = std::move(loop);
    }

    // 4) Patches: any rim left open between points cut from the SAME corner
    //    (three bevels meeting at a box corner leave a triangle there). Only
    //    rims that close are filled -- an open one is the mesh's own border.
    const std::unordered_set<std::uint64_t> half = halfEdges(m);
    std::unordered_map<int, int> next, srcOf;
    for (int f = 0; f < static_cast<int>(m.faces.size()); ++f) {
        const std::vector<int>& fv = m.faces[f];
        for (std::size_t i = 0; i < fv.size(); ++i) {
            const int x = fv[i], y = fv[(i + 1) % fv.size()];
            const auto ox = owner.find(x), oy = owner.find(y);
            if (ox == owner.end() || oy == owner.end() || ox->second != oy->second) continue;
            if (half.count(halfKey(y, x))) continue;
            next.try_emplace(y, x);
            srcOf.try_emplace(y, f);
        }
    }
    std::vector<int> starts;
    for (const auto& kv : next) starts.push_back(kv.first);
    std::sort(starts.begin(), starts.end());
    std::unordered_set<int> used;
    for (int s : starts) {
        if (used.count(s)) continue;
        std::vector<int> loop;
        int  cur    = s;
        bool closed = false;
        while (!used.count(cur)) {
            used.insert(cur);
            loop.push_back(cur);
            auto it = next.find(cur);
            if (it == next.end()) break;
            cur = it->second;
            if (cur == s) { closed = true; break; }
        }
        if (closed && loop.size() >= 3) addFaceLike(m, std::move(loop), srcOf[s]);
    }

    // The cut-off corners are used by nobody now.
    dropCollapsedFaces(m);
    return made;
}

} // namespace editmesh

// =============================================================================
// For the Blender-style modelling mode
// =============================================================================

namespace editmesh {

std::vector<std::pair<int, int>> extrudeEdges(EditMesh& m,
                                              const std::vector<std::pair<int, int>>& edges) {
    std::vector<std::pair<int, int>> out;
    const int nv = static_cast<int>(m.verts.size());
    std::unordered_map<int, int> copy;
    auto dup = [&](int v) {
        auto it = copy.find(v);
        if (it != copy.end()) return it->second;
        const glm::vec3 p = m.verts[v];
        const int c = addVert(m, p, m.paintAt(v));
        copy.emplace(v, c);
        return c;
    };
    const int nf = static_cast<int>(m.faces.size());
    for (const auto& e : edges) {
        const int a = e.first, b = e.second;
        if (a < 0 || b < 0 || a >= nv || b >= nv || a == b) continue;
        // The face beside it decides the winding: it runs the edge one way, the
        // new quad runs it the other, so the two meet like any two faces do.
        int  src = -1;
        bool fwd = true;
        for (int f = 0; f < nf && src < 0; ++f) {
            const std::vector<int>& fv = m.faces[f];
            for (std::size_t i = 0; i < fv.size(); ++i) {
                const int p = fv[i], q = fv[(i + 1) % fv.size()];
                if (p == a && q == b) { src = f; fwd = true;  break; }
                if (p == b && q == a) { src = f; fwd = false; break; }
            }
        }
        if (src < 0) continue;
        const int ca = dup(a), cb = dup(b);
        if (fwd) addFaceLike(m, {b, a, ca, cb}, src);
        else     addFaceLike(m, {a, b, cb, ca}, src);
        out.push_back({ca, cb});
    }
    return out;
}

std::vector<int> duplicateFaces(EditMesh& m, const std::vector<int>& faces) {
    const std::vector<int> fs = uniqueFaces(m, faces);
    std::unordered_map<int, int> copy;
    std::vector<int> out;
    for (int f : fs) {
        std::vector<int> loop;
        const std::vector<int> fv = m.faces[f];   // by value: m.faces grows
        for (int v : fv) {
            auto it = copy.find(v);
            if (it == copy.end()) {
                const glm::vec3 p = m.verts[v];
                it = copy.emplace(v, addVert(m, p, m.paintAt(v))).first;
            }
            loop.push_back(it->second);
        }
        out.push_back(addFaceLike(m, std::move(loop), f));
    }
    return out;
}

int recalcNormals(EditMesh& m, const std::vector<int>& faces) {
    const int nf = static_cast<int>(m.faces.size());
    std::vector<char> in(nf, faces.empty() ? 1 : 0);
    for (int f : faces)
        if (okFace(m, f)) in[f] = 1;
    // Faces meeting at each edge, among those taking part.
    std::unordered_map<std::uint64_t, std::vector<int>> at;
    for (int f = 0; f < nf; ++f) {
        if (!in[f] || !okFace(m, f)) continue;
        const std::vector<int>& fv = m.faces[f];
        for (std::size_t i = 0; i < fv.size(); ++i)
            at[edgeKey(fv[i], fv[(i + 1) % fv.size()])].push_back(f);
    }
    auto runs = [&](int f, int x, int y) {
        const std::vector<int>& fv = m.faces[f];
        for (std::size_t i = 0; i < fv.size(); ++i)
            if (fv[i] == x && fv[(i + 1) % fv.size()] == y) return true;
        return false;
    };
    auto flip = [&](int f) { std::reverse(m.faces[f].begin() + 1, m.faces[f].end()); };

    int flipped = 0;
    std::vector<char> seen(nf, 0);
    for (int seed = 0; seed < nf; ++seed) {
        if (!in[seed] || seen[seed] || !okFace(m, seed)) continue;
        // 1) Flood out from the seed, turning every neighbour to agree with the
        //    face it was reached from: a shared edge runs opposite ways.
        std::vector<int> piece{seed};
        std::vector<char> turned(nf, 0);
        seen[seed] = 1;
        for (std::size_t k = 0; k < piece.size(); ++k) {
            const int f = piece[k];
            const std::vector<int> fv = m.faces[f];
            for (std::size_t i = 0; i < fv.size(); ++i) {
                const int x = fv[i], y = fv[(i + 1) % fv.size()];
                for (int g : at[edgeKey(x, y)]) {
                    if (g == f || seen[g]) continue;
                    seen[g] = 1;
                    if (runs(g, x, y)) { flip(g); turned[g] = 1; }
                    piece.push_back(g);
                }
            }
        }
        // 2) Outward: the volume the piece encloses, measured from its own
        //    centre, comes out negative when its faces look in.
        glm::vec3 c(0.0f);
        int cn = 0;
        for (int f : piece)
            for (int v : m.faces[f]) { c += m.verts[v]; ++cn; }
        c /= static_cast<float>(std::max(cn, 1));
        float vol = 0.0f;
        for (int f : piece) {
            const std::vector<int>& fv = m.faces[f];
            for (std::size_t i = 1; i + 1 < fv.size(); ++i)
                vol += glm::dot(m.verts[fv[0]] - c,
                                glm::cross(m.verts[fv[i]] - c, m.verts[fv[i + 1]] - c));
        }
        if (vol < 0.0f)
            for (int f : piece) { flip(f); turned[f] = !turned[f]; }
        for (int f : piece) flipped += turned[f] ? 1 : 0;
    }
    return flipped;
}

std::vector<std::pair<int, int>> edgeLoop(const EditMesh& m, int a, int b) {
    std::vector<std::pair<int, int>> out;
    const std::vector<EdgeInfo> es = edges(m);
    std::unordered_map<std::uint64_t, int> idx;
    std::unordered_map<int, std::vector<int>> around;   // corner -> its edges
    for (int k = 0; k < static_cast<int>(es.size()); ++k) {
        idx[edgeKey(es[k].a, es[k].b)] = k;
        around[es[k].a].push_back(k);
        around[es[k].b].push_back(k);
    }
    auto it = idx.find(edgeKey(a, b));
    if (it == idx.end()) return out;
    const int  e0     = it->second;
    const bool border = es[e0].f1 < 0;
    std::vector<char> in(es.size(), 0);
    in[e0] = 1;
    out.push_back({std::min(a, b), std::max(a, b)});

    auto walk = [&](int v, int e) {
        for (;;) {
            const std::vector<int>& here = around[v];
            int next = -1;
            if (border) {
                for (int k : here)
                    if (k != e && es[k].f1 < 0) { if (next >= 0) return; next = k; }
            } else {
                if (here.size() != 4) return;   // not a plain crossing: the loop ends
                for (int k : here) {
                    if (k == e) continue;
                    const bool touches = es[k].f0 == es[e].f0 || es[k].f0 == es[e].f1 ||
                                         (es[k].f1 >= 0 && (es[k].f1 == es[e].f0 ||
                                                            es[k].f1 == es[e].f1));
                    if (!touches) { if (next >= 0) return; next = k; }
                }
            }
            if (next < 0 || in[next]) return;   // ran out, or came round
            in[next] = 1;
            out.push_back({std::min(es[next].a, es[next].b), std::max(es[next].a, es[next].b)});
            v = (es[next].a == v) ? es[next].b : es[next].a;
            e = next;
        }
    };
    walk(es[e0].b, e0);
    walk(es[e0].a, e0);
    return out;
}

} // namespace editmesh
