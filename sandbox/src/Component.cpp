#include "Component.hpp"

#include <filesystem>
#include <sstream>

#include <nlohmann/json.hpp>

#include "PropertyMeta.hpp"

namespace components {

std::vector<TypeInfo>& registry() {
    static std::vector<TypeInfo> r;
    return r;
}

void registerType(TypeInfo info) { registry().push_back(std::move(info)); }

std::unique_ptr<ComponentBase> create(const std::string& typeId) {
    for (const TypeInfo& t : registry())
        if (t.typeId == typeId) return t.make();
    return nullptr;
}

} // namespace components

void ComponentBase::save(nlohmann::json& j) const { writeProps(j, props(), this); }
void ComponentBase::load(const nlohmann::json& j) { readProps(j, props(), this); }

// --- ScriptParam JSON --------------------------------------------------------
// A tagged value: {name, type, value}. `type` is stored as a short string so the
// scene file stays readable and forward-compatible with new kinds.
namespace {
const char* scriptParamTypeName(ScriptParam::Type t) {
    switch (t) {
        case ScriptParam::Type::Number: return "number";
        case ScriptParam::Type::Bool:   return "bool";
        case ScriptParam::Type::String: return "string";
        case ScriptParam::Type::Vec3:   return "vec3";
        case ScriptParam::Type::Color:  return "color";
    }
    return "number";
}
ScriptParam::Type scriptParamType(const std::string& s) {
    if (s == "bool")   return ScriptParam::Type::Bool;
    if (s == "string") return ScriptParam::Type::String;
    if (s == "vec3")   return ScriptParam::Type::Vec3;
    if (s == "color")  return ScriptParam::Type::Color;
    return ScriptParam::Type::Number;
}
} // namespace

void to_json(nlohmann::json& j, const ScriptParam& p) {
    j = nlohmann::json{{"name", p.name}, {"type", scriptParamTypeName(p.type)}};
    switch (p.type) {
        case ScriptParam::Type::Number: j["value"] = p.num; break;
        case ScriptParam::Type::Bool:   j["value"] = p.b;   break;
        case ScriptParam::Type::String: j["value"] = p.str; break;
        case ScriptParam::Type::Vec3:
        case ScriptParam::Type::Color:
            j["value"] = nlohmann::json::array({p.vec.x, p.vec.y, p.vec.z}); break;
    }
}
void from_json(const nlohmann::json& j, ScriptParam& p) {
    p.name = j.value("name", std::string());
    p.type = scriptParamType(j.value("type", std::string("number")));
    const nlohmann::json& v = j.contains("value") ? j.at("value") : nlohmann::json();
    switch (p.type) {
        case ScriptParam::Type::Number: if (v.is_number())  p.num = v.get<double>(); break;
        case ScriptParam::Type::Bool:   if (v.is_boolean()) p.b   = v.get<bool>();   break;
        case ScriptParam::Type::String: if (v.is_string())  p.str = v.get<std::string>(); break;
        case ScriptParam::Type::Vec3:
        case ScriptParam::Type::Color:
            if (v.is_array() && v.size() == 3) {
                p.vec = glm::vec3(v[0].get<float>(), v[1].get<float>(), v[2].get<float>());
            }
            break;
    }
}

void ScriptComponent::save(nlohmann::json& j) const {
    j["file"] = file;
    if (!params.empty()) j["params"] = params; // vector<ScriptParam> -> array
}
void ScriptComponent::load(const nlohmann::json& j) {
    file = j.value("file", std::string());
    params.clear();
    if (j.contains("params") && j["params"].is_array())
        params = j["params"].get<std::vector<ScriptParam>>();
}

void MaterialComponent::save(nlohmann::json& j) const {
    if (material.valid()) j["material"] = material.toString();
}
void MaterialComponent::load(const nlohmann::json& j) {
    if (j.contains("material") && j["material"].is_string())
        material = fitzel::AssetId::fromString(j["material"].get<std::string>());
}

void PrefabComponent::save(nlohmann::json& j) const {
    if (source.valid()) j["source"] = source.toString();
    j["localId"] = localId;
}
void PrefabComponent::load(const nlohmann::json& j) {
    if (j.contains("source") && j["source"].is_string())
        source = fitzel::AssetId::fromString(j["source"].get<std::string>());
    localId = j.value("localId", 0);
}

void ModelComponent::save(nlohmann::json& j) const {
    j["scale"]     = scale;
    j["modelFile"] = std::filesystem::path(modelPath).filename().string();
    if (nodeIndex >= 0) j["node"] = nodeIndex; // structure-preserving import
    // The asset ref ("model" GUID) is added by ProjectIO (it has the database).
}
void ModelComponent::load(const nlohmann::json& j) {
    scale     = j.value("scale", 1.0f);
    nodeIndex = j.value("node", -1);
    // modelPath / modelId are resolved by ProjectIO (needs the asset context).
}

bool componentsEqual(const ComponentList& a, const ComponentList& b) {
    if (a.items.size() != b.items.size()) return false;
    for (std::size_t i = 0; i < a.items.size(); ++i) {
        const ComponentBase* ca = a.items[i].get();
        const ComponentBase* cb = b.items[i].get();
        if (std::string(ca->typeId()) != cb->typeId()) return false;
        // Value-compare by serializing each component.
        nlohmann::json ja, jb;
        ca->save(ja);
        cb->save(jb);
        if (ja != jb) return false;
    }
    return true;
}

const std::vector<Property>& SpinComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;

        Property axis;
        axis.label = "Axis"; axis.key = "axis"; axis.kind = PropKind::Vec3;
        axis.speed = 0.05f;
        axis.field = [](void* o) -> void* { return &static_cast<SpinComponent*>(o)->axis; };
        p.push_back(std::move(axis));

        Property speed;
        speed.label = "Speed"; speed.key = "speed"; speed.kind = PropKind::Float;
        speed.slider = true; speed.min = -720.0f; speed.max = 720.0f; speed.fmt = "%.0f deg/s";
        speed.field = [](void* o) -> void* { return &static_cast<SpinComponent*>(o)->speed; };
        p.push_back(std::move(speed));

        return p;
    }();
    return props;
}

const std::vector<Property>& AnimatorComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;

        // The clip is a Text property so it SERIALIZES here like any other
        // field; the Inspector draws it as a picker over the scene's clips
        // instead of a text box (see InspectorPanel), the same way the Script
        // card turns a filename into a list of the project's .lua files. A name
        // rather than an index, so reordering the clips cannot silently re-point
        // every Animator in the scene at the wrong animation.
        Property clip;
        clip.label = "Clip"; clip.key = "clip"; clip.kind = PropKind::Text;
        clip.field = [](void* o) -> void* { return &static_cast<AnimatorComponent*>(o)->clip; };
        p.push_back(std::move(clip));

        Property onStart;
        onStart.label = "Play on start"; onStart.key = "playOnStart";
        onStart.kind = PropKind::Bool;
        onStart.field = [](void* o) -> void* {
            return &static_cast<AnimatorComponent*>(o)->playOnStart; };
        p.push_back(std::move(onStart));

        Property loop;
        loop.label = "Loop"; loop.key = "loop"; loop.kind = PropKind::Bool;
        loop.field = [](void* o) -> void* {
            return &static_cast<AnimatorComponent*>(o)->loop; };
        p.push_back(std::move(loop));

        return p;
    }();
    return props;
}

const std::vector<Property>& AnimGraphComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        // Text so it serializes; the Inspector draws it as a picker over the
        // scene's graphs, the same as the Animator's clip.
        Property graph;
        graph.label = "Graph"; graph.key = "graph"; graph.kind = PropKind::Text;
        graph.field = [](void* o) -> void* {
            return &static_cast<AnimGraphComponent*>(o)->graph; };
        p.push_back(std::move(graph));
        return p;
    }();
    return props;
}

const std::vector<Property>& CollectibleComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        Property points;
        points.label = "Points"; points.key = "points"; points.kind = PropKind::Float;
        points.min = 0.0f; points.max = 1000.0f; points.speed = 1.0f; points.fmt = "%.0f";
        points.field = [](void* o) -> void* { return &static_cast<CollectibleComponent*>(o)->points; };
        p.push_back(std::move(points));
        Property radius;
        radius.label = "Pickup radius"; radius.key = "radius"; radius.kind = PropKind::Float;
        radius.slider = true; radius.min = 0.2f; radius.max = 8.0f; radius.fmt = "%.1f m";
        radius.field = [](void* o) -> void* { return &static_cast<CollectibleComponent*>(o)->radius; };
        p.push_back(std::move(radius));
        Property sound;
        sound.label = "Sound"; sound.key = "sound"; sound.kind = PropKind::Text;
        sound.field = [](void* o) -> void* { return &static_cast<CollectibleComponent*>(o)->sound; };
        p.push_back(std::move(sound));
        return p;
    }();
    return props;
}

const std::vector<Property>& MissilePickupComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        Property count;
        count.label = "Rounds"; count.key = "count"; count.kind = PropKind::Int;
        count.min = 1.0f; count.max = 12.0f; count.speed = 1.0f;
        count.field = [](void* o) -> void* { return &static_cast<MissilePickupComponent*>(o)->count; };
        p.push_back(std::move(count));
        Property radius;
        radius.label = "Pickup radius"; radius.key = "radius"; radius.kind = PropKind::Float;
        radius.slider = true; radius.min = 0.5f; radius.max = 15.0f; radius.fmt = "%.1f m";
        radius.field = [](void* o) -> void* { return &static_cast<MissilePickupComponent*>(o)->radius; };
        p.push_back(std::move(radius));
        Property respawn;
        respawn.label = "Respawn"; respawn.key = "respawn"; respawn.kind = PropKind::Float;
        respawn.slider = true; respawn.min = 0.0f; respawn.max = 90.0f; respawn.fmt = "%.0f s";
        respawn.field = [](void* o) -> void* { return &static_cast<MissilePickupComponent*>(o)->respawn; };
        p.push_back(std::move(respawn));
        Property sound;
        sound.label = "Sound"; sound.key = "sound"; sound.kind = PropKind::Text;
        sound.field = [](void* o) -> void* { return &static_cast<MissilePickupComponent*>(o)->sound; };
        p.push_back(std::move(sound));
        return p;
    }();
    return props;
}

const std::vector<Property>& EnergyPickupComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        Property amount;
        // Read against a 100-unit hull: 25 is a quarter of it back. The cap is
        // a full hull's worth, so one pickup can be authored as a full repair.
        amount.label = "Energy"; amount.key = "amount"; amount.kind = PropKind::Float;
        amount.slider = true; amount.min = 1.0f; amount.max = 100.0f; amount.fmt = "%.0f";
        amount.field = [](void* o) -> void* { return &static_cast<EnergyPickupComponent*>(o)->amount; };
        p.push_back(std::move(amount));
        Property radius;
        radius.label = "Pickup radius"; radius.key = "radius"; radius.kind = PropKind::Float;
        radius.slider = true; radius.min = 0.5f; radius.max = 15.0f; radius.fmt = "%.1f m";
        radius.field = [](void* o) -> void* { return &static_cast<EnergyPickupComponent*>(o)->radius; };
        p.push_back(std::move(radius));
        Property respawn;
        respawn.label = "Respawn"; respawn.key = "respawn"; respawn.kind = PropKind::Float;
        respawn.slider = true; respawn.min = 0.0f; respawn.max = 90.0f; respawn.fmt = "%.0f s";
        respawn.field = [](void* o) -> void* { return &static_cast<EnergyPickupComponent*>(o)->respawn; };
        p.push_back(std::move(respawn));
        Property sound;
        sound.label = "Sound"; sound.key = "sound"; sound.kind = PropKind::Text;
        sound.field = [](void* o) -> void* { return &static_cast<EnergyPickupComponent*>(o)->sound; };
        p.push_back(std::move(sound));
        return p;
    }();
    return props;
}

const std::vector<Property>& TriggerComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        Property radius;
        radius.label = "Radius"; radius.key = "radius"; radius.kind = PropKind::Float;
        radius.slider = true; radius.min = 0.5f; radius.max = 20.0f; radius.fmt = "%.1f m";
        radius.field = [](void* o) -> void* { return &static_cast<TriggerComponent*>(o)->radius; };
        p.push_back(std::move(radius));
        Property once;
        once.label = "Fire once"; once.key = "once"; once.kind = PropKind::Bool;
        once.field = [](void* o) -> void* { return &static_cast<TriggerComponent*>(o)->once; };
        p.push_back(std::move(once));
        Property message;
        message.label = "HUD message"; message.key = "message"; message.kind = PropKind::Text;
        message.field = [](void* o) -> void* { return &static_cast<TriggerComponent*>(o)->message; };
        p.push_back(std::move(message));
        Property sound;
        sound.label = "Sound"; sound.key = "sound"; sound.kind = PropKind::Text;
        sound.field = [](void* o) -> void* { return &static_cast<TriggerComponent*>(o)->sound; };
        p.push_back(std::move(sound));
        return p;
    }();
    return props;
}

