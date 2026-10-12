#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "SceneTypes.hpp"
#include "ScriptHost.hpp"
#include "ScriptParam.hpp"

class ScriptComponent;
struct CsManagedApi;

// C# entity scripting: the second script backend beside Lua (ScriptSystem).
//
// .NET 10 is hosted through hostfxr (the installed shared runtime, found by
// nethost). It starts once, on the first C# script the editor meets, and loads
// Fitzel.dll from <exe>/dotnet/ -- the managed half of the API (sandbox/dotnet).
// The game's own scripts are one assembly, Assembly-CSharp.dll, which the
// project's .csproj builds into Library/ScriptAssemblies/ (CSharpProject.hpp):
// read as BYTES (so a build is never blocked by a locked file, and an exported
// game can carry it in its .fpak) into a collectible load context, which is
// thrown away whole when Play starts again.
//
// A Script component whose file ends in .cs names a class: Rotator.cs ->
// class Rotator : Fitzel.Behaviour. Each (entity, file) pair gets its own
// instance, keyed exactly as ScriptSystem keys its Lua environments, and the
// same rules hold: Start before the first Update, an exception disables that
// one script until the next Play, the inspector's parameters are the class's
// public fields. The transform a script reads and writes is the entity's own,
// in place (localCenter / localRotation / half) -- what Lua's e.x / e.rx / e.sx
// copy back -- so the scene graph sees a C# move exactly as it sees a Lua one.
class CSharpScripts {
public:
    CSharpScripts();
    ~CSharpScripts();
    CSharpScripts(const CSharpScripts&)            = delete;
    CSharpScripts& operator=(const CSharpScripts&) = delete;

    // The scene's objects (read and written in place during a call) and the
    // host bridge (everything else). Both outlive this.
    void setEntities(std::vector<Entity>* list) { m_entities = list; }
    void setHost(ScriptHost* h) { m_host = h; }
    // A Lua script holds the keys (game.captureInput): C# then sees none either.
    std::function<bool()> inputBlocked;
    // The whole Lua API, for everything C# does not do directly: table.fn with
    // JSON arguments, JSON results (ScriptSystem::callApi). `who` = the script.
    std::function<bool(const std::string& table, const std::string& fn, const std::string& argsJson,
                       const std::string& who, std::string& resultJson)> callLua;
    // Names the running C# script to the Lua side (captureInput), "" after.
    std::function<void(const std::string& who)> enterScript;

    // The game's script assembly (CSharpScripts::assemblyFor). "" = none.
    void setAssembly(const std::string& dllPath);
    static std::string assemblyFor(const std::string& projectDir);
    // Where Fitzel.dll lives: <exe dir>/dotnet.
    static std::filesystem::path engineDir();

    // Play starts: every instance goes (OnDestroy runs), the assembly is read
    // again from disk on the first script -- whatever the last build made.
    void reset();
    // Play stops: the same, so the editor's view of the classes is the newest.
    void stop() { reset(); }

    // One script component's frame: create + Start on the first call, Update
    // after. Does nothing for a script that failed.
    void update(Entity& e, const ScriptComponent& sc, float dt, float time);
    void removeEntity(int id);

    // The inspector's fields of the class `file` names (Rotator.cs -> Rotator),
    // with their default values. Empty + `err` when there is no such class or
    // no build to read it from.
    std::vector<ScriptParam> scanParams(const std::string& file, std::string* err = nullptr);
    // When the assembly the scan reads last changed (for the inspector's cache).
    std::filesystem::file_time_type assemblyTime() const;

    // Most recent C# error ("" if none) -- the Script card shows it.
    const std::string& lastError() const { return m_lastError; }
    // The runtime could not start (no .NET 10, no Fitzel.dll): why. "" = fine.
    const std::string& runtimeProblem() const { return m_runtimeProblem; }

    // --- Called by the C ABI (CSharpHost.cpp) --------------------------------
    Entity*     entity(int id);
    ScriptHost* host() const { return m_host; }
    void        reportError(const std::string& msg);
    const std::string& currentScript() const { return m_current; }

private:
    bool startRuntime();   // once; false + m_runtimeProblem when it cannot
    bool ensureLoaded();   // the assembly as it is on disk now (not while instances live)
    void fail(const std::string& key);

    std::vector<Entity>* m_entities = nullptr;
    ScriptHost*          m_host     = nullptr;
    std::size_t          m_hint     = 0;      // entity(): where the last lookup hit

    bool              m_tried = false;        // startRuntime ran (successfully or not)
    CsManagedApi*     m_api   = nullptr;      // the managed function table (null = no runtime)
    std::string       m_runtimeProblem;

    std::string m_dllPath;
    bool        m_loaded = false;             // the managed side holds an assembly
    std::filesystem::file_time_type m_loadedTime{};

    std::unordered_map<std::string, int> m_handles;   // "<id>|<file>" -> instance
    std::unordered_set<std::string>      m_failed;
    std::string                          m_lastError;
    std::string                          m_current;   // "cs|<id>|<file>" during a call
};
