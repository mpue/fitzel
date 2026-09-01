#include "fitzel/physics/Physics.hpp"

#include <algorithm>
#include <cfloat>
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
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>
#include <Jolt/Physics/SoftBody/SoftBodySharedSettings.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>

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

    JPH::Ref<JPH::VehicleConstraint>      vehicle;      // the (single) car
    JPH::Ref<JPH::VehicleCollisionTester> vehicleTest;
    JPH::BodyID                           vehicleBody;
    float vehicleUpright     = 0.0f;  // keep-upright torque strength (0 = off)
    float vehicleUprightDamp = 0.0f;  // roll-rate damping for the assist
    float vehicleMass        = 0.0f;  // chassis mass (scales the assist torque)

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

PhysicsBodyId PhysicsWorld::addKinematicBox(glm::vec3 half, glm::vec3 pos,
                                            glm::quat rot) {
    JPH::ShapeRefC shape = new JPH::BoxShape(toJolt(glm::max(half, glm::vec3(0.02f))));
    JPH::BodyCreationSettings s(
        shape, JPH::RVec3(pos.x, pos.y, pos.z), toJolt(rot),
        JPH::EMotionType::Kinematic, Layers::MOVING);
    JPH::BodyInterface& bi = m_impl->system.GetBodyInterface();
    JPH::BodyID id = bi.CreateAndAddBody(s, JPH::EActivation::Activate);
    return id.GetIndexAndSequenceNumber();
}

void PhysicsWorld::setKinematicTarget(PhysicsBodyId id, glm::vec3 pos,
                                      glm::quat rot, float dt) {
    if (dt <= 0.0f) return;
    JPH::BodyID bid(id);
    JPH::BodyInterface& bi = m_impl->system.GetBodyInterface();
    if (!bi.IsAdded(bid)) return;
    bi.MoveKinematic(bid, JPH::RVec3(pos.x, pos.y, pos.z), toJolt(rot), dt);
}

