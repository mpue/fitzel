#pragma once

#include <array>
#include <functional>
#include <map>
#include <memory>
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
// and (as a bus) calls at the stops, a CPU driver instead of the player. The
// traffic sets where it is (no engine or tyres), but it is a real body with its
// mass (PhysicsWorld::addDrivenBox): hit it hard enough and it crashes -- it
// leaves the traffic and tumbles on as a wreck, and the traffic behind stops
// for it. Stop puts it back where it was authored, like everything else Play
// touches.
class TrafficDriverComponent : public ComponentBase {
public:
    int   kind     = 0;       // 0 car, 1 bus (calls at the stops), 2 lorry
    float topSpeed = 50.0f;   // km/h
    int   forward  = 0;       // which way its nose points: 0 +Z, 1 -Z, 2 +X, 3 -X
    float ride     = -1.0f;   // its centre above the road, metres; < 0 = its half height
    bool  collider = true;    // a body the player's car collides with (and can crash)
    float crashJolt = 10.0f;  // km/h: a knock that changes its speed this much crashes it

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
    // Where vehicle and person prefabs come from (the towns' Rule::vehiclePrefabs
    // and personPrefabs name them): a prefab by name, the models its entities
    // show, and the cache that uploads the ones modelled in the editor.
    std::function<const prefab::Prefab*(const std::string&)> findPrefab;
    ModelLibrary*  models    = nullptr;
    EditMeshCache* meshCache = nullptr;

    bool init();   // the motion shader (needs the GL context)

    // Where the camera is this frame, before update(): only what is near it
    // and in its frustum is skinned and drawn. cull = false draws everything
    // near (split screen: the second eye looks elsewhere).
    void setView(const glm::vec3& eye, const glm::mat4& viewProj, bool cull = true);

    // Move everyone on by `dt` (the signals' time is `clock`). Once a frame,
    // BEFORE the cameras and before playTick: whatever shoots or follows a
    // vehicle has to see where it is this frame, not where it was.
    void advance(float dt, double clock);

    // Rebuild when the towns changed, and re-skin what advance() moved. Call
    // before the frame's GPU materials are built: the palette is
    // find-or-created here.
    void update(const CitySystem& towns, float dt, std::vector<MaterialDef>& materials);

    // Every mesh and the material it wears (world space, identity model).
    void forEachDraw(const std::function<void(const fitzel::Mesh&, const fitzel::AssetId&)>& fn) const;
    // Every part of every prefab-dressed vehicle near enough to be drawn as
    // one: the mesh, its material and its model matrix this frame (motion
    // vectors come with the matrix), and whether it is close enough to be worth
    // a shadow and a reflection. Farther off a prefab vehicle is drawn as its
    // kind's placeholder -- a detailed model is tens of thousands of triangles
    // and a draw per part, and a street of them cost more than the landscape.
    // The same for the people dressed in a person prefab: the nearest of them
    // (see kPersonPrefabReach), each in the pose of the walk its steps reached.
    void forEachPrefabDraw(const std::function<void(const fitzel::Mesh&, const fitzel::AssetId&,
                                                    const glm::mat4&, bool detail)>& fn);

