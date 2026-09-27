#pragma once

#include <array>
#include <functional>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/asset/AssetId.hpp>
#include <fitzel/graphics/Mesh.hpp>
#include <fitzel/graphics/Shader.hpp>

#include <unordered_map>

#include "Component.hpp"
#include "SceneTypes.hpp"
#include "TrafficSim.hpp"

class CitySystem;
class EditMeshCache;
class ModelLibrary;
namespace fitzel { class PhysicsWorld; }
namespace prefab { struct Prefab; }

// Hand a scene object -- a car, a bus, anything -- to the towns' traffic: in
// Play it drives itself on the nearest lane, keeps its distance, stops at red
// and (as a bus) calls at the stops, a CPU driver instead of the player. It
// moves kinematically (the traffic sets where it is, no engine or tyres), with
// a kinematic box so the player's car can hit it; Stop puts it back where it
// was authored, like everything else Play touches.
class TrafficDriverComponent : public ComponentBase {
public:
    int   kind     = 0;       // 0 car, 1 bus (calls at the stops), 2 lorry
    float topSpeed = 50.0f;   // km/h
    int   forward  = 0;       // which way its nose points: 0 +Z, 1 -Z, 2 +X, 3 -X
    float ride     = -1.0f;   // its centre above the road, metres; < 0 = its half height
    bool  collider = true;    // a kinematic box the player's car collides with

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<TrafficDriverComponent>(*this);
    }
    const char* typeId() const override { return "trafficDriver"; }
    const char* displayName() const override { return "Traffic driver"; }
    const std::vector<Property>& props() const override;
};

// Draws what TrafficSim computes: the towns' cars, buses, lorries and people,
// as blocks and cylinders (placeholders to be replaced by models later).
//
// The crowd is ONE mesh per material, re-skinned on the CPU every frame and
// re-uploaded -- a few hundred vertices per vehicle, a hundred per person, and
// a handful of draws however many move. Drawing each vehicle on its own would
// cost a draw per vehicle per material per pass, and the renderer pays per draw.
// Only what the camera can see goes in (setView): a big town is thousands of
// people and hundreds of vehicles, a quarter of a million vertices, and skinning
// and uploading all of them every frame cost more than the rest of the town.
//
// The catch with that: a mesh whose vertices move under an identity model
// matrix has no motion vectors, so TAA would smear every car. drawMotion()
// fills them in from each vertex's position this frame and last, like the
// swaying trees' pass (VegetationSystem::drawTreeMotion).
namespace traffic {

class TownTraffic {
public:
    // The road surface under a point (see Sim::surfaceAt).
    std::function<bool(glm::vec2, float&)> surfaceAt;
    // Where vehicle prefabs come from (the towns' Rule::vehiclePrefabs name
    // them): a prefab by name, the models its entities show, and the cache that
    // uploads the ones modelled in the editor.
    std::function<const prefab::Prefab*(const std::string&)> findPrefab;
    ModelLibrary*  models    = nullptr;
    EditMeshCache* meshCache = nullptr;

    bool init();   // the motion shader (needs the GL context)

    // Where the camera is this frame, before update(): only what is near it
    // and in its frustum is skinned and drawn. cull = false draws everything
    // near (split screen: the second eye looks elsewhere).
    void setView(const glm::vec3& eye, const glm::mat4& viewProj, bool cull = true);

    // Rebuild when the towns changed, advance, and re-skin. Call before the
    // frame's GPU materials are built: the palette is find-or-created here.
    void update(const CitySystem& towns, float dt, double clock,
                std::vector<MaterialDef>& materials);

    // Every mesh and the material it wears (world space, identity model).
    void forEachDraw(const std::function<void(const fitzel::Mesh&, const fitzel::AssetId&)>& fn) const;
    // Every part of every prefab-dressed vehicle near enough to be drawn as
    // one: the mesh, its material and its model matrix this frame (motion
    // vectors come with the matrix), and whether it is close enough to be worth
    // a shadow and a reflection. Farther off a prefab vehicle is drawn as its
    // kind's placeholder -- a detailed model is tens of thousands of triangles
    // and a draw per part, and a street of them cost more than the landscape.
    void forEachPrefabDraw(const std::function<void(const fitzel::Mesh&, const fitzel::AssetId&,
                                                    const glm::mat4&, bool detail)>& fn);

    // --- Play: scene objects with a TrafficDriverComponent ------------------
    // beginPlay finds them, takes them out of the physics bodies they would
    // otherwise get (see drives()), gives each a kinematic box and puts it on a
    // lane. playTick moves them -- `place` sets an entity's world transform
    // (main's setWorld, parent-aware). endPlay lets them go.
    static bool drives(const Entity& e) { return e.components.get<TrafficDriverComponent>(); }
    void beginPlay(std::vector<Entity>& entities, fitzel::PhysicsWorld* physics);
    void playTick(std::vector<Entity>& entities, fitzel::PhysicsWorld* physics, float dt,
                  const std::function<void(Entity&, const glm::vec3&, const glm::vec3&)>& place);
    void endPlay();
    int  driverCount() const { return static_cast<int>(m_drivers.size()); }

    // Motion vectors for the moving crowd, into the bound motion target.
    void drawMotion(const glm::mat4& viewProj, const glm::mat4& curVP, const glm::mat4& prevVP);

    int vehicleCount() const { return static_cast<int>(m_sim.vehicles().size()); }
    int peopleCount() const { return static_cast<int>(m_sim.walkers().size()); }
    const Sim& sim() const { return m_sim; }

