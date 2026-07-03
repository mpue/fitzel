#include "fitzel/physics/Physics.hpp"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <thread>

// Jolt: keep its warning macros local to this TU.
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>

JPH_SUPPRESS_WARNINGS

namespace fitzel {

namespace {

// --- Collision layers (two is plenty: static world vs moving bodies) --------
namespace Layers {
static constexpr JPH::ObjectLayer NON_MOVING = 0;
static constexpr JPH::ObjectLayer MOVING     = 1;
static constexpr JPH::ObjectLayer NUM        = 2;
}
namespace BroadPhaseLayers {
static constexpr JPH::BroadPhaseLayer NON_MOVING(0);
static constexpr JPH::BroadPhaseLayer MOVING(1);
static constexpr JPH::uint NUM = 2;
}

class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::NUM; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer l) const override {
        return l == Layers::NON_MOVING ? BroadPhaseLayers::NON_MOVING
                                       : BroadPhaseLayers::MOVING;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer) const override {
        return "layer";
    }
#endif
};

class ObjectVsBroadPhaseLayerFilterImpl final
    : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer o, JPH::BroadPhaseLayer b) const override {
        // Moving collides with everything; static only with moving.
        if (o == Layers::NON_MOVING) return b == BroadPhaseLayers::MOVING;
        return true;
    }
};

class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        if (a == Layers::NON_MOVING) return b == Layers::MOVING;
        return true; // MOVING vs anything
    }
};

// Jolt's global one-time setup (allocator, factory, type registration). Done
// once per process; never torn down, so multiple PhysicsWorlds are fine.
void ensureJoltRegistered() {
    static std::once_flag once;
    std::call_once(once, [] {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    });
}

inline JPH::Vec3 toJolt(const glm::vec3& v) { return JPH::Vec3(v.x, v.y, v.z); }
inline JPH::Quat toJolt(const glm::quat& q) { return JPH::Quat(q.x, q.y, q.z, q.w); }
inline glm::vec3 toGlm(JPH::Vec3 v) { return glm::vec3(v.GetX(), v.GetY(), v.GetZ()); }
inline glm::quat toGlm(JPH::Quat q) {
    return glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ()); // glm order: w,x,y,z
}

} // namespace

struct PhysicsWorld::Impl {
    JPH::TempAllocatorImpl        temp{16 * 1024 * 1024};
    JPH::JobSystemThreadPool      jobs{/*maxJobs=*/2048, /*maxBarriers=*/8,
        static_cast<int>(std::max(1u, std::thread::hardware_concurrency()) - 1)};
    BPLayerInterfaceImpl          bpLayers;
    ObjectVsBroadPhaseLayerFilterImpl objVsBp;
    ObjectLayerPairFilterImpl     objVsObj;
    JPH::PhysicsSystem            system;

    JPH::Ref<JPH::CharacterVirtual> character;
    float charRadius = 0.3f, charHalfHeight = 0.6f, charVertVel = 0.0f;

    Impl() {
        system.Init(/*maxBodies=*/4096, /*numBodyMutexes=*/0,
                    /*maxBodyPairs=*/8192, /*maxContactConstraints=*/4096,
                    bpLayers, objVsBp, objVsObj);
    }