const std::vector<Property>& SceneTriggerComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        Property radius;
        radius.label = "Radius"; radius.key = "radius"; radius.kind = PropKind::Float;
        radius.slider = true; radius.min = 0.5f; radius.max = 20.0f; radius.fmt = "%.1f m";
        radius.field = [](void* o) -> void* { return &static_cast<SceneTriggerComponent*>(o)->radius; };
        p.push_back(std::move(radius));
        // Serialized like the other Text fields; the inspector draws it as a scene
        // picker rather than a raw text box (see main's component inspector).
        Property scene;
        scene.label = "Scene"; scene.key = "scene"; scene.kind = PropKind::Text;
        scene.field = [](void* o) -> void* { return &static_cast<SceneTriggerComponent*>(o)->scene; };
        p.push_back(std::move(scene));
        Property once;
        once.label = "Fire once"; once.key = "once"; once.kind = PropKind::Bool;
        once.field = [](void* o) -> void* { return &static_cast<SceneTriggerComponent*>(o)->once; };
        p.push_back(std::move(once));
        return p;
    }();
    return props;
}

const std::vector<Property>& MoverComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        Property offset;
        offset.label = "Offset"; offset.key = "offset"; offset.kind = PropKind::Vec3;
        offset.speed = 0.05f;
        offset.field = [](void* o) -> void* { return &static_cast<MoverComponent*>(o)->offset; };
        p.push_back(std::move(offset));
        Property duration;
        duration.label = "Duration"; duration.key = "duration"; duration.kind = PropKind::Float;
        duration.slider = true; duration.min = 0.2f; duration.max = 20.0f; duration.fmt = "%.1f s";
        duration.field = [](void* o) -> void* { return &static_cast<MoverComponent*>(o)->duration; };
        p.push_back(std::move(duration));
        return p;
    }();
    return props;
}

const std::vector<Property>& SpawnerComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        Property prefab;
        prefab.label = "Prefab"; prefab.key = "prefab"; prefab.kind = PropKind::Text;
        prefab.field = [](void* o) -> void* { return &static_cast<SpawnerComponent*>(o)->prefab; };
        p.push_back(std::move(prefab));
        // Only relevant while no prefab is picked -- a prefab brings its own shape.
        Property type;
        type.label = "Spawns"; type.key = "spawnType"; type.kind = PropKind::EnumInt;
        type.enumLabels = {"Box", "Ramp", "Cylinder", "Sphere"};
        type.visible = [](const void* o) {
            return static_cast<const SpawnerComponent*>(o)->prefab.empty();
        };
        type.field = [](void* o) -> void* { return &static_cast<SpawnerComponent*>(o)->spawnType; };
        p.push_back(std::move(type));
        Property interval;
        interval.label = "Interval"; interval.key = "interval"; interval.kind = PropKind::Float;
        interval.slider = true; interval.min = 0.1f; interval.max = 10.0f; interval.fmt = "%.2f s";
        interval.field = [](void* o) -> void* { return &static_cast<SpawnerComponent*>(o)->interval; };
        p.push_back(std::move(interval));
        Property speed;
        speed.label = "Launch speed"; speed.key = "speed"; speed.kind = PropKind::Float;
        speed.slider = true; speed.min = 0.0f; speed.max = 30.0f; speed.fmt = "%.1f m/s";
        speed.field = [](void* o) -> void* { return &static_cast<SpawnerComponent*>(o)->speed; };
        p.push_back(std::move(speed));
        Property spread;
        spread.label = "Spread"; spread.key = "spread"; spread.kind = PropKind::Float;
        spread.slider = true; spread.min = 0.0f; spread.max = 180.0f; spread.fmt = "%.0f deg";
        spread.field = [](void* o) -> void* { return &static_cast<SpawnerComponent*>(o)->spread; };
        p.push_back(std::move(spread));
        Property maxCount;
        maxCount.label = "Max count"; maxCount.key = "maxCount"; maxCount.kind = PropKind::Float;
        maxCount.min = 1.0f; maxCount.max = 200.0f; maxCount.speed = 1.0f; maxCount.fmt = "%.0f";
        maxCount.field = [](void* o) -> void* { return &static_cast<SpawnerComponent*>(o)->maxCount; };
        p.push_back(std::move(maxCount));
        return p;
    }();
    return props;
}

const std::vector<Property>& LiftComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        Property offset;
        offset.label = "Offset"; offset.key = "offset"; offset.kind = PropKind::Vec3;
        offset.speed = 0.05f;
        offset.field = [](void* o) -> void* { return &static_cast<LiftComponent*>(o)->offset; };
        p.push_back(std::move(offset));
        Property speed;
        speed.label = "Speed"; speed.key = "speed"; speed.kind = PropKind::Float;
        speed.slider = true; speed.min = 0.2f; speed.max = 10.0f; speed.fmt = "%.1f m/s";
        speed.field = [](void* o) -> void* { return &static_cast<LiftComponent*>(o)->speed; };
        p.push_back(std::move(speed));
        Property radius;
        radius.label = "Call radius"; radius.key = "radius"; radius.kind = PropKind::Float;
        radius.slider = true; radius.min = 0.5f; radius.max = 12.0f; radius.fmt = "%.1f m";
        radius.field = [](void* o) -> void* { return &static_cast<LiftComponent*>(o)->radius; };
        p.push_back(std::move(radius));
        return p;
    }();
    return props;
}

const std::vector<Property>& TriggerSoundComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        Property radius;
        radius.label = "Radius"; radius.key = "radius"; radius.kind = PropKind::Float;
        radius.slider = true; radius.min = 0.5f; radius.max = 40.0f; radius.fmt = "%.1f m";
        radius.field = [](void* o) -> void* { return &static_cast<TriggerSoundComponent*>(o)->radius; };
        p.push_back(std::move(radius));
        Property vol;
        vol.label = "Volume"; vol.key = "volume"; vol.kind = PropKind::Float;
        vol.slider = true; vol.min = 0.0f; vol.max = 1.0f; vol.fmt = "%.2f";
        vol.field = [](void* o) -> void* { return &static_cast<TriggerSoundComponent*>(o)->volume; };
        p.push_back(std::move(vol));
        Property oneShot;
        oneShot.label = "One-shot (full play)"; oneShot.key = "oneShot"; oneShot.kind = PropKind::Bool;
        oneShot.field = [](void* o) -> void* { return &static_cast<TriggerSoundComponent*>(o)->oneShot; };
        p.push_back(std::move(oneShot));
        Property loop;
        loop.label = "Loop (zone)"; loop.key = "loop"; loop.kind = PropKind::Bool;
        loop.visible = [](const void* o) { return !static_cast<const TriggerSoundComponent*>(o)->oneShot; };
        loop.field = [](void* o) -> void* { return &static_cast<TriggerSoundComponent*>(o)->loop; };
        p.push_back(std::move(loop));
        Property once;
        once.label = "Once"; once.key = "once"; once.kind = PropKind::Bool;
        once.visible = [](const void* o) {
            const auto* t = static_cast<const TriggerSoundComponent*>(o);
            return !t->loop && !t->oneShot;
        };
        once.field = [](void* o) -> void* { return &static_cast<TriggerSoundComponent*>(o)->once; };
        p.push_back(std::move(once));
        return p;
    }();
    return props;
}

const std::vector<Property>& AudioSourceComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        Property sound;
        sound.label = "Sound"; sound.key = "sound"; sound.kind = PropKind::Text;
        sound.field = [](void* o) -> void* { return &static_cast<AudioSourceComponent*>(o)->sound; };
        p.push_back(std::move(sound));
        Property vol;
        vol.label = "Volume"; vol.key = "volume"; vol.kind = PropKind::Float;
        vol.slider = true; vol.min = 0.0f; vol.max = 1.0f; vol.fmt = "%.2f";
        vol.field = [](void* o) -> void* { return &static_cast<AudioSourceComponent*>(o)->volume; };
        p.push_back(std::move(vol));
        Property loop;
        loop.label = "Loop"; loop.key = "loop"; loop.kind = PropKind::Bool;
        loop.field = [](void* o) -> void* { return &static_cast<AudioSourceComponent*>(o)->loop; };
        p.push_back(std::move(loop));
        Property pos;
        pos.label = "Play on start"; pos.key = "playOnStart"; pos.kind = PropKind::Bool;
        pos.field = [](void* o) -> void* { return &static_cast<AudioSourceComponent*>(o)->playOnStart; };
        p.push_back(std::move(pos));
        Property sp;
        sp.label = "Spatial (3D)"; sp.key = "spatial"; sp.kind = PropKind::Bool;
        sp.field = [](void* o) -> void* { return &static_cast<AudioSourceComponent*>(o)->spatial; };
        p.push_back(std::move(sp));
        Property rad;
        rad.label = "Radius"; rad.key = "radius"; rad.kind = PropKind::Float;
        rad.slider = true; rad.min = 0.5f; rad.max = 80.0f; rad.fmt = "%.1f m";
        rad.visible = [](const void* o) { return static_cast<const AudioSourceComponent*>(o)->spatial; };
        rad.field = [](void* o) -> void* { return &static_cast<AudioSourceComponent*>(o)->radius; };
        p.push_back(std::move(rad));
        return p;
    }();
    return props;
}

const std::vector<Property>& CameraComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        Property mode;
        mode.label = "Mode"; mode.key = "mode"; mode.kind = PropKind::EnumInt;
        mode.enumLabels = {"Static", "Follow parent", "Multishot"};
        mode.field = [](void* o) -> void* { return &static_cast<CameraComponent*>(o)->mode; };
        p.push_back(std::move(mode));
        // Only a follow camera has anything to catch up with; on a static one
        // these two would be knobs that do nothing.
        const auto follows = [](const void* o) {
            return static_cast<const CameraComponent*>(o)->mode == CameraComponent::Follow;
        };
        Property look;
        look.label = "Look height"; look.key = "lookHeight"; look.kind = PropKind::Float;
        look.slider = true; look.min = -5.0f; look.max = 10.0f; look.fmt = "%.2f m";
        look.visible = follows;
        look.field = [](void* o) -> void* { return &static_cast<CameraComponent*>(o)->lookHeight; };
        p.push_back(std::move(look));
        Property stiff;
        stiff.label = "Stiffness"; stiff.key = "stiffness"; stiff.kind = PropKind::Float;
        stiff.slider = true; stiff.min = 0.5f; stiff.max = 30.0f; stiff.fmt = "%.1f /s";
        stiff.visible = follows;
        stiff.field = [](void* o) -> void* { return &static_cast<CameraComponent*>(o)->stiffness; };
        p.push_back(std::move(stiff));
        Property rollw;
        rollw.label = "Roll with craft"; rollw.key = "rollWith";
        rollw.kind = PropKind::Float;
        rollw.slider = true; rollw.min = 0.0f; rollw.max = 1.0f; rollw.fmt = "%.2f";
        rollw.visible = follows;
        rollw.field = [](void* o) -> void* {
            return &static_cast<CameraComponent*>(o)->rollWith;
        };
        p.push_back(std::move(rollw));
        Property fov;
        fov.label = "FOV"; fov.key = "fov"; fov.kind = PropKind::Float;
        fov.slider = true; fov.min = 20.0f; fov.max = 120.0f; fov.fmt = "%.0f deg";
        fov.field = [](void* o) -> void* { return &static_cast<CameraComponent*>(o)->fov; };
        p.push_back(std::move(fov));
        Property act;
        act.label = "Active on start"; act.key = "activeOnStart"; act.kind = PropKind::Bool;
        act.field = [](void* o) -> void* { return &static_cast<CameraComponent*>(o)->activeOnStart; };
        p.push_back(std::move(act));

        // --- Multishot ---------------------------------------------------------
        // The shot list itself (which moves are in the rotation) and the subject
        // are drawn by the inspector, not from here: a bool per shot as thirteen
        // separate rows would bury the six numbers that actually change the look.
        // These are those numbers, and they are hidden on the other two modes,
        // where they would be knobs that do nothing.
        const auto shooting = [](const void* o) {
            return static_cast<const CameraComponent*>(o)->mode == CameraComponent::Multishot;
        };
        const auto addShot = [&p, &shooting](const char* label, const char* key,
                                             float multishot::Settings::*m,
                                             float lo, float hi, const char* fmt) {
            Property f;
            f.label = label; f.key = key; f.kind = PropKind::Float;
            f.slider = true; f.min = lo; f.max = hi; f.fmt = fmt;
            f.visible = shooting;
            f.field = [m](void* o) -> void* {
                return &(static_cast<CameraComponent*>(o)->shots.*m);
            };
            p.push_back(std::move(f));
        };
        // Timing: how long a shot is held, and how much that length wanders.
        // Variance is not decoration -- cuts landing on an exact metronome are
        // the other clear tell of a generated sequence.
        addShot("Shot length",  "shotDuration", &multishot::Settings::duration,  1.0f, 15.0f, "%.1f s");
        addShot("Length spread","shotVariance", &multishot::Settings::variance,  0.0f, 0.9f,  "%.2f");
        // Framing. Multipliers on what the subject's own size already decides,
        // which is why they read 1.0 rather than in metres (see MultiShot.hpp).
        addShot("Framing",      "framing",      &multishot::Settings::distance,  0.3f, 3.0f,  "%.2fx");
        addShot("Eye height",   "eyeHeight",    &multishot::Settings::height,    0.2f, 2.5f,  "%.2fx");
        addShot("Move pace",    "movePace",     &multishot::Settings::speed,     0.2f, 2.5f,  "%.2fx");
        addShot("Aim lead",     "aimLead",      &multishot::Settings::lead,      0.0f, 1.5f,  "%.2f s");
        // Look.
        addShot("Dutch angle",  "dutch",        &multishot::Settings::dutch,     0.0f, 25.0f, "%.0f deg");
        addShot("Handheld",     "handheld",     &multishot::Settings::shake,     0.0f, 1.0f,  "%.2f");
        // A cut is the default because it is what an edit is made of; the fade is
        // here for the cases where it is not (a slow product turntable).
        addShot("Cross fade",   "crossFade",    &multishot::Settings::blend,     0.0f, 2.0f,  "%.2f s");
        addShot("Ground clear", "clearance",    &multishot::Settings::clearance, 0.0f, 5.0f,  "%.2f m");
        Property story;
        story.label = "Storyboard order"; story.key = "storyboard"; story.kind = PropKind::Bool;
        story.visible = shooting;
        story.field = [](void* o) -> void* {
            return &static_cast<CameraComponent*>(o)->shots.sequential;
        };
        p.push_back(std::move(story));
        Property seed;
        seed.label = "Seed"; seed.key = "shotSeed"; seed.kind = PropKind::Int;
        seed.slider = true; seed.min = 0.0f; seed.max = 999.0f;
        seed.visible = shooting;
        seed.field = [](void* o) -> void* {
            return &static_cast<CameraComponent*>(o)->shots.seed;
        };
        p.push_back(std::move(seed));
        return p;
    }();
    return props;
}

