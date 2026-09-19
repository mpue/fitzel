#include "PhysicsShapes.hpp"

#include <cstdint>
#include <vector>

#include "Component.hpp"
#include "EditMesh.hpp"
#include "SceneGraph.hpp"

using fitzel::PhysicsBodyId;

namespace {

// A modelled mesh as a collider, or 0 where it cannot be one.
PhysicsBodyId addMeshBody(fitzel::PhysicsWorld& w, const Entity& e,
                          const EditMesh& m, float mass, const glm::quat& q) {
    if (m.verts.size() < 3 || m.faces.empty()) return 0;
    // The scale the mesh is DRAWN with, so it collides where it is seen.
    const glm::vec3 s = editmesh::fitScale(m, e.half);
    if (mass <= 0.0f) {
        const glm::mat4 M = scenegraph::compose(e.center, e.rotation, s);
        std::vector<glm::vec3> verts;
        verts.reserve(m.verts.size());
        for (const glm::vec3& v : m.verts) verts.push_back(glm::vec3(M * glm::vec4(v, 1.0f)));
        // A fan per face: EditMesh faces are convex polygons (buildGroups
        // triangulates them the same way for drawing).
        std::vector<std::uint32_t> idx;
        for (const std::vector<int>& f : m.faces)
            for (std::size_t k = 1; k + 1 < f.size(); ++k) {
                idx.push_back(static_cast<std::uint32_t>(f[0]));
                idx.push_back(static_cast<std::uint32_t>(f[k]));
                idx.push_back(static_cast<std::uint32_t>(f[k + 1]));
            }
        if (idx.empty()) return 0;
        return w.addMesh(verts.data(), static_cast<int>(verts.size()), idx.data(),
                         static_cast<int>(idx.size()));
    }
    if (m.verts.size() < 4) return 0;
    std::vector<glm::vec3> pts;
    pts.reserve(m.verts.size());
    for (const glm::vec3& v : m.verts) pts.push_back(v * s);
    return w.addConvexHull(pts.data(), static_cast<int>(pts.size()), e.center, q, mass);
}

} // namespace

PhysicsBodyId addEntityBody(fitzel::PhysicsWorld& w, const Entity& e, float mass,
                            const LoadedModel* lm) {
    const glm::quat q = glm::quat(glm::radians(e.rotation));
    if (const auto* mc = e.components.get<MeshComponent>())
        if (PhysicsBodyId id = addMeshBody(w, e, mc->mesh, mass, q)) return id;

    switch (e.type) {
        case EntityType::Sphere:
            return w.addSphere((e.half.x + e.half.y + e.half.z) / 3.0f, e.center, mass);
        case EntityType::Cylinder:
            return w.addCylinder(e.half.x, e.half.y, e.center, q, mass);
        case EntityType::Ramp: {
            // Triangular-prism wedge: rises along +Z (front-bottom to back-top),
            // matching the ramp mesh and the walk collider.
            const glm::vec3 h = e.half;
            const glm::vec3 pts[6] = {
                {-h.x, -h.y, -h.z}, { h.x, -h.y, -h.z},
                {-h.x, -h.y,  h.z}, { h.x, -h.y,  h.z},
                {-h.x,  h.y,  h.z}, { h.x,  h.y,  h.z}};
            if (PhysicsBodyId id = w.addConvexHull(pts, 6, e.center, q, mass)) return id;
            break;
        }
        case EntityType::Model: {
            // Convex hull of the model's vertices (centred + scaled to match the
            // render), falling back to the AABB box.
            const auto* mdl = e.components.get<ModelComponent>();
            if (mdl && lm && lm->hullPoints.size() >= 4) {
                const glm::vec3 c = lm->center();
                std::vector<glm::vec3> pts;
                pts.reserve(lm->hullPoints.size());
                for (const glm::vec3& v : lm->hullPoints) pts.push_back((v - c) * mdl->scale);
                if (PhysicsBodyId id = w.addConvexHull(
                        pts.data(), static_cast<int>(pts.size()), e.center, q, mass))
                    return id;
            }
            break;
        }
        default:
            break;
    }
    return w.addBox(e.half, e.center, q, mass);
}
