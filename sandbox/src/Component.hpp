#pragma once

#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json_fwd.hpp>

#include <fitzel/asset/AssetId.hpp>
#include <fitzel/world/Terrain.hpp>   // TerrainSettings, held by TerrainComponent

#include "EditMesh.hpp"               // EditMesh, held by MeshComponent
#include "FogMedium.hpp"              // FogMedium, held by VolumetricFogComponent
#include "Property.hpp"
#include "ScriptParam.hpp"

// A world-space debug-drawing sink for component gizmos. The viewport supplies a
// concrete implementation (projecting each shape onto the ImGui draw list);
// components emit only world-space shapes, so Component.cpp/.hpp stay free of any
// rendering dependency. Colours are RGBA in 0..1.
struct GizmoDraw {
    virtual ~GizmoDraw() = default;
    // Where the drawn entity's parent stands, when it has one. Filled by the
    // caller, which is the only place that can know it -- onGizmo is handed a
    // transform, not a scene. A component whose MEANING involves its parent
    // (a follow camera aims at it) cannot draw itself honestly without this.
    glm::vec3 parentCenter{0.0f};
    bool      hasParent = false;
    virtual void line(const glm::vec3& a, const glm::vec3& b, const glm::vec4& col) = 0;
    // A `radius` circle centred at `c`, in the plane whose normal is `axis`.
    virtual void circle(const glm::vec3& c, float radius,
                        const glm::vec3& axis, const glm::vec4& col) = 0;
    // A 3-ring wire sphere (built from circle()); the common component gizmo.
    void sphere(const glm::vec3& c, float radius, const glm::vec4& col) {
        circle(c, radius, {1.0f, 0.0f, 0.0f}, col);
        circle(c, radius, {0.0f, 1.0f, 0.0f}, col);
        circle(c, radius, {0.0f, 0.0f, 1.0f}, col);
    }
};

// A component is an optional, self-describing capability attached to an entity.
// It exposes its fields through the same Property metadata as entities, so the
// auto-inspector and JSON serialization work on it for free. Components are
// type-erased (cloneable) so an entity stays a copyable value -- which keeps the
// undo/redo snapshot mechanism working unchanged.
//
// This is the extensibility backbone: a new capability (a sound emitter, a
// trigger, a spinner) is a new ComponentBase subclass + a registration -- no
// central enum or switch to touch.
class ComponentBase {
public:
    virtual ~ComponentBase() = default;
    virtual std::unique_ptr<ComponentBase> clone() const = 0;
    virtual const char* typeId() const = 0;       // stable id (serialization + registry)
    virtual const char* displayName() const = 0;  // label in the inspector
    virtual const std::vector<Property>& props() const = 0; // field metadata over *this

    // JSON (de)serialization. The default drives it from props() (the common
    // case); components with non-property data (e.g. an asset reference) override.
    virtual void save(nlohmann::json& j) const;
    virtual void load(const nlohmann::json& j);

    // Optional viewport gizmo, drawn for the selected entity while authoring.
    // `worldCenter`/`worldRot` are the entity's derived world transform (rot lets
    // oriented gizmos like a camera frustum point the right way). Default: nothing.
    virtual void onGizmo(GizmoDraw& g, const glm::vec3& worldCenter,
                         const glm::quat& worldRot) const {}
};

// Holds an entity's components with value semantics: copying deep-clones, so an
// Entity remains copyable (undo snapshots) despite owning polymorphic parts.
class ComponentList {
public:
    ComponentList() = default;
    ComponentList(const ComponentList& o) { *this = o; }
    ComponentList& operator=(const ComponentList& o) {
        items.clear();
        items.reserve(o.items.size());
        for (const auto& c : o.items) items.push_back(c->clone());
        return *this;
    }
    ComponentList(ComponentList&&) = default;
    ComponentList& operator=(ComponentList&&) = default;

    // First attached component of type T, or nullptr.
    template <class T> T* get() {
        for (auto& c : items) if (auto* p = dynamic_cast<T*>(c.get())) return p;
        return nullptr;
    }
    template <class T> const T* get() const {
        for (auto& c : items) if (auto* p = dynamic_cast<const T*>(c.get())) return p;
        return nullptr;
    }

    std::vector<std::unique_ptr<ComponentBase>> items;
};

// True if two component lists are value-equal (used by undo to drop no-op edits).
bool componentsEqual(const ComponentList& a, const ComponentList& b);

// --- Type registry: every component kind registers a factory + label, so the
// "Add Component" menu and deserialization are open (no central switch). --------
namespace components {

struct TypeInfo {
    std::string typeId;
    std::string displayName;
    std::function<std::unique_ptr<ComponentBase>()> make;
    bool addable = true; // shown in the "Add Component" menu (false = engine-managed)
};

std::vector<TypeInfo>&           registry();
void                             registerType(TypeInfo info);
std::unique_ptr<ComponentBase>   create(const std::string& typeId);

} // namespace components

// --- Built-in component: Spin (rotates the entity while playing) -------------
class SpinComponent : public ComponentBase {
public:
    glm::vec3 axis{0.0f, 1.0f, 0.0f}; // rotation axis weights
    float     speed = 90.0f;          // degrees per second

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<SpinComponent>(*this);
    }
    const char* typeId() const override { return "spin"; }
    const char* displayName() const override { return "Spin"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
};

// --- Built-in component: Collectible (a pickup -- walk into it in Play) -------
// A game mechanic authored as data, no scripting: while playing, when the player
// comes within `radius` of this entity it awards `points` to the score, plays an
// optional one-shot `sound` (from the project's sounds/), and removes the
// entity. Attach it to any solid so an artist builds "collect the coins" with
// zero code. Ticked in the play loop alongside the other built-in behaviours.
class CollectibleComponent : public ComponentBase {
public:
    float       points = 10.0f; // added to the score on pickup (whole number)
    float       radius = 1.5f;  // pickup distance from the player (metres)
    std::string sound;          // one-shot file under the project's sounds/ ("" = none)

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<CollectibleComponent>(*this);
    }
    const char* typeId() const override { return "collectible"; }
    const char* displayName() const override { return "Collectible"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
    void onGizmo(GizmoDraw& g, const glm::vec3& c, const glm::quat&) const override {
        g.sphere(c, radius, {1.0f, 0.85f, 0.2f, 0.9f}); // gold pickup radius
    }
};

// --- Built-in component: Missile Pickup (fly over it to arm the launcher) -----
// Missiles are not issued, they are found: the launcher starts a race empty and
// every round on the rail was flown over on the track. Attach this to the pickup
// model and lay them down the course -- the racing line and the armed line stop
// being the same line, which is the whole point of the mechanic.
//
// Taking one deactivates the entity rather than destroying it, so it can come
// back `respawn` seconds later for the next lap (0 = gone for the rest of the
// race). `cooldown` is transient runtime state like the Trigger's `fired`, and
// resets for free when Play stops and the scene is restored from its backup.
//
// A pickup is NOT consumed by a full rack: flying over one with no room left
// leaves it standing for the lap where it is worth something.
class MissilePickupComponent : public ComponentBase {
public:
    int         count   = 1;    // rounds awarded (capped by the launcher's rack)
    float       radius  = 4.0f; // how close the craft must pass (metres)
    float       respawn = 15.0f;// seconds until it returns (0 = never)
    // Pickup cue: a Sound asset filename, chosen from the project's assets in
    // the Inspector rather than typed, exactly like the Collectible's and the
    // Boost Pad's. Empty = silent.
    std::string sound;

    float cooldown = 0.0f;      // runtime: seconds left before it returns

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<MissilePickupComponent>(*this);
    }
    const char* typeId() const override { return "missile_pickup"; }
    const char* displayName() const override { return "Missile Pickup"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
    void onGizmo(GizmoDraw& g, const glm::vec3& c, const glm::quat&) const override {
        // Warhead red, so a pickup is never mistaken for a gold collectible or a
        // cyan trigger at a glance while laying out a course.
        g.sphere(c, radius, {1.0f, 0.35f, 0.2f, 0.9f});
    }
};

// --- Built-in component: Trigger (a zone that fires an event on entry) --------
// Data-authored, no scripting: while playing, when the player enters within
// `radius` the trigger shows `message` on the HUD and/or plays `sound`. `once`
// fires a single time (until Play restarts). The `insideLast`/`fired` runtime
// flags are transient (not serialized) and reset for free when Play stops (the
// scene is restored from its pre-play backup). Attach to any entity (typically
// an invisible marker) for checkpoints, messages, "level complete", etc.
class TriggerComponent : public ComponentBase {
public:
    float       radius = 2.0f;  // activation distance from the player (metres)
    bool        once   = true;  // fire only once per Play session
    std::string message;        // shown on the HUD on entry ("" = none)
    std::string sound;          // one-shot file under the project's sounds/ ("" = none)

    bool insideLast = false;    // runtime: player was inside last frame (edge detect)
    bool fired      = false;    // runtime: has fired (for `once`)

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<TriggerComponent>(*this);
    }
    const char* typeId() const override { return "trigger"; }
    const char* displayName() const override { return "Trigger"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
    void onGizmo(GizmoDraw& g, const glm::vec3& c, const glm::quat&) const override {
        g.sphere(c, radius, {0.3f, 0.8f, 1.0f, 0.9f}); // cyan activation zone
    }
};

