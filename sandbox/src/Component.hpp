#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <fitzel/asset/AssetId.hpp>

#include "Property.hpp"

// A world-space debug-drawing sink for component gizmos. The viewport supplies a
// concrete implementation (projecting each shape onto the ImGui draw list);
// components emit only world-space shapes, so Component.cpp/.hpp stay free of any
// rendering dependency. Colours are RGBA in 0..1.
struct GizmoDraw {
    virtual ~GizmoDraw() = default;
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
    // `worldCenter` is the entity's derived world position. Default: nothing --
    // a component that wants a visual (a radius, a path) overrides this.
    virtual void onGizmo(GizmoDraw& g, const glm::vec3& worldCenter) const {}
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
    void onGizmo(GizmoDraw& g, const glm::vec3& c) const override {
        g.sphere(c, radius, {1.0f, 0.85f, 0.2f, 0.9f}); // gold pickup radius
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
    void onGizmo(GizmoDraw& g, const glm::vec3& c) const override {
        g.sphere(c, radius, {0.3f, 0.8f, 1.0f, 0.9f}); // cyan activation zone
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
    void onGizmo(GizmoDraw& g, const glm::vec3& c) const override {
        // Travel path from the start to the far end, with a ring at the target.
        const glm::vec4 col{0.4f, 1.0f, 0.55f, 0.9f};
        g.line(c, c + offset, col);
        g.sphere(c + offset, 0.3f, {col.r, col.g, col.b, 0.7f});
    }
};

// --- Built-in component: Spawner (emits entities on a timer in Play) ----------
// Data-authored, no scripting: while playing it spawns a solid of `spawnType`
// every `interval` seconds -- just above the spawner -- as a dynamic body with
// an initial upward `speed`, up to `maxCount` total. Reuses the same deferred
// spawn path as game.spawn. `timer`/`spawned` are transient and reset when Play
// stops. A fountain of balls, a wave of crates -- all without code.
class SpawnerComponent : public ComponentBase {
public:
    int   spawnType = 3;      // EntityType to emit (0 Box .. 3 Sphere)
    float interval  = 1.0f;   // seconds between spawns
    float speed     = 4.0f;   // initial upward velocity (m/s)
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
    void onGizmo(GizmoDraw& g, const glm::vec3& c) const override {
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
    void onGizmo(GizmoDraw& g, const glm::vec3& c) const override {
        g.sphere(c, radius, {0.5f, 0.7f, 1.0f, 0.8f});      // call zone
        g.line(c, c + offset, {0.6f, 0.85f, 1.0f, 1.0f});   // travel to the top
        g.sphere(c + offset, 0.25f, {0.6f, 0.85f, 1.0f, 0.9f});
    }
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
    void onGizmo(GizmoDraw& g, const glm::vec3& c) const override {
        g.sphere(c, radius, {1.0f, 0.4f, 0.3f, 0.8f}); // affect zone
        const float len = glm::length(direction);
        if (len > 1e-4f) {
            const glm::vec3 tip = c + (direction / len) * glm::min(radius, 3.0f);
            g.line(c, tip, {1.0f, 0.7f, 0.2f, 1.0f});   // push direction
            g.sphere(tip, 0.2f, {1.0f, 0.7f, 0.2f, 0.9f});
        }
    }
};

// --- Built-in component: Script (runs a Lua behaviour while playing) ----------
// The file field is serialized/undone via metadata; the inspector renders it
// with a bespoke file picker (it needs the project's script list).
class ScriptComponent : public ComponentBase {
public:
    std::string file; // .lua under the project's scripts/ ("" = none)

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<ScriptComponent>(*this);
    }
    const char* typeId() const override { return "script"; }
    const char* displayName() const override { return "Script"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
};

// --- Built-in component: Light (a point light; attach to any entity to glow) --
class LightComponent : public ComponentBase {
public:
    glm::vec3 color{1.0f, 0.95f, 0.8f};
    float     intensity   = 8.0f;
    float     range       = 12.0f;
    bool      castShadows = false;
    float     shadowBias  = 0.003f;

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<LightComponent>(*this);
    }
    const char* typeId() const override { return "light"; }
    const char* displayName() const override { return "Light"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
    void onGizmo(GizmoDraw& g, const glm::vec3& c) const override {
        g.sphere(c, range, {color.r, color.g, color.b, 0.5f}); // reach of the light
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