    // The material slots a crowd is painted with.
    enum Slot {
        Paint0, Paint1, Paint2, Paint3, Paint4,   // car colours
        Glass, Tyre, BusBody, TruckCab, TruckBox,
        Coat0, Coat1, Coat2, Coat3, Skin,
        SlotCount
    };

private:
    struct Part {
        int slot = 0;                        // a Slot, or Paint0/Coat0 + the instance's look
        bool varies = false;                 // slot + look
        std::vector<fitzel::Vertex> verts;   // local frame: x forward, y up
        std::vector<std::uint32_t>  idx;
    };
    struct Instance {
        const std::vector<Part>* parts = nullptr;
        int look = 0;
        bool person = false;
        glm::mat4 prev{1.0f};                // last frame's pose, for the motion vectors
    };

    // A vehicle prefab, flattened: its drawn parts in the vehicle's frame (x
    // along its nose, y up, standing on y = 0, centred), and how long it is.
    struct PrefabPart {
        const fitzel::Mesh* mesh = nullptr;
        fitzel::AssetId     material;
        glm::mat4           local{1.0f};
        int                 wheel = -1;      // FL FR RL RR: turns with that wheel
        glm::mat4           rest{1.0f};      // in the prefab's own frame (local = frame * rest)
    };
    // A wheel of the prefab's vehicle rig (its VehicleComponent): turned the way
    // the race sim turns a driven car's -- spin on its own X, steer on Y.
    struct RigWheel {
        bool      valid = false;
        glm::mat4 parentWorld{1.0f};
        glm::vec3 localCenter{0.0f}, localRotation{0.0f};
        glm::vec3 turn{0.0f};            // the rig's correction (VehicleComponent::wheelTurn)
        glm::mat4 restInv{1.0f};
    };
    struct Rig {
        std::array<RigWheel, 4> wheels{};
        float radius = 0.35f, wheelbase = 2.7f, maxSteer = 0.55f, spinSign = 1.0f;
        bool  any = false;
    };
    // A wheel's rest-to-now transform, for a spin (radians) and a steer.
    static glm::mat4 wheelTurn(const Rig& rig, int i, float spin, float steer);
    // The steer that makes a car of this wheelbase turn at the vehicle's rate.
    static float steerOf(const Vehicle& v, float wheelbase, float maxSteer);
    struct PrefabLook {
        std::string             name;
        int                     kind = 0;
        float                   weight = 1.0f;
        float                   length = 4.3f;
        std::vector<PrefabPart> parts;
        glm::mat4               frame{1.0f};   // prefab frame -> vehicle frame
        Rig                     rig;
    };
    struct Driver {
        int       entity = -1;
        traffic::Kind kind = traffic::Kind::Car;
        float     length = 4.3f, vmax = 13.0f, ride = 0.0f;
        int       forward = 0;
        std::uint32_t body = 0;
        glm::vec2 pos{0.0f}, heading{0.0f, 1.0f};   // last known, for a rebuild
        bool      placed = false;   // on a lane yet (Play can start before the towns are)
        std::string name;
        int       wheel[4] = {-1, -1, -1, -1};   // its rig's wheel entities, if it has one
        glm::vec3 wheelRest[4]{};                // their authored local rotations
        float     wheelR = 0.35f, wheelbase = 2.7f, maxSteer = 0.55f, spinSign = 1.0f;
    };
    void placeDrivers();   // put every driver not yet on a lane onto one

    void rebuild(const CitySystem& towns);
    bool flatten(const prefab::Prefab& p, int forward, PrefabLook& out);
    void ensurePalette(std::vector<MaterialDef>& materials);
    int  slotOf(const Part& p, int look) const;
    bool inView(const glm::vec3& p, float reach, float radius) const;
    bool asPrefab(const Vehicle& v, const glm::vec3& at) const;   // else as the placeholder
    void skin(const Instance& in, const glm::mat4& m);

    Sim m_sim;
    int m_revision = -1;
    float m_retry = 0.0f;   // seconds to the next try when the roads were not there yet
    std::array<fitzel::AssetId, SlotCount> m_mats;
    std::array<std::vector<Part>, 4> m_templates;   // Car, Bus, Truck, Person

    std::array<std::vector<fitzel::Vertex>, SlotCount> m_verts;   // this frame's, in view only
    std::array<std::vector<std::uint32_t>, SlotCount>  m_idx;
    std::vector<std::uint32_t>                         m_motionIdx;
    std::array<fitzel::Mesh, SlotCount>                m_meshes;
    std::array<bool, SlotCount>                        m_live{};
    std::vector<fitzel::Vertex> m_motionVerts;
    fitzel::Mesh                m_motionMesh;
    bool                        m_motionLive = false;
    std::vector<Instance>       m_instances;         // vehicles first, then people
    glm::vec3                   m_eye{0.0f};
    std::array<glm::vec4, 6>    m_planes{};          // frustum, inward normals
    bool                        m_cull = false;
    bool                        m_haveView = false;  // no setView yet: everything is in view
    fitzel::Shader              m_motion;
    std::vector<PrefabLook>     m_looks;             // every town's vehicle prefabs
    std::vector<std::vector<int>> m_townLooks;       // per town: indices into m_looks
    std::vector<Driver>         m_drivers;
    bool                        m_playing = false;
};

} // namespace traffic