    // --- Play: scene objects with a TrafficDriverComponent ------------------
    // beginPlay finds them, takes them out of the physics bodies they would
    // otherwise get (see drives()), gives each a driven box and puts it on a
    // lane. playTick -- after the physics step -- crashes whoever was knocked
    // hard enough, moves the rest and lays the wrecks where the physics has
    // them -- `place` sets an entity's world transform (main's setWorld,
    // parent-aware). endPlay lets them go.
    //
    // The towns' own vehicles get the same near the player (or the eye): the
    // nearest few are lent a driven body each, so the player's car meets real
    // mass there, and a hard knock crashes them into a wreck. Far off they stay
    // what they were -- the simulation, drawn, and nothing to collide with --
    // which is how the big open-world games keep a whole city of traffic cheap.
    // After Play the traffic is built again, so the crashed come back.
    static bool drives(const Entity& e) { return e.components.get<TrafficDriverComponent>(); }
    void beginPlay(std::vector<Entity>& entities, fitzel::PhysicsWorld* physics);
    void playTick(std::vector<Entity>& entities, fitzel::PhysicsWorld* physics, float dt,
                  const std::function<void(Entity&, const glm::vec3&, const glm::vec3&)>& place);
    void endPlay();
    int  driverCount() const { return static_cast<int>(m_drivers.size()); }
    int  wreckCount() const;
    // The player's car (its chassis body, 0 = none, and the box's half size):
    // the traffic brakes for it, and follows it, instead of driving into it.
    void setPlayerCar(std::uint32_t body, const glm::vec3& half) { m_playerBody = body; m_playerHalf = half; }

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
        glm::vec3               size{4.3f, 1.5f, 1.8f};   // its box: long, high, wide
        std::vector<PrefabPart> parts;
        glm::mat4               frame{1.0f};   // prefab frame -> vehicle frame
        Rig                     rig;
    };
    // A person prefab, flattened: its drawn parts in the person's frame (x the
    // way they walk, y up, feet on y = 0, centred). An animated model's parts
    // come as the poses of one stride pair of its walk, skinned once; a person
    // shows the one their steps have reached, so no one is skinned per frame
    // however many walk.
    // A model's walk, skinned: `count` poses of each of its primitives
    // (pose-major) over one `period` of the clip, and its pace
    // (walkpace::stanceSpeed). Kept across rebuilds per model and clip --
    // skinning takes a moment, and a town is rebuilt on every edit of it.
    struct Walk {
        std::vector<std::unique_ptr<fitzel::Mesh>> poses;
        std::size_t prims  = 0;
        int         count  = 0;
        float       period = 1.0f;   // seconds of clip
        float       pace   = 0.0f;   // model units per second, along its front
    };
    struct PersonPart {
        std::vector<const fitzel::Mesh*> poses;   // 1 (static) or a Walk's count
        fitzel::AssetId                  material;
        glm::mat4                        local{1.0f};
    };
    struct PersonLook {
        std::vector<PersonPart>            parts;
        std::vector<std::shared_ptr<Walk>> walks;   // what its parts' poses live in
        float cycle = 1.4f;    // metres one run of the walk carries them
    };
    bool flattenPerson(const prefab::Prefab& p, int forward, PersonLook& out,
                       std::map<std::string, std::shared_ptr<Walk>>& used);
    struct PersonChoice { int look = -1; float weight = 1.0f; };

    struct Driver {
        int       entity = -1;
        traffic::Kind kind = traffic::Kind::Car;
        float     length = 4.3f, vmax = 13.0f, ride = 0.0f;
        int       forward = 0;
        std::uint32_t body = 0;
        glm::vec3 half{1.0f};       // the body's box
        float     crashJolt = 2.8f; // m/s
        glm::vec3 askedVel{0.0f}, askedSpin{0.0f};   // what the last target asked of the body
        bool      wrecked = false;  // crashed: out of the traffic, the physics has it
        glm::vec2 pos{0.0f}, heading{0.0f, 1.0f};   // last known, for a rebuild
        bool      placed = false;   // on a lane yet (Play can start before the towns are)
        std::string name;
        int       wheel[4] = {-1, -1, -1, -1};   // its rig's wheel entities, if it has one
        glm::vec3 wheelRest[4]{};                // their authored local rotations
        float     wheelR = 0.35f, wheelbase = 2.7f, maxSteer = 0.55f, spinSign = 1.0f;
    };
    void placeDrivers();   // put every driver not yet on a lane onto one

    // A town vehicle near the player, lent a driven body so it can be hit (the
    // way the drivers' are); keyed by Vehicle::uid, given back when it is out
    // of reach again.
    struct Proxy {
        std::uint32_t uid = 0, body = 0;
        glm::vec3     half{1.0f};
        glm::vec3     askedVel{0.0f}, askedSpin{0.0f};
        bool          wanted = false;   // still near this tick
    };
    // A town vehicle that crashed: out of the traffic, drawn where its body lies.
    struct Wreck {
        std::uint32_t body = 0;
        glm::vec3     half{1.0f};
        int           prefab = -1;
        float         odo = 0.0f;        // its wheels stay turned where they stopped
        Instance      inst;              // its placeholder, and last frame's pose
        glm::mat4     now{1.0f};         // this frame's pose, in the vehicle frame
    };
    void tickTownBodies(fitzel::PhysicsWorld& physics, float dt);
    glm::vec3 halfOf(const Vehicle& v) const;
    // The parts of a prefab look, posed by m with its wheels turned.
    void drawLook(const PrefabLook& look, const glm::mat4& m, float spin, float steer, bool detail,
                  const std::function<void(const fitzel::Mesh&, const fitzel::AssetId&,
                                           const glm::mat4&, bool)>& fn) const;

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
    // The person prefabs in use, per town the ones it walks (by weight), and
    // the skinned walks (see Walk) by model and clip.
    std::vector<PersonLook>                         m_personLooks;
    std::vector<std::vector<PersonChoice>>          m_townPeople;
    std::map<std::string, std::shared_ptr<Walk>>    m_walks;
    std::vector<unsigned char>                      m_personNear;   // per walker, this frame
    std::vector<Driver>         m_drivers;
    bool                        m_playing = false;
    std::uint32_t               m_playerBody = 0;
    glm::vec3                   m_playerHalf{1.0f};
    std::vector<Proxy>          m_proxies;
    std::vector<Wreck>          m_wrecks;
    bool                        m_crashedTown = false;   // rebuild after Play: bring them back
};

} // namespace traffic
