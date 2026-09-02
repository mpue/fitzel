#include "EditMesh.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <unordered_map>

namespace {

// Is `f` a usable index into a mesh with at least a triangle there?
bool okFace(const EditMesh& m, int f) {
    return f >= 0 && f < static_cast<int>(m.faces.size()) && m.faces[f].size() >= 3;
}

// Add a corner and its weights in one step. Every operation that grows the mesh
// goes through here: `paint` runs parallel to `verts`, and the one way to keep
// two arrays in step is to never lengthen one of them alone.
int addVert(EditMesh& m, const glm::vec3& p, const glm::vec4& w) {
    m.syncPaint();
    m.verts.push_back(p);
    m.paint.push_back(w);
    return static_cast<int>(m.verts.size()) - 1;
}

// Add a face grown out of `src`, the same bargain as addVert two arrays over:
// `faceMat` and `faceUV` both run parallel to `faces`, so nothing appends to one
// of them alone. It takes the SOURCE FACE rather than the values, because that
// is the only signature you cannot call while remembering one array and
// forgetting the other -- and a face grown out of another inherits both, which
// is what makes extruding a brick wall produce brick sides at the same brick
// size rather than four bare stubs with the texture starting over on each.
//
// Reading src's values before the push_backs is deliberate: they come out of the
// very vectors about to grow, and a reference into one of those does not survive
// the reallocation.
int addFaceLike(EditMesh& m, std::vector<int> loop, int src) {
    const fitzel::AssetId  mat = m.faceMaterial(src);
    const EditMesh::FaceUV uv  = m.faceUv(src);
    m.syncFaceMat();
    m.syncFaceUv();
    m.faces.push_back(std::move(loop));
    m.faceMat.push_back(mat);
    m.faceUV.push_back(uv);
    return static_cast<int>(m.faces.size()) - 1;
}

// Both ends of an edge in one key, order-independent -- the same edge is (a, b)
// in one face and (b, a) in its neighbour, and they have to hash alike or every
// face would think it stands alone.
std::uint64_t edgeKey(int a, int b) {
    const std::uint32_t lo = static_cast<std::uint32_t>(std::min(a, b));
    const std::uint32_t hi = static_cast<std::uint32_t>(std::max(a, b));
    return (static_cast<std::uint64_t>(hi) << 32) | lo;
}

// One face of a loop cut: which quad, which of its four edges the cut crosses,
// and which END of that edge `t` is measured from. That last one is the whole
// bookkeeping of the walk -- neighbouring quads wind opposite ways round their
// shared edge, so "0.3 along it" means two different points depending on which
// face is asked, and a ring that forgot to carry the reference end would cut a
// zigzag around the mesh instead of a line.
struct RingStep {
    int face = -1;
    int edge = 0;   // index into the face's loop: the edge (fv[e], fv[e+1])
    int ref  = -1;  // the end of that edge `t` counts from
};

// Faces meeting at each edge, at most the two a surface can have. Built once per
// loop cut: the mesh carries no adjacency of its own, and deriving it on demand
// is cheaper in every sense than keeping a half-edge structure in step through
// every operation above.
using EdgeMap = std::unordered_map<std::uint64_t, std::pair<int, int>>;

EdgeMap buildEdges(const EditMesh& m) {
    EdgeMap e;
    for (std::size_t f = 0; f < m.faces.size(); ++f) {
        const std::vector<int>& fv = m.faces[f];
        for (std::size_t i = 0; i < fv.size(); ++i) {
            auto& slot = e.try_emplace(edgeKey(fv[i], fv[(i + 1) % fv.size()]),
                                       -1, -1).first->second;
            if (slot.first < 0)       slot.first  = static_cast<int>(f);
            else if (slot.second < 0) slot.second = static_cast<int>(f);
            // A third face on one edge is not a surface. Ignored rather than
            // rejected: the cut stops there, which is the honest answer.
        }
    }
    return e;
}

// Walk the band one quad at a time, always leaving through the edge OPPOSITE the
// one it came in by -- which is what makes the cut a straight line rather than a
// wander. Stops at anything that is not an unvisited quad: the mesh's border, an
// n-gon, or the face it started from (a closed band around a box).
void walkRing(const EditMesh& m, const EdgeMap& edges, RingStep from,
              std::vector<char>& visited, std::vector<RingStep>& out) {
    RingStep cur = from;
    while (cur.face >= 0 && !visited[cur.face]) {
        visited[cur.face] = 1;
        out.push_back(cur);

        const std::vector<int>& fv = m.faces[cur.face];
        const int j   = cur.edge;
        const int opp = (j + 2) % 4;
        // The end of the far edge that corresponds to `ref`: the one joined to it
        // by a side of the quad rather than by the cut.
        const int oppRef = (fv[j] == cur.ref) ? fv[(j + 3) % 4] : fv[(j + 2) % 4];

        const int a = fv[opp], b = fv[(opp + 1) % 4];
        auto it = edges.find(edgeKey(a, b));
        if (it == edges.end()) break;
        const int next = (it->second.first == cur.face) ? it->second.second
                                                        : it->second.first;
        if (next < 0 || m.faces[next].size() != 4 || visited[next]) break;

        const std::vector<int>& nv = m.faces[next];
        int ne = -1;
        for (int k = 0; k < 4 && ne < 0; ++k) {
            const int p = nv[k], q = nv[(k + 1) % 4];
            if ((p == a && q == b) || (p == b && q == a)) ne = k;
        }
        if (ne < 0) break;
        cur = RingStep{next, ne, oppRef};
    }
}

// The whole band `face` lies in, in `dir` (0 or 1), start face included. Walked
// in both directions: an open band -- a wall, a strip of steps -- has the
// starting face somewhere in its middle, and a one-way walk would cut only the
// half of it that happens to lie ahead.
std::vector<RingStep> loopRing(const EditMesh& m, int face, int dir) {
    std::vector<RingStep> ring;
    if (!okFace(m, face) || m.faces[face].size() != 4) return ring;
    const EdgeMap edges = buildEdges(m);
    std::vector<char> visited(m.faces.size(), 0);

    const std::vector<int>& fv = m.faces[face];
    const int e0 = (dir & 1);
    walkRing(m, edges, RingStep{face, e0, fv[e0]}, visited, ring);

    // ...and the other way: step across the entry edge into the face behind, then
    // let the same walk carry on from there.
    const int a = fv[e0], b = fv[(e0 + 1) % 4];
    auto it = edges.find(edgeKey(a, b));
    if (it != edges.end()) {
        const int back = (it->second.first == face) ? it->second.second
                                                    : it->second.first;
        if (back >= 0 && m.faces[back].size() == 4 && !visited[back]) {
            const std::vector<int>& bv = m.faces[back];
            int be = -1;
            for (int k = 0; k < 4 && be < 0; ++k) {
                const int p = bv[k], q = bv[(k + 1) % 4];
                if ((p == a && q == b) || (p == b && q == a)) be = k;
            }
            if (be >= 0) walkRing(m, edges, RingStep{back, be, fv[e0]}, visited, ring);
        }
    }
    return ring;
}

} // namespace

