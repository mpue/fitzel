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

namespace {
// Register built-in component types at startup.
struct AutoRegister {
    AutoRegister() {
        components::registerType({"spin", "Spin",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<SpinComponent>()); }});
    }
} g_autoRegister;
} // namespace
