#include "PropertyMeta.hpp"

namespace {

using E = EntityType;

// Convenience type masks.
constexpr unsigned bit(E t) { return 1u << static_cast<unsigned>(t); }
const unsigned SOLID    = bit(E::Box) | bit(E::Ramp) | bit(E::Cylinder) | bit(E::Sphere);
const unsigned PHYSICAL = SOLID | bit(E::Model);
const unsigned MOVABLE  = SOLID | bit(E::Model) | bit(E::Light); // has a world position
const unsigned ALLTYPES = ~0u;

} // namespace

const std::vector<Property>& entityProperties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;

        Property name;
        name.label = "Name"; name.key = "name"; name.kind = PropKind::Text;
        name.typeMask = ALLTYPES;
        name.field = [](Entity& e) -> void* { return &e.name; };
        p.push_back(std::move(name));

        Property pos;
        pos.label = "Position"; pos.key = "center"; pos.kind = PropKind::Vec3;
        pos.typeMask = MOVABLE; pos.speed = 0.05f;
        pos.field = [](Entity& e) -> void* { return &e.center; };
        p.push_back(std::move(pos));

        Property rot;
        rot.label = "Rotation"; rot.key = "rotation"; rot.kind = PropKind::Vec3;
        rot.typeMask = SOLID | bit(E::Model); rot.speed = 1.0f; rot.fmt = "%.0f deg";
        rot.field = [](Entity& e) -> void* { return &e.rotation; };
        p.push_back(std::move(rot));

        Property half;
        half.label = "Half size"; half.key = "half"; half.kind = PropKind::Vec3;
        half.typeMask = SOLID; half.speed = 0.02f; half.min = 0.05f; half.max = 60.0f;
        half.field = [](Entity& e) -> void* { return &e.half; };
        p.push_back(std::move(half));

        Property scale;
        scale.label = "Scale"; scale.key = "scale"; scale.kind = PropKind::Float;
        scale.typeMask = bit(E::Model); scale.slider = true;
        scale.min = 0.05f; scale.max = 20.0f; scale.fmt = "%.2f";
        scale.field = [](Entity& e) -> void* { return &e.scale; };
        p.push_back(std::move(scale));

        Property phys;
        phys.label = "Physics"; phys.key = "physics"; phys.kind = PropKind::EnumInt;
        phys.typeMask = PHYSICAL;
        phys.enumLabels = {"None", "Static", "Dynamic"};
        phys.field = [](Entity& e) -> void* { return &e.physics; };
        p.push_back(std::move(phys));

        Property mass;
        mass.label = "Mass"; mass.key = "mass"; mass.kind = PropKind::Float;
        mass.typeMask = PHYSICAL; mass.min = 0.01f; mass.max = 1000.0f;
        mass.speed = 0.1f; mass.fmt = "%.2f kg";
        mass.field = [](Entity& e) -> void* { return &e.mass; };
        mass.visible = [](const Entity& e) { return e.physics == 2; };
        p.push_back(std::move(mass));

        Property lcol;
        lcol.label = "Light colour"; lcol.key = "color"; lcol.kind = PropKind::Color;
        lcol.typeMask = bit(E::Light);
        lcol.field = [](Entity& e) -> void* { return &e.color; };
        p.push_back(std::move(lcol));

        Property lint;
        lint.label = "Intensity"; lint.key = "intensity"; lint.kind = PropKind::Float;
        lint.typeMask = bit(E::Light); lint.slider = true; lint.min = 0.0f; lint.max = 30.0f;
        lint.field = [](Entity& e) -> void* { return &e.intensity; };
        p.push_back(std::move(lint));

        Property range;
        range.label = "Range"; range.key = "range"; range.kind = PropKind::Float;
        range.typeMask = bit(E::Light); range.slider = true;
        range.min = 0.5f; range.max = 60.0f; range.fmt = "%.1f m";
        range.field = [](Entity& e) -> void* { return &e.range; };
        p.push_back(std::move(range));

        Property shadows;
        shadows.label = "Cast shadows"; shadows.key = "castShadows";
        shadows.kind = PropKind::Bool; shadows.typeMask = bit(E::Light);
        shadows.field = [](Entity& e) -> void* { return &e.castShadows; };
        p.push_back(std::move(shadows));

        Property bias;
        bias.label = "Shadow bias"; bias.key = "shadowBias"; bias.kind = PropKind::Float;
        bias.typeMask = bit(E::Light); bias.slider = true;
        bias.min = 0.0f; bias.max = 0.03f; bias.fmt = "%.4f";
        bias.field = [](Entity& e) -> void* { return &e.shadowBias; };
        bias.visible = [](const Entity& e) { return e.castShadows; };
        p.push_back(std::move(bias));

        Property scol;
        scol.label = "Sun colour"; scol.key = "color"; scol.kind = PropKind::Color;
        scol.typeMask = bit(E::Sun);
        scol.field = [](Entity& e) -> void* { return &e.color; };
        p.push_back(std::move(scol));

        Property sint;
        sint.label = "Intensity"; sint.key = "intensity"; sint.kind = PropKind::Float;
        sint.typeMask = bit(E::Sun); sint.slider = true; sint.min = 0.0f; sint.max = 3.0f;
        sint.field = [](Entity& e) -> void* { return &e.intensity; };
        p.push_back(std::move(sint));

        return p;
    }();
    return props;
}

void writeEntityProps(nlohmann::json& j, const Entity& e) {
    for (const Property& p : entityProperties()) {
        void* f = p.field(const_cast<Entity&>(e)); // read-only use
        switch (p.kind) {
            case PropKind::Text:
                j[p.key] = *static_cast<std::string*>(f); break;
            case PropKind::Float:
                j[p.key] = *static_cast<float*>(f); break;
            case PropKind::Vec3:
            case PropKind::Color: {
                const glm::vec3* v = static_cast<glm::vec3*>(f);
                j[p.key] = nlohmann::json::array({v->x, v->y, v->z}); break;
            }
            case PropKind::Bool:
                j[p.key] = *static_cast<bool*>(f); break;
            case PropKind::EnumInt:
                j[p.key] = *static_cast<int*>(f); break;
        }
    }
}

void readEntityProps(const nlohmann::json& j, Entity& e) {
    for (const Property& p : entityProperties()) {
        if (!j.contains(p.key)) continue;
        const nlohmann::json& val = j.at(p.key);
        void* f = p.field(e);
        switch (p.kind) {
            case PropKind::Text:
                if (val.is_string()) *static_cast<std::string*>(f) = val.get<std::string>();
                break;
            case PropKind::Float:
                if (val.is_number()) *static_cast<float*>(f) = val.get<float>();
                break;
            case PropKind::Vec3:
            case PropKind::Color:
                if (val.is_array() && val.size() == 3) {
                    glm::vec3* v = static_cast<glm::vec3*>(f);
                    v->x = val[0].get<float>();
                    v->y = val[1].get<float>();
                    v->z = val[2].get<float>();
                }
                break;
            case PropKind::Bool:
                if (val.is_boolean()) *static_cast<bool*>(f) = val.get<bool>();
                break;
            case PropKind::EnumInt:
                if (val.is_number_integer()) *static_cast<int*>(f) = val.get<int>();
                break;
        }
    }
}