    JPH::BodyID create(const JPH::ShapeRefC& shape, glm::vec3 pos, glm::quat rot,
                       float mass) {
        const bool dynamic = mass > 0.0f;
        JPH::BodyCreationSettings s(
            shape, JPH::RVec3(pos.x, pos.y, pos.z), toJolt(rot),
            dynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static,
            dynamic ? Layers::MOVING : Layers::NON_MOVING);
        if (dynamic) {
            s.mOverrideMassProperties =
                JPH::EOverrideMassProperties::CalculateInertia;
            s.mMassPropertiesOverride.mMass = mass;
        }
        JPH::BodyInterface& bi = system.GetBodyInterface();
        JPH::BodyID id = bi.CreateAndAddBody(
            s, dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
        return id;
    }
};

PhysicsWorld::PhysicsWorld() {
    ensureJoltRegistered();
    m_impl = std::make_unique<Impl>();
}

PhysicsWorld::~PhysicsWorld() = default;

void PhysicsWorld::setGravity(glm::vec3 g) {
    m_impl->system.SetGravity(toJolt(g));
}

PhysicsBodyId PhysicsWorld::addBox(glm::vec3 half, glm::vec3 pos, glm::quat rot,
                                   float mass) {
    JPH::ShapeRefC shape = new JPH::BoxShape(toJolt(glm::max(half, glm::vec3(0.02f))));
    return m_impl->create(shape, pos, rot, mass).GetIndexAndSequenceNumber();
}

PhysicsBodyId PhysicsWorld::addSphere(float radius, glm::vec3 pos, float mass) {
    JPH::ShapeRefC shape = new JPH::SphereShape(glm::max(radius, 0.02f));
    return m_impl->create(shape, pos, glm::quat(1, 0, 0, 0), mass)
        .GetIndexAndSequenceNumber();
}

PhysicsBodyId PhysicsWorld::addCylinder(float radius, float halfHeight,
                                        glm::vec3 pos, glm::quat rot, float mass) {
    JPH::ShapeRefC shape = new JPH::CylinderShape(
        glm::max(halfHeight, 0.02f), glm::max(radius, 0.02f));
    return m_impl->create(shape, pos, rot, mass).GetIndexAndSequenceNumber();
}

PhysicsBodyId PhysicsWorld::addHeightField(const float* heights, int size,
                                           glm::vec3 origin, float scaleXZ) {
    if (!heights || size < 2) return 0;
    // Samples lie on a unit grid; scale X/Z to metres via the shape's scale.
    JPH::HeightFieldShapeSettings hs(
        heights, JPH::Vec3(0, 0, 0), JPH::Vec3(scaleXZ, 1.0f, scaleXZ),
        static_cast<JPH::uint32>(size));
    JPH::Shape::ShapeResult res = hs.Create();
    if (res.HasError()) {
        std::fprintf(stderr, "[Fitzel] heightfield shape error: %s\n",
                     res.GetError().c_str());
        return 0;
    }
    return m_impl->create(res.Get(), origin, glm::quat(1, 0, 0, 0), 0.0f)
        .GetIndexAndSequenceNumber();
}

void PhysicsWorld::step(float dt) {
    if (dt <= 0.0f) return;
    // Clamp long frames so a hitch can't explode the simulation.
    const float clamped = dt > 0.1f ? 0.1f : dt;
    m_impl->system.Update(clamped, /*collisionSteps=*/1, &m_impl->temp,
                          &m_impl->jobs);
}

bool PhysicsWorld::getTransform(PhysicsBodyId id, glm::vec3& pos,
                                glm::quat& rot) const {
    JPH::BodyID bid(id);
    JPH::BodyInterface& bi = m_impl->system.GetBodyInterface();
    if (!bi.IsAdded(bid)) return false;
    JPH::RVec3 p = bi.GetPosition(bid);
    pos = glm::vec3(float(p.GetX()), float(p.GetY()), float(p.GetZ()));
    rot = toGlm(bi.GetRotation(bid));
    return true;
}

// --- Character controller ---------------------------------------------------

void PhysicsWorld::spawnCharacter(float radius, float halfHeight,
                                  glm::vec3 footPos) {
    Impl& d = *m_impl;
    d.charRadius     = std::max(radius, 0.05f);
    d.charHalfHeight = std::max(halfHeight, 0.05f);
    d.charVertVel    = 0.0f;
    // CharacterVirtual position is the capsule centre; lift the foot by half the
    // full height (cylinder half + one radius cap).
    const float lift = d.charHalfHeight + d.charRadius;
    JPH::CharacterVirtualSettings s;
    s.mShape = new JPH::CapsuleShape(d.charHalfHeight, d.charRadius);
    s.mMaxSlopeAngle = JPH::DegreesToRadians(46.0f);
    // Only the cylinder body (not the bottom cap) counts as "supported ground".
    s.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -d.charRadius);
    d.character = new JPH::CharacterVirtual(
        &s, JPH::RVec3(footPos.x, footPos.y + lift, footPos.z),
        JPH::Quat::sIdentity(), 0, &d.system);
}

void PhysicsWorld::removeCharacter() { m_impl->character = nullptr; }

bool PhysicsWorld::hasCharacter() const { return m_impl->character != nullptr; }

glm::vec3 PhysicsWorld::moveCharacter(glm::vec3 horizVel, bool jump, float dt,
                                      bool& outOnGround) {
    Impl& d = *m_impl;
    outOnGround = false;
    if (!d.character || dt <= 0.0f) return glm::vec3(0.0f);
    if (dt > 0.1f) dt = 0.1f;

    const bool grounded = d.character->GetGroundState() ==
                          JPH::CharacterBase::EGroundState::OnGround;
    const JPH::Vec3 g = d.system.GetGravity();
    if (grounded && d.charVertVel < 0.0f)
        d.charVertVel = 0.0f;            // rest on the ground
    if (jump && grounded)
        d.charVertVel = 6.0f;            // jump impulse
    d.charVertVel += g.GetY() * dt;      // gravity

    d.character->SetLinearVelocity(JPH::Vec3(horizVel.x, d.charVertVel, horizVel.z));

    JPH::CharacterVirtual::ExtendedUpdateSettings us;
    d.character->ExtendedUpdate(
        dt, g, us,
        d.system.GetDefaultBroadPhaseLayerFilter(Layers::MOVING),
        d.system.GetDefaultLayerFilter(Layers::MOVING),
        JPH::BodyFilter{}, JPH::ShapeFilter{}, d.temp);

    outOnGround = d.character->GetGroundState() ==
                  JPH::CharacterBase::EGroundState::OnGround;
    const JPH::RVec3 c = d.character->GetPosition();
    const float lift = d.charHalfHeight + d.charRadius;
    return glm::vec3(float(c.GetX()), float(c.GetY()) - lift, float(c.GetZ()));
}

} // namespace fitzel
