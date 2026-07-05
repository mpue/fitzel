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