// --- Built-in component: SceneTrigger (loads another scene on entry) ----------
// Data-authored level transition, no scripting: while playing, when the player
// enters within `radius` it loads the scene named `scene` (a .fitzel in the same
// project folder). If the game was playing it keeps playing the new scene, so it
// reads as a seamless level change. `once` guards a re-fire while still inside
// (moot once the scene swaps, kept for parity). insideLast/fired are transient
// runtime flags, reset for free when Play stops (the scene restores from backup).
// Attach to an invisible marker at a doorway, portal, level exit, etc.
class SceneTriggerComponent : public ComponentBase {
public:
    float       radius = 2.0f;  // activation distance from the player (metres)
    std::string scene;          // target scene: the stem of a .fitzel in the project
    bool        once   = true;  // fire only once per Play session

    bool insideLast = false;    // runtime: player inside last frame (edge detect)
    bool fired      = false;    // runtime: has fired (for `once`)

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<SceneTriggerComponent>(*this);
    }
    const char* typeId() const override { return "scene_trigger"; }
    const char* displayName() const override { return "Scene Trigger"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
    void onGizmo(GizmoDraw& g, const glm::vec3& c, const glm::quat&) const override {
        g.sphere(c, radius, {0.95f, 0.6f, 0.2f, 0.9f}); // orange scene-load zone
    }
};

// --- Built-in component: Mover (moves the entity back and forth in Play) -------
// Data-authored motion, no scripting: while playing the entity oscillates
// smoothly from its start position to start + `offset` and back, one full cycle
// per `duration` seconds. Writes LOCAL position, so the scene graph carries
// children along (a crate on a moving platform rides it). `home`/`phase` are
// transient runtime state, reset when Play stops. Good for platforms, doors,
// patrolling obstacles.
class MoverComponent : public ComponentBase {
public:
    glm::vec3 offset{0.0f, 3.0f, 0.0f}; // travel vector from the start position
    float     duration = 3.0f;          // seconds for one there-and-back cycle

    glm::vec3 home{0.0f};    // runtime: captured start position
    bool      homeSet = false;
    float     phase   = 0.0f; // runtime: cycle position

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<MoverComponent>(*this);
    }
    const char* typeId() const override { return "mover"; }
    const char* displayName() const override { return "Mover"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
    void onGizmo(GizmoDraw& g, const glm::vec3& c, const glm::quat&) const override {
        // Travel path from the start to the far end, with a ring at the target.
        const glm::vec4 col{0.4f, 1.0f, 0.55f, 0.9f};
        g.line(c, c + offset, col);
        g.sphere(c + offset, 0.3f, {col.r, col.g, col.b, 0.7f});
    }
};

// --- Built-in component: Spawner (emits entities on a timer in Play) ----------
// Data-authored, no scripting: while playing it emits something just above
// itself every `interval` seconds, as a dynamic body with an initial upward
// `speed`, up to `maxCount` total. What it emits is either a primitive solid
// (`spawnType`) or -- when `prefab` names one -- a whole prefab instance, turned
// to the spawner's own yaw. Reuses the same deferred spawn path as game.spawn /
// game.spawnPrefab. `timer`/`spawned` are transient and reset when Play stops.
// A fountain of balls, a wave of crates, a stream of enemies -- all without code.
class SpawnerComponent : public ComponentBase {
public:
    // Prefab to emit, by name (as in the project's prefabs/ folder). Empty = emit
    // the primitive `spawnType` instead. Serialized like the other Text fields;
    // the inspector draws it as a prefab picker (see main's component inspector).
    std::string prefab;
    int   spawnType = 3;      // EntityType to emit (0 Box .. 3 Sphere)
    float interval  = 1.0f;   // seconds between spawns
    float speed     = 4.0f;   // initial launch velocity (m/s)
    float spread    = 0.0f;   // launch-direction randomization: cone half-angle
                              // in degrees around +Y (0 = straight up, 180 = any)
    float maxCount  = 20.0f;  // stop after this many (whole number)

    float timer   = 0.0f;     // runtime: time since the last spawn
    int   spawned = 0;        // runtime: how many emitted this Play session

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<SpawnerComponent>(*this);
    }
    const char* typeId() const override { return "spawner"; }
    const char* displayName() const override { return "Spawner"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
    void onGizmo(GizmoDraw& g, const glm::vec3& c, const glm::quat&) const override {
        g.sphere(c, 0.5f, {1.0f, 0.5f, 0.9f, 0.9f}); // emit point
    }
};

// --- Built-in component: Lift (an elevator called by the player) --------------
// Data-authored, no scripting: the platform rests at its start (bottom) and
// rises to start+`offset` (top) at `speed` while the player is within `radius`,
// then descends when they leave. Unlike Mover (which oscillates on its own), a
// Lift is called and rests at either end. A kinematic collider (`bodyId`,
// created lazily on the first tick) follows the platform so it actually carries
// the player and any crates on it. `home`/`t`/`bodyId` are transient runtime
// state, cleared when Play stops (scene restored from backup; physics world
// destroyed). Don't also add a Physics component to a lift.
class LiftComponent : public ComponentBase {
public:
    glm::vec3 offset{0.0f, 4.0f, 0.0f}; // travel from bottom (start) to top
    float     speed  = 2.0f;            // travel speed (m/s)
    float     radius = 2.5f;            // player within this range calls the lift

    glm::vec3 home{0.0f};    // runtime: captured bottom position
    bool      homeSet = false;
    float     t       = 0.0f; // runtime: 0 bottom .. 1 top
    unsigned  bodyId  = 0;    // runtime: kinematic collider (PhysicsBodyId; 0 = none)

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<LiftComponent>(*this);
    }
    const char* typeId() const override { return "lift"; }
    const char* displayName() const override { return "Lift"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
    void onGizmo(GizmoDraw& g, const glm::vec3& c, const glm::quat&) const override {
        g.sphere(c, radius, {0.5f, 0.7f, 1.0f, 0.8f});      // call zone
        g.line(c, c + offset, {0.6f, 0.85f, 1.0f, 1.0f});   // travel to the top
        g.sphere(c + offset, 0.25f, {0.6f, 0.85f, 1.0f, 0.9f});
    }
};

// --- Built-in component: Door (swings or slides open on command) --------------
// A door that opens when `open` is set (by a DoorOpener or startOpen) and closes
// when it clears. `slide` translates by `offset`; otherwise it swings by `angle`
// degrees around Y. `speed` is the open/close rate (fraction per second). Writes
// LOCAL transform (scene-graph children ride along) and drives a kinematic
// collider (bodyId) so it physically blocks when closed and clears when open.
// home/homeRot/t/started/bodyId are transient runtime state. Don't also add a
// Physics component to a door.
class DoorComponent : public ComponentBase {
public:
    bool      slide     = false;             // slide (translate) vs swing (rotate)
    float     angle     = 90.0f;             // swing angle (degrees, around Y)
    glm::vec3 offset{0.0f, 0.0f, 2.0f};      // slide travel
    float     speed     = 2.5f;              // open/close rate (1/sec)
    bool      startOpen = false;             // initial state at Play start

    bool      open    = false;   // runtime: target state (set by a DoorOpener)
    float     t       = 0.0f;    // runtime: 0 closed .. 1 open
    glm::vec3 home{0.0f};        // runtime: closed local position
    glm::vec3 homeRot{0.0f};     // runtime: closed local rotation
    bool      started = false;
    unsigned  bodyId  = 0;       // runtime: kinematic collider (PhysicsBodyId)

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<DoorComponent>(*this);
    }
    const char* typeId() const override { return "door"; }
    const char* displayName() const override { return "Door"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
    void onGizmo(GizmoDraw& g, const glm::vec3& c, const glm::quat& rot) const override {
        if (slide) {
            g.line(c, c + offset, {0.6f, 0.85f, 1.0f, 1.0f});
            g.sphere(c + offset, 0.2f, {0.6f, 0.85f, 1.0f, 0.8f});
        } else { // swing: show the closed and open leading edges
            const glm::vec3 closedDir = rot * glm::vec3(0.0f, 0.0f, -1.0f);
            const glm::vec3 openDir =
                (rot * glm::angleAxis(glm::radians(angle), glm::vec3(0, 1, 0)))
                * glm::vec3(0.0f, 0.0f, -1.0f);
            g.line(c, c + closedDir * 1.5f, {0.6f, 0.85f, 1.0f, 0.5f});
            g.line(c, c + openDir * 1.5f, {0.6f, 1.0f, 0.8f, 1.0f});
        }
    }
};

// --- Built-in component: DoorOpener (opens a Door on player approach) ----------
// A proximity zone: while the player is within `radius`, its target Door is open;
// when they leave it closes -- unless `stayOpen` latches it open after the first
// entry. `target` is a Door entity id (-1 = the entity this opener is attached to,
// i.e. an automatic door). Serializes the target id itself.
class DoorOpenerComponent : public ComponentBase {
public:
    int   target   = -1;     // Door entity id (-1 = self)
    float radius   = 3.0f;
    bool  stayOpen = false;  // latch open after the first trigger

    bool insideLast = false; // runtime
    bool opened     = false; // runtime: latch for stayOpen

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<DoorOpenerComponent>(*this);
    }
    const char* typeId() const override { return "door_opener"; }
    const char* displayName() const override { return "Door Opener"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
    void save(nlohmann::json& j) const override;
    void load(const nlohmann::json& j) override;
    void onGizmo(GizmoDraw& g, const glm::vec3& c, const glm::quat&) const override {
        g.sphere(c, radius, {0.5f, 0.9f, 1.0f, 0.8f}); // sensor zone
    }
};

