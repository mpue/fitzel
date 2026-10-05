#pragma once

#include <nlohmann/json.hpp>

struct lua_State;

// A Lua value <-> JSON, as game.saveData stores it and json.encode writes it.
// Tables with keys 1..n and nothing else become arrays, every other table an
// object with its keys as strings; functions and the like are not data and are
// left out. Nesting is capped at 32. Defined in ScriptSystem.cpp.
namespace luajson {

nlohmann::json toJson(lua_State* L, int idx, int depth = 0);
// Pushes the value (null -> nil).
void push(lua_State* L, const nlohmann::json& j);

} // namespace luajson
