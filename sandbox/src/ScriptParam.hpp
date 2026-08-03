#pragma once

#include <string>

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

// One script-exported parameter: a module-level Lua global (declared without
// `local`, so it lives in the script's environment) that the inspector surfaces
// as an editable, persisted field. The script author writes a plain default --
//
//   speed  = 5.0          -- a Number slider/drag in the inspector
//   loops  = true         -- a checkbox
//   label  = "hello"      -- a text field
//   colour = {1, 0.5, 0}  -- a colour / vector picker
//
// -- and the value set here overrides that default when Play starts (see
// ScriptSystem::scanParams / loadFor). `type` selects which value member is live;
// the others are dormant. Kept Lua-free so Component.hpp can hold a list of these
// without dragging the VM into every translation unit.
struct ScriptParam {
    enum class Type { Number, Bool, String, Vec3, Color };

    std::string name;                 // the Lua global's name (the override key)
    Type        type = Type::Number;
    double      num  = 0.0;           // Type::Number
    bool        b    = false;         // Type::Bool
    std::string str;                  // Type::String
    glm::vec3   vec{0.0f};            // Type::Vec3 / Type::Color

    bool sameShape(const ScriptParam& o) const {
        return name == o.name && type == o.type;
    }
};

// JSON round-trip (defined in Component.cpp, which already links nlohmann::json).
void to_json(nlohmann::json& j, const ScriptParam& p);
void from_json(const nlohmann::json& j, ScriptParam& p);
