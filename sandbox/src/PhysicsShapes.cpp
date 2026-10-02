#include "PhysicsShapes.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <glm/gtc/quaternion.hpp>

#include "Component.hpp"
#include "EditMesh.hpp"
#include "Modifiers.hpp"
#include "SceneGraph.hpp"

using fitzel::PhysicsBodyId;

namespace {

// A corner rounded to a tenth of a millimetre: close enough that the copies an
// exporter makes of every corner (one per face, for flat shading) fall together.
struct Corner {
    std::int32_t x, y, z;
    bool operator==(const Corner& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct CornerHash {
    std::size_t operator()(const Corner& c) const {
        return (static_cast<std::size_t>(c.x) * 73856093u) ^
               (static_cast<std::size_t>(c.y) * 19349663u) ^
               (static_cast<std::size_t>(c.z) * 83492791u);
    }
};

// A static model as its own triangles (LoadedModel::meshTris), or 0 where it
// has none. The triangles go into the world once per model, as one shape every
// copy of it shares -- a rock scattered two hundred times is one mesh -- and
// each copy is placed by the transform it is DRAWN with (SceneSubmit: centre,
// rotation, half-extents over the model's size), so it collides where it is seen.
PhysicsBodyId addModelMeshBody(fitzel::PhysicsWorld& w, const Entity& e,
                               const LoadedModel& lm) {
    const std::size_t n = lm.meshTris.size() / 3 * 3;
    if (n < 3) return 0;
    // The id alone is not enough: a model re-imported under it is a new mesh.
    const std::uint64_t key =
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(lm.id)) << 32) ^ n;
    if (!w.hasMeshShape(key)) {
        // Welded, so neighbouring triangles share their corners. That is how
        // Jolt tells the diagonal inside a flat floor from a real edge; without
        // it a crate sliding over the floor is kicked sideways by every one
        // (collidecheck measures it).
        const glm::vec3 c = lm.center();
        std::vector<glm::vec3>     verts;
        std::vector<std::uint32_t> idx;
        idx.reserve(n);
        std::unordered_map<Corner, std::uint32_t, CornerHash> at;
        at.reserve(n / 2);
        for (std::size_t i = 0; i < n; ++i) {
            const glm::vec3 p = lm.meshTris[i] - c;
            const glm::vec3 g = glm::round(p * 1.0e4f);
            const Corner k{static_cast<std::int32_t>(g.x), static_cast<std::int32_t>(g.y),
                           static_cast<std::int32_t>(g.z)};
            auto [it, fresh] = at.try_emplace(k, static_cast<std::uint32_t>(verts.size()));
            if (fresh) verts.push_back(p);
            idx.push_back(it->second);
        }
        w.cacheMeshShape(key, verts.data(), static_cast<int>(verts.size()), idx.data(),
                         static_cast<int>(idx.size()));
    }
    const glm::vec3 sz = lm.size();
    glm::vec3 s(1.0f);
    for (int k = 0; k < 3; ++k)
        if (sz[k] > 1e-4f) s[k] = 2.0f * e.half[k] / sz[k];
    // The rotation out of the very matrix the renderer composes, so the two
    // cannot disagree about Euler order.
    const glm::quat q = glm::normalize(glm::quat_cast(
        glm::mat3(scenegraph::compose(glm::vec3(0.0f), e.rotation, glm::vec3(1.0f)))));
    return w.addMeshInstance(key, e.center, q, s);
}

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
    // A modelled mesh collides as what it shows -- its modifier stack's result.
    if (const auto* mc = e.components.get<MeshComponent>())
        if (PhysicsBodyId id = addMeshBody(w, e, *modifiers::shown(e, *mc).mesh, mass, q))
            return id;

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
            // Static: its own triangles. Otherwise -- or for a model that is all
            // leaves -- the convex hull of its vertices (centred + scaled to
            // match the render), falling back to the AABB box.
            const auto* mdl = e.components.get<ModelComponent>();
            if (mdl && lm && mass <= 0.0f)
                if (PhysicsBodyId id = addModelMeshBody(w, e, *lm)) return id;
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

bool isModelGroup(const Entity& e) {
    return e.type == EntityType::Model && !e.components.get<ModelComponent>();
}

int addGroupBodies(fitzel::PhysicsWorld& w, const std::vector<Entity>& entities,
                   const Entity& root,
                   const std::function<const LoadedModel*(const Entity&)>& modelOf) {
    std::unordered_map<int, std::vector<const Entity*>> kids;
    for (const Entity& c : entities)
        if (c.parent >= 0) kids[c.parent].push_back(&c);
    int made = 0;
    std::vector<const Entity*> open{&root};
    while (!open.empty()) {
        const Entity* p = open.back();
        open.pop_back();
        auto it = kids.find(p->id);
        if (it == kids.end()) continue;
        for (const Entity* c : it->second) {
            if (!c->activeInHierarchy) continue;
            // Its own collider -- the barrel in the hall that is meant to roll --
            // and whatever hangs below it is its own business too.
            if (c->components.get<PhysicsComponent>()) continue;
            // A part that is nothing but leaves or decals stays out: on its own it
            // would fall back to its hull, and the grass around a building
            // hulls into a ring round the whole of it.
            if (c->components.get<ModelComponent>())
                if (const LoadedModel* lm = modelOf(*c))
                    if (lm->meshTris.size() >= 3 && addEntityBody(w, *c, 0.0f, lm)) ++made;
            open.push_back(c);
        }
    }
    return made;
}