PhysicsBodyId PhysicsWorld::addVehicle(glm::vec3 chassisHalf, float mass,
                                       glm::vec3 pos, glm::quat rot,
                                       float wheelRadius, float wheelWidth,
                                       float halfTrack, float frontZ, float rearZ,
                                       float maxSteerDeg, float engineTorque,
                                       const VehicleTuning& tuning) {
    Impl& d = *m_impl;
    JPH::BodyInterface& bi = d.system.GetBodyInterface();

    const glm::vec3 ch = glm::max(chassisHalf, glm::vec3(0.05f));
    // Drop the centre of mass toward the wheel line. A box's COM sits at its
    // geometric centre, which is high for a car -- the main reason it flips when
    // cornering hard. OffsetCenterOfMassShape keeps the geometry (and the wheel
    // attachment points below) in place while moving only the COM downward.
    const float comDrop = ch.y * glm::clamp(tuning.comLower, 0.0f, 1.0f);
    JPH::RefConst<JPH::Shape> box =
        new JPH::BoxShape(toJolt(ch));
    JPH::RefConst<JPH::Shape> shape =
        JPH::OffsetCenterOfMassShapeSettings(JPH::Vec3(0.0f, -comDrop, 0.0f), box)
            .Create().Get();
    JPH::BodyCreationSettings bcs(shape, toJolt(pos), toJolt(rot),
                                  JPH::EMotionType::Dynamic, Layers::MOVING);
    bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    bcs.mMassPropertiesOverride.mMass = mass;
    // Swept (continuous) collision: a fast car over a coarse terrain heightfield
    // would otherwise tunnel straight through between steps and fall out of the
    // world. LinearCast makes the chassis cast its motion against static geometry.
    bcs.mMotionQuality = JPH::EMotionQuality::LinearCast;
    JPH::Body* body = bi.CreateBody(bcs);
    if (!body) return 0;
    bi.AddBody(body->GetID(), JPH::EActivation::Activate);
    d.vehicleBody = body->GetID();
    d.vehicleUpright     = glm::max(tuning.uprightAssist, 0.0f);
    d.vehicleUprightDamp = glm::max(tuning.uprightDamp, 0.0f);
    d.vehicleMass        = mass;

    JPH::VehicleConstraintSettings vc;
    vc.mMaxPitchRollAngle = JPH::DegreesToRadians(60.0f);
    const float wy = -ch.y; // wheel attachment near the chassis bottom
    const struct { float x, z; bool front; } wp[4] = {
        {-halfTrack, frontZ, true}, {halfTrack, frontZ, true},
        {-halfTrack, rearZ,  false}, {halfTrack, rearZ,  false}};
    for (int i = 0; i < 4; ++i) {
        JPH::WheelSettingsWV* w = new JPH::WheelSettingsWV;
        w->mPosition            = JPH::Vec3(wp[i].x, wy, wp[i].z);
        w->mRadius              = wheelRadius;
        w->mWidth               = wheelWidth;
        // Travel around the rest length the caller asked for. The 0.75/1.5 span
        // reproduces the old fixed 0.30/0.60 at the default rest of 0.4 exactly,
        // so a car tuned before this was adjustable rides where it always did.
        const float rest = glm::max(tuning.suspensionRest, 0.05f);
        w->mSuspensionMinLength = rest * 0.75f;
        w->mSuspensionMaxLength = rest * 1.5f;
        // A stiffer, well-damped suspension resists body roll and settles the
        // weight transfer instead of letting it pitch the car over.
        w->mSuspensionSpring.mFrequency = glm::max(tuning.suspensionFreq, 0.1f);
        w->mSuspensionSpring.mDamping   = glm::clamp(tuning.suspensionDamp, 0.0f, 1.0f);
        // Tyre grip: rebuild Jolt's default friction curves scaled by `grip`
        // (peak / plateau of the longitudinal + lateral slip curves). Higher =
        // more traction under power and in corners.
        const float g = glm::max(tuning.grip, 0.05f);
        w->mLongitudinalFriction.Clear();
        w->mLongitudinalFriction.Reserve(3);
        w->mLongitudinalFriction.AddPoint(0.0f, 0.0f);
        w->mLongitudinalFriction.AddPoint(0.06f, 1.6f * g);
        w->mLongitudinalFriction.AddPoint(0.2f,  1.4f * g);
        w->mLateralFriction.Clear();
        w->mLateralFriction.Reserve(3);
        w->mLateralFriction.AddPoint(0.0f,  0.0f);
        w->mLateralFriction.AddPoint(3.0f,  1.4f * g);
        w->mLateralFriction.AddPoint(20.0f, 1.2f * g);
        w->mMaxSteerAngle       = wp[i].front ? JPH::DegreesToRadians(maxSteerDeg) : 0.0f;
        w->mMaxHandBrakeTorque  = wp[i].front ? 0.0f : 4000.0f; // handbrake locks rear
        vc.mWheels.push_back(w);
    }
    // Anti-roll bars couple each axle's left/right suspension so cornering load
    // is shared across the axle rather than compressing one side and tipping.
    if (tuning.antiRoll > 0.0f) {
        vc.mAntiRollBars.resize(2);
        vc.mAntiRollBars[0].mLeftWheel = 0; vc.mAntiRollBars[0].mRightWheel = 1; // front
        vc.mAntiRollBars[1].mLeftWheel = 2; vc.mAntiRollBars[1].mRightWheel = 3; // rear
        vc.mAntiRollBars[0].mStiffness = tuning.antiRoll;
        vc.mAntiRollBars[1].mStiffness = tuning.antiRoll;
    }
    JPH::WheeledVehicleControllerSettings* ctrl =
        new JPH::WheeledVehicleControllerSettings;
    ctrl->mEngine.mMaxTorque = engineTorque;
    ctrl->mEngine.mMaxRPM    = 6000.0f;
    // Driven axle(s): RWD (rear 2,3), FWD (front 0,1), or AWD (both, torque
    // split 50/50). Front-drive pulls the nose through a corner and is far
    // harder to spin out under power than rear-drive.
    if (tuning.drive == 1) {                 // FWD
        ctrl->mDifferentials.resize(1);
        ctrl->mDifferentials[0].mLeftWheel  = 0;
        ctrl->mDifferentials[0].mRightWheel = 1;
    } else if (tuning.drive == 2) {          // AWD
        ctrl->mDifferentials.resize(2);
        ctrl->mDifferentials[0].mLeftWheel  = 0;
        ctrl->mDifferentials[0].mRightWheel = 1;
        ctrl->mDifferentials[1].mLeftWheel  = 2;
        ctrl->mDifferentials[1].mRightWheel = 3;
        ctrl->mDifferentials[0].mEngineTorqueRatio = 0.5f;
        ctrl->mDifferentials[1].mEngineTorqueRatio = 0.5f;
    } else {                                 // RWD (default)
        ctrl->mDifferentials.resize(1);
        ctrl->mDifferentials[0].mLeftWheel  = 2;
        ctrl->mDifferentials[0].mRightWheel = 3;
    }
    vc.mController = ctrl;

    d.vehicle     = new JPH::VehicleConstraint(*body, vc);
    d.vehicleTest = new JPH::VehicleCollisionTesterRay(Layers::MOVING);
    JPH::VehicleConstraint* con = static_cast<JPH::VehicleConstraint*>(d.vehicle.GetPtr());
    con->SetVehicleCollisionTester(d.vehicleTest);
    d.system.AddConstraint(con);
    d.system.AddStepListener(con);
    return d.vehicleBody.GetIndexAndSequenceNumber();
}