// --- Built-in component: Vehicle (makes this model drivable) ------------------
// Attach to a model's root entity to hook it into the vehicle system: in Play,
// V spawns the Jolt physics car from this geometry at the entity's transform
// and streams the chassis/wheel transforms back into the entity (and its wheel
// children), so the actual model drives; in the editor, V test-drives it with
// the arcade sim. `wheelId` holds the four wheel child entity ids (FL FR RL RR,
// -1 = none: the body still drives, that wheel just doesn't animate). The
// Vehicle panel's "Make drivable" fills everything from the model's AABBs.
class VehicleComponent : public ComponentBase {
public:
    float     mass         = 1200.0f;         // chassis mass (kg)
    float     engineTorque = 2500.0f;         // N*m
    float     maxSteerDeg  = 32.0f;           // front-wheel steering lock
    float     steerSpeed   = 7.0f;            // how fast the wheels reach lock
    float     wheelRadius  = 0.42f;           // m (all four wheels)
    float     wheelWidth   = 0.30f;
    glm::vec3 chassisHalf{0.9f, 0.35f, 2.0f}; // collision-box half extents
    float     halfTrack = 0.85f;              // half the left-right wheel distance
    float     frontZ    = 1.35f;              // front axle Z (chassis frame, +Z fwd)
    float     rearZ     = -1.35f;             // rear axle Z
    int       forward   = 0;                  // model's nose: 0 = +Z, 1 = -Z
    float     wheelY    = -0.35f;             // wheel-centre height (model-local Y)

    // Handling (Jolt physics car): keep it planted through corners. comLower
    // drops the centre of mass toward the wheels (1 = onto the wheel line, the
    // biggest anti-rollover lever); the suspension + anti-roll bar resist body
    // roll; drive picks which axle(s) get engine torque.
    float     comLower       = 1.0f;          // 0..1 of chassisHalf.y to drop COM
    float     suspensionFreq = 2.0f;          // spring stiffness (Hz)
    float     suspensionDamp = 0.85f;         // spring damping (0..1)
    float     antiRoll       = 1000.0f;       // anti-roll bar stiffness (0 = none)
    float     grip           = 1.5f;          // tyre friction scale (1 = default)
    int       drive          = 0;             // 0 = RWD, 1 = FWD, 2 = AWD
    float     uprightAssist  = 6.0f;          // keep-upright roll torque (0 = pure sim)

    // Boat mode (auto-engages when the vehicle floats deep enough in water): how
    // high it rides, how hard the motor pushes, and how much spray it throws.
    float     boatFloat   = 0.45f;            // resting submersion 0..1 (lower = higher)
    float     boatThrust  = 15.0f;            // motor acceleration (m/s^2)
    float     sprayAmount = 1.0f;             // spray emission scale (0 = off)
    float     sprayHeight = 1.0f;             // spray kick-up strength
    float     spraySize   = 1.0f;             // spray droplet size scale

    // No follow-camera knobs here any more -- see GliderComponent: the view is a
    // camera entity parented to the vehicle, not five numbers on the vehicle.

    int wheelId[4] = {-1, -1, -1, -1};        // wheel child entity ids: FL FR RL RR

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<VehicleComponent>(*this);
    }
    const char* typeId() const override { return "vehicle"; }
    const char* displayName() const override { return "Vehicle"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
    void save(nlohmann::json& j) const override; // props + the wheel entity ids
    void load(const nlohmann::json& j) override;
};

// --- Built-in component: Glider (a Wipeout-style anti-grav hover racer) -------
// Attach to any model/box to make it a flyable hovercraft. Driven exactly like a
// Vehicle -- press G in the viewport (editor test-flight or in Play) to take the
// nearest glider; W/S thrust, A/D steer, Space air-brake, Esc exits. The motion
// is a bespoke ARCADE hover sim (no Jolt): the craft floats a `rideHeight` above
// whatever ground is under it (terrain OR the top of a placed block/ramp, so a
// track can be built from blocks), thrusts along its heading, banks into turns
// and keeps a chase camera behind it. Fully tunable below; all runtime flight
// state lives in main (like the car's), so this component is pure tuning data and
// serializes through the default props() path -- no wheels, no save() override.
class GliderComponent : public ComponentBase {
public:
    // Flight
    float thrust     = 26.0f; // forward acceleration on W (m/s^2)
    float maxSpeed   = 60.0f; // horizontal top speed (m/s)
    float brakeForce = 34.0f; // deceleration on Space (m/s^2)
    float turnRate   = 95.0f; // yaw rate at full steer (deg/s)
    // How quickly the craft REACHES that yaw rate (1/s). This is the craft's
    // rotational inertia, and it is what separates a hover racer from a cursor:
    // below it the stick set the heading directly, so the nose snapped to full
    // lock the instant the stick moved and stopped dead the instant it came back
    // -- no lean-in, no carry-through, nothing to catch. Now the stick asks for a
    // rate and the craft takes time to get there and to give it up again.
    //
    // Lower = heavier. 1.5 is a freighter you have to plan corners in, 4 is a
    // nimble racer, 20 is effectively the old instant behaviour.
    float steerResponse = 3.0f;
    bool  invertSteer = false;// swap A/D (left stick) so the craft steers the other way
    float grip       = 3.0f;  // how fast sideways drift is killed (1/s; higher = less slide)
    float drag       = 0.4f;  // linear damping (1/s)
    // Manual boost (gamepad A / Left Shift): a tank the pilot spends rather than
    // a pad they drive over. Holding the button drains `boostCapacity` at
    // `boostDrain` a second and pushes the craft past its own top speed; letting
    // go refills it at `boostRegen` a second, but only after `boostDelay` has
    // passed -- without that pause, tapping the button would be strictly better
    // than holding it, which is the wrong thing to teach.
    float boostCapacity = 100.0f; // full tank (the HUD bar's 100%)
    float boostDrain    = 34.0f;  // tank units per second while held (~3 s of boost)
    float boostRegen    = 9.0f;   // tank units per second while idle (~11 s to refill)
    float boostDelay    = 1.2f;   // seconds after release before it starts refilling
    float boostThrust   = 55.0f;  // extra forward acceleration while boosting (m/s^2)
    float boostTopSpeed = 30.0f;  // m/s of speed cap ABOVE maxSpeed while boosting
    // The ignition, played once when the pilot switches the boost ON -- and only
    // when there is something in the tank to switch on. A press on an empty tank
    // stays silent on purpose: a sound that fired anyway would promise a push
    // that is not coming, and the empty tank is already told on the HUD. Empty =
    // silent, and it takes a comma-separated list like the hull thud does.
    std::string soundBoost;
    float       soundBoostGain = 1.0f;
    // --- Energy (the hull) ---------------------------------------------------
    // The craft flies on a shield: every collision costs energy, and at zero the
    // run is over. It recharges on its own, but only after a pause and slowly
    // enough that a clean line is worth flying -- an energy bar that refills
    // instantly is just a hit counter with extra steps.
    //
    // Damage is charged on the CLOSING speed at the moment of contact, not on
    // the craft's own speed: brushing a wall at a shallow angle at 300 km/h has
    // to cost less than driving into it head-on, or the only safe way to fly is
    // slowly, which is the wrong lesson for a racer to teach.
    float energyCapacity = 100.0f; // full hull (the HUD bar's 100%)
    float energyRegen    = 3.5f;   // energy per second once recharging
    float energyDelay    = 2.5f;   // seconds after a hit before it recharges
    float energyWarnAt   = 0.10f;  // fraction that fires the low-energy alarm
    float hullRadius     = 1.6f;   // collision radius around the body centre (m)
    float crashDamage    = 1.3f;   // energy per m/s of impact above the threshold
    float crashMinSpeed  = 4.0f;   // impacts slower than this are a free nudge
    float crashBounce    = 0.35f;  // how much of the impact speed comes back
    // Collision SFX: the hull thud, and the alarm at the warning threshold
    // (which repeats while the energy stays low). Empty = silent.
    //
    // The hull thud takes SEVERAL samples, comma-separated ("thud_a.wav,
    // thud_b.wav"), and one of them is picked per impact -- a single crash sound
    // stops being heard as a crash after the third one. One name is a list of
    // one, so a craft that names a single sound is unchanged. The inspector
    // draws it as rows of sound pickers; see SoundList.hpp for why it is a
    // string rather than a vector.
    std::string soundHit  = "impact.wav";
    std::string soundWarn = "energy_low.wav";
    float       soundHitGain  = 1.0f;
    float       soundWarnGain = 1.0f;
    // Hover
    float rideHeight     = 1.6f; // body-centre height held above the ground (m)
    float hoverStiffness = 6.0f; // spring pulling back to rideHeight (1/s)
    float hoverDamp      = 3.0f; // vertical-velocity damping (1/s)
    float gravity        = 22.0f;// downward pull when launched above the ride band (m/s^2)
    // Attitude (visual only -- the craft leans, motion stays flat)
    float bankAngle   = 22.0f;   // roll into a full-lock turn (deg)
    float pitchFollow = 0.6f;    // nose tips with climb/descent (0..1)
    float levelRate   = 5.0f;    // how fast bank/pitch settle (1/s)
    int   forward     = 0;       // model nose: 0 = +Z, 1 = -Z (matches Vehicle)
    // No follow-camera knobs here any more. A craft's camera is an ENTITY hung
    // on it with a CameraComponent -- where it sits is that entity's position,
    // which you drag in the viewport instead of typing three offsets. A craft
    // without such a child simply has no view of its own.

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<GliderComponent>(*this);
    }
    const char* typeId() const override { return "glider"; }
    const char* displayName() const override { return "Glider"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
    void onGizmo(GizmoDraw& g, const glm::vec3& c, const glm::quat&) const override {
        // The hover cushion: a ring at the ride height under the craft.
        g.circle(c - glm::vec3(0.0f, rideHeight, 0.0f), 1.0f,
                 glm::vec3(0.0f, 1.0f, 0.0f), {0.4f, 0.85f, 1.0f, 0.85f});
    }
};

