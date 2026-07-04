#pragma once

#include <vector>

#include <nlohmann/json.hpp>

#include "Property.hpp"
#include "SceneTypes.hpp"

// The Entity's field metadata (see Property.hpp for the shape of a Property).
// One declaration per field drives the auto-inspector AND scene serialization.
// Bespoke fields whose widgets enumerate project state (material/script pickers,
// model reference) stay custom in the Inspector; everything scalar/vector/enum
// lives in this table.

// Bit for an EntityType in a Property's typeMask.
constexpr unsigned typeBit(EntityType t) { return 1u << static_cast<unsigned>(t); }

// The Entity property table, built once.
const std::vector<Property>& entityProperties();

// Generic property (de)serialization: apply a property set to/from a JSON object
// against an opaque owner (an Entity* or a Component*). Reused for both.
void writeProps(nlohmann::json& j, const std::vector<Property>& props, const void* owner);
void readProps(const nlohmann::json& j, const std::vector<Property>& props, void* owner);

// Convenience wrappers for an entity's table-covered fields.
void writeEntityProps(nlohmann::json& j, const Entity& e);
void readEntityProps(const nlohmann::json& j, Entity& e);