EditMesh EditMesh::box(const glm::vec3& h) {
    EditMesh m;
    m.verts = {
        {-h.x, -h.y, -h.z}, { h.x, -h.y, -h.z}, { h.x,  h.y, -h.z}, {-h.x,  h.y, -h.z},
        {-h.x, -h.y,  h.z}, { h.x, -h.y,  h.z}, { h.x,  h.y,  h.z}, {-h.x,  h.y,  h.z},
    };
    // Each quad wound counter-clockwise seen from outside the box.
    m.faces = {
        {4, 5, 6, 7},   // +Z
        {1, 0, 3, 2},   // -Z
        {5, 1, 2, 6},   // +X
        {0, 4, 7, 3},   // -X
        {3, 7, 6, 2},   // +Y
        {0, 1, 5, 4},   // -Y
    };
    m.syncPaint();
    return m;
}

bool EditMesh::painted() const {
    for (const glm::vec4& w : paint)
        if (w.x > 0.0f || w.y > 0.0f || w.z > 0.0f || w.w > 0.0f) return true;
    return false;
}

void EditMesh::setFaceUv(int f, const FaceUV& u) {
    if (f < 0 || f >= static_cast<int>(faces.size())) return;
    syncFaceUv();
    faceUV[f] = u;
}

bool EditMesh::unwrapped() const {
    for (const FaceUV& u : faceUV)
        if (!u.isDefault()) return true;
    return false;
}