// The scalars ride the property metadata above; the subject and the shot list do
// not. The list is keyed by NAME rather than by index (see multishot::shotKey),
// so a scene saved today still says "no fly-bys" after a shot is inserted in the
// middle of the enum -- an index would have quietly moved that veto onto whatever
// shot landed in the slot.
void CameraComponent::save(nlohmann::json& j) const {
    writeProps(j, props(), this);
    j["shotTarget"] = shotTarget;
    if (mode == Multishot) {
        nlohmann::json list = nlohmann::json::object();
        for (int i = 0; i < multishot::ShotCount; ++i)
            list[multishot::shotKey(i)] = shots.use[i];
        j["shotList"] = std::move(list);
    }
}
void CameraComponent::load(const nlohmann::json& j) {
    readProps(j, props(), this);
    shotTarget = j.value("shotTarget", -1);
    // A camera saved before the shot list existed gets the default (everything
    // on), which is also what a freshly added component has.
    if (j.contains("shotList") && j["shotList"].is_object())
        for (int i = 0; i < multishot::ShotCount; ++i)
            shots.use[i] = j["shotList"].value(multishot::shotKey(i), true);
}

const std::vector<Property>& CameraSwitcherComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        Property radius;
        radius.label = "Radius"; radius.key = "radius"; radius.kind = PropKind::Float;
        radius.slider = true; radius.min = 0.5f; radius.max = 20.0f; radius.fmt = "%.1f m";
        radius.field = [](void* o) -> void* { return &static_cast<CameraSwitcherComponent*>(o)->radius; };
        p.push_back(std::move(radius));
        return p;
    }();
    return props;
}

// Persists the radius (a property) plus the target camera's entity id (a raw
// reference, not a property, so it needs an explicit save/load like Material).
void CameraSwitcherComponent::save(nlohmann::json& j) const {
    writeProps(j, props(), this);
    j["target"] = target;
}
void CameraSwitcherComponent::load(const nlohmann::json& j) {
    readProps(j, props(), this);
    target = j.value("target", -1);
}

const std::vector<Property>& DoorComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        Property slide;
        slide.label = "Slide"; slide.key = "slide"; slide.kind = PropKind::Bool;
        slide.field = [](void* o) -> void* { return &static_cast<DoorComponent*>(o)->slide; };
        p.push_back(std::move(slide));
        Property angle;
        angle.label = "Swing angle"; angle.key = "angle"; angle.kind = PropKind::Float;
        angle.slider = true; angle.min = -180.0f; angle.max = 180.0f; angle.fmt = "%.0f deg";
        angle.visible = [](const void* o) { return !static_cast<const DoorComponent*>(o)->slide; };
        angle.field = [](void* o) -> void* { return &static_cast<DoorComponent*>(o)->angle; };
        p.push_back(std::move(angle));
        Property offset;
        offset.label = "Slide offset"; offset.key = "offset"; offset.kind = PropKind::Vec3;
        offset.speed = 0.05f;
        offset.visible = [](const void* o) { return static_cast<const DoorComponent*>(o)->slide; };
        offset.field = [](void* o) -> void* { return &static_cast<DoorComponent*>(o)->offset; };
        p.push_back(std::move(offset));
        Property speed;
        speed.label = "Speed"; speed.key = "speed"; speed.kind = PropKind::Float;
        speed.slider = true; speed.min = 0.2f; speed.max = 10.0f; speed.fmt = "%.1f /s";
        speed.field = [](void* o) -> void* { return &static_cast<DoorComponent*>(o)->speed; };
        p.push_back(std::move(speed));
        Property startOpen;
        startOpen.label = "Start open"; startOpen.key = "startOpen"; startOpen.kind = PropKind::Bool;
        startOpen.field = [](void* o) -> void* { return &static_cast<DoorComponent*>(o)->startOpen; };
        p.push_back(std::move(startOpen));
        return p;
    }();
    return props;
}

const std::vector<Property>& DoorOpenerComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        Property radius;
        radius.label = "Radius"; radius.key = "radius"; radius.kind = PropKind::Float;
        radius.slider = true; radius.min = 0.5f; radius.max = 20.0f; radius.fmt = "%.1f m";
        radius.field = [](void* o) -> void* { return &static_cast<DoorOpenerComponent*>(o)->radius; };
        p.push_back(std::move(radius));
        Property stay;
        stay.label = "Stay open"; stay.key = "stayOpen"; stay.kind = PropKind::Bool;
        stay.field = [](void* o) -> void* { return &static_cast<DoorOpenerComponent*>(o)->stayOpen; };
        p.push_back(std::move(stay));
        return p;
    }();
    return props;
}
void DoorOpenerComponent::save(nlohmann::json& j) const {
    writeProps(j, props(), this);
    j["target"] = target;
}
void DoorOpenerComponent::load(const nlohmann::json& j) {
    readProps(j, props(), this);
    target = j.value("target", -1);
}

const std::vector<Property>& VehicleComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        auto addFloat = [&](const char* label, const char* key, float VehicleComponent::* m,
                            bool slider, float lo, float hi, const char* fmt) {
            Property f; f.label = label; f.key = key; f.kind = PropKind::Float;
            f.slider = slider; f.min = lo; f.max = hi; f.speed = 0.05f; f.fmt = fmt;
            f.field = [m](void* o) -> void* { return &(static_cast<VehicleComponent*>(o)->*m); };
            p.push_back(std::move(f));
        };
        addFloat("Mass",          "mass",        &VehicleComponent::mass,        true, 100.0f, 8000.0f, "%.0f kg");
        addFloat("Engine torque", "torque",      &VehicleComponent::engineTorque,true, 200.0f, 12000.0f, "%.0f Nm");
        addFloat("Max steer",     "maxSteer",    &VehicleComponent::maxSteerDeg, true, 5.0f,  60.0f,  "%.0f deg");
        addFloat("Steer speed",   "steerSpeed",  &VehicleComponent::steerSpeed,  true, 1.0f,  20.0f,  "%.1f");
        addFloat("Wheel radius",  "wheelRadius", &VehicleComponent::wheelRadius, false, 0.05f, 3.0f,  "%.2f m");
        addFloat("Wheel width",   "wheelWidth",  &VehicleComponent::wheelWidth,  false, 0.05f, 2.0f,  "%.2f m");
        Property ch;
        ch.label = "Chassis half"; ch.key = "chassisHalf"; ch.kind = PropKind::Vec3;
        ch.speed = 0.05f;
        ch.field = [](void* o) -> void* { return &static_cast<VehicleComponent*>(o)->chassisHalf; };
        p.push_back(std::move(ch));
        // Reads next to "Chassis half" because the pair is size + position: how
        // big the box is, and how high it rides on its springs.
        addFloat("Chassis Y",     "chassisY",  &VehicleComponent::chassisY,  false, 0.05f, 2.0f, "%.2f m");
        addFloat("Half track",    "halfTrack", &VehicleComponent::halfTrack, false, 0.1f, 5.0f,   "%.2f m");
        addFloat("Front axle Z",  "frontZ",    &VehicleComponent::frontZ,    false, -10.0f, 10.0f, "%.2f m");
        addFloat("Rear axle Z",   "rearZ",     &VehicleComponent::rearZ,     false, -10.0f, 10.0f, "%.2f m");
        Property fwd;
        fwd.label = "Model nose"; fwd.key = "forward"; fwd.kind = PropKind::EnumInt;
        fwd.enumLabels = {"+Z", "-Z"};
        fwd.field = [](void* o) -> void* { return &static_cast<VehicleComponent*>(o)->forward; };
        p.push_back(std::move(fwd));
        addFloat("Wheel height Y", "wheelY",   &VehicleComponent::wheelY,    false, -5.0f, 5.0f,   "%.2f m");
        // Handling tuning (grouped under a "Handling" header in the inspector,
        // starting at "comLower" -- see vehicleui::inspector).
        addFloat("COM lower",     "comLower",   &VehicleComponent::comLower,       true, 0.0f, 1.0f,   "%.2f");
        addFloat("Suspension Hz", "suspFreq",   &VehicleComponent::suspensionFreq, true, 0.5f, 5.0f,   "%.2f Hz");
        addFloat("Suspension damp","suspDamp",  &VehicleComponent::suspensionDamp, true, 0.0f, 1.0f,   "%.2f");
        addFloat("Anti-roll",     "antiRoll",   &VehicleComponent::antiRoll,       true, 0.0f, 5000.0f,"%.0f");
        addFloat("Grip",          "grip",       &VehicleComponent::grip,           true, 0.3f, 5.0f,   "%.2f");
        addFloat("Upright assist","upright",    &VehicleComponent::uprightAssist,  true, 0.0f, 30.0f,  "%.1f");
        Property drv;
        drv.label = "Drive"; drv.key = "drive"; drv.kind = PropKind::EnumInt;
        drv.enumLabels = {"RWD", "FWD", "AWD"};
        drv.field = [](void* o) -> void* { return &static_cast<VehicleComponent*>(o)->drive; };
        p.push_back(std::move(drv));
        // Boat mode (grouped under a "Boat" header -- see vehicleui::inspector).
        addFloat("Boat float",   "boatFloat",   &VehicleComponent::boatFloat,   true, 0.15f, 0.90f, "%.2f");
        addFloat("Boat thrust",  "boatThrust",  &VehicleComponent::boatThrust,  true, 2.0f,  40.0f, "%.0f");
        addFloat("Spray amount", "sprayAmount", &VehicleComponent::sprayAmount, true, 0.0f,  3.0f,  "%.2f");
        addFloat("Spray height", "sprayHeight", &VehicleComponent::sprayHeight, true, 0.0f,  3.0f,  "%.2f");
        addFloat("Spray size",   "spraySize",   &VehicleComponent::spraySize,   true, 0.2f,  5.0f,  "%.2f");
        // Follow-camera tuning (kept last so the inspector can group them under
        // a "Follow camera" header -- see vehicleui::inspector).
        return p;
    }();
    return props;
}
// Persists the properties above plus the wheel child entity ids (raw entity
// references, like the trigger targets -- ids are stable across save/load).
void VehicleComponent::save(nlohmann::json& j) const {
    writeProps(j, props(), this);
    j["wheels"] = {wheelId[0], wheelId[1], wheelId[2], wheelId[3]};
}
void VehicleComponent::load(const nlohmann::json& j) {
    readProps(j, props(), this);
    if (j.contains("wheels") && j["wheels"].is_array())
        for (std::size_t i = 0; i < 4 && i < j["wheels"].size(); ++i)
            wheelId[i] = j["wheels"][i].is_number_integer() ? j["wheels"][i].get<int>() : -1;
}

