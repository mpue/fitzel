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

    // A movable (kinematic) box: it never falls, but setKinematicTarget() drives
    // it each frame and it carries the character and dynamic bodies resting on it
    // -- moving platforms, lifts. Lives in the MOVING layer so both collide.
    PhysicsBodyId addKinematicBox(glm::vec3 halfExtents, glm::vec3 pos, glm::quat rot);

    // Move a kinematic body toward a world transform over `dt`, giving it the
    // velocity needed to arrive so it pushes/carries whatever rests on it. No-op
    // on an unknown id or a non-positive dt.
    void setKinematicTarget(PhysicsBodyId id, glm::vec3 pos, glm::quat rot, float dt);

    // --- Wheeled vehicle (Jolt VehicleConstraint) ---------------------------
    // Handling knobs that keep the car planted (defaults match the tuned car).
    struct VehicleTuning {
        float comLower       = 1.0f;    // 0..1 of chassisHalf.y to drop the COM
        float suspensionFreq = 2.0f;    // suspension spring stiffness (Hz)
        float suspensionDamp = 0.85f;   // suspension spring damping (0..1)
        float antiRoll       = 1000.0f; // anti-roll bar stiffness (0 = none)
        int   drive          = 0;       // 0 = RWD, 1 = FWD, 2 = AWD
        float grip           = 1.5f;    // tyre friction scale (1 = Jolt default)
        // Roll-stabilising "keep upright" assist: an arcade righting torque about
        // the car's forward axis that fights body roll (0 = pure simulation, the
        // car can tip freely; higher = harder to roll over). `uprightDamp` bleeds
        // off roll spin so the assist settles instead of oscillating.
        float uprightAssist  = 6.0f;    // righting torque strength (0 = off)
        float uprightDamp    = 2.5f;    // roll-rate damping for the assist
    };
    // Spawn a physics car: a dynamic box chassis with four wheels (suspension,
    // engine, steering). One vehicle per world. Forward is the body's +Z; front
    // wheels (0,1) steer. Which axle drives depends on `tuning.drive`. Returns
    // the chassis body id (read it with getTransform); wheels via getWheelTransform.
    PhysicsBodyId addVehicle(glm::vec3 chassisHalf, float mass, glm::vec3 pos,
                             glm::quat rot, float wheelRadius, float wheelWidth,
                             float halfTrack, float frontZ, float rearZ,
                             float maxSteerDeg, float engineTorque,
                             const VehicleTuning& tuning = {});
    // Driver input for this frame: `forward` accelerates (-1 reverse .. 1),
    // `right` steers (-1 left .. 1 right), `brake`/`handBrake` are 0..1.
    void setVehicleInput(float forward, float right, float brake, float handBrake);
    // World transform of wheel `i` (0 FL, 1 FR, 2 RL, 3 RR). False if no vehicle.
    bool getWheelTransform(int wheel, glm::vec3& pos, glm::quat& rot) const;
    // Ground-contact + slip state of wheel `i`, valid only right after step().
    // `onGround` is false when the wheel is airborne (pos/normal/lateral then
    // undefined). `longSlip` is the longitudinal slip ratio (0 = rolling, ~1 =
    // locked/spinning); `latSlip` is the lateral slip angle in radians. Used to
    // lay tyre skid marks. Returns false if there is no vehicle / bad index.
    bool getWheelContact(int wheel, glm::vec3& pos, glm::vec3& normal,
                         glm::vec3& lateral, float& longSlip, float& latSlip,
                         bool& onGround) const;
    bool hasVehicle() const;

    // Convex-hull collider from a point cloud (>= 4 points) given in the body's
    // local space. Used for ramps (a wedge) and imported models.
    PhysicsBodyId addConvexHull(const glm::vec3* points, int count, glm::vec3 pos,
                                glm::quat rot, float mass);

    // --- Soft bodies (Jolt SoftBody: jelly, cloth, an inflated shell) -------
    // A soft body is not a transform the way a rigid body is: it is a cloud of
    // particles held together by constraints, and what comes back out of it is
    // POSITIONS, not a position and a rotation. So the two readbacks below are
    // the whole interface -- the caller re-skins its mesh from them every frame.
    struct SoftBodyDesc {
        float mass            = 20.0f;  // total, spread evenly over the particles
        float compliance      = 0.0f;   // inverse edge stiffness (0 = as stiff as
                                        // the solver gets; ~1e-3 = properly soft)
        float shearCompliance = 0.0f;   // same, for the diagonals across a quad
        float bendCompliance  = -1.0f;  // < 0 = create no bend constraints at all
        float pressure        = 0.0f;   // inflates a CLOSED shell (n*R*T); a shell
                                        // with no pressure and no volume constraints
                                        // collapses into a bag, which is correct
        float damping         = 0.1f;   // linear damping per particle
        float friction        = 0.5f;
        float restitution     = 0.0f;
        float gravityFactor   = 1.0f;
        int   iterations      = 5;      // solver iterations (more = stiffer/steadier)
    };

    // A closed (or open, for cloth) triangle surface as a soft body. `verts` are
    // in the body's local space, `indices` a triangle list; `pinned`, if given,
    // is one flag per vertex -- a pinned particle has infinite mass and hangs the
    // rest of the body off it. Edge/shear/bend constraints are derived from the
    // triangles. Returns 0 on failure.
    PhysicsBodyId addSoftMesh(const glm::vec3* verts, int vertCount,
                              const std::uint32_t* indices, int indexCount,
                              const std::uint8_t* pinned, glm::vec3 pos,
                              glm::quat rot, const SoftBodyDesc& desc);

    // A jelly cube: a `grid`x`grid`x`grid` lattice of particles filling the box,
    // with tetrahedral VOLUME constraints, which is what makes it wobble back
    // into shape instead of deflating. Only the outer shell gets faces (that is
    // what getSoftFaces returns, and what there is to draw). Returns 0 on failure.
    PhysicsBodyId addSoftBox(glm::vec3 halfExtents, int grid, glm::vec3 pos,
                             glm::quat rot, const SoftBodyDesc& desc);

    // How many particles / how many surface triangles this body has. 0 for an
    // unknown id or a rigid body.
    int  softVertexCount(PhysicsBodyId id) const;
    int  softFaceCount(PhysicsBodyId id) const;
    // The surface triangle list (3 * softFaceCount indices). Fixed for the life
    // of the body, so read it once when the body is made.
    bool getSoftFaces(PhysicsBodyId id, std::uint32_t* out, int count) const;
    // This frame's particle positions, as offsets from `outCenter` (the body's
    // centre of mass, in world space) with the body's rotation already applied.
    // World position of particle i is therefore outCenter + out[i], which is what
    // lets a caller draw the body as a mesh sitting at outCenter with no rotation
    // of its own. Writes min(count, softVertexCount) entries.
    bool getSoftVertices(PhysicsBodyId id, glm::vec3* out, int count,
                         glm::vec3& outCenter) const;

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
    // World-space linear velocity (m/s) of a body. False if the id is unknown.
    bool getLinearVelocity(PhysicsBodyId id, glm::vec3& out) const;
    // World-space angular velocity (rad/s, axis*speed) of a body. False if unknown.
    bool getAngularVelocity(PhysicsBodyId id, glm::vec3& out) const;

    // Runtime edits to a dynamic body (used by Lua scripts during Play). All are
    // no-ops if the id is unknown or the body is static/removed.
    void setLinearVelocity(PhysicsBodyId id, glm::vec3 v);
    void setAngularVelocity(PhysicsBodyId id, glm::vec3 v);
    void applyImpulse(PhysicsBodyId id, glm::vec3 impulse);

    // Remove and destroy a body (e.g. an entity deleted mid-play). Safe on an
    // unknown/already-removed id.
    void removeBody(PhysicsBodyId id);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fitzel
