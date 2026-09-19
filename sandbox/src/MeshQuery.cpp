#include "MeshQuery.hpp"

#include <algorithm>
#include <cmath>

#include "SceneGraph.hpp"

namespace meshquery {

void verticalHits(const Entity& e, const EditMesh& m, float x, float z,
                  std::vector<Hit>& out) {
    out.clear();
    if (m.verts.empty()) return;
    // Nothing of the mesh reaches outside the sphere round its box, however it
    // is turned -- the cheap no before the faces are looked at.
    const float r = glm::length(e.half);
    const float dx = x - e.center.x, dz = z - e.center.z;
    if (dx * dx + dz * dz > r * r) return;

    const glm::mat4 M = scenegraph::compose(e.center, e.rotation,
                                            editmesh::fitScale(m, e.half));
    const glm::mat4 inv = glm::inverse(M);
    // Start above the top and run down: t along this local ray is metres in the
    // world, so a hit's height is simply yTop - t.
    const float     yTop = e.center.y + r + 1.0f;
    const glm::vec3 o    = glm::vec3(inv * glm::vec4(x, yTop, z, 1.0f));
    const glm::vec3 d    = glm::vec3(inv * glm::vec4(0.0f, -1.0f, 0.0f, 0.0f));

    // Moller-Trumbore, both sides: a face counts whichever way it is wound.
    auto hit = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
        const glm::vec3 e1 = b - a, e2 = c - a;
        const glm::vec3 p  = glm::cross(d, e2);
        const float det = glm::dot(e1, p);
        if (std::fabs(det) < 1e-12f) return;
        const float     f = 1.0f / det;
        const glm::vec3 s = o - a;
        const float u = f * glm::dot(s, p);
        if (u < 0.0f || u > 1.0f) return;
        const glm::vec3 q = glm::cross(s, e1);
        const float v = f * glm::dot(d, q);
        if (v < 0.0f || u + v > 1.0f) return;
        const float t = f * glm::dot(e2, q);
        // det = -dot(d, normal): positive where the face looks against the
        // downward line, i.e. up.
        if (t >= 0.0f) out.push_back({yTop - t, det > 0.0f});
    };
    // A fan per face, as buildGroups draws it.
    for (const std::vector<int>& f : m.faces)
        for (std::size_t k = 1; k + 1 < f.size(); ++k)
            hit(m.verts[f[0]], m.verts[f[k]], m.verts[f[k + 1]]);
    std::sort(out.begin(), out.end(), [](const Hit& a, const Hit& b) { return a.y > b.y; });
    // A line through a shared edge crosses both faces beside it: one crossing.
    out.erase(std::unique(out.begin(), out.end(),
                          [](const Hit& a, const Hit& b) {
                              return std::fabs(a.y - b.y) < 1e-4f && a.up == b.up;
                          }),
              out.end());
}

bool surfaceBelow(const Entity& e, const EditMesh& m, float x, float z,
                  float yMax, float& y) {
    thread_local std::vector<Hit> hits;
    verticalHits(e, m, x, z, hits);
    for (const Hit& h : hits)
        if (h.up && h.y <= yMax) { y = h.y; return true; }
    return false;
}

bool blocks(const Entity& e, const EditMesh& m, float x, float z, float yLo, float yHi) {
    thread_local std::vector<Hit> hits;
    verticalHits(e, m, x, z, hits);
    int above = 0;
    for (const Hit& h : hits) {
        if (h.y >= yLo && h.y <= yHi) return true;
        if (h.y > yHi) ++above;
    }
    return (above % 2) == 1;
}

} // namespace meshquery