const std::vector<Property>& GliderComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        auto addFloat = [&](const char* label, const char* key, float GliderComponent::* m,
                            float lo, float hi, const char* fmt) {
            Property f; f.label = label; f.key = key; f.kind = PropKind::Float;
            f.slider = true; f.min = lo; f.max = hi; f.speed = 0.05f; f.fmt = fmt;
            f.field = [m](void* o) -> void* { return &(static_cast<GliderComponent*>(o)->*m); };
            p.push_back(std::move(f));
        };
        // Flight
        addFloat("Thrust",       "thrust",     &GliderComponent::thrust,     2.0f, 120.0f, "%.0f m/s2");
        addFloat("Max speed",    "maxSpeed",   &GliderComponent::maxSpeed,   5.0f, 200.0f, "%.0f m/s");
        addFloat("Brake",        "brakeForce", &GliderComponent::brakeForce, 2.0f, 120.0f, "%.0f m/s2");
        addFloat("Turn rate",    "turnRate",   &GliderComponent::turnRate,   10.0f, 300.0f,"%.0f deg/s");
        addFloat("Steer response", "steerResponse", &GliderComponent::steerResponse,
                 0.5f, 20.0f, "%.1f /s");
        Property inv;
        inv.label = "Invert steering"; inv.key = "invertSteer"; inv.kind = PropKind::Bool;
        inv.field = [](void* o) -> void* { return &static_cast<GliderComponent*>(o)->invertSteer; };
        p.push_back(std::move(inv));
        addFloat("Grip",         "grip",       &GliderComponent::grip,       0.0f, 12.0f,  "%.2f");
        addFloat("Drag",         "drag",       &GliderComponent::drag,       0.0f, 4.0f,   "%.2f");
        // Manual boost (grouped under a "Boost" header at "boostCapacity" --
        // see gliderui::inspector)
        addFloat("Boost tank",   "boostCapacity", &GliderComponent::boostCapacity, 10.0f, 500.0f, "%.0f");
        addFloat("Boost drain",  "boostDrain",    &GliderComponent::boostDrain,     1.0f, 200.0f, "%.0f /s");
        addFloat("Boost regen",  "boostRegen",    &GliderComponent::boostRegen,     0.0f, 100.0f, "%.1f /s");
        addFloat("Boost delay",  "boostDelay",    &GliderComponent::boostDelay,     0.0f,  10.0f, "%.2f s");
        addFloat("Boost thrust", "boostThrust",   &GliderComponent::boostThrust,    0.0f, 200.0f, "%.0f m/s2");
        addFloat("Boost top-up", "boostTopSpeed", &GliderComponent::boostTopSpeed,  0.0f, 120.0f, "%.0f m/s");
        addFloat("Boost volume", "soundBoostGain", &GliderComponent::soundBoostGain, 0.0f,   2.0f, "%.2f");
        Property sb;
        sb.label = "Boost sound"; sb.key = "soundBoost"; sb.kind = PropKind::Text;
        sb.field = [](void* o) -> void* { return &static_cast<GliderComponent*>(o)->soundBoost; };
        p.push_back(std::move(sb));
        // Energy / hull (grouped under an "Energy" header at "energyCapacity").
        // The two sound filenames are Text props the Inspector turns into Sound
        // pickers, the same way a boost pad's punch is handled.
        addFloat("Energy tank",  "energyCapacity", &GliderComponent::energyCapacity, 10.0f, 500.0f, "%.0f");
        addFloat("Energy regen", "energyRegen",    &GliderComponent::energyRegen,     0.0f,  50.0f, "%.1f /s");
        addFloat("Regen delay",  "energyDelay",    &GliderComponent::energyDelay,     0.0f,  15.0f, "%.2f s");
        addFloat("Warn at",      "energyWarnAt",   &GliderComponent::energyWarnAt,    0.0f,   0.5f, "%.2f");
        addFloat("Hull radius",  "hullRadius",     &GliderComponent::hullRadius,      0.2f,  10.0f, "%.2f m");
        addFloat("Crash damage", "crashDamage",    &GliderComponent::crashDamage,     0.0f,  10.0f, "%.2f /m/s");
        addFloat("Crash floor",  "crashMinSpeed",  &GliderComponent::crashMinSpeed,   0.0f,  30.0f, "%.1f m/s");
        addFloat("Crash bounce", "crashBounce",    &GliderComponent::crashBounce,     0.0f,   1.0f, "%.2f");
        addFloat("Hit volume",   "soundHitGain",   &GliderComponent::soundHitGain,    0.0f,   2.0f, "%.2f");
        addFloat("Alarm volume", "soundWarnGain",  &GliderComponent::soundWarnGain,   0.0f,   2.0f, "%.2f");
        Property sh;
        sh.label = "Hit sound"; sh.key = "soundHit"; sh.kind = PropKind::Text;
        sh.field = [](void* o) -> void* { return &static_cast<GliderComponent*>(o)->soundHit; };
        p.push_back(std::move(sh));
        Property sw;
        sw.label = "Alarm sound"; sw.key = "soundWarn"; sw.kind = PropKind::Text;
        sw.field = [](void* o) -> void* { return &static_cast<GliderComponent*>(o)->soundWarn; };
        p.push_back(std::move(sw));
        // Hover (grouped under a "Hover" header at "rideHeight" -- see gliderui::inspector)
        addFloat("Ride height",  "rideHeight",     &GliderComponent::rideHeight,     0.2f, 12.0f, "%.2f m");
        addFloat("Hover spring", "hoverStiffness", &GliderComponent::hoverStiffness, 0.5f, 20.0f, "%.1f");
        addFloat("Hover damp",   "hoverDamp",      &GliderComponent::hoverDamp,      0.0f, 12.0f, "%.1f");
        addFloat("Gravity",      "gravity",        &GliderComponent::gravity,        0.0f, 60.0f, "%.0f");
        // Attitude (grouped under an "Attitude" header at "bankAngle")
        addFloat("Bank angle",   "bankAngle",   &GliderComponent::bankAngle,   0.0f, 60.0f, "%.0f deg");
        addFloat("Pitch follow", "pitchFollow", &GliderComponent::pitchFollow, 0.0f, 2.0f,  "%.2f");
        addFloat("Level rate",   "levelRate",   &GliderComponent::levelRate,   0.5f, 20.0f, "%.1f");
        Property fwd;
        fwd.label = "Model nose"; fwd.key = "forward"; fwd.kind = PropKind::EnumInt;
        fwd.enumLabels = {"+Z", "-Z"};
        fwd.field = [](void* o) -> void* { return &static_cast<GliderComponent*>(o)->forward; };
        p.push_back(std::move(fwd));
        // Follow camera (kept last so the inspector groups them under a header)
        return p;
    }();
    return props;
}

const std::vector<Property>& ParticleComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        auto f = [&](const char* label, const char* key, float ParticleComponent::* m,
                     float lo, float hi, const char* fmt) {
            Property q; q.label = label; q.key = key; q.kind = PropKind::Float;
            q.slider = true; q.min = lo; q.max = hi; q.speed = 0.05f; q.fmt = fmt;
            q.field = [m](void* o) -> void* { return &(static_cast<ParticleComponent*>(o)->*m); };
            p.push_back(std::move(q));
        };
        auto b = [&](const char* label, const char* key, bool ParticleComponent::* m) {
            Property q; q.label = label; q.key = key; q.kind = PropKind::Bool;
            q.field = [m](void* o) -> void* { return &(static_cast<ParticleComponent*>(o)->*m); };
            p.push_back(std::move(q));
        };
        auto v3 = [&](const char* label, const char* key, glm::vec3 ParticleComponent::* m) {
            Property q; q.label = label; q.key = key; q.kind = PropKind::Vec3;
            q.speed = 0.05f;
            q.field = [m](void* o) -> void* { return &(static_cast<ParticleComponent*>(o)->*m); };
            p.push_back(std::move(q));
        };
        auto col = [&](const char* label, const char* key, glm::vec3 ParticleComponent::* m) {
            Property q; q.label = label; q.key = key; q.kind = PropKind::Color;
            q.field = [m](void* o) -> void* { return &(static_cast<ParticleComponent*>(o)->*m); };
            p.push_back(std::move(q));
        };

        // Emission
        b("Playing",     "playing",  &ParticleComponent::playing);
        b("Loop",        "loop",     &ParticleComponent::loop);
        f("Duration",    "duration", &ParticleComponent::duration, 0.05f,  60.0f, "%.2f s");
        f("Rate",        "rate",     &ParticleComponent::rate,      0.0f, 500.0f, "%.0f /s");
        f("Burst",       "burst",    &ParticleComponent::burst,     0.0f, 500.0f, "%.0f");
        f("Max count",   "maxCount", &ParticleComponent::maxCount,  1.0f, 5000.0f, "%.0f");
        f("Lifetime",    "lifetime", &ParticleComponent::lifetime, 0.05f,  30.0f, "%.2f s");
        f("Life spread", "lifeVar",  &ParticleComponent::lifeVar,   0.0f,   1.0f, "%.2f");

        // Shape
        Property sh;
        sh.label = "Shape"; sh.key = "shape"; sh.kind = PropKind::EnumInt;
        sh.enumLabels = {"Point", "Sphere", "Cone", "Box"};
        sh.field = [](void* o) -> void* { return &static_cast<ParticleComponent*>(o)->shape; };
        p.push_back(std::move(sh));
        f("Radius",     "radius",    &ParticleComponent::radius,    0.0f, 50.0f, "%.2f m");
        f("Cone angle", "coneAngle", &ParticleComponent::coneAngle, 0.0f, 90.0f, "%.0f deg");
        v3("Box size",  "boxSize",   &ParticleComponent::boxSize);

        // Motion
        f("Speed",          "speed",    &ParticleComponent::speed,    0.0f, 80.0f, "%.2f m/s");
        f("Speed spread",   "speedVar", &ParticleComponent::speedVar, 0.0f, 40.0f, "%.2f m/s");
        v3("Gravity",       "gravity",  &ParticleComponent::gravity);
        f("Drag",           "drag",     &ParticleComponent::drag,     0.0f,  8.0f, "%.2f");
        v3("Wind",          "wind",     &ParticleComponent::wind);
        b("World space",    "worldSpace", &ParticleComponent::worldSpace);
        f("Inherit motion", "inherit",  &ParticleComponent::inherit,  0.0f,  1.0f, "%.2f");

        // Look. `sprite` is a Text prop so it persists; the Inspector swaps in a
        // texture picker for it, the same way the sound fields get one.
        Property sp;
        sp.label = "Sprite"; sp.key = "sprite"; sp.kind = PropKind::Text;
        sp.field = [](void* o) -> void* { return &static_cast<ParticleComponent*>(o)->sprite; };
        p.push_back(std::move(sp));
        col("Colour start", "colorStart", &ParticleComponent::colorStart);
        col("Colour end",   "colorEnd",   &ParticleComponent::colorEnd);
        f("Alpha start", "alphaStart", &ParticleComponent::alphaStart, 0.0f, 1.0f, "%.2f");
        f("Alpha end",   "alphaEnd",   &ParticleComponent::alphaEnd,   0.0f, 1.0f, "%.2f");
        f("Size start",  "sizeStart",  &ParticleComponent::sizeStart, 0.01f, 20.0f, "%.2f m");
        f("Size end",    "sizeEnd",    &ParticleComponent::sizeEnd,   0.01f, 20.0f, "%.2f m");
        f("Size spread", "sizeVar",    &ParticleComponent::sizeVar,    0.0f, 1.0f, "%.2f");
        f("Rotation",    "rotation",   &ParticleComponent::rotation,   0.0f, 360.0f, "%.0f deg");
        f("Spin",        "spin",       &ParticleComponent::spin,    -720.0f, 720.0f, "%.0f deg/s");
        Property bl;
        bl.label = "Blend"; bl.key = "blend"; bl.kind = PropKind::EnumInt;
        bl.enumLabels = {"Alpha (smoke)", "Additive (fire)"};
        bl.field = [](void* o) -> void* { return &static_cast<ParticleComponent*>(o)->blend; };
        p.push_back(std::move(bl));
        f("Brightness", "brightness", &ParticleComponent::brightness, 0.0f, 8.0f, "%.2f");
        f("Glow at rest",  "speedGlowMin", &ParticleComponent::speedGlowMin, 0.0f, 8.0f, "%.2f");
        f("Glow at speed", "speedGlowMax", &ParticleComponent::speedGlowMax, 0.0f, 8.0f, "%.2f");
        f("Stretch",    "stretch",    &ParticleComponent::stretch,    0.0f, 1.0f, "%.3f");
        return p;
    }();
    return props;
}

