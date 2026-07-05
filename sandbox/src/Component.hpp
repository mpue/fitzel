#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <fitzel/asset/AssetId.hpp>

#include "Property.hpp"

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
