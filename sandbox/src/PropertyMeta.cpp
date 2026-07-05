#include "PropertyMeta.hpp"

#include <cstdio>

#include <glm/glm.hpp>
#include <imgui.h>

namespace {

using E = EntityType;

constexpr unsigned bit(E t) { return 1u << static_cast<unsigned>(t); }
const unsigned SOLID    = bit(E::Box) | bit(E::Ramp) | bit(E::Cylinder) | bit(E::Sphere);
const unsigned MOVABLE  = SOLID | bit(E::Model) | bit(E::Light); // has a world position
const unsigned ALLTYPES = ~0u;

// Accessor helpers: build a `void*(void*)` that returns the address of member M
// of an Entity given the entity's address.
template <class T>
std::function<void*(void*)> at(T Entity::* m) {
    return [m](void* o) -> void* { return &(static_cast<Entity*>(o)->*m); };
}

} // namespace

const std::vector<Property>& entityProperties() {
    static const std::vector<Property> props = [] {
        std::vector<Property> p;

        Property name;
        name.label = "Name"; name.key = "name"; name.kind = PropKind::Text;
        name.typeMask = ALLTYPES; name.field = at(&Entity::name);
        p.push_back(std::move(name));

        // Position/Rotation edit the LOCAL transform (relative to the parent);
        // world is derived by the scene-graph resolve. Serialized under the same
        // keys, so a saved scene stores local transforms.
        Property pos;
        pos.label = "Position"; pos.key = "center"; pos.kind = PropKind::Vec3;
        pos.typeMask = MOVABLE; pos.speed = 0.05f; pos.field = at(&Entity::localCenter);
        p.push_back(std::move(pos));

        Property rot;
        rot.label = "Rotation"; rot.key = "rotation"; rot.kind = PropKind::Vec3;
        rot.typeMask = SOLID | bit(E::Model); rot.speed = 1.0f; rot.fmt = "%.0f deg";
        rot.field = at(&Entity::localRotation);
        p.push_back(std::move(rot));

        Property half;
        half.label = "Half size"; half.key = "half"; half.kind = PropKind::Vec3;
        half.typeMask = SOLID; half.speed = 0.02f; half.min = 0.05f; half.max = 60.0f;
        half.field = at(&Entity::half);
        p.push_back(std::move(half));

        Property scale;
        scale.label = "Scale"; scale.key = "scale"; scale.kind = PropKind::Float;
        scale.typeMask = bit(E::Model); scale.slider = true;
        scale.min = 0.05f; scale.max = 20.0f; scale.fmt = "%.2f";
        scale.field = at(&Entity::scale);
        p.push_back(std::move(scale));

        // Physics (collider/mass) now lives in PhysicsComponent; light/sun look
        // in LightComponent / SunComponent.

        return p;
    }();
    return props;
}

bool drawProperty(const Property& p, void* owner) {
    void* f = p.field(owner);
    const char* fmt = p.fmt.empty() ? "%.3f" : p.fmt.c_str();
    switch (p.kind) {
        case PropKind::Text: {
            auto* s = static_cast<std::string*>(f);
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s", s->c_str());
            if (ImGui::InputText(p.label.c_str(), buf, sizeof(buf))) { *s = buf; return true; }
            return false;
        }
        case PropKind::Float: {
            float* v = static_cast<float*>(f);
            return p.slider
                ? ImGui::SliderFloat(p.label.c_str(), v, p.min, p.max, fmt)
                : ImGui::DragFloat(p.label.c_str(), v, p.speed, p.min, p.max, fmt);
        }
        case PropKind::Vec3:
            return ImGui::DragFloat3(p.label.c_str(),
                                     &static_cast<glm::vec3*>(f)->x,
                                     p.speed, p.min, p.max, fmt);
        case PropKind::Color:
            return ImGui::ColorEdit3(p.label.c_str(), &static_cast<glm::vec3*>(f)->x);
        case PropKind::Bool:
            return ImGui::Checkbox(p.label.c_str(), static_cast<bool*>(f));
        case PropKind::EnumInt: {
            std::vector<const char*> items;
            for (const std::string& s : p.enumLabels) items.push_back(s.c_str());
            return ImGui::Combo(p.label.c_str(), static_cast<int*>(f), items.data(),
                                static_cast<int>(items.size()));
        }
    }
    return false;
}

void writeProps(nlohmann::json& j, const std::vector<Property>& props, const void* owner) {
    for (const Property& p : props) {
        void* f = p.field(const_cast<void*>(owner));
        switch (p.kind) {
            case PropKind::Text:    j[p.key] = *static_cast<std::string*>(f); break;
            case PropKind::Float:   j[p.key] = *static_cast<float*>(f); break;
            case PropKind::Vec3:
            case PropKind::Color: {
                const glm::vec3* v = static_cast<glm::vec3*>(f);
                j[p.key] = nlohmann::json::array({v->x, v->y, v->z}); break;
            }
            case PropKind::Bool:    j[p.key] = *static_cast<bool*>(f); break;
            case PropKind::EnumInt: j[p.key] = *static_cast<int*>(f); break;
        }
    }
}

void readProps(const nlohmann::json& j, const std::vector<Property>& props, void* owner) {
    for (const Property& p : props) {
        if (!j.contains(p.key)) continue;
        const nlohmann::json& val = j.at(p.key);
        void* f = p.field(owner);
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

void writeEntityProps(nlohmann::json& j, const Entity& e) { writeProps(j, entityProperties(), &e); }
void readEntityProps(const nlohmann::json& j, Entity& e)  { readProps(j, entityProperties(), &e); }
