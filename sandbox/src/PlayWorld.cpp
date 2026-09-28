#include "PlayWorld.hpp"

#include <glm/gtc/quaternion.hpp>

#include "CitySystem.hpp"
#include "Component.hpp"
#include "ModelLibrary.hpp"
#include "PhysicsShapes.hpp"
#include "RoadSet.hpp"
#include "RoadSystem.hpp"
#include "SplineSystem.hpp"
#include "TownTraffic.hpp"

using fitzel::PhysicsBodyId;

namespace playworld {

void addStaticWorld(fitzel::PhysicsWorld& physics, const RoadSet& roads,
                    const CitySystem& towns, const SplineSystem& splines,
                    const std::function<LoadedModel*(const std::string&)>& resolveModel,
                    std::vector<SidePost>& posts) {
    // Roads: every one in the scene, each with its own collider, its own
    // rails and posts and its own city. A hidden road is not there to be
    // driven on either, which is what makes the checkbox in the road list
    // a way to try a layout without deleting the other one.
    posts.clear();
    for (const RoadSystem* rp : roads) {
        const RoadSystem& road = *rp;
        if (!road.enabled) continue;
        // A static triangle-mesh collider (from the last Build, graded
        // into the terrain), so the player and objects can walk/drive on it.
        if (road.collIndices().size() >= 3)
            physics.addMesh(road.collVerts().data(),
                            static_cast<int>(road.collVerts().size()),
                            road.collIndices().data(),
                            static_cast<int>(road.collIndices().size()));
        // Side objects collide as a box each, sized to the model's AABB. Rails
        // and curbs are static (mass 0) -- they stop a car driving off the edge.
        // Knockable lines (posts, bollards) are DYNAMIC, so the car bowls them
        // over; those go into `posts` and are drawn from their live
        // transform (main). The fresh physics world discards all of them when
        // Play stops, like the road mesh.
        for (const RoadSystem::SideBatch& batch : road.sideBatches()) {
            LoadedModel* lm = resolveModel(batch.model);
            if (!lm) continue;
            for (const roadside::Instance& in : batch.instances) {
                const glm::vec3 half =
                    glm::max(lm->size() * 0.5f * in.scale, glm::vec3(0.02f));
                glm::vec3 c =
                    in.pos + glm::vec3(0.0f, half.y, 0.0f); // base on the ground
                const glm::quat q = glm::angleAxis(in.yaw, glm::vec3(0, 1, 0));
                if (batch.knockable) {
                    // Start a hair clear of the ground so the body settles
                    // onto it instead of being ejected out of a penetration
                    // (the physics heightfield is coarser than the terrain).
                    c.y += 0.03f;
                    const PhysicsBodyId id = physics.addBox(
                        half, c, q, glm::max(batch.mass, 0.1f));
                    if (id) posts.push_back({id, lm->id, in.scale});
                } else {
                    physics.addBox(half, c, q, 0.0f); // static
                }
            }
        }
        // The city's facades: a static box per piece the generator flagged as
        // solid (its masses and podium, not the bands, fins or signs), so a
        // vehicle crashes into a building instead of driving through it. That
        // is a handful per tower rather than one per part -- BuildingGen only
        // marks the load-bearing shapes -- which is what keeps a whole
        // district's collision affordable. Discarded with the physics world
        // when Play stops, like the road mesh.
        if (road.cityEnabled)
            for (const city::Piece& pc : road.district().colliders) {
                physics.addBox(glm::max(pc.half, glm::vec3(0.05f)), pc.center,
                               glm::angleAxis(glm::radians(pc.yaw),
                                              glm::vec3(0, 1, 0)),
                               0.0f);
            }
    }
    // The towns' buildings: one static box per solid part, like the
    // roadside city's.
    towns.forEachCollider([&](const city::Piece& pc) {
        physics.addBox(glm::max(pc.half, glm::vec3(0.05f)), pc.center,
                       glm::angleAxis(glm::radians(pc.yaw), glm::vec3(0, 1, 0)),
                       0.0f);
    });
    // Fences, walls, track and bridges: one static box per short run of
    // path (see splinegen::Collider). Coarse on purpose -- a car needs the
    // wall to be there, not to be able to thread the gap between two
    // rails -- and discarded with the physics world when Play stops.
    for (const SplineSystem::Run& run : splines.runs())
        for (const splinegen::Collider& col : run.geo.colliders)
            physics.addBox(glm::max(col.half, glm::vec3(0.02f)), col.center,
                           col.rotation(), 0.0f);
}

void addEntityBodies(fitzel::PhysicsWorld& physics, const std::vector<Entity>& entities,
                     ModelLibrary& models, std::map<int, fitzel::PhysicsBodyId>& bodies) {
    bodies.clear();
    for (const Entity& e : entities) {
        const auto* pc = e.components.get<PhysicsComponent>();
        if (!pc || !e.activeInHierarchy ||
            e.type == EntityType::Light || e.type == EntityType::Sun)
            continue;
        // Opponents are kinematic (driven along the road each frame), so
        // they must never get a dynamic body -- one would be flung by the
        // solver (e.g. spawning inside the terrain) and fight the tick.
        if (e.components.get<OpponentComponent>()) continue;
        // ...and so are the ones the town traffic drives (they get a driven
        // box of their own, see TownTraffic::beginPlay).
        if (traffic::TownTraffic::drives(e)) continue;
        // A soft body IS this entity's physics; a rigid collider beside it
        // would be a second, differently shaped copy fighting the first.
        if (e.components.get<SoftBodyComponent>()) continue;
        const float m = pc->dynamic ? glm::max(pc->mass, 0.01f) : 0.0f;
        const auto* mdl = e.components.get<ModelComponent>();
        const PhysicsBodyId id = addEntityBody(
            physics, e, m, mdl ? models.byId(mdl->modelId) : nullptr);
        if (id) bodies[e.id] = id;
    }
}

} // namespace playworld
