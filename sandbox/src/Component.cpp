#include "Component.hpp"

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

bool componentsEqual(const ComponentList& a, const ComponentList& b) {
    if (a.items.size() != b.items.size()) return false;
    for (std::size_t i = 0; i < a.items.size(); ++i) {
        const ComponentBase* ca = a.items[i].get();
        const ComponentBase* cb = b.items[i].get();
        if (std::string(ca->typeId()) != cb->typeId()) return false;
        // Value-compare by serializing each component's fields.
        nlohmann::json ja, jb;
        writeProps(ja, ca->props(), ca);
        writeProps(jb, cb->props(), cb);
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
        components::registerType({"script", "Script",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<ScriptComponent>()); }});
        components::registerType({"light", "Light",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<LightComponent>()); }});
        components::registerType({"player_start", "Player Start",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<PlayerStartComponent>()); }});
        components::registerType({"sun", "Sun",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<SunComponent>()); },
            /*addable=*/false});
    }
} g_autoRegister;
} // namespace