void PhysicsWorld::setVehicleInput(float forward, float right, float brake,
                                   float handBrake) {
    if (!m_impl->vehicle) return;
    JPH::VehicleConstraint* con =
        static_cast<JPH::VehicleConstraint*>(m_impl->vehicle.GetPtr());
    static_cast<JPH::WheeledVehicleController*>(con->GetController())
        ->SetDriverInput(forward, right, brake, handBrake);
    m_impl->system.GetBodyInterface().ActivateBody(m_impl->vehicleBody);
}

bool PhysicsWorld::getWheelTransform(int wheel, glm::vec3& pos, glm::quat& rot) const {
    if (!m_impl->vehicle || wheel < 0 || wheel >= 4) return false;
    JPH::VehicleConstraint* con =
        static_cast<JPH::VehicleConstraint*>(m_impl->vehicle.GetPtr());
    const JPH::RMat44 wt = con->GetWheelWorldTransform(
        static_cast<JPH::uint>(wheel), JPH::Vec3::sAxisX(), JPH::Vec3::sAxisY());
    pos = toGlm(JPH::Vec3(wt.GetTranslation()));
    rot = toGlm(wt.GetQuaternion());
    return true;
}

bool PhysicsWorld::getWheelContact(int wheel, glm::vec3& pos, glm::vec3& normal,
                                   glm::vec3& lateral, float& longSlip,
                                   float& latSlip, bool& onGround) const {
    if (!m_impl->vehicle || wheel < 0 || wheel >= 4) return false;
    JPH::VehicleConstraint* con =
        static_cast<JPH::VehicleConstraint*>(m_impl->vehicle.GetPtr());
    const auto* w = static_cast<const JPH::WheelWV*>(
        con->GetWheel(static_cast<JPH::uint>(wheel)));
    onGround = w->HasContact();
    longSlip = w->mLongitudinalSlip;
    latSlip  = w->mLateralSlip;
    if (onGround) {
        pos     = toGlm(JPH::Vec3(w->GetContactPosition()));
        normal  = toGlm(w->GetContactNormal());
        lateral = toGlm(w->GetContactLateral());
    }
    return true;
}

bool PhysicsWorld::hasVehicle() const { return m_impl->vehicle != nullptr; }

PhysicsBodyId PhysicsWorld::addConvexHull(const glm::vec3* points, int count,
                                          glm::vec3 pos, glm::quat rot,
                                          float mass) {
    if (!points || count < 4) return 0;
    JPH::Array<JPH::Vec3> pv;
    pv.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) pv.push_back(toJolt(points[i]));
    JPH::ConvexHullShapeSettings hs(pv);
    JPH::Shape::ShapeResult res = hs.Create();
    if (res.HasError()) {
        std::fprintf(stderr, "[Fitzel] convex hull shape error: %s\n",
                     res.GetError().c_str());
        return 0;
    }
    return m_impl->create(res.Get(), pos, rot, mass).GetIndexAndSequenceNumber();
}

