#include "EditMesh.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace {

// Is `f` a usable index into a mesh with at least a triangle there?
bool okFace(const EditMesh& m, int f) {
    return f >= 0 && f < static_cast<int>(m.faces.size()) && m.faces[f].size() >= 3;
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
    return m;
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
        cap.push_back(static_cast<int>(m.verts.size()));
        m.verts.push_back(m.verts[i] + n * dist);
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
        m.faces.push_back({loop[i], loop[j], cap[j], cap[i]});
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
    // Four edge midpoints, then the centre.
    for (int i = 0; i < 4; ++i)
        m.verts.push_back(0.5f * (m.verts[q[i]] + m.verts[q[(i + 1) % 4]]));
    m.verts.push_back(m.faceCenter(face));
    const int e0 = base, e1 = base + 1, e2 = base + 2, e3 = base + 3, ct = base + 4;
    m.faces[face] = {q[0], e0, ct, e3};
    m.faces.push_back({e0, q[1], e1, ct});
    m.faces.push_back({ct, e1, q[2], e2});
    m.faces.push_back({e3, ct, e2, q[3]});
    return face;
}

int deleteFace(EditMesh& m, int face) {
    if (face < 0 || face >= static_cast<int>(m.faces.size())) return -1;
    m.faces.erase(m.faces.begin() + face);

    // Drop the vertices no face uses any more and renumber, or a long editing
    // session leaves the mesh carrying every corner it ever had.
    std::vector<int> remap(m.verts.size(), -1);
    for (const std::vector<int>& f : m.faces)
        for (int i : f)
            if (i >= 0 && i < static_cast<int>(remap.size())) remap[i] = 0;
    std::vector<glm::vec3> kept;
    kept.reserve(m.verts.size());
    for (std::size_t i = 0; i < m.verts.size(); ++i)
        if (remap[i] == 0) { remap[i] = static_cast<int>(kept.size()); kept.push_back(m.verts[i]); }
    m.verts = std::move(kept);
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

fitzel::MeshData build(const EditMesh& m) {
    fitzel::MeshData d;
    d.vertices.reserve(m.faces.size() * 6);
    for (std::size_t f = 0; f < m.faces.size(); ++f) {
        const std::vector<int>& fv = m.faces[f];
        if (fv.size() < 3) continue;
        const glm::vec3 n = m.faceNormal(static_cast<int>(f));
        // Planar UVs along whichever axis the face faces least, so the projection
        // never collapses. Metres per unit UV, so a texture keeps its scale as a
        // face is extruded rather than stretching with it.
        const glm::vec3 an = glm::abs(n);
        const glm::vec3 up = (an.y > an.x && an.y > an.z) ? glm::vec3(0, 0, 1)
                                                          : glm::vec3(0, 1, 0);
        const glm::vec3 tx = glm::normalize(glm::cross(up, n));
        const glm::vec3 ty = glm::cross(n, tx);
        auto vert = [&](int i) {
            fitzel::Vertex v;
            v.position = m.verts[i];
            v.normal   = n;
            v.uv       = glm::vec2(glm::dot(m.verts[i], tx), glm::dot(m.verts[i], ty));
            return v;
        };
        // Fan from the first corner. Fine for the convex faces these operations
        // produce; a concave one would fan across itself, which is a limit worth
        // having rather than a triangulator worth writing.
        for (std::size_t i = 1; i + 1 < fv.size(); ++i) {
            d.vertices.push_back(vert(fv[0]));
            d.vertices.push_back(vert(fv[i]));
            d.vertices.push_back(vert(fv[i + 1]));
        }
    }
    return d;
}

} // namespace editmesh

const fitzel::Mesh& EditMeshCache::mesh(int entityId, std::uint64_t revision,
                                        const EditMesh& m) {
    // A fresh entry has revision 0 and a real one never does (the stamps start at
    // 1), so this one comparison covers "not uploaded yet" as well as "stale".
    Entry& e = m_entries[entityId];
    if (e.revision != revision) {
        e.mesh     = fitzel::Mesh::create(editmesh::build(m));
        e.revision = revision;
    }
    return e.mesh;
}
