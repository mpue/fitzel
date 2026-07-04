#pragma once

#include <functional>
#include <string>
#include <vector>

// Editor property metadata: one self-describing field. The same Property drives
// the auto-inspector widget and JSON (de)serialization, for BOTH Entity fields
// and Component fields -- which is why the accessor is over an opaque owner
// pointer (`void*`) rather than a concrete type. The caller passes the address
// of the object the field lives in (an Entity* or a Component*), and the kind
// says how to interpret the returned field pointer.

enum class PropKind { Text, Float, Vec3, Color, Bool, EnumInt };

struct Property {
    std::string label;                    // inspector label
    std::string key;                      // JSON key
    PropKind    kind = PropKind::Float;
    unsigned    typeMask = ~0u;           // (entities only) applicable EntityTypes
    bool        slider = false;           // SliderFloat vs DragFloat (Float only)
    float       min = 0.0f, max = 0.0f, speed = 0.1f;
    std::string fmt;                       // printf-style ("" = ImGui default)
    std::vector<std::string> enumLabels;   // EnumInt choices
    std::function<void*(void*)>       field;   // owner ptr -> field ptr
    std::function<bool(const void*)>  visible; // owner ptr -> shown? ("" = always)
};

// Render one property as its widget against `owner`; returns true if edited.
bool drawProperty(const Property& p, void* owner);

// (De)serialize a property set against `owner`, keyed by Property::key.
// (Forward-declared with nlohmann::json; see PropertyMeta for definitions.)