// --- Built-in component: Particles -------------------------------------------
// A configurable emitter on any object: exhaust, sparks, steam, dust, embers,
// snow. The hard-wired effects (rain, boat spray, skid marks, contrails) stay
// where they are -- each knows something about its effect that a general system
// should not have to. This is for the effects a track needs and the engine cannot
// guess.
//
// Every field here is authored; nothing about the live particles is stored on the
// component. That is what lets a prefab, an undo step or Play's snapshot copy an
// emitter around without dragging a cloud of particles along with it (the pools
// live in ParticleSystem, keyed by entity id).
class ParticleComponent : public ComponentBase {
public:
    // --- Emission ------------------------------------------------------------
    bool  playing   = true;   // emitting at all (integration continues regardless)
    bool  loop      = true;   // repeat the cycle, or emit once and stop
    float duration  = 3.0f;   // seconds of emission per cycle
    float rate      = 40.0f;  // particles per second (0 = burst only)
    // Whole numbers carried as floats, the same way FinishLineComponent::laps is:
    // the Property metadata has no integer slider, and one rounding at the point
    // of use is cheaper than a new metadata kind for two fields.
    float burst     = 0.0f;   // extra particles at the start of every cycle
    float maxCount  = 500.0f; // hard ceiling; emission stalls rather than growing
    float lifetime  = 2.0f;   // seconds a particle lives
    float lifeVar   = 0.3f;   // +/- fraction of that

    // --- Where they start ----------------------------------------------------
    // 0 = point, 1 = sphere (filled), 2 = cone up the object's +Y, 3 = box.
    int       shape     = 0;
    float     radius    = 0.5f;
    float     coneAngle = 25.0f;      // degrees from the axis
    glm::vec3 boxSize{1.0f, 1.0f, 1.0f};

    // --- How they move -------------------------------------------------------
    float     speed    = 4.0f;
    float     speedVar = 1.0f;
    glm::vec3 gravity{0.0f, -3.0f, 0.0f};
    float     drag     = 0.4f;        // 1/s; air resistance, not a speed cap
    glm::vec3 wind{0.0f, 0.0f, 0.0f};
    // World space: a particle stays where it was born while the emitter moves on
    // -- a trail, a smoke plume, a shower of sparks left behind.
    //
    // OFF is full local space: positions and velocities are held in the emitter's
    // own frame, so the object's motion cannot reach them at all. It accelerates,
    // it banks, and the plume sits on the nozzle looking exactly the same. That is
    // what a thruster or an engine flame wants, and it is why following only the
    // object's translation is not enough -- a craft that turned would leave its
    // flame pointing where it used to be.
    //
    // Gravity and wind still act in WORLD directions either way (they are rotated
    // into the local frame), so a local-space plume still falls downward.
    bool      worldSpace = true;
    // How much of the emitter's own motion a new particle keeps: exhaust from a
    // craft at speed should not hang in the air at the point it left the pipe.
    // World space only -- in local space there is no emitter motion to inherit,
    // by construction.
    float     inherit  = 0.0f;        // 0..1 of the emitter's velocity

    // --- How they look -------------------------------------------------------
    // Sprite by file name, resolved through the asset database. Empty draws a
    // soft round dot generated in the shader, so a fresh emitter shows something
    // before any texture has been chosen.
    std::string sprite;
    glm::vec3 colorStart{1.0f, 0.85f, 0.5f};
    glm::vec3 colorEnd{1.0f, 0.25f, 0.05f};
    float     alphaStart = 1.0f, alphaEnd = 0.0f;
    float     sizeStart  = 0.4f, sizeEnd  = 1.2f;
    float     sizeVar    = 0.25f;
    float     rotation   = 0.0f;      // degrees of random spread at birth
    float     spin       = 0.0f;      // degrees per second
    int       blend      = 1;         // 0 = alpha (smoke), 1 = additive (fire)
    float     brightness = 1.0f;      // multiplies the colour; >1 to feed bloom
    // Brightness driven by how fast the craft is going: `brightness` is scaled
    // between these two as its speed runs from a standstill to its own top speed.
    // A thruster that glows the same parked as it does flat out reads as a decal;
    // one that flares is the single cue that sells thrust.
    //
    // The craft is whatever Glider or Opponent sits on this object or on one of
    // its ancestors -- an exhaust is a child of the ship, so looking only at the
    // emitter would find nothing. Its OWN top speed is the reference, so there is
    // no second number to keep in sync with the flight model.
    //
    // Both default to 1, which is exactly no effect: equal values disable the
    // whole thing, so existing emitters are untouched and there is no separate
    // on/off flag to forget.
    float     speedGlowMin = 1.0f;    // multiplier at a standstill
    float     speedGlowMax = 1.0f;    // ...and at the craft's top speed
    // Metres of extra length per m/s of speed. 0 = a round billboard; above that
    // the quad turns along its travel, which is what makes sparks read as
    // streaks instead of dots.
    float     stretch    = 0.0f;

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<ParticleComponent>(*this);
    }
    const char* typeId() const override { return "particles"; }
    const char* displayName() const override { return "Particles"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
    void onGizmo(GizmoDraw& g, const glm::vec3& c, const glm::quat& rot) const override {
        // Draw the spawn volume: aiming an emitter should not be guesswork, and
        // a point emitter is otherwise invisible until it is running.
        const glm::vec4 col{1.0f, 0.75f, 0.35f, 0.9f};
        if (shape == 1) {
            g.sphere(c, radius, col);
        } else if (shape == 2) {
            const glm::vec3 axis = rot * glm::vec3(0.0f, 1.0f, 0.0f);
            const float h = 2.0f;
            const glm::vec3 tip = c + axis * h;
            g.circle(tip, h * glm::tan(glm::radians(coneAngle)), axis, col);
            const glm::vec3 a1 = rot * glm::vec3(1.0f, 0.0f, 0.0f);
            const glm::vec3 a2 = rot * glm::vec3(0.0f, 0.0f, 1.0f);
            const float r = h * glm::tan(glm::radians(coneAngle));
            g.line(c, tip + a1 * r, col);
            g.line(c, tip - a1 * r, col);
            g.line(c, tip + a2 * r, col);
            g.line(c, tip - a2 * r, col);
        } else if (shape == 3) {
            const glm::vec3 h = boxSize * 0.5f;
            glm::vec3 v[8];
            for (int i = 0; i < 8; ++i)
                v[i] = c + rot * glm::vec3((i & 1) ? h.x : -h.x,
                                           (i & 2) ? h.y : -h.y,
                                           (i & 4) ? h.z : -h.z);
            const int e[12][2] = {{0,1},{2,3},{4,5},{6,7}, {0,2},{1,3},{4,6},{5,7},
                                  {0,4},{1,5},{2,6},{3,7}};
            for (const auto& k : e) g.line(v[k[0]], v[k[1]], col);
        } else {
            g.circle(c, 0.25f, {0.0f, 1.0f, 0.0f}, col);
            g.circle(c, 0.25f, {1.0f, 0.0f, 0.0f}, col);
        }
    }
};

// --- Built-in component: Boost Pad (a Wipeout-style speed strip) --------------
// Attach to a flat object placed on the track: while playing, when a GLIDER
// flies over the object's footprint it gets shoved forward and its speed cap is
// lifted to `boostSpeed` (which may exceed the craft's own max), then the extra
// speed decays back over `hold` seconds. `accel` is the forward push while on
// the pad; `usePadDir` boosts along the pad's own forward (+Z) instead of the
// craft's heading (aim the pad down the track for a directional boost). The
// gizmo draws chevrons in the boost direction. Only affects gliders (the arcade
// hover sim); the trigger area is the entity's own box, so a wide thin box reads
// as a boost strip.
class BoostPadComponent : public ComponentBase {
public:
    float boostSpeed = 95.0f;  // speed cap while boosting (m/s; may exceed glider max)
    float accel      = 70.0f;  // forward push while over the pad (m/s^2)
    float hold       = 1.5f;   // seconds the extra speed lingers after leaving
    bool  usePadDir  = false;  // push along the pad's +Z instead of the craft's heading
    bool  reverse    = false;  // flip the boost direction (+Z <-> -Z / forward <-> back)
    // Punch SFX played once the instant a craft mounts the pad, so the boost lands
    // as a felt "kick". A Sound asset filename (chosen in the Inspector; empty =
    // silent), with its own gain and a pitch that defaults low for a deep thump.
    std::string sound;
    float       soundGain  = 1.0f;  // punch volume (0..2)
    float       soundPitch = 0.7f;  // punch pitch (lower = deeper)

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<BoostPadComponent>(*this);
    }
    const char* typeId() const override { return "boost_pad"; }
    const char* displayName() const override { return "Boost Pad"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
    void onGizmo(GizmoDraw& g, const glm::vec3& c, const glm::quat& rot) const override {
        const glm::vec3 fwd  = rot * glm::vec3(0.0f, 0.0f, reverse ? -1.0f : 1.0f);
        const glm::vec3 side = rot * glm::vec3(1.0f, 0.0f, 0.0f);
        const glm::vec4 col{0.3f, 0.9f, 1.0f, 0.95f}; // cyan boost chevrons
        for (int i = 0; i < 3; ++i) {
            const glm::vec3 base = c + fwd * (static_cast<float>(i) * 0.8f - 0.8f);
            const glm::vec3 tip  = base + fwd * 0.9f;
            g.line(base + side * 0.7f, tip, col);
            g.line(base - side * 0.7f, tip, col);
        }
    }
};