PhysicsBodyId PhysicsWorld::addMesh(const glm::vec3* verts, int vertCount,
                                    const std::uint32_t* indices, int indexCount) {
    if (!verts || vertCount < 3 || !indices || indexCount < 3) return 0;
    JPH::VertexList vl;
    vl.reserve(static_cast<std::size_t>(vertCount));
    for (int i = 0; i < vertCount; ++i)
        vl.push_back(JPH::Float3(verts[i].x, verts[i].y, verts[i].z));
    JPH::IndexedTriangleList tl;
    tl.reserve(static_cast<std::size_t>(indexCount / 3));
    for (int i = 0; i + 2 < indexCount; i += 3)
        tl.push_back(JPH::IndexedTriangle(indices[i], indices[i + 1],
                                          indices[i + 2], 0));
    JPH::MeshShapeSettings s(vl, tl);
    s.Sanitize(); // drop degenerate/duplicate triangles
    JPH::Shape::ShapeResult res = s.Create();
    if (res.HasError()) {
        std::fprintf(stderr, "[Fitzel] mesh shape error: %s\n",
                     res.GetError().c_str());
        return 0;
    }
    return m_impl->create(res.Get(), glm::vec3(0.0f), glm::quat(1, 0, 0, 0), 0.0f)
        .GetIndexAndSequenceNumber();
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
    // Roll-stabilising assist: before stepping, apply a righting torque about the
    // car's forward axis proportional to how far it has rolled (minus its roll
    // rate). Jolt's wheeled controller has no anti-rollover of its own; with the
    // COM already at the floor this arcade aid is what keeps hard corners from
    // flipping the car. It only touches roll -- pitch (hills) and yaw (steering)
    // are left to the simulation. Scaled by mass so light and heavy cars right
    // themselves at the same rate (roll inertia grows with mass).
    Impl& d = *m_impl;
    if (d.vehicle && d.vehicleUpright > 0.0f) {
        JPH::BodyInterface& bi = d.system.GetBodyInterface();
        if (bi.IsAdded(d.vehicleBody) &&
            bi.GetMotionType(d.vehicleBody) == JPH::EMotionType::Dynamic) {
            const JPH::Quat q   = bi.GetRotation(d.vehicleBody);
            const JPH::Vec3 up  = q * JPH::Vec3(0, 1, 0);
            const JPH::Vec3 fwd = q * JPH::Vec3(0, 0, 1);
            // cross(bodyUp, worldUp) ~ sin(tilt) along the tilt axis; its forward
            // component is the roll (a pitch tilt lands on the right axis instead).
            const float rollErr  = up.Cross(JPH::Vec3(0, 1, 0)).Dot(fwd);
            const float rollRate = bi.GetAngularVelocity(d.vehicleBody).Dot(fwd);
            const float t = (d.vehicleUpright * rollErr - d.vehicleUprightDamp * rollRate)
                          * d.vehicleMass;
            bi.AddTorque(d.vehicleBody, fwd * t);
        }
    }
    // Two collision sub-steps: halves the distance a body moves per solve, which
    // keeps fast bodies (the vehicle) from punching through thin/coarse colliders.
    d.system.Update(clamped, /*collisionSteps=*/2, &d.temp, &d.jobs);
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

bool PhysicsWorld::getLinearVelocity(PhysicsBodyId id, glm::vec3& out) const {
    JPH::BodyID bid(id);
    JPH::BodyInterface& bi = m_impl->system.GetBodyInterface();
    if (!bi.IsAdded(bid)) return false;
    out = toGlm(bi.GetLinearVelocity(bid));
    return true;
}

bool PhysicsWorld::getAngularVelocity(PhysicsBodyId id, glm::vec3& out) const {
    JPH::BodyID bid(id);
    JPH::BodyInterface& bi = m_impl->system.GetBodyInterface();
    if (!bi.IsAdded(bid)) return false;
    out = toGlm(bi.GetAngularVelocity(bid));
    return true;
}

void PhysicsWorld::setLinearVelocity(PhysicsBodyId id, glm::vec3 v) {
    JPH::BodyID bid(id);
    JPH::BodyInterface& bi = m_impl->system.GetBodyInterface();
    if (!bi.IsAdded(bid)) return;
    bi.SetLinearVelocity(bid, toJolt(v));
    bi.ActivateBody(bid);
}

void PhysicsWorld::setAngularVelocity(PhysicsBodyId id, glm::vec3 v) {
    JPH::BodyID bid(id);
    JPH::BodyInterface& bi = m_impl->system.GetBodyInterface();
    if (!bi.IsAdded(bid)) return;
    bi.SetAngularVelocity(bid, toJolt(v));
    bi.ActivateBody(bid);
}

void PhysicsWorld::applyImpulse(PhysicsBodyId id, glm::vec3 impulse) {
    JPH::BodyID bid(id);
    JPH::BodyInterface& bi = m_impl->system.GetBodyInterface();
    if (!bi.IsAdded(bid)) return;
    bi.AddImpulse(bid, toJolt(impulse));
    bi.ActivateBody(bid);
}

void PhysicsWorld::removeBody(PhysicsBodyId id) {
    JPH::BodyID bid(id);
    JPH::BodyInterface& bi = m_impl->system.GetBodyInterface();
    if (bi.IsAdded(bid)) bi.RemoveBody(bid);
    bi.DestroyBody(bid);
}

// --- Soft bodies -------------------------------------------------------------

namespace {

// Everything a soft body needs once its particles and constraints exist: wrap
// the shared settings in a body, put it in the moving layer, switch it on.
// Shared because addSoftMesh and addSoftBox differ only in the geometry above.
JPH::BodyID createSoftBody(JPH::PhysicsSystem& system,
                           const JPH::Ref<JPH::SoftBodySharedSettings>& shared,
                           glm::vec3 pos, glm::quat rot,
                           const PhysicsWorld::SoftBodyDesc& d) {
    JPH::SoftBodyCreationSettings s(shared, JPH::RVec3(pos.x, pos.y, pos.z),
                                    toJolt(rot), Layers::MOVING);
    s.mNumIterations = static_cast<JPH::uint32>(std::max(d.iterations, 1));
    s.mLinearDamping = std::max(d.damping, 0.0f);
    s.mPressure      = std::max(d.pressure, 0.0f);
    s.mFriction      = glm::clamp(d.friction, 0.0f, 1.0f);
    s.mRestitution   = glm::clamp(d.restitution, 0.0f, 1.0f);
    s.mGravityFactor = d.gravityFactor;
    // Bake the rotation into the particles -- Jolt solves a soft body a little
    // more accurately with the body's own rotation left at identity, and
    // getSoftVertices puts the rotation back on the way out.
    s.mMakeRotationIdentity = true;
    return system.GetBodyInterface().CreateAndAddSoftBody(s, JPH::EActivation::Activate);
}

// The soft-body state behind a body, or null if it is a rigid one. Every reader
// below runs outside Update(), which is why the no-lock interface is safe.
const JPH::SoftBodyMotionProperties* softProps(const JPH::Body& b) {
    if (!b.IsSoftBody()) return nullptr;
    return static_cast<const JPH::SoftBodyMotionProperties*>(b.GetMotionProperties());
}

} // namespace

PhysicsBodyId PhysicsWorld::addSoftMesh(const glm::vec3* verts, int vertCount,
                                        const std::uint32_t* indices, int indexCount,
                                        const std::uint8_t* pinned, glm::vec3 pos,
                                        glm::quat rot, const SoftBodyDesc& desc) {
    if (!verts || !indices || vertCount < 3 || indexCount < 3) return 0;
    JPH::Ref<JPH::SoftBodySharedSettings> shared = new JPH::SoftBodySharedSettings;

    // One particle per vertex, the total mass split evenly between them. A pinned
    // particle gets zero inverse mass -- infinitely heavy, so the rest of the body
    // hangs off it instead of moving it. That is what nails a cloth to its corners.
    const float invMass = static_cast<float>(vertCount) / std::max(desc.mass, 0.01f);
    shared->mVertices.reserve(static_cast<JPH::uint>(vertCount));
    for (int i = 0; i < vertCount; ++i) {
        JPH::SoftBodySharedSettings::Vertex v;
        v.mPosition = JPH::Float3(verts[i].x, verts[i].y, verts[i].z);
        v.mInvMass  = (pinned && pinned[i]) ? 0.0f : invMass;
        shared->mVertices.push_back(v);
    }
    const auto vc = static_cast<std::uint32_t>(vertCount);
    for (int i = 0; i + 2 < indexCount; i += 3) {
        JPH::SoftBodySharedSettings::Face f(indices[i], indices[i + 1], indices[i + 2]);
        if (f.mVertex[0] >= vc || f.mVertex[1] >= vc || f.mVertex[2] >= vc) continue;
        if (f.IsDegenerate()) continue; // AddFace asserts on these; drop them quietly
        shared->mFaces.push_back(f);
    }
    if (shared->mFaces.empty()) return 0;

    // Edges, shear diagonals and (optionally) bend constraints, all derived from
    // the triangles: this is what turns a triangle soup into something that holds
    // its shape. Compliance is the inverse of stiffness, so 0 is as rigid as the
    // solver gets and ~1e-3 is properly soft.
    const JPH::SoftBodySharedSettings::VertexAttributes attr(
        std::max(desc.compliance, 0.0f), std::max(desc.shearCompliance, 0.0f),
        desc.bendCompliance >= 0.0f ? desc.bendCompliance : FLT_MAX);
    shared->CreateConstraints(&attr, 1,
                              JPH::SoftBodySharedSettings::EBendType::Distance);
    shared->Optimize();

    return createSoftBody(m_impl->system, shared, pos, rot, desc)
        .GetIndexAndSequenceNumber();
}

PhysicsBodyId PhysicsWorld::addSoftBox(glm::vec3 halfExtents, int grid,
                                       glm::vec3 pos, glm::quat rot,
                                       const SoftBodyDesc& desc) {
    // A surface-only box would be a bag: with no pressure inside it, it folds up
    // the moment it lands. A jelly cube is a solid LATTICE instead -- particles
    // through the inside as well, tied into tetrahedra whose volume the solver
    // preserves. That is the difference between a balloon and a block of jelly,
    // and it is why this one cannot be expressed through addSoftMesh.
    const int n = std::clamp(grid, 2, 12);
    const glm::vec3 half = glm::max(halfExtents, glm::vec3(0.02f));
    const glm::vec3 step = (half * 2.0f) / static_cast<float>(n - 1);
    const float compliance = std::max(desc.compliance, 0.0f);

    JPH::Ref<JPH::SoftBodySharedSettings> shared = new JPH::SoftBodySharedSettings;
    const int count = n * n * n;
    const float invMass = static_cast<float>(count) / std::max(desc.mass, 0.01f);
    auto index = [n](int x, int y, int z) {
        return static_cast<std::uint32_t>(x + y * n + z * n * n);
    };
    shared->mVertices.reserve(static_cast<JPH::uint>(count));
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const glm::vec3 p =
                    -half + step * glm::vec3(float(x), float(y), float(z));
                JPH::SoftBodySharedSettings::Vertex v;
                v.mPosition = JPH::Float3(p.x, p.y, p.z);
                v.mInvMass  = invMass;
                shared->mVertices.push_back(v);
            }

    // Springs along the three axes of the lattice.
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const std::uint32_t a = index(x, y, z);
                if (x < n - 1)
                    shared->mEdgeConstraints.push_back({a, index(x + 1, y, z), compliance});
                if (y < n - 1)
                    shared->mEdgeConstraints.push_back({a, index(x, y + 1, z), compliance});
                if (z < n - 1)
                    shared->mEdgeConstraints.push_back({a, index(x, y, z + 1), compliance});
            }
    shared->CalculateEdgeLengths();

    // Six tetrahedra fill one cell of the lattice (the standard split, the same
    // one Jolt uses in its own sample), and their volume is what the body springs
    // back to after a hit.
    static const int kTetra[6][4][3] = {
        {{0, 0, 0}, {0, 1, 1}, {0, 0, 1}, {1, 1, 1}},
        {{0, 0, 0}, {0, 1, 0}, {0, 1, 1}, {1, 1, 1}},
        {{0, 0, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}},
        {{0, 0, 0}, {1, 0, 1}, {1, 0, 0}, {1, 1, 1}},
        {{0, 0, 0}, {1, 1, 0}, {0, 1, 0}, {1, 1, 1}},
        {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {1, 1, 1}}};
    for (int z = 0; z < n - 1; ++z)
        for (int y = 0; y < n - 1; ++y)
            for (int x = 0; x < n - 1; ++x)
                for (const auto& t : kTetra) {
                    JPH::SoftBodySharedSettings::Volume v;
                    for (int i = 0; i < 4; ++i)
                        v.mVertex[i] = index(x + t[i][0], y + t[i][1], z + t[i][2]);
                    v.mCompliance = compliance;
                    shared->mVolumeConstraints.push_back(v);
                }
    shared->CalculateVolumeConstraintVolumes();

    // The outer shell, wound outwards: the only part of the lattice there is
    // anything to draw, or for anything else to collide against.
    auto quad = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c,
                    std::uint32_t d) {
        shared->mFaces.push_back(JPH::SoftBodySharedSettings::Face(a, b, c));
        shared->mFaces.push_back(JPH::SoftBodySharedSettings::Face(a, c, d));
    };
    const int e = n - 1;
    for (int b = 0; b < e; ++b)
        for (int a = 0; a < e; ++a) {
            quad(index(a, b, 0), index(a, b + 1, 0),
                 index(a + 1, b + 1, 0), index(a + 1, b, 0));            // -Z
            quad(index(a, b, e), index(a + 1, b, e),
                 index(a + 1, b + 1, e), index(a, b + 1, e));            // +Z
            quad(index(a, 0, b), index(a + 1, 0, b),
                 index(a + 1, 0, b + 1), index(a, 0, b + 1));            // -Y
            quad(index(a, e, b), index(a, e, b + 1),
                 index(a + 1, e, b + 1), index(a + 1, e, b));            // +Y
            quad(index(0, a, b), index(0, a, b + 1),
                 index(0, a + 1, b + 1), index(0, a + 1, b));            // -X
            quad(index(e, a, b), index(e, a + 1, b),
                 index(e, a + 1, b + 1), index(e, a, b + 1));            // +X
        }
    shared->Optimize();

    return createSoftBody(m_impl->system, shared, pos, rot, desc)
        .GetIndexAndSequenceNumber();
}