const std::vector<Property>& BoostPadComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        Property bs;
        bs.label = "Boost speed"; bs.key = "boostSpeed"; bs.kind = PropKind::Float;
        bs.slider = true; bs.min = 10.0f; bs.max = 250.0f; bs.fmt = "%.0f m/s";
        bs.field = [](void* o) -> void* { return &static_cast<BoostPadComponent*>(o)->boostSpeed; };
        p.push_back(std::move(bs));
        Property ac;
        ac.label = "Acceleration"; ac.key = "accel"; ac.kind = PropKind::Float;
        ac.slider = true; ac.min = 5.0f; ac.max = 300.0f; ac.fmt = "%.0f m/s2";
        ac.field = [](void* o) -> void* { return &static_cast<BoostPadComponent*>(o)->accel; };
        p.push_back(std::move(ac));
        Property hd;
        hd.label = "Hold"; hd.key = "hold"; hd.kind = PropKind::Float;
        hd.slider = true; hd.min = 0.1f; hd.max = 6.0f; hd.fmt = "%.1f s";
        hd.field = [](void* o) -> void* { return &static_cast<BoostPadComponent*>(o)->hold; };
        p.push_back(std::move(hd));
        // Size. DragFloat, not a slider: a strip is usually a length you know
        // ("the last twenty metres of the straight"), and typing it beats hunting
        // for it. 0 falls back to the object's box, so the field reads as an
        // override rather than as a value that must be filled in.
        Property pl;
        pl.label = "Pad length"; pl.key = "padLength"; pl.kind = PropKind::Float;
        pl.min = 0.0f; pl.max = 400.0f; pl.speed = 0.1f; pl.fmt = "%.2f m";
        pl.field = [](void* o) -> void* { return &static_cast<BoostPadComponent*>(o)->padLength; };
        p.push_back(std::move(pl));
        Property pw;
        pw.label = "Pad width"; pw.key = "padWidth"; pw.kind = PropKind::Float;
        pw.min = 0.0f; pw.max = 400.0f; pw.speed = 0.1f; pw.fmt = "%.2f m";
        pw.field = [](void* o) -> void* { return &static_cast<BoostPadComponent*>(o)->padWidth; };
        p.push_back(std::move(pw));
        Property pd;
        pd.label = "Boost along pad"; pd.key = "usePadDir"; pd.kind = PropKind::Bool;
        pd.field = [](void* o) -> void* { return &static_cast<BoostPadComponent*>(o)->usePadDir; };
        p.push_back(std::move(pd));
        Property rv;
        rv.label = "Reverse direction"; rv.key = "reverse"; rv.kind = PropKind::Bool;
        rv.field = [](void* o) -> void* { return &static_cast<BoostPadComponent*>(o)->reverse; };
        p.push_back(std::move(rv));
        // Punch SFX: the filename is a Text prop (so it persists); the Inspector
        // swaps in a Sound picker for it. Gain + pitch tune how the kick lands.
        Property sg;
        sg.label = "Punch volume"; sg.key = "soundGain"; sg.kind = PropKind::Float;
        sg.slider = true; sg.min = 0.0f; sg.max = 2.0f; sg.fmt = "%.2f";
        sg.field = [](void* o) -> void* { return &static_cast<BoostPadComponent*>(o)->soundGain; };
        p.push_back(std::move(sg));
        Property sp;
        sp.label = "Punch pitch"; sp.key = "soundPitch"; sp.kind = PropKind::Float;
        sp.slider = true; sp.min = 0.3f; sp.max = 2.0f; sp.fmt = "%.2f";
        sp.field = [](void* o) -> void* { return &static_cast<BoostPadComponent*>(o)->soundPitch; };
        p.push_back(std::move(sp));
        Property snd;
        snd.label = "Punch sound"; snd.key = "sound"; snd.kind = PropKind::Text;
        snd.field = [](void* o) -> void* { return &static_cast<BoostPadComponent*>(o)->sound; };
        p.push_back(std::move(snd));
        return p;
    }();
    return props;
}

const std::vector<Property>& ShowroomComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        auto addFloat = [&](const char* label, const char* key, float ShowroomComponent::* m,
                            float lo, float hi, const char* fmt) {
            Property f; f.label = label; f.key = key; f.kind = PropKind::Float;
            f.slider = true; f.min = lo; f.max = hi; f.speed = 0.05f; f.fmt = fmt;
            f.field = [m](void* o) -> void* { return &(static_cast<ShowroomComponent*>(o)->*m); };
            p.push_back(std::move(f));
        };
        auto addText = [&](const char* label, const char* key, std::string ShowroomComponent::* m) {
            Property f; f.label = label; f.key = key; f.kind = PropKind::Text;
            f.field = [m](void* o) -> void* { return &(static_cast<ShowroomComponent*>(o)->*m); };
            p.push_back(std::move(f));
        };
        addText("Headline", "title",    &ShowroomComponent::title);
        addText("Overline", "subtitle", &ShowroomComponent::subtitle);
        // Stage
        addFloat("Ring radius", "ringRadius", &ShowroomComponent::ringRadius, 0.0f, 40.0f, "%.1f m");
        addFloat("Rise height", "riseHeight", &ShowroomComponent::riseHeight, 0.0f, 10.0f, "%.2f m");
        addFloat("Spin speed",  "spinSpeed",  &ShowroomComponent::spinSpeed, -90.0f, 90.0f, "%.0f deg/s");
        addFloat("Bob amount",  "bobAmount",  &ShowroomComponent::bobAmount,  0.0f,  1.5f, "%.2f m");
        // Orbit camera
        addFloat("Cam distance", "camDistance", &ShowroomComponent::camDistance, 1.0f, 60.0f, "%.1f m");
        addFloat("Cam height",   "camHeight",   &ShowroomComponent::camHeight, -5.0f, 30.0f, "%.1f m");
        addFloat("Cam pitch",    "camPitch",    &ShowroomComponent::camPitch, -60.0f, 30.0f, "%.0f deg");
        addFloat("Cam orbit",    "camOrbit",    &ShowroomComponent::camOrbit, -60.0f, 60.0f, "%.0f deg/s");
        addFloat("Cam FOV",      "camFov",      &ShowroomComponent::camFov,   20.0f, 100.0f, "%.0f deg");
        Property acc;
        acc.label = "Accent"; acc.key = "accent"; acc.kind = PropKind::Color;
        acc.field = [](void* o) -> void* { return &static_cast<ShowroomComponent*>(o)->accent; };
        p.push_back(std::move(acc));
        // SFX (filenames are Text props; the Inspector swaps in a Sound picker)
        addText("Move sound",   "soundMove",   &ShowroomComponent::soundMove);
        addText("Select sound", "soundSelect", &ShowroomComponent::soundSelect);
        addText("Start sound",  "soundStart",  &ShowroomComponent::soundStart);
        addFloat("Sound volume", "soundGain", &ShowroomComponent::soundGain, 0.0f, 2.0f, "%.2f");
        return p;
    }();
    return props;
}

const std::vector<Property>& CraftEntryComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        auto addText = [&](const char* label, const char* key, std::string CraftEntryComponent::* m) {
            Property f; f.label = label; f.key = key; f.kind = PropKind::Text;
            f.field = [m](void* o) -> void* { return &(static_cast<CraftEntryComponent*>(o)->*m); };
            p.push_back(std::move(f));
        };
        addText("Name",  "title", &CraftEntryComponent::title);
        addText("Team",  "team",  &CraftEntryComponent::team);
        addText("Blurb", "blurb", &CraftEntryComponent::blurb);
        Property acc;
        acc.label = "Accent"; acc.key = "accent"; acc.kind = PropKind::Color;
        acc.field = [](void* o) -> void* { return &static_cast<CraftEntryComponent*>(o)->accent; };
        p.push_back(std::move(acc));
        Property ord;
        ord.label = "Order"; ord.key = "order"; ord.kind = PropKind::Float;
        ord.speed = 1.0f; ord.fmt = "%.0f";
        ord.field = [](void* o) -> void* { return &static_cast<CraftEntryComponent*>(o)->order; };
        p.push_back(std::move(ord));
        return p;
    }();
    return props;
}

const std::vector<Property>& TrackEntryComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        auto addText = [&](const char* label, const char* key, std::string TrackEntryComponent::* m) {
            Property f; f.label = label; f.key = key; f.kind = PropKind::Text;
            f.field = [m](void* o) -> void* { return &(static_cast<TrackEntryComponent*>(o)->*m); };
            p.push_back(std::move(f));
        };
        auto addFloat = [&](const char* label, const char* key, float TrackEntryComponent::* m,
                            float lo, float hi, const char* fmt) {
            Property f; f.label = label; f.key = key; f.kind = PropKind::Float;
            f.slider = true; f.min = lo; f.max = hi; f.speed = 0.1f; f.fmt = fmt;
            f.field = [m](void* o) -> void* { return &(static_cast<TrackEntryComponent*>(o)->*m); };
            p.push_back(std::move(f));
        };
        // The scene stem is a Text prop so it persists; the Inspector swaps in a
        // scene picker for it, exactly like the Scene Trigger's.
        addText("Scene", "scene", &TrackEntryComponent::scene);
        addText("Name",  "title", &TrackEntryComponent::title);
        addText("Blurb", "blurb", &TrackEntryComponent::blurb);
        addText("Preview image", "image", &TrackEntryComponent::image);
        addFloat("Laps",       "laps",       &TrackEntryComponent::laps,       0.0f, 30.0f, "%.0f");
        addFloat("Length",     "lengthKm",   &TrackEntryComponent::lengthKm,   0.0f, 30.0f, "%.2f km");
        addFloat("Difficulty", "difficulty", &TrackEntryComponent::difficulty,  1.0f,  5.0f, "%.0f");
        Property ord;
        ord.label = "Order"; ord.key = "order"; ord.kind = PropKind::Float;
        ord.speed = 1.0f; ord.fmt = "%.0f";
        ord.field = [](void* o) -> void* { return &static_cast<TrackEntryComponent*>(o)->order; };
        p.push_back(std::move(ord));
        return p;
    }();
    return props;
}

const std::vector<Property>& OpponentComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        auto addFloat = [&](const char* label, const char* key, float OpponentComponent::* m,
                            float lo, float hi, const char* fmt) {
            Property f; f.label = label; f.key = key; f.kind = PropKind::Float;
            f.slider = true; f.min = lo; f.max = hi; f.speed = 0.1f; f.fmt = fmt;
            f.field = [m](void* o) -> void* { return &(static_cast<OpponentComponent*>(o)->*m); };
            p.push_back(std::move(f));
        };
        addFloat("Speed",         "speed",        &OpponentComponent::speed,        2.0f, 200.0f, "%.0f m/s");
        addFloat("Grip",          "grip",         &OpponentComponent::grip,         2.0f, 60.0f,  "%.0f m/s2");
        addFloat("Accel",         "accel",        &OpponentComponent::accel,        1.0f, 60.0f,  "%.0f m/s2");
        addFloat("Brake",         "brake",        &OpponentComponent::brake,        1.0f, 80.0f,  "%.0f m/s2");
        addFloat("Catch-up",      "catchup",      &OpponentComponent::catchup,      0.0f, 1.0f,   "%.2f");
        addFloat("Racing line",   "racingLine",   &OpponentComponent::racingLine,   0.0f, 1.0f,   "%.2f");
        addFloat("Awareness",     "awareness",    &OpponentComponent::awareness,    0.0f, 1.0f,   "%.2f");
        addFloat("Pad seek",      "padSeek",      &OpponentComponent::padSeek,      0.0f, 1.0f,   "%.2f");
        Property en;
        en.label = "In the race"; en.key = "entered"; en.kind = PropKind::Bool;
        en.field = [](void* o) -> void* { return &static_cast<OpponentComponent*>(o)->entered; };
        p.push_back(std::move(en));
        addFloat("Lane offset",   "laneOffset",   &OpponentComponent::laneOffset,  -20.0f, 20.0f, "%.1f m");
        addFloat("Ride height",   "rideHeight",   &OpponentComponent::rideHeight,   0.0f, 12.0f,  "%.2f m");
        addFloat("Start distance","startDistance",&OpponentComponent::startDistance,0.0f, 2000.0f,"%.0f m");
        addFloat("Bank angle",    "bankAngle",    &OpponentComponent::bankAngle,    0.0f, 60.0f,  "%.0f deg");
        Property lp;
        lp.label = "Loop"; lp.key = "loop"; lp.kind = PropKind::Bool;
        lp.field = [](void* o) -> void* { return &static_cast<OpponentComponent*>(o)->loop; };
        p.push_back(std::move(lp));
        Property fwd;
        fwd.label = "Model nose"; fwd.key = "forward"; fwd.kind = PropKind::EnumInt;
        fwd.enumLabels = {"+Z", "-Z"};
        fwd.field = [](void* o) -> void* { return &static_cast<OpponentComponent*>(o)->forward; };
        p.push_back(std::move(fwd));
        return p;
    }();
    return props;
}