// --- Built-in component: Opponent (an AI racer that follows the road spline) --
// Attach to any craft/model to turn it into a rival: while playing it travels
// along the built road's centreline at `speed`, hovering `rideHeight` above the
// ground and facing along the road, so it laps a closed track (or runs to the
// end of an open one). `laneOffset` shifts it sideways off the centre (place
// several opponents at different offsets + `startDistance` for a starting grid).
// It banks into corners for flair. Purely kinematic (no physics) and driven in
// the play loop from the RoadSystem, exactly like the other data-authored
// behaviours -- no scripting. `dist`/`bankCur`/`started` are transient runtime
// state, reset when Play stops (scene restored from backup).
class OpponentComponent : public ComponentBase {
public:
    float speed        = 40.0f; // top speed on straights (m/s); slows for corners
    float laneOffset   = 0.0f;  // sideways offset from the centreline (m, + = right)
    float rideHeight   = 1.6f;  // height held above the ground (m)
    float startDistance = 0.0f; // forward offset from the placed spot along the road (m)
    bool  loop         = true;  // restart at the road start (closed track) vs stop at the end
    float bankAngle    = 18.0f; // max visual roll into a corner (deg)
    int   forward      = 0;     // model nose: 0 = +Z, 1 = -Z (matches Vehicle/Glider)
    // Cornering: corner speed = sqrt(grip / curvature), so a tighter bend is
    // taken slower; the racer brakes into it (up to `brake`) and gets back on
    // the throttle (up to `accel`) on the way out.
    float grip         = 14.0f; // lateral grip -> how fast corners can be taken (m/s^2)
    float accel        = 10.0f; // acceleration out of corners / off the line (m/s^2)
    float brake        = 22.0f; // braking into corners (m/s^2)
    // Race sense: rubber-band toward the player. A leading racer eases off and a
    // trailing one pushes on by up to this fraction of its speed, so the pack
    // stays close. 0 = ignore the player (constant pace).
    float catchup      = 0.35f; // rubber-band strength (0..1)
    float racingLine   = 0.7f;  // how hard to cut to the apex (0 = hug base lane)
    float awareness    = 1.0f;  // avoid/overtake other racers + the player (0 = blind)
    // Boost pads: how far off the racing line a racer will swerve to drive over
    // a pad ahead. 1 = aim dead centre at it, 0 = only take pads it happens to
    // cross. Dodging a blocker always wins (no boost is worth a rear-end).
    float padSeek      = 0.8f;  // eagerness to detour onto a boost pad (0..1)
    // Does this one actually take part? Unticked, the craft stays in the scene
    // but sits out the session -- which is how a track can carry a full field and
    // a given race pick three of them. A time trial ignores this and sits
    // everybody out (see racegrid).
    bool  entered      = true;

    float dist    = 0.0f;  // runtime: distance travelled along the road (m)
    float bankCur = 0.0f;  // runtime: smoothed bank
    float pitchCur = 0.0f; // runtime: smoothed pitch (nose follows the road slope)
    float curSpeed = 0.0f; // runtime: current speed (eased toward the corner target)
    float laneCur = 0.0f;  // runtime: current lateral offset (racing line + avoidance)
    float overspeed = 0.0f;// runtime: speed cap above `speed` from a boost pad
    float boostHold = 1.5f;// runtime: linger time of the last pad's boost (s)
    float mistakeT = 0.0f; // runtime: time left in the current slip-up (s)
    float mistakeCd = 0.0f;// runtime: countdown to the next slip-up (s)
    // Runtime, from the difficulty step (see difficulty::applyToField): a
    // multiplier on the time BETWEEN slip-ups, so a bigger number is a steadier
    // field. Not saved with the scene -- it says how this RACE is being played,
    // which is not a property of the circuit.
    float slipScale = 1.0f;
    bool  laneSeeded = false; // runtime: laneCur seeded to laneOffset on first tick
    bool  started = false; // runtime: dist + curSpeed seeded

    // --- Runtime: race progress ------------------------------------------
    // Opponents race by the player's rules: every checkpoint has to be crossed
    // (within the gate's width) before a pass over the start/finish line counts
    // as a lap, and the race ends for them after the line's lap count.
    int   lap        = 0;     // completed laps
    float raceT      = 0.0f;  // running time since GO (s)
    float lapT       = 0.0f;  // current lap time (s)
    float lastLapT   = 0.0f;  // previous lap's time (s)
    float bestLapT   = 0.0f;  // fastest lap so far (s, 0 = none yet)
    float lapDist    = 0.0f;  // metres covered on this lap (guards double counts)
    float finishT    = 0.0f;  // raceT when it took the flag
    int   place      = 0;     // 1-based position (live while racing, final after)
    bool  finishedRace = false;
    bool  raceSeeded = false; // race fields initialised for this run
    std::unordered_set<int> cpDone; // checkpoint ids crossed this lap

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<OpponentComponent>(*this);
    }
    const char* typeId() const override { return "opponent"; }
    const char* displayName() const override { return "Opponent"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
};

// Draw a wireframe gate box (span `w`, height `h`, thickness `d`) standing on the
// ground at `c`, oriented by the entity rotation `rot` plus a `yaw` offset. Shared
// by the Checkpoint and Start/Finish gizmos so their trigger box is drawn exactly.
inline void drawGateGizmo(GizmoDraw& g, const glm::vec3& c, const glm::quat& rot,
                          float w, float h, float d, float yaw, const glm::vec4& col) {
    const glm::quat q = rot * glm::angleAxis(glm::radians(yaw), glm::vec3(0, 1, 0));
    const glm::vec3 X = q * glm::vec3(w * 0.5f, 0.0f, 0.0f);
    const glm::vec3 Z = q * glm::vec3(0.0f, 0.0f, d * 0.5f);
    const glm::vec3 Y(0.0f, h, 0.0f);
    const glm::vec3 b0 = c - X - Z, b1 = c + X - Z, b2 = c + X + Z, b3 = c - X + Z;
    const glm::vec3 t0 = b0 + Y, t1 = b1 + Y, t2 = b2 + Y, t3 = b3 + Y;
    g.line(b0, b1, col); g.line(b1, b2, col); g.line(b2, b3, col); g.line(b3, b0, col);
    g.line(t0, t1, col); g.line(t1, t2, col); g.line(t2, t3, col); g.line(t3, t0, col);
    g.line(b0, t0, col); g.line(b1, t1, col); g.line(b2, t2, col); g.line(b3, t3, col);
}

// --- Built-in component: Start/Finish line (lap timing for the racer) ---------
// Attach to a flat object spanning the track: while playing, when the flown
// GLIDER crosses its footprint it marks a lap. The first crossing after Play
// starts the clock (the start), each later crossing completes a lap and records
// the lap time; after `laps` laps the race is finished (0 = endless practice).
// A short re-arm guard stops a single pass counting twice. Drives the on-screen
// lap/time HUD. The gizmo draws a start/finish gate.
class FinishLineComponent : public ComponentBase {
public:
    float laps = 3.0f; // race length in laps (whole number; 0 = endless practice)
    // What kind of session this scene holds: 0 = race (the entered opponents fly
    // against the player), 1 = time trial (the player alone, opponents sat out).
    // It lives here rather than in a project-wide setting because this object is
    // already what defines the race -- putting the mode anywhere else would split
    // it from the lap count it belongs with.
    int   mode = 0;
    // The grid, laid out BEHIND this line. Craft are placed automatically at the
    // start of a race, so setting one up is choosing who is in it rather than
    // dragging craft onto a start line by hand -- which is not something that
    // should need a steady mouse.
    float gridBack  = 12.0f; // metres behind the line the pole slot sits
    float gridRow   =  8.0f; // metres between rows
    float gridLane  =  2.4f; // lateral stagger, alternating side to side
    bool  playerPole = false;// player on pole; otherwise at the back of the grid
    // The trigger gate -- its own size and orientation, independent of the
    // (possibly rotated) visual object it is attached to. width spans the track,
    // depth is the thickness along travel, height the vertical reach; yaw turns
    // the gate to line it up with the road (e.g. +90 for a plane authored sideways).
    float width  = 12.0f;
    float height = 6.0f;
    float depth  = 3.0f;
    float yaw    = 0.0f;   // degrees, added to the entity's rotation
    // Start sequence SFX: one Sound asset per step of Ready/Set/Go, played as the
    // countdown reaches it (empty = silent). They sit on the start/finish line
    // because that is the object which defines the race, not on the craft.
    std::string soundReady;
    std::string soundSet;
    std::string soundGo;
    float       soundGain = 1.0f;   // volume for all three (0..2)

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<FinishLineComponent>(*this);
    }
    const char* typeId() const override { return "finish_line"; }
    const char* displayName() const override { return "Start/Finish"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
    void onGizmo(GizmoDraw& g, const glm::vec3& c, const glm::quat& rot) const override {
        drawGateGizmo(g, c, rot, width, height, depth, yaw, {1.0f, 1.0f, 1.0f, 0.95f});
    }
};

// --- Built-in component: Checkpoint (must be passed for a lap to count) --------
// Attach to gates along the track: while playing, a lap only counts at the
// Start/Finish line once the glider has passed EVERY checkpoint since the lap
// began -- so the whole circuit has to be flown, no shortcutting. Order doesn't
// matter (all must be touched). A pure marker (the entity's box is the pass
// zone); the gizmo draws a ringed gate.
class CheckpointComponent : public ComponentBase {
public:
    // The trigger gate -- its own size and orientation, independent of the
    // (possibly rotated) visual object. See FinishLineComponent for the fields.
    float width  = 12.0f;
    float height = 6.0f;
    float depth  = 3.0f;
    float yaw    = 0.0f;   // degrees, added to the entity's rotation
    // Gate SFX, played once the moment the flown craft passes through (empty =
    // silent). Only the player's own pass sounds -- a field of opponents lapping
    // would otherwise machine-gun the same sample.
    std::string sound;
    float       soundGain  = 1.0f;  // volume (0..2)
    float       soundPitch = 1.0f;  // pitch (lower = deeper)

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<CheckpointComponent>(*this);
    }
    const char* typeId() const override { return "checkpoint"; }
    const char* displayName() const override { return "Checkpoint"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
    void onGizmo(GizmoDraw& g, const glm::vec3& c, const glm::quat& rot) const override {
        drawGateGizmo(g, c, rot, width, height, depth, yaw, {0.5f, 0.9f, 1.0f, 0.9f});
    }
};

