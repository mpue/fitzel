#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>

#include "SceneTypes.hpp"

struct lua_State;

// Lua entity scripting for play mode. Each scripted entity loads its script
// file into its own environment (so globals and chunk locals are per-entity)
// inside one shared VM. Scripts may define:
//
//   function start(e)         -- once, on first update after Play
//   function update(e, dt, t) -- every frame while playing
//
// `e` is a plain table: x/y/z (position), rx/ry/rz (rotation, degrees),
// sx/sy/sz (half extents), name, id. Numeric fields written by the script are
// copied back to the entity after the call.
class ScriptSystem {
public:
    ScriptSystem();
    ~ScriptSystem();
    ScriptSystem(const ScriptSystem&)            = delete;
    ScriptSystem& operator=(const ScriptSystem&) = delete;

    // Fresh VM: all scripts reload and start() runs again (call on Play).
    void reset();

    // Run the entity's script update (loading + start() on first call).
    // Errors are reported once per entity and disable that script until reset().
    void update(Entity& e, const std::string& scriptPath, float dt, float time);

    // Most recent script error ("" if none) -- shown in the editor UI.
    const std::string& lastError() const { return m_lastError; }

private:
    bool loadFor(const Entity& e, const std::string& path); // chunk -> env, start()
    bool callFunction(Entity& e, const char* fn, float dt, float time);
    void pushEntityTable(const Entity& e);
    void readEntityTable(Entity& e); // reads the table at the stack top
    void fail(int id, const char* what);

    lua_State* m_lua = nullptr;
    std::unordered_map<int, int> m_env;    // entity id -> registry ref of its env
    std::unordered_set<int>      m_failed; // scripts disabled by an error
    std::string                  m_lastError;
};
