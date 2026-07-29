#pragma once

#include <functional>
#include <string>

#include <glm/glm.hpp>

#include "ScriptHost.hpp"

class Document;
class ModelLibrary;
namespace fitzel { class AssetDatabase; }

// Wires the data-driven half of the Lua `game` API onto a ScriptHost: the asset
// database, the model library, the material library, entity queries and the
// world helpers. Everything here is expressible from the document + the asset
// database alone, which is why it lives outside main() -- main only supplies the
// handful of hooks that need its own state (scene-graph rebasing, terrain,
// scene loading, the console).
//
// The callbacks capture `deps` by value, so the referenced Document /
// ModelLibrary / AssetDatabase must outlive the ScriptHost (they all live for
// the whole session in main).
namespace scriptbridge {

struct Deps {
    Document*              doc     = nullptr;
    ModelLibrary*          models  = nullptr;
    fitzel::AssetDatabase* assetDb = nullptr;

    // Set an entity's WORLD rotation (main converts it back to the local
    // transform, which is the source of truth).
    std::function<void(int, glm::vec3)>     setWorldRot;
    // Re-parent an entity (-1 = detach) keeping its current world transform.
    std::function<void(int, int)>           reparent;
    // Terrain height at a world XZ.
    std::function<float(float, float)>      terrainHeight;
    // Load another scene of the open project by name (deferred by the host).
    std::function<void(const std::string&)> loadScene;
    // Print a line to the console / editor log.
    std::function<void(const std::string&)> log;
};

// Install the asset / model / material / entity-query / world callbacks.
// Callbacks main sets itself (input, spawn, physics, audio, camera) are left
// untouched, so the call order between this and main's wiring doesn't matter.
void install(ScriptHost& host, Deps deps);

} // namespace scriptbridge