// --- Built-in component: Showroom (turns a scene into the craft/track picker) --
// Attach to ONE entity of a scene -- the podium. While playing, that scene stops
// being a level and becomes the start screen: the camera orbits the podium, every
// glider in the scene becomes a selectable craft, every Track Entry becomes a
// selectable circuit, and pressing START carries the chosen craft into the chosen
// circuit and begins the race.
//
// The entity's own position is the podium centre, so the stage is placed by
// dropping this component on whatever object the craft should float above; the
// craft are arranged BY the showroom (on the podium, or around a ring of
// `ringRadius`) and never have to be positioned by hand -- which is the point:
// authoring a start screen must not require precise dragging.
class ShowroomComponent : public ComponentBase {
public:
    std::string title    = "SELECT YOUR CRAFT";  // headline
    std::string subtitle = "FITZEL RACING";      // small line above it
    // Stage layout. `ringRadius` 0 = podium mode (only the chosen craft is on
    // stage, the rest are off); > 0 = carousel (all craft on a ring that turns to
    // bring the chosen one to the front).
    float ringRadius = 0.0f;   // m
    float riseHeight = 1.6f;   // how high the chosen craft floats over the podium (m)
    float spinSpeed  = 16.0f;  // turntable rate of the chosen craft (deg/s)
    float bobAmount  = 0.18f;  // vertical float of the chosen craft (m)
    // Orbit camera. The showroom drives the camera outright while it is up.
    float camDistance = 9.0f;   // m from the podium
    float camHeight   = 2.6f;   // m above the podium
    float camPitch    = -8.0f;  // degrees (negative looks down)
    float camOrbit    = 6.0f;   // idle orbit rate (deg/s)
    float camFov      = 48.0f;
    glm::vec3 accent{0.38f, 0.87f, 1.0f}; // UI accent (the craft's own overrides it)
    // SFX, by Sound-asset filename (empty = silent), like the boost pad's punch.
    std::string soundMove;    // moving through a list
    std::string soundSelect;  // a craft comes on stage
    std::string soundStart;   // the race is launched
    float       soundGain = 1.0f;

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<ShowroomComponent>(*this);
    }
    const char* typeId() const override { return "showroom"; }
    const char* displayName() const override { return "Showroom"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
    void onGizmo(GizmoDraw& g, const glm::vec3& c, const glm::quat&) const override {
        // The podium: the ring the craft float over, and the camera's orbit.
        g.circle(c, glm::max(ringRadius, 1.5f), glm::vec3(0.0f, 1.0f, 0.0f),
                 {0.4f, 0.9f, 1.0f, 0.9f});
        g.circle(c + glm::vec3(0.0f, camHeight, 0.0f), camDistance,
                 glm::vec3(0.0f, 1.0f, 0.0f), {1.0f, 0.82f, 0.35f, 0.5f});
    }
};

// --- Built-in component: Craft Entry (a craft's showroom presentation) ---------
// OPTIONAL, and only meaningful next to a Glider: every glider in a showroom
// scene is selectable with or without one. It exists purely so a craft can be
// described in the picker -- a display name that isn't the entity name, the team
// line under it, a sentence of flavour, and the accent colour the whole UI takes
// on while that craft is on stage.
class CraftEntryComponent : public ComponentBase {
public:
    std::string title;                    // "" = the entity's name
    std::string team  = "PRIVATEER";      // small line under the name
    std::string blurb;                    // one sentence of flavour
    glm::vec3   accent{0.38f, 0.87f, 1.0f};
    float       order = 0.0f;             // sort key (low first)

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<CraftEntryComponent>(*this);
    }
    const char* typeId() const override { return "craft_entry"; }
    const char* displayName() const override { return "Craft Entry"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
};

// --- Built-in component: Track Entry (one circuit in the showroom's picker) ----
// Attach to any object in a showroom scene (a plinth, a holo-map, an Empty): it
// contributes one card to the circuit list. `scene` is the .fitzel stem the race
// loads. A showroom with no Track Entry at all falls back to listing every other
// scene in the project, so the picker is never empty.
class TrackEntryComponent : public ComponentBase {
public:
    std::string scene;                 // scene stem to load
    std::string title;                 // "" = the scene stem
    std::string blurb;                 // one sentence of flavour
    std::string image;                 // preview texture, project-relative ("" = none)
    float       laps       = 3.0f;     // laps the race is run over (0 = the scene's own)
    float       lengthKm   = 0.0f;     // circuit length, for the card (0 = hidden)
    float       difficulty = 3.0f;     // 1..5, drawn as pips
    float       order      = 0.0f;     // sort key (low first)

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<TrackEntryComponent>(*this);
    }
    const char* typeId() const override { return "track_entry"; }
    const char* displayName() const override { return "Track Entry"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
};

// --- Built-in component: Pusher (a directional force field in Play) -----------
// Data-authored, no scripting: while playing it pushes every dynamic body within
// `radius` along `direction`. `continuous` = a steady force each frame (wind,
// conveyor); otherwise a single impulse when a body enters the zone (bumper,
// launch pad). `strength` scales the push. `insideBodies` tracks entry edges for
// the impulse mode and is transient (reset when Play stops). Pairs nicely with
// Spawner -- spawn balls, then blow or launch them.
class PusherComponent : public ComponentBase {
public:
    glm::vec3 direction{0.0f, 1.0f, 0.0f}; // push direction (world)
    float     strength   = 10.0f;          // force / impulse magnitude
    float     radius     = 3.0f;           // affect dynamic bodies within this range
    bool      continuous = true;           // steady force vs one impulse on entry

    std::unordered_set<int> insideBodies;  // runtime: bodies inside (impulse edges)

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<PusherComponent>(*this);
    }
    const char* typeId() const override { return "pusher"; }
    const char* displayName() const override { return "Pusher"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
    void onGizmo(GizmoDraw& g, const glm::vec3& c, const glm::quat&) const override {
        g.sphere(c, radius, {1.0f, 0.4f, 0.3f, 0.8f}); // affect zone
        const float len = glm::length(direction);
        if (len > 1e-4f) {
            const glm::vec3 tip = c + (direction / len) * glm::min(radius, 3.0f);
            g.line(c, tip, {1.0f, 0.7f, 0.2f, 1.0f});   // push direction
            g.sphere(tip, 0.2f, {1.0f, 0.7f, 0.2f, 0.9f});
        }
    }
};

// --- Built-in component: TriggerSound (a proximity sound zone) ----------------
// Data-authored audio, no scripting: while playing, when the player is within
// `radius` it plays `sound`. `loop` = an ambient zone that keeps looping while
// the player is inside, its volume fading from `volume` at the centre to 0 at the
// edge (a waterfall, a machine, a music area); otherwise a one-shot fired once on
// entry (`once`) or on every entry. The looping voice is owned by main (see the
// zoneSounds map); `insideLast`/`fired` are transient one-shot state.
class TriggerSoundComponent : public ComponentBase {
public:
    float       radius = 4.0f;   // audible/activation distance from the player
    float       volume = 1.0f;   // 0..1
    bool        loop   = false;  // loop while inside (zone) vs one-shot on entry
    bool        once   = true;   // one-shot mode: fire only once per Play
    // Fire-and-forget: on every entry, play the sample to completion (never cut
    // off by leaving the zone, replays on each pass). For fast fly-throughs where
    // the craft is only in range for a frame -- the sound still plays fully.
    // Overrides `loop`/`once` when set.
    bool        oneShot = false;
    std::string sound;           // file under the project's sounds/ ("" = none)

    bool insideLast = false;     // runtime: player inside last frame (one-shot edge)
    bool fired      = false;     // runtime: one-shot latch

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<TriggerSoundComponent>(*this);
    }
    const char* typeId() const override { return "trigger_sound"; }
    const char* displayName() const override { return "Trigger Sound"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
    void onGizmo(GizmoDraw& g, const glm::vec3& c, const glm::quat&) const override {
        g.sphere(c, radius, {0.4f, 0.9f, 0.7f, 0.8f}); // sound zone
    }
};

// --- Built-in component: AudioSource (play a sound/music from an object) -------
// Attach to any entity to emit audio. `playOnStart` fires it automatically when
// Play begins (the static case); scripts can start/stop it on demand through
// game.playAudio(id) / game.stopAudio(id). `loop` keeps it looping (music, a
// hum) vs a one-shot. `spatial` attenuates the volume with distance from the
// player within `radius` (a positional effect); off = a constant, non-positional
// bed (background music). The playing voice is owned by main (audioVoices map).
class AudioSourceComponent : public ComponentBase {
public:
    std::string sound;              // file under the project's sounds/ ("" = none)
    float       volume      = 1.0f; // 0..1
    bool        loop        = true; // loop (music/ambient) vs one-shot
    bool        playOnStart = true; // auto-play when Play begins
    bool        spatial     = false;// attenuate with distance vs global (music)
    float       radius      = 15.0f;// audible distance when spatial

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<AudioSourceComponent>(*this);
    }
    const char* typeId() const override { return "audio_source"; }
    const char* displayName() const override { return "Audio Source"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
    void onGizmo(GizmoDraw& g, const glm::vec3& c, const glm::quat&) const override {
        if (spatial) g.sphere(c, radius, {0.5f, 0.8f, 1.0f, 0.8f}); // audible range
    }
};

