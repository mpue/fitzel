#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>

#include <vector>

#include "SceneTypes.hpp"
#include "ScriptHost.hpp"
#include "ScriptParam.hpp"

struct lua_State;

// Lua entity scripting for play mode. Each script COMPONENT loads its file into
// its own environment (so globals and chunk locals are private to that script on
// that object) inside one shared VM. Scripts may define:
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

    // Run one script component's update (loading + start() on first call).
    //
    // AN OBJECT MAY CARRY SEVERAL. It used to be one per entity -- the caller
    // asked for the first ScriptComponent and the environment was keyed by
    // entity id -- so a second script card could be attached, looked complete,
    // and silently never ran. The component is passed in rather than looked up
    // because it decides two things this cannot guess: which file to load, and
    // whose inspector parameters to apply.
    //
    // An error is reported once per (entity, file) and disables THAT script
    // until reset(), leaving the object's other scripts running: one broken
    // script should not take its neighbours with it.
    void update(Entity& e, const ScriptComponent& sc, const std::string& scriptPath,
                float dt, float time);

    // Drop an entity's script state -- every script on it (call when the entity
    // is destroyed at runtime) so the environment refs are freed and a reused id
    // cannot inherit them.
    void removeEntity(int id);

    // The host bridge backing the Lua `game` table. Set once after construction;
    // the C functions read it at call time (see installApi).
    void        setHost(ScriptHost* h) { m_host = h; }
    ScriptHost* host() const { return m_host; }

    // Most recent script error ("" if none) -- shown in the editor UI.
    const std::string& lastError() const { return m_lastError; }

    // --- The keys held by one script (game.captureInput) ----------------------
    // A menu a script draws -- an inventory, a map -- wants the keys to itself:
    // the figure must not walk off while the player picks an item. The script
    // asks on every frame it wants them, and from then on every OTHER script sees
    // no keys and no mouse buttons; the host leaves Esc and the mouse look to it
    // as well (inputCaptured). It lets go by no longer asking: a frame without a
    // call releases them, so a script that fails or is switched off can never
    // leave the game deaf. The first to ask holds them until it stops.
    void beginFrame();          // once per Play frame, before the scripts tick
    bool inputCaptured() const { return !m_captureKey.empty(); }
    bool claimInput();          // the running script asks; true = it holds them
    bool inputBlocked() const;  // somebody else holds them

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
    // One running script is an (entity, file) pair, spelled "<id>|<file>". A
    // string key rather than a pair so the two maps stay plain unordered ones;
    // ids never contain the separator.
    static std::string keyOf(int id, const std::string& file);

    void installApi();                                      // build global `game`
    bool loadFor(const Entity& e, const ScriptComponent& sc,
                 const std::string& key, const std::string& path);
    void applyParams(int envRef, const ScriptComponent& sc); // overrides -> globals
    bool callFunction(Entity& e, const std::string& key, const char* fn,
                      float dt, float time);
    void pushEntityTable(const Entity& e);
    void readEntityTable(Entity& e); // reads the table at the stack top
    void fail(const std::string& key, int id, const char* what);

    lua_State*  m_lua  = nullptr;
    ScriptHost* m_host = nullptr;                  // host bridge for `game`
    std::unordered_map<std::string, int> m_env;    // "<id>|<file>" -> env ref
    std::unordered_set<std::string>      m_failed; // scripts disabled by an error
    std::string                          m_lastError;
    std::string m_current;            // the script running right now ("" = none)
    std::string m_captureKey;         // the script holding the keys ("" = none)
    bool        m_captureAsked = false; // ...and it asked again this frame
};
