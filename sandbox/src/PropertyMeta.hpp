#pragma once

#include <functional>
#include <string>
#include <vector>

#include "SceneTypes.hpp"

// Editor-facing metadata for the uniform, scalar-ish fields of an Entity. Each
// field is declared once here; from that single declaration the Inspector is
// generated (and, next, scene serialization). Adding a field or a content type
// then costs a table entry -- not hand-written panel code plus save/load code.
//
// Bespoke fields whose widgets enumerate project state (material picker, script
// picker, model reference) stay custom in the Inspector; everything numeric,
// vector, colour, boolean or enum lives in this table.

enum class PropKind { Text, Float, Vec3, Color, Bool, EnumInt };

struct Property {
    std::string label;                       // Inspector label
    std::string key;                         // JSON key (for serialization)
    PropKind    kind = PropKind::Float;
    unsigned    typeMask = ~0u;              // bit(1<<EntityType) per applicable type
    bool        slider = false;              // SliderFloat vs DragFloat (Float only)
    float       min = 0.0f, max = 0.0f, speed = 0.1f;
    std::string fmt;                         // printf-style ("" = ImGui default)
    std::vector<std::string> enumLabels;     // EnumInt choices
    // Address of the field inside a given entity (kind says how to read it).
    std::function<void*(Entity&)> field;
    // Optional extra visibility predicate (e.g. Mass only when Dynamic).
    std::function<bool(const Entity&)> visible;
};

// Bit for an EntityType in a Property's typeMask.
constexpr unsigned typeBit(EntityType t) { return 1u << static_cast<unsigned>(t); }

// The Entity property table, built once.
const std::vector<Property>& entityProperties();