namespace {
// Append the shared gate size/orientation properties (width/height/depth/yaw) for
// a Checkpoint- or FinishLine-style component. `mW`... are member pointers.
template <class C>
void addGateProps(std::vector<Property>& p,
                  float C::* mW, float C::* mH, float C::* mD, float C::* mYaw) {
    auto add = [&](const char* label, const char* key, float C::* m,
                   float lo, float hi, const char* fmt) {
        Property f; f.label = label; f.key = key; f.kind = PropKind::Float;
        f.slider = true; f.min = lo; f.max = hi; f.speed = 0.1f; f.fmt = fmt;
        f.field = [m](void* o) -> void* { return &(static_cast<C*>(o)->*m); };
        p.push_back(std::move(f));
    };
    add("Width",  "gateW",   mW,   1.0f, 120.0f, "%.1f m");
    add("Height", "gateH",   mH,   1.0f,  40.0f, "%.1f m");
    add("Depth",  "gateD",   mD,   0.5f,  40.0f, "%.1f m");
    add("Yaw",    "gateYaw", mYaw, -180.0f, 180.0f, "%.0f deg");
}
} // namespace

const std::vector<Property>& FinishLineComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        Property l;
        l.label = "Laps"; l.key = "laps"; l.kind = PropKind::Float;
        l.slider = true; l.min = 0.0f; l.max = 50.0f; l.fmt = "%.0f";
        l.field = [](void* o) -> void* { return &static_cast<FinishLineComponent*>(o)->laps; };
        p.push_back(std::move(l));
        Property md;
        md.label = "Session"; md.key = "mode"; md.kind = PropKind::EnumInt;
        md.enumLabels = {"Race", "Time trial"};
        md.field = [](void* o) -> void* { return &static_cast<FinishLineComponent*>(o)->mode; };
        p.push_back(std::move(md));
        auto addGrid = [&](const char* label, const char* key,
                           float FinishLineComponent::* m, float lo, float hi) {
            Property f; f.label = label; f.key = key; f.kind = PropKind::Float;
            f.slider = true; f.min = lo; f.max = hi; f.speed = 0.1f; f.fmt = "%.1f m";
            f.field = [m](void* o) -> void* { return &(static_cast<FinishLineComponent*>(o)->*m); };
            p.push_back(std::move(f));
        };
        addGrid("Grid setback", "gridBack", &FinishLineComponent::gridBack, 0.0f, 120.0f);
        addGrid("Row spacing",  "gridRow",  &FinishLineComponent::gridRow,  2.0f,  40.0f);
        addGrid("Lane offset",  "gridLane", &FinishLineComponent::gridLane, 0.0f,  20.0f);
        Property pp;
        pp.label = "Player on pole"; pp.key = "playerPole"; pp.kind = PropKind::Bool;
        pp.field = [](void* o) -> void* { return &static_cast<FinishLineComponent*>(o)->playerPole; };
        p.push_back(std::move(pp));
        addGateProps<FinishLineComponent>(p, &FinishLineComponent::width,
            &FinishLineComponent::height, &FinishLineComponent::depth,
            &FinishLineComponent::yaw);
        // Start sequence SFX: the filenames are Text props (so they persist);
        // the Inspector swaps in Sound pickers for them.
        Property sg;
        sg.label = "Start volume"; sg.key = "soundGain"; sg.kind = PropKind::Float;
        sg.slider = true; sg.min = 0.0f; sg.max = 2.0f; sg.fmt = "%.2f";
        sg.field = [](void* o) -> void* { return &static_cast<FinishLineComponent*>(o)->soundGain; };
        p.push_back(std::move(sg));
        auto addSound = [&](const char* label, const char* key,
                            std::string FinishLineComponent::* m) {
            Property s; s.label = label; s.key = key; s.kind = PropKind::Text;
            s.field = [m](void* o) -> void* { return &(static_cast<FinishLineComponent*>(o)->*m); };
            p.push_back(std::move(s));
        };
        addSound("Ready sound", "soundReady", &FinishLineComponent::soundReady);
        addSound("Set sound",   "soundSet",   &FinishLineComponent::soundSet);
        addSound("Go sound",    "soundGo",    &FinishLineComponent::soundGo);
        return p;
    }();
    return props;
}

const std::vector<Property>& CheckpointComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        addGateProps<CheckpointComponent>(p, &CheckpointComponent::width,
            &CheckpointComponent::height, &CheckpointComponent::depth,
            &CheckpointComponent::yaw);
        // Gate SFX, same shape as the boost pad's punch: sliders here, and the
        // filename as a Text prop the Inspector turns into a Sound picker.
        Property sg;
        sg.label = "Gate volume"; sg.key = "soundGain"; sg.kind = PropKind::Float;
        sg.slider = true; sg.min = 0.0f; sg.max = 2.0f; sg.fmt = "%.2f";
        sg.field = [](void* o) -> void* { return &static_cast<CheckpointComponent*>(o)->soundGain; };
        p.push_back(std::move(sg));
        Property sp;
        sp.label = "Gate pitch"; sp.key = "soundPitch"; sp.kind = PropKind::Float;
        sp.slider = true; sp.min = 0.3f; sp.max = 2.0f; sp.fmt = "%.2f";
        sp.field = [](void* o) -> void* { return &static_cast<CheckpointComponent*>(o)->soundPitch; };
        p.push_back(std::move(sp));
        Property snd;
        snd.label = "Gate sound"; snd.key = "sound"; snd.kind = PropKind::Text;
        snd.field = [](void* o) -> void* { return &static_cast<CheckpointComponent*>(o)->sound; };
        p.push_back(std::move(snd));
        return p;
    }();
    return props;
}

const std::vector<Property>& PusherComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        Property dir;
        dir.label = "Direction"; dir.key = "direction"; dir.kind = PropKind::Vec3;
        dir.speed = 0.05f;
        dir.field = [](void* o) -> void* { return &static_cast<PusherComponent*>(o)->direction; };
        p.push_back(std::move(dir));
        Property strength;
        strength.label = "Strength"; strength.key = "strength"; strength.kind = PropKind::Float;
        strength.slider = true; strength.min = 0.0f; strength.max = 50.0f; strength.fmt = "%.1f";
        strength.field = [](void* o) -> void* { return &static_cast<PusherComponent*>(o)->strength; };
        p.push_back(std::move(strength));
        Property radius;
        radius.label = "Radius"; radius.key = "radius"; radius.kind = PropKind::Float;
        radius.slider = true; radius.min = 0.5f; radius.max = 20.0f; radius.fmt = "%.1f m";
        radius.field = [](void* o) -> void* { return &static_cast<PusherComponent*>(o)->radius; };
        p.push_back(std::move(radius));
        Property cont;
        cont.label = "Continuous"; cont.key = "continuous"; cont.kind = PropKind::Bool;
        cont.field = [](void* o) -> void* { return &static_cast<PusherComponent*>(o)->continuous; };
        p.push_back(std::move(cont));
        return p;
    }();
    return props;
}

const std::vector<Property>& AnimationComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        auto addBool = [&](const char* label, const char* key, bool AnimationComponent::* m) {
            Property b; b.label = label; b.key = key; b.kind = PropKind::Bool;
            b.field = [m](void* o) -> void* { return &(static_cast<AnimationComponent*>(o)->*m); };
            p.push_back(std::move(b));
        };
        auto addFloat = [&](const char* label, const char* key, float AnimationComponent::* m,
                            bool slider, float lo, float hi, const char* fmt) {
            Property f; f.label = label; f.key = key; f.kind = PropKind::Float;
            f.slider = slider; f.min = lo; f.max = hi; f.speed = 0.05f; f.fmt = fmt;
            f.field = [m](void* o) -> void* { return &(static_cast<AnimationComponent*>(o)->*m); };
            p.push_back(std::move(f));
        };
        addFloat("Speed", "speed", &AnimationComponent::speed, true, 0.0f, 4.0f, "%.2fx");
        addBool ("Autostart", "autostart", &AnimationComponent::autostart);
        addBool ("Loop", "loop", &AnimationComponent::loop);
        addBool ("Reverse", "reverse", &AnimationComponent::reverse);
        addFloat("Start (s)", "start", &AnimationComponent::start, false, 0.0f, 600.0f, "%.2f");
        addFloat("End (s)",   "end",   &AnimationComponent::end,   false, 0.0f, 600.0f, "%.2f");
        return p;
    }();
    return props;
}

// Persists the properties above plus the chosen clip index.
void AnimationComponent::save(nlohmann::json& j) const {
    writeProps(j, props(), this);
    j["clip"] = clip;
}
void AnimationComponent::load(const nlohmann::json& j) {
    readProps(j, props(), this);
    clip = j.value("clip", 0);
}

const std::vector<Property>& AnimationTriggerComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        Property radius;
        radius.label = "Radius"; radius.key = "radius"; radius.kind = PropKind::Float;
        radius.slider = true; radius.min = 0.5f; radius.max = 20.0f; radius.fmt = "%.1f m";
        radius.field = [](void* o) -> void* { return &static_cast<AnimationTriggerComponent*>(o)->radius; };
        p.push_back(std::move(radius));
        Property once;
        once.label = "Once"; once.key = "once"; once.kind = PropKind::Bool;
        once.field = [](void* o) -> void* { return &static_cast<AnimationTriggerComponent*>(o)->once; };
        p.push_back(std::move(once));
        return p;
    }();
    return props;
}
void AnimationTriggerComponent::save(nlohmann::json& j) const {
    writeProps(j, props(), this);
    j["target"] = target;
}
void AnimationTriggerComponent::load(const nlohmann::json& j) {
    readProps(j, props(), this);
    target = j.value("target", -1);
}

const std::vector<Property>& ScriptComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        Property file;
        file.label = "Script"; file.key = "file"; file.kind = PropKind::Text;
        file.field = [](void* o) -> void* { return &static_cast<ScriptComponent*>(o)->file; };
        p.push_back(std::move(file));
        return p;
    }();
    return props;
}

const std::vector<Property>& LightComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        Property type;
        // Key must NOT be "type": that collides with the component's serialization
        // type id (cj["type"] = "light"), which corrupts the save. See the loader's
        // legacy-recovery in ProjectIO.cpp for files written before this fix.
        type.label = "Type"; type.key = "lightType"; type.kind = PropKind::EnumInt;
        type.enumLabels = {"Point", "Spot"};
        type.field = [](void* o) -> void* { return &static_cast<LightComponent*>(o)->type; };
        p.push_back(std::move(type));
        Property col;
        col.label = "Colour"; col.key = "color"; col.kind = PropKind::Color;
        col.field = [](void* o) -> void* { return &static_cast<LightComponent*>(o)->color; };
        p.push_back(std::move(col));
        Property intensity;
        intensity.label = "Intensity"; intensity.key = "intensity"; intensity.kind = PropKind::Float;
        intensity.slider = true; intensity.min = 0.0f; intensity.max = 30.0f;
        intensity.field = [](void* o) -> void* { return &static_cast<LightComponent*>(o)->intensity; };
        p.push_back(std::move(intensity));
        Property range;
        range.label = "Range"; range.key = "range"; range.kind = PropKind::Float;
        range.slider = true; range.min = 0.5f; range.max = 60.0f; range.fmt = "%.1f m";
        range.field = [](void* o) -> void* { return &static_cast<LightComponent*>(o)->range; };
        p.push_back(std::move(range));
        Property spotAngle;
        spotAngle.label = "Spot angle"; spotAngle.key = "spotAngle"; spotAngle.kind = PropKind::Float;
        spotAngle.slider = true; spotAngle.min = 5.0f; spotAngle.max = 80.0f; spotAngle.fmt = "%.0f deg";
        spotAngle.field = [](void* o) -> void* { return &static_cast<LightComponent*>(o)->spotAngle; };
        p.push_back(std::move(spotAngle));
        Property spotBlend;
        spotBlend.label = "Spot blend"; spotBlend.key = "spotBlend"; spotBlend.kind = PropKind::Float;
        spotBlend.slider = true; spotBlend.min = 0.0f; spotBlend.max = 1.0f; spotBlend.fmt = "%.2f";
        spotBlend.field = [](void* o) -> void* { return &static_cast<LightComponent*>(o)->spotBlend; };
        p.push_back(std::move(spotBlend));
        Property shadows;
        shadows.label = "Cast shadows"; shadows.key = "castShadows"; shadows.kind = PropKind::Bool;
        shadows.field = [](void* o) -> void* { return &static_cast<LightComponent*>(o)->castShadows; };
        p.push_back(std::move(shadows));
        Property bias;
        bias.label = "Shadow bias"; bias.key = "shadowBias"; bias.kind = PropKind::Float;
        bias.slider = true; bias.min = 0.0f; bias.max = 0.03f; bias.fmt = "%.4f";
        bias.field = [](void* o) -> void* { return &static_cast<LightComponent*>(o)->shadowBias; };
        p.push_back(std::move(bias));
        return p;
    }();
    return props;
}

