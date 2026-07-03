#pragma once

#include <cstdint>
#include <memory>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace fitzel {

// Opaque rigid-body handle (0 = invalid).
using PhysicsBodyId = std::uint32_t;

// A rigid-body dynamics world backed by Jolt Physics. Jolt is kept entirely out
// of this header (PIMPL) so its config macros never leak into engine users.
// Create one per play session, add bodies, step it each frame, and read back the
// transforms of the dynamic bodies to drive the scene.
class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&)            = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    void setGravity(glm::vec3 g);

    // Body creation. `mass <= 0` makes the body static (immovable); otherwise it
    // is dynamic with that mass. Returns 0 on failure.
    PhysicsBodyId addBox(glm::vec3 halfExtents, glm::vec3 pos, glm::quat rot, float mass);
    PhysicsBodyId addSphere(float radius, glm::vec3 pos, float mass);
    PhysicsBodyId addCylinder(float radius, float halfHeight, glm::vec3 pos,
                              glm::quat rot, float mass);

    // Convex-hull collider from a point cloud (>= 4 points) given in the body's
    // local space. Used for ramps (a wedge) and imported models.
    PhysicsBodyId addConvexHull(const glm::vec3* points, int count, glm::vec3 pos,
                                glm::quat rot, float mass);

    // Static triangle-mesh collider for concave world geometry (e.g. roads).
    // `verts` are world-space positions; `indices` is a triangle list (a multiple
    // of 3). Always static. Returns 0 on failure.
    PhysicsBodyId addMesh(const glm::vec3* verts, int vertCount,
                          const std::uint32_t* indices, int indexCount);

    // Static terrain collider from a square, row-major grid of `size`x`size`
    // world-space heights. The grid's (0,0) sample sits at `origin`, and adjacent
    // samples are `scaleXZ` metres apart along X and Z. `size` must be a positive
    // multiple of 2.
    PhysicsBodyId addHeightField(const float* heights, int size, glm::vec3 origin,
                                 float scaleXZ);

    // Advance the simulation by `dt` seconds.
    void step(float dt);

    // --- Character controller (a capsule the player drives) -----------------
    // Spawn a capsule character at `footPos` (bottom of the capsule), replacing
    // any existing one. `radius`/`halfHeight` are the capsule dimensions (the
    // cylinder part is 2*halfHeight tall). It collides with the world but is
    // driven kinematically (see moveCharacter).
    void spawnCharacter(float radius, float halfHeight, glm::vec3 footPos);
    void removeCharacter();
    bool hasCharacter() const;

    // Move the character this frame. `horizVel` is the desired horizontal world
    // velocity (m/s); `jump` requests a jump when grounded. The world applies
    // gravity + vertical motion and collide-and-slide, then returns the new foot
    // position and sets `outOnGround`.
    glm::vec3 moveCharacter(glm::vec3 horizVel, bool jump, float dt,
                            bool& outOnGround);

    // World transform of a body. False if the id is unknown.
    bool getTransform(PhysicsBodyId id, glm::vec3& pos, glm::quat& rot) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fitzel