// --- Built-in component: Camera (a viewpoint you can switch to in Play) --------
// Attach to an entity to make it a camera: in Play the view can render from its
// position + orientation at this `fov`. `activeOnStart` makes it the initial view
// when Play begins (otherwise the player camera). Switch between cameras at
// runtime with CameraSwitcher. The gizmo draws a frustum so you can aim it.
class CameraComponent : public ComponentBase {
public:
    // What kind of eye this is. Static stands where it is put (a cutscene angle,
    // a fixed trackside shot); Follow trails the object it hangs on.
    enum Mode { Static = 0, Follow = 1 };

    int   mode          = Static;
    float fov           = 60.0f; // vertical field of view (degrees)
    bool  activeOnStart = false; // this camera is the view when Play starts

    // --- Follow ---------------------------------------------------------------
    // WHOSE camera this is, the hierarchy already answers: a follow camera trails
    // its PARENT. There is nothing to pick and nothing to get out of sync, and a
    // second craft carrying a camera child is a second view without anyone being
    // asked which craft belongs to whom.
    //
    // WHERE it sits is the entity's own local position: drag the camera to where
    // you want it and that IS the chase offset. It is applied in the parent's
    // HEADING frame only -- yaw, not bank or pitch -- so a craft rolling into a
    // corner turns under a camera that stays level instead of taking the horizon
    // with it. Which leaves exactly two things a position cannot express:
    float lookHeight = 1.4f;  // aim this far above the followed object's centre
    float stiffness  = 5.0f;  // how fast it catches up (1/s; higher = snappier)
    // ...and how much of the craft's own attitude the shot takes with it.
    //
    // 0 keeps the horizon level through a bank, which is the safer default and
    // what every follow camera did before this: the craft leans, the view does
    // not. 1 puts the camera fully in the craft's frame, so it banks and pitches
    // with it -- the ride rather than the view, which is exactly why some people
    // want it and why it is not the default.
    //
    // This only raises a FLOOR under the blend the system already runs: a craft
    // going round a vertical loop takes the camera with it either way, because
    // "behind, in yaw" stops meaning anything when the nose points at the sky.
    // So 0 is not "never roll", it is "roll only when the geometry demands it".
    //
    // It exists because the alternative people reached for was a STATIC camera
    // parented to the craft, which does bank with it -- and gives up the
    // smoothing, the aim-at-the-craft logic and the loop handling to get there.
    float rollWith   = 0.0f;  // 0 = level horizon .. 1 = fully attached

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<CameraComponent>(*this);
    }
    const char* typeId() const override { return "camera"; }
    const char* displayName() const override { return "Camera"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
    void onGizmo(GizmoDraw& g, const glm::vec3& c, const glm::quat& rot) const override {
        glm::vec3 fwd = rot * glm::vec3(0.0f, 0.0f, -1.0f);
        glm::vec3 up  = rot * glm::vec3(0.0f, 1.0f, 0.0f);
        glm::vec3 rt  = rot * glm::vec3(1.0f, 0.0f, 0.0f);
        // A follow camera does NOT look where it is turned -- it aims at the
        // object it hangs on, `lookHeight` above the centre (see CameraSystem).
        // Drawing the frustum along its own rotation would be a lie, and it is
        // the lie that hides the mistake this gizmo exists to catch: a camera
        // parked in FRONT of the craft, watching it come at the viewer. Aimed
        // properly, a camera on the wrong side is obvious at a glance.
        if (mode == Follow && g.hasParent) {
            const glm::vec3 aim = g.parentCenter + glm::vec3(0.0f, lookHeight, 0.0f);
            const glm::vec3 d   = aim - c;
            if (glm::length(d) > 1e-4f) {
                fwd = glm::normalize(d);
                const glm::vec3 wUp{0.0f, 1.0f, 0.0f};
                glm::vec3 r = glm::cross(fwd, wUp);
                // Straight down at its own parent: no sideways to be had, so
                // keep the authored basis rather than normalizing a zero vector.
                if (glm::length(r) > 1e-4f) {
                    rt = glm::normalize(r);
                    up = glm::cross(rt, fwd);
                }
            }
            // The tether: which object this camera belongs to, drawn as the line
            // it is. In a hierarchy of thirty entities that is the fastest way to
            // see whose eye you have selected.
            g.line(c, aim, {0.5f, 0.85f, 1.0f, 0.35f});
        }
        const float D = 2.0f;                                   // frustum depth
        const float h = D * glm::tan(glm::radians(fov) * 0.5f); // half height at D
        const float w = h * 1.5f;                               // ~16:9-ish
        const glm::vec3 ctr = c + fwd * D;
        const glm::vec4 col{0.5f, 0.85f, 1.0f, 0.95f};
        const glm::vec3 a = ctr + up * h + rt * w, b = ctr + up * h - rt * w;
        const glm::vec3 d = ctr - up * h - rt * w, e = ctr - up * h + rt * w;
        g.line(c, a, col); g.line(c, b, col); g.line(c, d, col); g.line(c, e, col);
        g.line(a, b, col); g.line(b, d, col); g.line(d, e, col); g.line(e, a, col);
    }
};

// --- Built-in component: CameraSwitcher (switch the active camera in Play) -----
// A zone that, when the player enters `radius`, makes `target` the active camera
// (a Camera entity's id, or -1 for the normal player view). Place several along a
// path for cinematic cuts, no code. The target is picked in the inspector from
// the scene's cameras; it serializes itself (an entity id, not a plain property).
class CameraSwitcherComponent : public ComponentBase {
public:
    int   target = -1;    // entity id of the Camera to switch to (-1 = player view)
    float radius = 2.5f;  // player within this range triggers the switch

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<CameraSwitcherComponent>(*this);
    }
    const char* typeId() const override { return "camera_switcher"; }
    const char* displayName() const override { return "Camera Switcher"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
    void save(nlohmann::json& j) const override;
    void load(const nlohmann::json& j) override;
    void onGizmo(GizmoDraw& g, const glm::vec3& c, const glm::quat&) const override {
        g.sphere(c, radius, {0.7f, 0.6f, 1.0f, 0.85f}); // switch zone
    }
};

// --- Built-in component: Animation (plays a skinned model's clip) -------------
// Attach to a Model entity whose glTF has a skeleton + animation clips. Plays
// `clip` at `speed`, CPU-skinning the mesh each frame (see the tick in main).
// `autostart` plays it from Play start; otherwise it waits for an AnimationTrigger
// to fire. `reverse` plays backward. `start`/`end` restrict playback to a
// sub-range of the clip in seconds (end <= start -> the whole clip). Loops or
// stops at the range end per `loop`. Works in the editor preview too.
class AnimationComponent : public ComponentBase {
public:
    int   clip      = 0;      // which animation clip of the model
    float speed     = 1.0f;   // playback rate multiplier
    bool  loop      = true;
    bool  autostart = true;   // play from Play start (else await AnimationTrigger)
    bool  reverse   = false;  // play backward
    float start     = 0.0f;   // sub-range start (seconds)
    float end       = 0.0f;   // sub-range end (seconds; <= start -> whole clip)

    float time    = 0.0f;     // runtime: current playback time (seconds)
    bool  playing = false;    // runtime: driven by autostart / triggers
    bool  started = false;    // runtime: initial autostart applied this Play
    bool  restart = false;    // runtime: a trigger requested a (re)start

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<AnimationComponent>(*this);
    }
    const char* typeId() const override { return "animation"; }
    const char* displayName() const override { return "Animation"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
    void save(nlohmann::json& j) const override;
    void load(const nlohmann::json& j) override;
};

// --- Built-in component: AnimationTrigger (start an Animation on entry) --------
// A zone that, when the player enters within `radius`, (re)starts the Animation
// on `target` (an entity id carrying an Animation component) from its range
// start. `once` fires a single time per Play. Serializes the target id itself.
class AnimationTriggerComponent : public ComponentBase {
public:
    int   target = -1;    // entity id whose Animation to (re)start
    float radius = 2.5f;
    bool  once   = true;

    bool insideLast = false; // runtime: player inside last frame (edge)
    bool fired      = false; // runtime: one-shot latch

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<AnimationTriggerComponent>(*this);
    }
    const char* typeId() const override { return "animation_trigger"; }
    const char* displayName() const override { return "Animation Trigger"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
    void save(nlohmann::json& j) const override;
    void load(const nlohmann::json& j) override;
    void onGizmo(GizmoDraw& g, const glm::vec3& c, const glm::quat&) const override {
        g.sphere(c, radius, {0.9f, 0.5f, 0.9f, 0.8f}); // trigger zone
    }
};

// --- Built-in component: Script (runs a Lua behaviour while playing) ----------
// The file field is serialized/undone via metadata; the inspector renders it
// with a bespoke file picker (it needs the project's script list).
class ScriptComponent : public ComponentBase {
public:
    std::string file; // .lua under the project's scripts/ ("" = none)
    // Per-instance overrides for the script's module-level globals (see
    // ScriptParam). One entry per exposed global; the inspector edits them and
    // ScriptSystem injects them into the script's environment at Play start.
    std::vector<ScriptParam> params;

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<ScriptComponent>(*this);
    }
    const char* typeId() const override { return "script"; }
    const char* displayName() const override { return "Script"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
    void save(nlohmann::json& j) const override; // file + params
    void load(const nlohmann::json& j) override;
};