const std::vector<Property>& PhysicsComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        Property dyn;
        dyn.label = "Dynamic"; dyn.key = "dynamic"; dyn.kind = PropKind::Bool;
        dyn.field = [](void* o) -> void* { return &static_cast<PhysicsComponent*>(o)->dynamic; };
        p.push_back(std::move(dyn));
        Property mass;
        mass.label = "Mass"; mass.key = "mass"; mass.kind = PropKind::Float;
        mass.min = 0.01f; mass.max = 1000.0f; mass.speed = 0.1f; mass.fmt = "%.2f kg";
        mass.field = [](void* o) -> void* { return &static_cast<PhysicsComponent*>(o)->mass; };
        mass.visible = [](const void* o) { return static_cast<const PhysicsComponent*>(o)->dynamic; };
        p.push_back(std::move(mass));
        return p;
    }();
    return props;
}

const std::vector<Property>& SoftBodyComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        Property kind;
        kind.label = "Shape"; kind.key = "kind"; kind.kind = PropKind::EnumInt;
        kind.enumLabels = {"Jelly", "Balloon", "Cloth", "This mesh"};
        kind.field = [](void* o) -> void* { return &static_cast<SoftBodyComponent*>(o)->kind; };
        p.push_back(std::move(kind));
        Property res;
        res.label = "Resolution"; res.key = "resolution"; res.kind = PropKind::Int;
        res.min = 2.0f; res.max = 10.0f; res.slider = true;
        res.field = [](void* o) -> void* { return &static_cast<SoftBodyComponent*>(o)->resolution; };
        // A modelled mesh brings its own particles -- one per corner it already has.
        res.visible = [](const void* o) {
            return static_cast<const SoftBodyComponent*>(o)->kind != SoftBodyComponent::FromMesh;
        };
        p.push_back(std::move(res));
        Property mass;
        mass.label = "Mass"; mass.key = "mass"; mass.kind = PropKind::Float;
        mass.min = 0.1f; mass.max = 1000.0f; mass.speed = 0.5f; mass.fmt = "%.1f kg";
        mass.field = [](void* o) -> void* { return &static_cast<SoftBodyComponent*>(o)->mass; };
        p.push_back(std::move(mass));
        Property soft;
        soft.label = "Softness"; soft.key = "softness"; soft.kind = PropKind::Float;
        soft.slider = true; soft.min = 0.0f; soft.max = 1.0f; soft.fmt = "%.2f";
        soft.field = [](void* o) -> void* { return &static_cast<SoftBodyComponent*>(o)->softness; };
        p.push_back(std::move(soft));
        Property press;
        press.label = "Pressure"; press.key = "pressure"; press.kind = PropKind::Float;
        press.min = 0.0f; press.max = 20000.0f; press.speed = 50.0f; press.fmt = "%.0f";
        press.field = [](void* o) -> void* { return &static_cast<SoftBodyComponent*>(o)->pressure; };
        // Only a hollow shell has anything to inflate: a jelly lattice holds its
        // own volume, and a cloth has none to hold.
        press.visible = [](const void* o) {
            const int k = static_cast<const SoftBodyComponent*>(o)->kind;
            return k == SoftBodyComponent::Balloon || k == SoftBodyComponent::FromMesh;
        };
        p.push_back(std::move(press));
        Property damp;
        damp.label = "Damping"; damp.key = "damping"; damp.kind = PropKind::Float;
        damp.slider = true; damp.min = 0.0f; damp.max = 1.0f; damp.fmt = "%.2f";
        damp.field = [](void* o) -> void* { return &static_cast<SoftBodyComponent*>(o)->damping; };
        p.push_back(std::move(damp));
        Property pin;
        pin.label = "Pinned"; pin.key = "pinning"; pin.kind = PropKind::EnumInt;
        pin.enumLabels = {"Nothing", "Corners", "One edge"};
        pin.field = [](void* o) -> void* { return &static_cast<SoftBodyComponent*>(o)->pinning; };
        pin.visible = [](const void* o) {
            return static_cast<const SoftBodyComponent*>(o)->kind == SoftBodyComponent::Cloth;
        };
        p.push_back(std::move(pin));
        return p;
    }();
    return props;
}

const std::vector<Property>& VolumetricFogComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        // A local helper, because this component is twenty fields of the same
        // three shapes and spelling each one out would bury what is actually
        // different about them -- the range, which IS the design of the knob.
        auto addF = [&p](const char* label, const char* key, float FogMedium::*m,
                         float lo, float hi, const char* fmt = "") {
            Property q;
            q.label = label; q.key = key; q.kind = PropKind::Float;
            q.slider = true; q.min = lo; q.max = hi; q.fmt = fmt;
            q.field = [m](void* o) -> void* {
                return &(static_cast<VolumetricFogComponent*>(o)->fog.*m);
            };
            p.push_back(std::move(q));
        };
        auto addV = [&p](const char* label, const char* key, glm::vec3 FogMedium::*m,
                         PropKind kind, float speed = 0.1f) {
            Property q;
            q.label = label; q.key = key; q.kind = kind; q.speed = speed;
            q.field = [m](void* o) -> void* {
                return &(static_cast<VolumetricFogComponent*>(o)->fog.*m);
            };
            p.push_back(std::move(q));
        };
        auto addB = [&p](const char* label, const char* key, bool FogMedium::*m) {
            Property q;
            q.label = label; q.key = key; q.kind = PropKind::Bool;
            q.field = [m](void* o) -> void* {
                return &(static_cast<VolumetricFogComponent*>(o)->fog.*m);
            };
            p.push_back(std::move(q));
        };

        addF("Thickness", "density", &FogMedium::density, 0.0f, 0.5f, "%.3f /m");
        addV("Tint", "color", &FogMedium::color, PropKind::Color);
        addF("Coverage", "coverage", &FogMedium::coverage, 0.0f, 0.95f);
        addF("Noise scale", "noiseScale", &FogMedium::noiseScale, 0.002f, 0.20f, "%.3f");
        addF("Vertical detail", "verticalDetail", &FogMedium::verticalDetail, 0.25f,
             8.0f, "%.2fx");
        addF("Detail", "detail", &FogMedium::detail, 0.0f, 0.95f);
        addF("Swirl", "warp", &FogMedium::warp, 0.0f, 1.5f);
        addV("Wind", "wind", &FogMedium::wind, PropKind::Vec3, 0.05f);
        addF("Edge fade", "edge", &FogMedium::edge, 0.02f, 1.0f);
        addF("Height falloff", "heightFalloff", &FogMedium::heightFalloff, 0.0f, 3.0f);
        addF("Forward scatter", "anisotropy", &FogMedium::anisotropy, -0.9f, 0.9f);
        addF("Sun", "sunIntensity", &FogMedium::sunIntensity, 0.0f, 4.0f);
        addF("Ambient", "ambientIntensity", &FogMedium::ambientIntensity, 0.0f, 4.0f);
        addB("Sun shafts", "shafts", &FogMedium::shafts);
        addB("Self-shadow", "selfShadow", &FogMedium::selfShadow);

        Property steps;
        steps.label = "Steps"; steps.key = "steps"; steps.kind = PropKind::Int;
        steps.slider = true; steps.min = 8.0f; steps.max = 128.0f;
        steps.field = [](void* o) -> void* {
            return &static_cast<VolumetricFogComponent*>(o)->fog.steps;
        };
        p.push_back(std::move(steps));
        return p;
    }();
    return props;
}

const std::vector<Property>& PlayerStartComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        Property speed;
        speed.label = "Move speed"; speed.key = "moveSpeed"; speed.kind = PropKind::Float;
        speed.slider = true; speed.min = 2.0f; speed.max = 80.0f; speed.fmt = "%.0f m/s";
        speed.field = [](void* o) -> void* { return &static_cast<PlayerStartComponent*>(o)->moveSpeed; };
        p.push_back(std::move(speed));
        return p;
    }();
    return props;
}

const std::vector<Property>& SunComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        Property col;
        col.label = "Sun colour"; col.key = "color"; col.kind = PropKind::Color;
        col.field = [](void* o) -> void* { return &static_cast<SunComponent*>(o)->color; };
        p.push_back(std::move(col));
        Property intensity;
        intensity.label = "Intensity"; intensity.key = "intensity"; intensity.kind = PropKind::Float;
        intensity.slider = true; intensity.min = 0.0f; intensity.max = 3.0f;
        intensity.field = [](void* o) -> void* { return &static_cast<SunComponent*>(o)->intensity; };
        p.push_back(std::move(intensity));
        return p;
    }();
    return props;
}

// Geometry as two compact blobs, for the same reason the terrain's sculpt cells
// are stored that way: a JSON array per coordinate would turn a modest shape
// into thousands of pretty-printed lines. Vertices are "x y z ...", faces are
// "n i0..in-1 ..." -- a corner count followed by that many indices, which is
// what lets a mesh of quads and the odd triangle share one stream.
void MeshComponent::save(nlohmann::json& j) const {
    std::ostringstream vs;
    vs.precision(7);
    for (const glm::vec3& v : mesh.verts) vs << v.x << ' ' << v.y << ' ' << v.z << ' ';
    j["verts"] = vs.str();
    std::ostringstream fs;
    for (const std::vector<int>& f : mesh.faces) {
        fs << f.size() << ' ';
        for (int i : f) fs << i << ' ';
    }
    j["faces"] = fs.str();
    // Per-face materials, only where a face wears one of its own: "face guid",
    // sparse for the same reason the paint is -- most faces wear the object's
    // material, and saying so face by face would put a GUID in every scene file
    // to say nothing.
    if (mesh.dressed()) {
        std::ostringstream ms;
        for (std::size_t i = 0; i < mesh.faceMat.size(); ++i) {
            if (!mesh.faceMat[i].valid()) continue;
            ms << i << ' ' << mesh.faceMat[i].toString() << ' ';
        }
        j["faceMats"] = ms.str();
    }
    // Where a face's texture sits, only for the faces somebody has moved:
    // "face axis sizeU sizeV rot offU offV flags", sparse like the materials
    // above. A mesh nobody has opened the UV panel on writes nothing, and an
    // older scene that says nothing loads as every face at the default -- which
    // is the projection it was authored against.
    if (mesh.unwrapped()) {
        std::ostringstream us;
        us.precision(6);
        for (std::size_t i = 0; i < mesh.faceUV.size(); ++i) {
            const EditMesh::FaceUV& u = mesh.faceUV[i];
            if (u.isDefault()) continue;
            us << i << ' ' << u.axis << ' ' << u.size.x << ' ' << u.size.y << ' '
               << u.rotate << ' ' << u.offset.x << ' ' << u.offset.y << ' '
               << ((u.flipU ? 1 : 0) | (u.flipV ? 2 : 0)) << ' ';
        }
        j["faceUVs"] = us.str();
    }
    // Texture paint, only where there is any: "corner w0 w1 w2 w3", sparse. Most
    // meshes are never painted and most corners of a painted one are not, so a
    // dense array would put four numbers per corner into every scene file to say
    // nothing. Absent means unpainted, which is also what an older scene says.
    if (mesh.painted()) {
        std::ostringstream ps;
        ps.precision(4);
        for (std::size_t i = 0; i < mesh.paint.size(); ++i) {
            const glm::vec4& w = mesh.paint[i];
            if (w.x <= 0.0f && w.y <= 0.0f && w.z <= 0.0f && w.w <= 0.0f) continue;
            ps << i << ' ' << w.x << ' ' << w.y << ' ' << w.z << ' ' << w.w << ' ';
        }
        j["paint"] = ps.str();
    }
    // What those weights mean on this mesh. Written whenever a slot points
    // somewhere, painted or not: filling the slots is a step of the work, and a
    // reload that lost them would leave the weights meaning nothing.
    bool anySlot = false;
    for (const MeshPaintSlot& sl : paintSlots) anySlot = anySlot || sl.material.valid();
    if (anySlot) {
        nlohmann::json slots = nlohmann::json::array();
        for (const MeshPaintSlot& sl : paintSlots) {
            nlohmann::json e;
            if (sl.material.valid()) e["material"] = sl.material.toString();
            e["scale"] = sl.scale;
            slots.push_back(std::move(e));
        }
        j["paintSlots"] = std::move(slots);
    }
}

