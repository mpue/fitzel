#pragma once

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/physics/Physics.hpp>

#include "SceneTypes.hpp" // Entity

// Soft bodies at Play time: the entities that wobble.
//
// A rigid body hands back a transform, and the entity it belongs to simply moves
// there. A soft body hands back a few hundred PARTICLE POSITIONS, and there is
// no transform in them at all -- the shape itself is the result. So this system
// exists to close that gap in both directions: it builds the particles from what
// the author placed (a box becomes a lattice of jelly, a sphere an inflated
// shell, a modelled mesh its own skin), and every frame it writes the simulated
// surface back into the entity as a Mesh component.
//
// Writing into a Mesh is what makes the whole thing cheap: the modelled-mesh path
// already draws arbitrary triangles with the entity's material, its paint, its
// shadows. A deformable object is that path with new vertices each frame, not a
// renderer of its own. It costs a mesh rebuild per body per frame, which is what
// the resolution knob is really spending -- a handful of soft bodies, not a
// forest of them.
//
// Everything here lives and dies with the physics world: Play makes both, Stop
// throws both away and restores the scene from its backup, which is what undoes
// the Mesh component this put on entities that had none.
class SoftBodySystem {
public:
    // Write an entity's world transform back through the scene graph. The same
    // conversion the rigid bodies go through -- injected rather than duplicated,
    // because local-relative-to-parent is the source of truth and only the
    // editor's own composition may derive it (see scenegraph::compose).
    using SetWorld = std::function<void(Entity&, const glm::vec3& pos,
                                        const glm::vec3& rotationDeg)>;

    // Build a soft body for every entity carrying a SoftBodyComponent. Call once
    // when Play starts, after the world has its static geometry: a body that
    // spawns inside the ground is pushed out of it, which looks like an explosion.
    void spawn(std::vector<Entity>& entities, fitzel::PhysicsWorld& world);

    // Read this frame's particles back into the entities. Call right after
    // PhysicsWorld::step, before the scene is submitted.
    void sync(std::vector<Entity>& entities, fitzel::PhysicsWorld& world,
              const SetWorld& setWorld);

    // Drop the body an entity that is being destroyed mid-run was wearing. Not
    // optional bookkeeping: the particles are the collider too, so a forgotten
    // one goes on being bumped into by a player who can no longer see it.
    void remove(int entityId, fitzel::PhysicsWorld& world);

    void clear() { m_bodies.clear(); }
    bool empty() const { return m_bodies.empty(); }
    // Does this entity simulate as a soft body? (Play asks before giving it the
    // rigid collider it would otherwise get -- it may not have both.)
    bool has(int entityId) const { return m_bodies.count(entityId) != 0; }

private:
    struct Body {
        fitzel::PhysicsBodyId      id = 0;
        std::vector<std::uint32_t> tris;    // surface triangles, read once
        std::vector<glm::vec3>     verts;   // scratch for the per-frame readback
    };
    std::unordered_map<int, Body> m_bodies; // entity id -> its soft body
};