int PhysicsWorld::softVertexCount(PhysicsBodyId id) const {
    JPH::BodyLockRead lock(m_impl->system.GetBodyLockInterfaceNoLock(), JPH::BodyID(id));
    if (!lock.Succeeded()) return 0;
    const JPH::SoftBodyMotionProperties* mp = softProps(lock.GetBody());
    return mp ? static_cast<int>(mp->GetVertices().size()) : 0;
}

int PhysicsWorld::softFaceCount(PhysicsBodyId id) const {
    JPH::BodyLockRead lock(m_impl->system.GetBodyLockInterfaceNoLock(), JPH::BodyID(id));
    if (!lock.Succeeded()) return 0;
    const JPH::SoftBodyMotionProperties* mp = softProps(lock.GetBody());
    return mp ? static_cast<int>(mp->GetFaces().size()) : 0;
}

bool PhysicsWorld::getSoftFaces(PhysicsBodyId id, std::uint32_t* out, int count) const {
    if (!out || count < 3) return false;
    JPH::BodyLockRead lock(m_impl->system.GetBodyLockInterfaceNoLock(), JPH::BodyID(id));
    if (!lock.Succeeded()) return false;
    const JPH::SoftBodyMotionProperties* mp = softProps(lock.GetBody());
    if (!mp) return false;
    const auto& faces = mp->GetFaces();
    const int n = std::min(count / 3, static_cast<int>(faces.size()));
    for (int i = 0; i < n; ++i) {
        out[3 * i + 0] = faces[i].mVertex[0];
        out[3 * i + 1] = faces[i].mVertex[1];
        out[3 * i + 2] = faces[i].mVertex[2];
    }
    return n > 0;
}

bool PhysicsWorld::getSoftVertices(PhysicsBodyId id, glm::vec3* out, int count,
                                   glm::vec3& outCenter) const {
    if (!out || count <= 0) return false;
    JPH::BodyLockRead lock(m_impl->system.GetBodyLockInterfaceNoLock(), JPH::BodyID(id));
    if (!lock.Succeeded()) return false;
    const JPH::Body& body = lock.GetBody();
    const JPH::SoftBodyMotionProperties* mp = softProps(body);
    if (!mp) return false;
    // Particle positions are stored relative to the body's centre of mass, which
    // is exactly the split a caller wants: one moving origin for the whole body,
    // and a mesh around it that only ever changes shape.
    const JPH::RVec3 com = body.GetCenterOfMassPosition();
    outCenter = glm::vec3(float(com.GetX()), float(com.GetY()), float(com.GetZ()));
    const JPH::Quat rot = body.GetRotation();
    const auto& vs = mp->GetVertices();
    const int n = std::min(count, static_cast<int>(vs.size()));
    for (int i = 0; i < n; ++i) out[i] = toGlm(rot * vs[i].mPosition);
    return n > 0;
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
