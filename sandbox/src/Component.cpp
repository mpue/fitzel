#include "Component.hpp"

#include <filesystem>

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

void MaterialComponent::save(nlohmann::json& j) const {
    if (material.valid()) j["material"] = material.toString();
}
void MaterialComponent::load(const nlohmann::json& j) {
    if (j.contains("material") && j["material"].is_string())
        material = fitzel::AssetId::fromString(j["material"].get<std::string>());
}

void ModelComponent::save(nlohmann::json& j) const {
    j["scale"]     = scale;
    j["modelFile"] = std::filesystem::path(modelPath).filename().string();
    // The asset ref ("model" GUID) is added by ProjectIO (it has the database).
}
void ModelComponent::load(const nlohmann::json& j) {
    scale = j.value("scale", 1.0f);
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
        Property type;
        type.label = "Spawns"; type.key = "spawnType"; type.kind = PropKind::EnumInt;
        type.enumLabels = {"Box", "Ramp", "Cylinder", "Sphere"};
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
        Property loop;
        loop.label = "Loop (zone)"; loop.key = "loop"; loop.kind = PropKind::Bool;
        loop.field = [](void* o) -> void* { return &static_cast<TriggerSoundComponent*>(o)->loop; };
        p.push_back(std::move(loop));
        Property once;
        once.label = "Once"; once.key = "once"; once.kind = PropKind::Bool;
        once.visible = [](const void* o) { return !static_cast<const TriggerSoundComponent*>(o)->loop; };
        once.field = [](void* o) -> void* { return &static_cast<TriggerSoundComponent*>(o)->once; };
        p.push_back(std::move(once));
        return p;
    }();
    return props;
}

const std::vector<Property>& CameraComponent::properties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;
        Property fov;
        fov.label = "FOV"; fov.key = "fov"; fov.kind = PropKind::Float;
        fov.slider = true; fov.min = 20.0f; fov.max = 120.0f; fov.fmt = "%.0f deg";
        fov.field = [](void* o) -> void* { return &static_cast<CameraComponent*>(o)->fov; };
        p.push_back(std::move(fov));
        Property act;
        act.label = "Active on start"; act.key = "activeOnStart"; act.kind = PropKind::Bool;
        act.field = [](void* o) -> void* { return &static_cast<CameraComponent*>(o)->activeOnStart; };
        p.push_back(std::move(act));
        return p;
    }();
    return props;
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
        Property speed;
        speed.label = "Speed"; speed.key = "speed"; speed.kind = PropKind::Float;
        speed.slider = true; speed.min = 0.0f; speed.max = 4.0f; speed.fmt = "%.2fx";
        speed.field = [](void* o) -> void* { return &static_cast<AnimationComponent*>(o)->speed; };
        p.push_back(std::move(speed));
        Property playing;
        playing.label = "Playing"; playing.key = "playing"; playing.kind = PropKind::Bool;
        playing.field = [](void* o) -> void* { return &static_cast<AnimationComponent*>(o)->playing; };
        p.push_back(std::move(playing));
        Property loop;
        loop.label = "Loop"; loop.key = "loop"; loop.kind = PropKind::Bool;
        loop.field = [](void* o) -> void* { return &static_cast<AnimationComponent*>(o)->loop; };
        p.push_back(std::move(loop));
        return p;
    }();
    return props;
}

// Persists speed/playing/loop (properties) plus the chosen clip index.
void AnimationComponent::save(nlohmann::json& j) const {
    writeProps(j, props(), this);
    j["clip"] = clip;
}
void AnimationComponent::load(const nlohmann::json& j) {
    readProps(j, props(), this);
    clip = j.value("clip", 0);
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

namespace {
// Register built-in component types at startup.
struct AutoRegister {
    AutoRegister() {
        components::registerType({"spin", "Spin",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<SpinComponent>()); }});
        components::registerType({"collectible", "Collectible",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<CollectibleComponent>()); }});
        components::registerType({"trigger", "Trigger",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<TriggerComponent>()); }});
        components::registerType({"trigger_sound", "Trigger Sound",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<TriggerSoundComponent>()); }});
        components::registerType({"mover", "Mover",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<MoverComponent>()); }});
        components::registerType({"spawner", "Spawner",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<SpawnerComponent>()); }});
        components::registerType({"pusher", "Pusher",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<PusherComponent>()); }});
        components::registerType({"lift", "Lift",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<LiftComponent>()); }});
        components::registerType({"camera", "Camera",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<CameraComponent>()); }});
        components::registerType({"camera_switcher", "Camera Switcher",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<CameraSwitcherComponent>()); }});
        components::registerType({"animation", "Animation",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<AnimationComponent>()); }});
        components::registerType({"script", "Script",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<ScriptComponent>()); }});
        components::registerType({"light", "Light",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<LightComponent>()); }});
        components::registerType({"material", "Material",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<MaterialComponent>()); }});
        components::registerType({"model", "Model",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<ModelComponent>()); },
            /*addable=*/false});
        components::registerType({"physics", "Physics",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<PhysicsComponent>()); }});
        components::registerType({"player_start", "Player Start",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<PlayerStartComponent>()); }});
        components::registerType({"sun", "Sun",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<SunComponent>()); },
            /*addable=*/false});
    }
} g_autoRegister;
} // namespace