void EditMesh::setFaceMaterial(int f, const fitzel::AssetId& id) {
    if (f < 0 || f >= static_cast<int>(faces.size())) return;
    syncFaceMat();
    faceMat[f] = id;
}

bool EditMesh::dressed() const {
    for (const fitzel::AssetId& id : faceMat)
        if (id.valid()) return true;
    return false;
}

bool EditMesh::validFace(int f) const { return okFace(*this, f); }

glm::vec3 EditMesh::faceCenter(int f) const {
    if (!okFace(*this, f)) return glm::vec3(0.0f);
    glm::vec3 c(0.0f);
    for (int i : faces[f]) c += verts[i];
    return c / static_cast<float>(faces[f].size());
}

glm::vec3 EditMesh::faceNormal(int f) const {
    if (!okFace(*this, f)) return glm::vec3(0.0f, 1.0f, 0.0f);
    // Newell: sum the edge cross terms around the loop. Unlike one cross product
    // of two edges this survives a near-degenerate corner, which a face scaled
    // almost to nothing has plenty of.
    const std::vector<int>& fv = faces[f];
    glm::vec3 n(0.0f);
    for (std::size_t i = 0; i < fv.size(); ++i) {
        const glm::vec3& a = verts[fv[i]];
        const glm::vec3& b = verts[fv[(i + 1) % fv.size()]];
        n.x += (a.y - b.y) * (a.z + b.z);
        n.y += (a.z - b.z) * (a.x + b.x);
        n.z += (a.x - b.x) * (a.y + b.y);
    }
    const float len = glm::length(n);
    return len > 1e-12f ? n / len : glm::vec3(0.0f, 1.0f, 0.0f);
}

float EditMesh::faceArea(int f) const {
    if (!okFace(*this, f)) return 0.0f;
    const std::vector<int>& fv = faces[f];
    const glm::vec3 c = faceCenter(f);
    float a = 0.0f;
    for (std::size_t i = 0; i < fv.size(); ++i)
        a += 0.5f * glm::length(glm::cross(verts[fv[i]] - c,
                                           verts[fv[(i + 1) % fv.size()]] - c));
    return a;
}

void EditMesh::bounds(glm::vec3& mn, glm::vec3& mx) const {
    if (verts.empty()) { mn = mx = glm::vec3(0.0f); return; }
    mn = mx = verts[0];
    for (const glm::vec3& v : verts) { mn = glm::min(mn, v); mx = glm::max(mx, v); }
}