// --- Built-in component: Light (a point light; attach to any entity to glow) --
class LightComponent : public ComponentBase {
public:
    // type 0 = point (omni), 1 = spot (cone along the entity's forward). A spot
    // shines down the entity's local +Z (the engine's forward), so aim it with the
    // entity's rotation -- parent it to a car with no local rotation and it becomes
    // a headlight pointing where the car drives, moving with it via the hierarchy.
    int       type        = 0;
    glm::vec3 color{1.0f, 0.95f, 0.8f};
    float     intensity   = 8.0f;
    float     range       = 12.0f;
    float     spotAngle   = 32.0f;   // spot: outer cone half-angle (degrees)
    float     spotBlend   = 0.2f;    // spot: 0 hard edge .. 1 fully soft
    bool      castShadows = false;   // point only (spots are unshadowed)
    float     shadowBias  = 0.003f;

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<LightComponent>(*this);
    }
    const char* typeId() const override { return "light"; }
    const char* displayName() const override { return "Light"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
    void onGizmo(GizmoDraw& g, const glm::vec3& c, const glm::quat& rot) const override {
        if (type == 1) {
            // Spot: draw the cone axis + a rim circle at the reach, so it can be aimed.
            const glm::vec3 dir = rot * glm::vec3(0.0f, 0.0f, 1.0f);
            const glm::vec3 tip = c + dir * range;
            const float     rad = range * std::tan(glm::radians(spotAngle));
            const glm::vec4 col{color.r, color.g, color.b, 0.6f};
            const glm::vec3 side = rot * glm::vec3(1.0f, 0.0f, 0.0f);
            g.line(c, tip, col);
            g.circle(tip, rad, dir, col);
            g.line(c, tip + side * rad, col);
            g.line(c, tip - side * rad, col);
        } else {
            g.sphere(c, range, {color.r, color.g, color.b, 0.5f}); // reach of the light
        }
    }
};

// --- Built-in component: Material (assigns a library material to a solid) -----
// Holds an asset GUID (not a plain property), so it serializes itself; the
// inspector renders a bespoke material picker. Absent -> the default material.
class MaterialComponent : public ComponentBase {
public:
    fitzel::AssetId material;

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<MaterialComponent>(*this);
    }
    const char* typeId() const override { return "material"; }
    const char* displayName() const override { return "Material"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> none; return none;
    }
    void save(nlohmann::json& j) const override;
    void load(const nlohmann::json& j) override;
};

// --- Built-in component: Model (an imported glTF/GLB on a Model entity) --------
// modelId is a runtime handle (resolved on load from the asset ref); modelPath
// is the source file; scale drives the pick box. Engine-managed (created by
// model import / drag-drop), so not in the Add menu. Serialized specially by
// ProjectIO (it needs the asset database to resolve the model).
class ModelComponent : public ComponentBase {
public:
    int         modelId = -1;
    std::string modelPath;
    float       scale = 1.0f;
    int         nodeIndex = -1; // structure-preserving import: which model node
                                // (-1 = the whole model)

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<ModelComponent>(*this);
    }
    const char* typeId() const override { return "model"; }
    const char* displayName() const override { return "Model"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> none; return none;
    }
    void save(nlohmann::json& j) const override; // scale + modelFile
    void load(const nlohmann::json& j) override; // scale (path/import via ProjectIO)
};

// --- Built-in component: Prefab link (marks an entity as a prefab instance) ---
// Attached to every entity produced by instantiating a .fprefab, so an instance
// remembers which prefab asset it came from. `source` is that prefab's GUID;
// `localId` is this entity's id *within* the prefab (0 = the instance root),
// which future work will use to diff per-instance overrides and re-apply prefab
// edits. Engine-managed (created by instantiation), so not in the Add menu.
class PrefabComponent : public ComponentBase {
public:
    fitzel::AssetId source;    // GUID of the .fprefab this instance came from
    int             localId = 0; // this entity's id inside the prefab (0 = root)

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<PrefabComponent>(*this);
    }
    const char* typeId() const override { return "prefab"; }
    const char* displayName() const override { return "Prefab Instance"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> none; return none;
    }
    bool isRoot() const { return localId == 0; }
    void save(nlohmann::json& j) const override;
    void load(const nlohmann::json& j) override;
};

// --- Built-in component: Physics (gives an entity a rigid-body collider) ------
// Presence = has a collider in Play. dynamic falls & collides; otherwise static.
class PhysicsComponent : public ComponentBase {
public:
    bool  dynamic = true;
    float mass    = 1.0f; // kg (dynamic only)

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<PhysicsComponent>(*this);
    }
    const char* typeId() const override { return "physics"; }
    const char* displayName() const override { return "Physics"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
};

// --- Built-in component: PlayerStart (where the FPS player spawns in Play) ----
// Attach to any entity to mark it as the player's start: on Play the walking
// character spawns at that entity (position + facing), moving at moveSpeed. The
// marker entity is hidden while playing.
class PlayerStartComponent : public ComponentBase {
public:
    float moveSpeed = 20.0f; // walk speed (m/s)

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<PlayerStartComponent>(*this);
    }
    const char* typeId() const override { return "player_start"; }
    const char* displayName() const override { return "Player Start"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
};

// --- Built-in component: Mesh (an editable shape, modelled in the editor) -----
// Turns an entity's geometry from "one of the four primitives" into a polygon
// mesh the author shapes in place -- extrude a face, scale one, push one in.
// Attached to a Box entity, it takes over what that entity draws, and everything
// else about the entity (its transform, its material, its pick box, its
// collider) goes on working as before: the box's half-extents are kept equal to
// the mesh's own bounds after every edit, so the AABB never lies about the shape
// inside it.
//
// The geometry lives here, on the entity, as plain vertex/face data -- which is
// what makes an edit undoable for free (Entity is a copied value and the undo
// stack snapshots it) and what puts the shape in the scene file rather than in
// some asset beside it. The GPU copy lives in an EditMeshCache, keyed by
// `revision`, because a GL mesh is move-only and an Entity must stay copyable.
class MeshComponent : public ComponentBase {
public:
    EditMesh      mesh     = EditMesh::box(glm::vec3(0.5f));
    std::uint64_t revision = editmesh::nextRevision(); // bump on every edit

    // Take a fresh stamp: every mutation ends with this, and it is what tells the
    // GPU cache its copy is out of date.
    void touch() { revision = editmesh::nextRevision(); }

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<MeshComponent>(*this);
    }
    const char* typeId() const override { return "mesh"; }
    const char* displayName() const override { return "Mesh"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> none; return none; // shaped in the viewport
    }
    // Geometry is not a property list: vertices and faces go out as two compact
    // blobs, the same way the terrain's sculpt cells do.
    void save(nlohmann::json& j) const override;
    void load(const nlohmann::json& j) override;
};

// --- Built-in component: Terrain (the scene's ground) -------------------------
// The terrain is an OBJECT in the scene, not a backdrop that is always there: a
// scene has ground because someone put this component in it, and deleting it
// leaves a scene with genuinely no ground (every height query answers 0 -- see
// fitzel::setTerrainPresent). Scenes authored before terrain was an entity are
// migrated on load from their stored terrain settings, so their world survives.
//
// One per scene: the field is global by construction (world-space noise streamed
// around the viewer), so this component says what the terrain IS, and the
// entity's own transform does not place it. A second one is ignored -- the first
// active one in the scene wins.
//
// The palette and texture layers (TerrainLook) are NOT here: they are the
// terrain's *material*, shared with the vegetation/water look, and stay in the
// scene's settings block where the rest of the world's look lives.
class TerrainComponent : public ComponentBase {
public:
    // The generator itself. Held as the engine's own settings struct rather than
    // copied field by field, so handing it to the streamer is one assignment and
    // a new generator knob is one entry in properties() away from being authored,
    // saved and undone.
    fitzel::TerrainSettings settings;

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<TerrainComponent>(*this);
    }
    const char* typeId() const override { return "terrain"; }
    const char* displayName() const override { return "Terrain"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
};

// --- Built-in component: Volumetric Fog --------------------------------------
// A body of mist filling the entity's BOX. Hang it on an Empty, scale that Empty
// with the gizmo, and the outline you see selected is the volume that gets
// marched -- which is the whole point of it being a component rather than a
// scene setting: fog you can put somewhere, several of, each with its own look.
//
// It carries no extents of its own on purpose. An entity already has a centre, a
// rotation and half-extents that the gizmo edits and the viewport draws; giving
// the component a second set would mean two boxes that can disagree, and the one
// you could see would be the wrong one.
class VolumetricFogComponent : public ComponentBase {
public:
    // Every knob lives in the shared medium struct, so the component and the
    // renderer cannot drift apart about what a field means. Seeded for a volume
    // that is METRES across rather than hundreds of them -- see
    // placedFogDefaults(), where the three that do not survive the change of
    // scale are spelled out.
    FogMedium fog = placedFogDefaults();

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<VolumetricFogComponent>(*this);
    }
    const char* typeId() const override { return "volumetric_fog"; }
    const char* displayName() const override { return "Volumetric Fog"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
    // No gizmo: the entity's own selection outline already IS this volume's box,
    // and a second wireframe on the same eight corners would only be something
    // to keep in sync.
};

// --- Built-in component: Sun (the singleton directional light's look) ---------
// Engine-managed (auto-attached to the Sun entity), so not in the Add menu.
class SunComponent : public ComponentBase {
public:
    glm::vec3 color{1.0f, 0.97f, 0.9f};
    float     intensity = 1.0f;

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<SunComponent>(*this);
    }
    const char* typeId() const override { return "sun"; }
    const char* displayName() const override { return "Sun"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
};