void MeshComponent::load(const nlohmann::json& j) {
    mesh.verts.clear();
    mesh.faces.clear();
    mesh.faceMat.clear();
    mesh.faceUV.clear();
    if (j.contains("verts") && j["verts"].is_string()) {
        std::istringstream vs(j["verts"].get<std::string>());
        glm::vec3 v;
        while (vs >> v.x >> v.y >> v.z) mesh.verts.push_back(v);
    }
    if (j.contains("faces") && j["faces"].is_string()) {
        std::istringstream fs(j["faces"].get<std::string>());
        int n = 0;
        while (fs >> n) {
            if (n < 3 || n > 64) break;          // corrupt stream: stop, don't guess
            std::vector<int> f;
            f.reserve(static_cast<std::size_t>(n));
            for (int k = 0; k < n; ++k) {
                int i = -1;
                if (!(fs >> i) || i < 0 || i >= static_cast<int>(mesh.verts.size())) {
                    f.clear();
                    break;
                }
                f.push_back(i);
            }
            if (f.size() != static_cast<std::size_t>(n)) break;
            mesh.faces.push_back(std::move(f));
        }
    }
    // A scene that stored nothing usable still has to draw something.
    if (mesh.faces.empty()) mesh = EditMesh::box(glm::vec3(0.5f));
    mesh.syncFaceMat();
    if (j.contains("faceMats") && j["faceMats"].is_string()) {
        std::istringstream ms(j["faceMats"].get<std::string>());
        std::size_t f = 0;
        std::string guid;
        while (ms >> f >> guid) {
            if (f >= mesh.faceMat.size()) break;  // corrupt stream: stop, don't guess
            mesh.faceMat[f] = fitzel::AssetId::fromString(guid);
        }
    }
    mesh.syncFaceUv();
    if (j.contains("faceUVs") && j["faceUVs"].is_string()) {
        std::istringstream us(j["faceUVs"].get<std::string>());
        std::size_t       f = 0;
        EditMesh::FaceUV  u;
        int               flags = 0;
        while (us >> f >> u.axis >> u.size.x >> u.size.y >> u.rotate >>
                     u.offset.x >> u.offset.y >> flags) {
            if (f >= mesh.faceUV.size()) break;  // corrupt stream: stop, don't guess
            u.flipU = (flags & 1) != 0;
            u.flipV = (flags & 2) != 0;
            mesh.faceUV[f] = u;
        }
    }
    mesh.syncPaint();
    if (j.contains("paint") && j["paint"].is_string()) {
        std::istringstream ps(j["paint"].get<std::string>());
        std::size_t i = 0;
        glm::vec4   w(0.0f);
        while (ps >> i >> w.x >> w.y >> w.z >> w.w) {
            if (i >= mesh.paint.size()) break;   // corrupt stream: stop, don't guess
            mesh.paint[i] = glm::clamp(w, 0.0f, 1.0f);
        }
    }
    for (MeshPaintSlot& sl : paintSlots) sl = MeshPaintSlot{};
    if (j.contains("paintSlots") && j["paintSlots"].is_array()) {
        const nlohmann::json& a = j["paintSlots"];
        for (std::size_t i = 0; i < a.size() && i < paintSlots.size(); ++i) {
            if (a[i].contains("material") && a[i]["material"].is_string())
                paintSlots[i].material =
                    fitzel::AssetId::fromString(a[i]["material"].get<std::string>());
            paintSlots[i].scale = a[i].value("scale", 0.5f);
        }
    }
    touch();
}

const std::vector<Property>& TerrainComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        // Every knob addresses the engine's TerrainSettings through a
        // pointer-to-member, so the component and the streamer stay one struct
        // apart -- there is no second copy of the terrain's definition to keep in
        // step. Ranges match the Terrain panel's sliders, which edits the same
        // fields from the other side.
        auto addF = [&](const char* label, const char* key,
                        float fitzel::TerrainSettings::* m,
                        float lo, float hi, const char* fmt) {
            Property f; f.label = label; f.key = key; f.kind = PropKind::Float;
            f.slider = true; f.min = lo; f.max = hi; f.speed = 0.01f; f.fmt = fmt;
            f.field = [m](void* o) -> void* {
                return &(static_cast<TerrainComponent*>(o)->settings.*m);
            };
            p.push_back(std::move(f));
        };
        auto addI = [&](const char* label, const char* key,
                        int fitzel::TerrainSettings::* m, int lo, int hi) {
            Property f; f.label = label; f.key = key; f.kind = PropKind::Int;
            f.slider = true; f.min = float(lo); f.max = float(hi); f.speed = 1.0f;
            f.field = [m](void* o) -> void* {
                return &(static_cast<TerrainComponent*>(o)->settings.*m);
            };
            p.push_back(std::move(f));
        };
        // Relief
        addF("Height",      "heightScale",  &fitzel::TerrainSettings::heightScale,   0.0f, 30.0f,  "%.1f");
        addF("Ridges",      "ridgeScale",   &fitzel::TerrainSettings::ridgeScale,    0.0f, 60.0f,  "%.1f");
        addF("Continents",  "continentAmp", &fitzel::TerrainSettings::continentAmp,  0.0f, 4.0f,   "%.2f");
        addF("Region size", "biomeFreq",    &fitzel::TerrainSettings::biomeFreq,     0.0002f, 0.01f, "%.4f");
        addF("Terracing",   "terrace",      &fitzel::TerrainSettings::terrace,       0.0f, 1.0f,   "%.2f");
        addF("Warp",        "warpStrength", &fitzel::TerrainSettings::warpStrength,  0.0f, 60.0f,  "%.1f");
        addF("Warp size",   "warpFrequency",&fitzel::TerrainSettings::warpFrequency, 0.001f, 0.05f, "%.4f");
        addF("Detail",      "frequency",    &fitzel::TerrainSettings::frequency,     0.001f, 0.06f, "%.4f");
        addI("Octaves",     "octaves",      &fitzel::TerrainSettings::octaves,       1, 10);
        addF("Lacunarity",  "lacunarity",   &fitzel::TerrainSettings::lacunarity,    1.2f, 4.0f,   "%.2f");
        addF("Gain",        "gain",         &fitzel::TerrainSettings::gain,          0.1f, 0.9f,   "%.2f");
        addF("Seed",        "seed",         &fitzel::TerrainSettings::seed,       -1000.0f, 1000.0f,"%.0f");
        // Epic shaping
        addF("Valleys",     "valleyDepth",  &fitzel::TerrainSettings::valleyDepth,   0.0f, 60.0f,  "%.1f");
        addF("Peaks",       "peakSharpness",&fitzel::TerrainSettings::peakSharpness, 0.4f, 3.0f,   "%.2f");
        addF("Relief gain", "reliefGain",   &fitzel::TerrainSettings::reliefGain,    0.2f, 3.0f,   "%.2f");
        // Island mask (radius 0 = an endless field, the default)
        addF("Island radius","islandRadius", &fitzel::TerrainSettings::islandRadius,  0.0f, 4000.0f,"%.0f m");
        addF("Island X",    "islandCenterX",&fitzel::TerrainSettings::islandCenterX,-4000.0f, 4000.0f,"%.0f");
        addF("Island Z",    "islandCenterZ",&fitzel::TerrainSettings::islandCenterZ,-4000.0f, 4000.0f,"%.0f");
        addF("Atoll",       "islandShape",  &fitzel::TerrainSettings::islandShape,   0.0f, 1.0f,   "%.2f");
        // Streaming granularity -- what the ground is cut into, not what it looks
        // like. Last, because changing either rebuilds every loaded chunk.
        addF("Chunk size",  "chunkSize",    &fitzel::TerrainSettings::chunkSize,     16.0f, 256.0f,"%.0f m");
        addI("Chunk detail","resolution",   &fitzel::TerrainSettings::resolution,    8, 192);
        return p;
    }();
    return props;
}

namespace {
// Register built-in component types at startup.
struct AutoRegister {
    AutoRegister() {
        components::registerType({"spin", "Spin",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<SpinComponent>()); }});
        components::registerType({"animator", "Animator",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<AnimatorComponent>()); }});
        components::registerType({"anim_graph", "Animation Graph",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<AnimGraphComponent>()); }});
        components::registerType({"collectible", "Collectible",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<CollectibleComponent>()); }});
        components::registerType({"missile_pickup", "Missile Pickup",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<MissilePickupComponent>()); }});
        components::registerType({"energy_pickup", "Energy Pickup",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<EnergyPickupComponent>()); }});
        components::registerType({"trigger", "Trigger",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<TriggerComponent>()); }});
        components::registerType({"scene_trigger", "Scene Trigger",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<SceneTriggerComponent>()); }});
        components::registerType({"trigger_sound", "Trigger Sound",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<TriggerSoundComponent>()); }});
        components::registerType({"audio_source", "Audio Source",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<AudioSourceComponent>()); }});
        components::registerType({"mover", "Mover",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<MoverComponent>()); }});
        components::registerType({"spawner", "Spawner",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<SpawnerComponent>()); }});
        components::registerType({"pusher", "Pusher",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<PusherComponent>()); }});
        components::registerType({"vehicle", "Vehicle",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<VehicleComponent>()); }});
        components::registerType({"glider", "Glider",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<GliderComponent>()); }});
        components::registerType({"particles", "Particles",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<ParticleComponent>()); }});
        components::registerType({"boost_pad", "Boost Pad",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<BoostPadComponent>()); }});
        components::registerType({"opponent", "Opponent",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<OpponentComponent>()); }});
        components::registerType({"finish_line", "Start/Finish",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<FinishLineComponent>()); }});
        components::registerType({"checkpoint", "Checkpoint",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<CheckpointComponent>()); }});
        components::registerType({"showroom", "Showroom",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<ShowroomComponent>()); }});
        components::registerType({"craft_entry", "Craft Entry",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<CraftEntryComponent>()); }});
        components::registerType({"track_entry", "Track Entry",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<TrackEntryComponent>()); }});
        components::registerType({"door", "Door",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<DoorComponent>()); }});
        components::registerType({"door_opener", "Door Opener",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<DoorOpenerComponent>()); }});
        components::registerType({"lift", "Lift",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<LiftComponent>()); }});
        components::registerType({"camera", "Camera",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<CameraComponent>()); }});
        components::registerType({"camera_switcher", "Camera Switcher",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<CameraSwitcherComponent>()); }});
        components::registerType({"animation", "Animation",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<AnimationComponent>()); }});
        components::registerType({"animation_trigger", "Animation Trigger",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<AnimationTriggerComponent>()); }});
        components::registerType({"script", "Script",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<ScriptComponent>()); }});
        components::registerType({"light", "Light",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<LightComponent>()); }});
        components::registerType({"material", "Material",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<MaterialComponent>()); }});
        components::registerType({"model", "Model",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<ModelComponent>()); },
            /*addable=*/false});
        components::registerType({"prefab", "Prefab Instance",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<PrefabComponent>()); },
            /*addable=*/false});
        components::registerType({"physics", "Physics",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<PhysicsComponent>()); }});
        components::registerType({"soft_body", "Soft Body",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<SoftBodyComponent>()); }});
        components::registerType({"player_start", "Player Start",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<PlayerStartComponent>()); }});
        // Not in the Add Component menu: a mesh replaces what an entity draws, so
        // it is made by "Make editable" / the Mesh button, which also squares the
        // entity's half-extents with the geometry.
        components::registerType({"mesh", "Mesh",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<MeshComponent>()); },
            /*addable=*/false});
        components::registerType({"terrain", "Terrain",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<TerrainComponent>()); }});
        components::registerType({"volumetric_fog", "Volumetric Fog",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<VolumetricFogComponent>()); }});
        components::registerType({"sun", "Sun",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<SunComponent>()); },
            /*addable=*/false});
    }
} g_autoRegister;
} // namespace