namespace editmesh {

std::uint64_t nextRevision() {
    static std::atomic<std::uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

int extrude(EditMesh& m, int face, float dist) {
    if (!okFace(m, face)) return -1;
    const glm::vec3 n = m.faceNormal(face);
    const std::vector<int> loop = m.faces[face];   // by value: m.faces grows below

    // The moved copy gets its OWN corners. Without that, walking the face out
    // would drag every neighbour that shares those corners along with it, and
    // extrude would be indistinguishable from moveFace.
    std::vector<int> cap;
    cap.reserve(loop.size());
    for (int i : loop) {
        // The copy inherits the corner's paint, so extruding a painted face
        // carries the paint out with it instead of leaving the new cap bare.
        cap.push_back(addVert(m, m.verts[i] + n * dist, m.paintAt(i)));
    }

    // Wall in the gap: one quad per edge of the loop, wound (a, b, b', a'). Its
    // normal works out as dist * cross(edge, n), which is the whole trick -- the
    // winding follows the sign of the distance by itself. Pulled out, the walls
    // face away from the new tower; pushed in, they face into the recess, which
    // is the side you look at when you sink a window into a wall. Flipping the
    // order for a negative distance (as this once did) turns the recess
    // inside out and its walls vanish.
    for (std::size_t i = 0; i < loop.size(); ++i) {
        const std::size_t j = (i + 1) % loop.size();
        // The walls wear what the face wore: extruding a brick panel grows brick
        // sides, not four faces in the object's own material.
        addFaceLike(m, {loop[i], loop[j], cap[j], cap[i]}, face);
    }
    m.faces[face] = cap;   // the cap takes the selection with it
    return face;
}

int scaleFace(EditMesh& m, int face, float factor) {
    if (!okFace(m, face)) return -1;
    const glm::vec3 c = m.faceCenter(face);
    for (int i : m.faces[face]) m.verts[i] = c + (m.verts[i] - c) * factor;
    return face;
}

int moveFace(EditMesh& m, int face, float dist) {
    if (!okFace(m, face)) return -1;
    const glm::vec3 d = m.faceNormal(face) * dist;
    // Shared corners are the point here: the neighbouring faces stretch to
    // follow, so a box gets taller instead of growing a stub.
    for (int i : m.faces[face]) m.verts[i] += d;
    return face;
}

int inset(EditMesh& m, int face, float amount) {
    if (!okFace(m, face)) return -1;
    // An extrude that goes nowhere gives the face its own corners; scaling those
    // then leaves a rim of side quads with zero height -- the inset border.
    extrude(m, face, 0.0f);
    // `amount` is the width of the border in metres, turned into a scale by the
    // face's own size. The size that matters is the distance from the centre to
    // the EDGES, not to the corners: the border is the strip along an edge, and
    // measuring to the corners (which are further out by a factor of root two on
    // a square) quietly gives a narrower border than was asked for.
    const std::vector<int>& fv = m.faces[face];
    const glm::vec3 c = m.faceCenter(face);
    float r = 0.0f;
    for (std::size_t i = 0; i < fv.size(); ++i) {
        const glm::vec3& a = m.verts[fv[i]];
        const glm::vec3& b = m.verts[fv[(i + 1) % fv.size()]];
        const float len = glm::length(b - a);
        if (len > 1e-6f) r += glm::length(glm::cross(b - a, c - a)) / len;
    }
    r /= static_cast<float>(fv.size());
    const float factor = (r > 1e-5f) ? std::clamp(1.0f - amount / r, 0.0f, 4.0f) : 1.0f;
    return scaleFace(m, face, factor);
}

int subdivide(EditMesh& m, int face) {
    if (!okFace(m, face) || m.faces[face].size() != 4) return -1;
    const std::vector<int> q = m.faces[face];
    const int base = static_cast<int>(m.verts.size());
    // Four edge midpoints, then the centre. The new corners take the average of
    // the ones they sit between, which is exactly what the shader would have
    // interpolated there -- so splitting a face for a finer brush does not change
    // how the face already looks.
    glm::vec4 pc(0.0f);
    for (int i = 0; i < 4; ++i) {
        addVert(m, 0.5f * (m.verts[q[i]] + m.verts[q[(i + 1) % 4]]),
                0.5f * (m.paintAt(q[i]) + m.paintAt(q[(i + 1) % 4])));
        pc += 0.25f * m.paintAt(q[i]);
    }
    addVert(m, m.faceCenter(face), pc);
    const int e0 = base, e1 = base + 1, e2 = base + 2, e3 = base + 3, ct = base + 4;
    m.faces[face] = {q[0], e0, ct, e3};
    addFaceLike(m, {e0, q[1], e1, ct}, face);
    addFaceLike(m, {ct, e1, q[2], e2}, face);
    addFaceLike(m, {e3, ct, e2, q[3]}, face);
    return face;
}

int loopLength(const EditMesh& m, int face, int dir) {
    return static_cast<int>(loopRing(m, face, dir).size());
}

int loopCut(EditMesh& m, int face, int dir, float t) {
    const std::vector<RingStep> ring = loopRing(m, face, dir);
    if (ring.empty()) return -1;
    // Never on top of an existing corner: a cut at 0 or 1 is a face split into
    // itself and a zero-width sliver, which is geometry that draws as nothing and
    // gets in the way of everything afterwards.
    t = std::clamp(t, 0.02f, 0.98f);

    // One new corner per crossed EDGE, not per face: two faces share the edge
    // between them, and cutting it twice would leave the halves meeting at two
    // corners in the same place -- a seam that shows the moment either side moves.
    // Keyed without regard to direction, which is safe precisely because the walk
    // carries the reference end around: both faces measure the edge from the same
    // corner, so the corner one of them made is the one the other wanted.
    std::unordered_map<std::uint64_t, int> cutVert;
    auto cutOn = [&](int from, int to) {
        const std::uint64_t k = edgeKey(from, to);
        auto it = cutVert.find(k);
        if (it != cutVert.end()) return it->second;
        const int v = addVert(m, glm::mix(m.verts[from], m.verts[to], t),
                              glm::mix(m.paintAt(from), m.paintAt(to), t));
        cutVert.emplace(k, v);
        return v;
    };

    // Split every face in the band. The near half keeps the face's index -- and
    // with it the selection, since the ring always starts at the face that was
    // asked for -- and the far half is appended; both wear the material the face
    // had.
    for (const RingStep& st : ring) {
        const std::vector<int> fv = m.faces[st.face];   // by value: m.faces grows
        const int j = st.edge;
        // Read the quad starting at the crossed edge, so the two cuts are always
        // on sides 0 and 2 of `w` and the rest is one shape rather than four cases.
        const int w0 = fv[j], w1 = fv[(j + 1) % 4],
                  w2 = fv[(j + 2) % 4], w3 = fv[(j + 3) % 4];
        // `ref` says which end of the crossed edge t counts from; the far edge is
        // measured from the corner joined to it along the quad's side.
        const bool fwd = (w0 == st.ref);
        const int  a   = fwd ? cutOn(w0, w1) : cutOn(w1, w0);
        const int  b   = fwd ? cutOn(w3, w2) : cutOn(w2, w3);

        m.faces[st.face] = {w0, a, b, w3};
        addFaceLike(m, {a, w1, w2, b}, st.face);
    }
    return face;
}

int deleteFace(EditMesh& m, int face) {
    if (face < 0 || face >= static_cast<int>(m.faces.size())) return -1;
    // The face's material and texture placement go with it, or every face after
    // this one would inherit its neighbour's -- the parallel-array failure, one
    // array over, twice.
    m.syncFaceMat();
    m.syncFaceUv();
    m.faceMat.erase(m.faceMat.begin() + face);
    m.faceUV.erase(m.faceUV.begin() + face);
    m.faces.erase(m.faces.begin() + face);

    // Drop the vertices no face uses any more and renumber, or a long editing
    // session leaves the mesh carrying every corner it ever had.
    std::vector<int> remap(m.verts.size(), -1);
    for (const std::vector<int>& f : m.faces)
        for (int i : f)
            if (i >= 0 && i < static_cast<int>(remap.size())) remap[i] = 0;
    m.syncPaint();
    std::vector<glm::vec3> kept;
    std::vector<glm::vec4> keptPaint;
    kept.reserve(m.verts.size());
    keptPaint.reserve(m.paint.size());
    for (std::size_t i = 0; i < m.verts.size(); ++i)
        if (remap[i] == 0) {
            remap[i] = static_cast<int>(kept.size());
            kept.push_back(m.verts[i]);
            keptPaint.push_back(m.paint[i]);   // weights follow their corner
        }
    m.verts = std::move(kept);
    m.paint = std::move(keptPaint);
    for (std::vector<int>& f : m.faces)
        for (int& i : f) i = remap[i];

    // Selecting the face that slid into the deleted one's place is what the hand
    // expects when clearing several in a row.
    if (m.faces.empty()) return -1;
    return std::min(face, static_cast<int>(m.faces.size()) - 1);
}

int transformFace(EditMesh& m, int face, const glm::mat4& xform) {
    if (!okFace(m, face)) return -1;
    // Each corner exactly once. A loop that lists the same vertex twice (which a
    // face collapsed by scaling to zero can produce) would otherwise have it
    // rotated twice as far as the rest.
    const std::vector<int>& fv = m.faces[face];
    for (std::size_t k = 0; k < fv.size(); ++k) {
        bool seen = false;
        for (std::size_t p = 0; p < k && !seen; ++p) seen = (fv[p] == fv[k]);
        if (!seen) m.verts[fv[k]] = glm::vec3(xform * glm::vec4(m.verts[fv[k]], 1.0f));
    }
    return face;
}

glm::vec3 recenter(EditMesh& m) {
    if (m.verts.empty()) return glm::vec3(0.0f);
    glm::vec3 mn, mx;
    m.bounds(mn, mx);
    const glm::vec3 shift = 0.5f * (mn + mx);
    if (glm::dot(shift, shift) < 1e-12f) return glm::vec3(0.0f);
    for (glm::vec3& v : m.verts) v -= shift;
    return shift;
}

std::vector<glm::vec2> faceUvs(const EditMesh& m, int face) {
    return faceUvs(m, face, m.faceUv(face));
}

std::vector<glm::vec2> faceUvs(const EditMesh& m, int face,
                               const EditMesh::FaceUV& u) {
    std::vector<glm::vec2> out;
    if (!m.validFace(face)) return out;
    const std::vector<int>& fv = m.faces[face];

    // The direction the texture is projected ALONG. By default the face's own
    // normal, which is the projection that never collapses; a named axis is the
    // user saying "these faces belong to one surface, texture them as one".
    glm::vec3 d = (u.axis == 1)   ? glm::vec3(1, 0, 0)
                  : (u.axis == 2) ? glm::vec3(0, 1, 0)
                  : (u.axis == 3) ? glm::vec3(0, 0, 1)
                                  : m.faceNormal(face);
    if (glm::dot(d, d) < 1e-12f) d = glm::vec3(0, 1, 0);   // a degenerate face
    d = glm::normalize(d);
    const glm::vec3 ad = glm::abs(d);
    const glm::vec3 up = (ad.y > ad.x && ad.y > ad.z) ? glm::vec3(0, 0, 1)
                                                      : glm::vec3(0, 1, 0);
    const glm::vec3 tx = glm::normalize(glm::cross(up, d));
    const glm::vec3 ty = glm::cross(d, tx);

    // Rotation, flip and size act about the face's own centre in that plane. Not
    // about the projection's origin: a face twenty metres from it would see a
    // one-degree turn as a slide of half a metre, which is not what turning a
    // texture means to anyone.
    const glm::vec3 c3 = m.faceCenter(face);
    const glm::vec2 c(glm::dot(c3, tx), glm::dot(c3, ty));
    const float a  = glm::radians(u.rotate);
    const float ca = std::cos(a), sa = std::sin(a);
    // A size of zero is a division by zero and a mesh full of NaN UVs, which
    // draws as nothing and looks like the geometry broke.
    const glm::vec2 sz(std::abs(u.size.x) < 1e-4f ? 1e-4f : u.size.x,
                       std::abs(u.size.y) < 1e-4f ? 1e-4f : u.size.y);

    out.reserve(fv.size());
    for (int i : fv) {
        const glm::vec2 p(glm::dot(m.verts[i], tx), glm::dot(m.verts[i], ty));
        glm::vec2       q = p - c;
        q = glm::vec2(q.x * ca + q.y * sa, -q.x * sa + q.y * ca);
        if (u.flipU) q.x = -q.x;
        if (u.flipV) q.y = -q.y;
        // At every default this is (p - c + c) / 1 + 0, i.e. p -- the metres-in-
        // the-projection-plane the mesh has always been textured with.
        out.push_back((q + c) / sz + u.offset);
    }
    return out;
}

std::vector<Group> buildGroups(const EditMesh& m) {
    std::vector<Group> groups;
    groups.push_back(Group{});   // the object's own material, always first
    // Which group a face's material belongs to. Linear: a mesh wears a handful of
    // materials, and a map over that is bookkeeping nobody reads back.
    auto groupFor = [&](const fitzel::AssetId& id) -> fitzel::MeshData& {
        if (!id.valid()) return groups[0].data;
        for (Group& g : groups)
            if (g.material == id) return g.data;
        groups.push_back(Group{id, fitzel::MeshData{}});
        return groups.back().data;
    };

    for (std::size_t f = 0; f < m.faces.size(); ++f) {
        const std::vector<int>& fv = m.faces[f];
        if (fv.size() < 3) continue;
        fitzel::MeshData& d = groupFor(m.faceMaterial(static_cast<int>(f)));
        const glm::vec3 n = m.faceNormal(static_cast<int>(f));
        // The face's texture coordinates, in its own loop order. Indexed by
        // POSITION in the loop, not by vertex id: corners are shared between
        // faces and each face places its texture on them itself.
        const std::vector<glm::vec2> uvs = faceUvs(m, static_cast<int>(f));
        auto vert = [&](std::size_t k) {
            fitzel::Vertex v;
            v.position = m.verts[fv[k]];
            v.normal   = n;
            v.uv       = uvs[k];
            v.paint    = m.paintAt(fv[k]);
            return v;
        };
        // Fan from the first corner. Fine for the convex faces these operations
        // produce; a concave one would fan across itself, which is a limit worth
        // having rather than a triangulator worth writing.
        for (std::size_t i = 1; i + 1 < fv.size(); ++i) {
            d.vertices.push_back(vert(0));
            d.vertices.push_back(vert(i));
            d.vertices.push_back(vert(i + 1));
        }
    }
    // An empty first group means every face wears a material of its own; drawing
    // an empty mesh is a draw call that paints nothing, so drop it.
    if (groups[0].data.vertices.empty() && groups.size() > 1)
        groups.erase(groups.begin());
    return groups;
}

fitzel::MeshData build(const EditMesh& m) {
    fitzel::MeshData d;
    for (const Group& g : buildGroups(m))
        d.vertices.insert(d.vertices.end(), g.data.vertices.begin(),
                          g.data.vertices.end());
    return d;
}

} // namespace editmesh

const std::vector<EditMeshCache::Sub>& EditMeshCache::submeshes(
    int entityId, std::uint64_t revision, const EditMesh& m) {
    // A fresh entry has revision 0 and a real one never does (the stamps start at
    // 1), so this one comparison covers "not uploaded yet" as well as "stale".
    Entry& e = m_entries[entityId];
    if (e.revision != revision) {
        e.subs.clear();
        for (editmesh::Group& g : editmesh::buildGroups(m))
            e.subs.push_back(Sub{g.material, fitzel::Mesh::create(g.data)});
        e.revision = revision;
    }
    return e.subs;
}
