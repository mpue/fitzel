#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>

#include <vector>

#include "SceneTypes.hpp"
#include "ScriptHost.hpp"
#include "ScriptParam.hpp"

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

    // Drop an entity's script state (call when the entity is destroyed at
    // runtime) so its environment ref is freed and a reused id can't inherit it.
    void removeEntity(int id);

    // The host bridge backing the Lua `game` table. Set once after construction;
    // the C functions read it at call time (see installApi).
    void        setHost(ScriptHost* h) { m_host = h; }
    ScriptHost* host() const { return m_host; }

    // Most recent script error ("" if none) -- shown in the editor UI.
    const std::string& lastError() const { return m_lastError; }

    // Editor introspection: load `path` in a throwaway sandbox, run its chunk,
    // and return one ScriptParam (with its default value) for every module-level
    // global of a supported type -- the fields the inspector then exposes. Runs
    // the module body once; `game.*` is stubbed to a harmless no-op so a script
    // that touches the API at load time won't fault. On a load/parse error the
    // list is empty and `err` (if given) holds the message. Does not touch the
    // play VM or any entity state.
    std::vector<ScriptParam> scanParams(const std::string& path,
                                        std::string* err = nullptr);

private:
    void installApi();                                      // build global `game`
    bool loadFor(const Entity& e, const std::string& path); // chunk -> env, start()
    void applyParams(int envRef, const Entity& e);          // overrides -> env globals
    bool callFunction(Entity& e, const char* fn, float dt, float time);
    void pushEntityTable(const Entity& e);
    void readEntityTable(Entity& e); // reads the table at the stack top
    void fail(int id, const char* what);

    lua_State*  m_lua  = nullptr;
    ScriptHost* m_host = nullptr;          // host bridge for the `game` table
    std::unordered_map<int, int> m_env;    // entity id -> registry ref of its env
    std::unordered_set<int>      m_failed; // scripts disabled by an error
    std::string                  m_lastError;
};
