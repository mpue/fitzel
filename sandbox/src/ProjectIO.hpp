#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "SceneTypes.hpp"

namespace fitzel { class AssetDatabase; }

// Project / scene persistence, extracted from the sandbox's main(). A project is
// a folder under projects/: <name>/<name>.fitzel (the scene, JSON schema v3) plus
// <name>/materials/*.fmat (one material asset each, with a .meta GUID sidecar).
// Scenes reference materials and models by GUID; legacy v2 (inline materials) and
// the older space-separated text format still load, gaining fresh GUIDs.
//
// These functions still operate on state owned by main() (the entity list,
// material library, the asset database, editor prefs, ...), so that state is
// gathered into a Context of references + callbacks and threaded through. The
// callbacks cover things main keeps to itself (model import, default materials,
// the scene-settings registry).
namespace projectio {

struct Context {
    // Scene data (owned by main).
    std::vector<Entity>&      entities;
    std::vector<MaterialDef>& materials;
    int&                      matSel;
    int&                      entityCounter;
    int&                      entitySel;

    // Project + editor-prefs state.
    std::string&              currentProject;   // path of the open .fitzel ("" = none)
    char*                     projNameBuf;       // fixed-size display-name buffer
    std::size_t               projNameBufSize;
    std::string&              prefLocation;      // wizard default dir
    std::vector<std::string>& recentProjects;    // folders, newest first
    std::string               prefsPath;         // editor.json path
    std::string&              exportStatus;      // last export result message
    // UI comfort settings (View > Interface). Kept in the prefs rather than the
    // scene: they describe this machine's editor, not the project.
    float&                    uiFontSize;        // points, before display scale
    std::string&              uiFontFamily;      // typeface name ("" = default)

    // Engine context.
    fitzel::AssetDatabase&    assetDb;
    std::string               contentRoot;       // engine/portable content root
    std::string               modelDir;          // where model files resolve

    // Callbacks into main (state main owns exclusively).
    std::function<void()>                       seedDefaultMaterials;
    std::function<int(const std::string&)>      importModel;      // -> modelId
    std::function<int(const std::string&, int)> importModelNode;  // path,node -> modelId
    std::function<LoadedModel*(int)>            loadedModelById;
    std::function<void()>                       clearModels;      // loadedModels.clear()
    // Scene look/settings hooks -- references, since main assigns them after the
    // tunable registry is built (later than the Context is constructed).
    std::function<void(nlohmann::json&)>&       writeSettings;
    std::function<void(const nlohmann::json&)>& readSettings;
    // Called once at the end of every scene load, after readSettings and after the
    // entity id counter is settled (so it may create entities). Scene migrations
    // live here: state that used to be stored outside the entity list and is now
    // an entity gets built from what the file did store.
    std::function<void()>&                      afterSceneLoad;
};

// Editor prefs (last location + recent projects), persisted to prefsPath.
void savePrefs(const Context& ctx);
void loadPrefs(Context& ctx);
void rememberProject(Context& ctx, const std::string& folder);

// Path helpers (pure).
std::string safeName(const std::string& s); // filesystem-safe token from a name
std::string matsDirIn(const std::string& folder);
std::string sceneFileIn(const std::string& folder);
std::vector<std::pair<std::string, std::string>> listProjectsIn(const std::string& root);

// One entity <-> JSON: the single source of truth for entity serialization,
// shared by scene files and prefab (.fprefab) files, so the two never drift.
// writeEntityJson emits the table-driven fields + id/parent + components; a
// ModelComponent's source asset GUID is resolved via `modelGuidOf`, which maps a
// runtime modelId to its GUID string ("" if unknown -- only the caller with the
// loaded-model table can do this). readEntityJson rebuilds the entity, recreating
// each component from the registry and re-importing a model through `ctx`.
nlohmann::json writeEntityJson(const Entity& e,
                               const std::function<std::string(int)>& modelGuidOf);
Entity readEntityJson(Context& ctx, const nlohmann::json& e);

// Serialization.
void saveScene(const Context& ctx, const std::string& path);
void writeProjectMaterials(const Context& ctx, const std::string& matsDir);
void loadProjectMaterials(Context& ctx, const std::string& matsDir);
bool loadScene(Context& ctx, const std::string& path);

// --- Incremental (non-blocking) scene loading --------------------------------
// A big scene freezes the editor while every model is parsed and uploaded. This
// loader slices that work across frames: begin*, then call stepLoad() once per
// frame until `done`, so the render loop keeps drawing (and can show `progress`).
// All GPU work still runs on the calling thread -- only the *timing* is spread
// out, so there are no GL or asset-cache data races.
struct SceneLoad {
    bool  active   = false;  // a load is in flight (begin* succeeded, not yet done)
    bool  done     = false;  // set the frame the load finishes (check `ok`)
    bool  ok       = false;  // final result, valid once `done`
    float progress = 0.0f;   // 0..1 for a progress bar
    std::string label;       // human-readable current step

    // Internals -- do not touch from the caller.
    std::string    scenePath, projectFolder;
    bool           fullOpen = false;    // whole-project open vs in-project scene switch
    nlohmann::json j;                   // parsed scene (JSON path)
    std::size_t    entIdx = 0, entTotal = 0;
    int            maxId = -1;
};

// Start opening a whole project asynchronously: mounts its assets + loads its
// materials now (quick), then streams the scene's entities over the following
// frames. Returns false (and leaves load.done=true, ok=false) when the folder
// has no readable scene. On success load.active is true -- call stepLoad() each
// frame until load.done.
bool beginOpenProject(Context& ctx, SceneLoad& load, const std::string& folder);
// Start switching to another scene inside the already-open project (materials and
// mounts are left untouched, as the synchronous loadSceneFile does).
bool beginLoadScene(Context& ctx, SceneLoad& load, const std::string& scenePath);
// Advance a running load: builds entities for up to ~budgetMs (always at least
// one, so it can't stall), updating progress/label. When the last entity is in it
// applies scene settings, enforces the Sun invariant, sets currentProject (and,
// for a full open, the project name + recent list) and sets done/ok.
void stepLoad(Context& ctx, SceneLoad& load, double budgetMs = 8.0);

// Project operations.
void saveProjectTo(Context& ctx, const std::string& folder);
void saveCurrent(Context& ctx);
void exportGame(Context& ctx, const std::string& outDir);
bool openProjectFolder(Context& ctx, const std::string& folder);
void newProject(Context& ctx);

// --- Scenes within a project --------------------------------------------------
// A project folder may hold several .fitzel scenes (levels). They share the
// project's materials and its mounted asset database; only the entity list +
// scene settings differ per scene. currentProject always points at the active
// scene file.

// Every scene in `folder` as (display name, full path), sorted by name.
std::vector<std::pair<std::string, std::string>> listScenesIn(const std::string& folder);

// Switch to another scene file inside the already-open project. Materials and the
// mounted asset db are left as-is (scenes share them). Sets currentProject.
// Returns false if the file can't be read.
bool loadSceneFile(Context& ctx, const std::string& scenePath);

// Create a fresh, empty scene named `name` in `folder`, write it, make it current.
// Keeps the project's materials + mounts. Returns the new scene path, or "" if the
// inputs are empty or a scene of that name already exists.
std::string newSceneInProject(Context& ctx, const std::string& folder,
                              const std::string& name);

// Rename a scene file to `newName` (same folder, .fitzel). Updates currentProject
// if it was the active scene. Returns the new path, or "" on failure / name clash.
std::string renameScene(Context& ctx, const std::string& scenePath,
                        const std::string& newName);

// Delete a scene file from disk (does not touch the in-memory scene). True on
// success. Callers switch to another scene themselves when the active one is gone.
bool deleteSceneFile(const std::string& scenePath);

// Write `keys` into another scene's "settings" object, in place: each key
// replaces the one already there, everything else in the file (entities, the
// rest of the settings) is left exactly as it was. This is how a per-scene block
// is copied into a scene without opening it -- see the UI Overlay panel's
// "Copy to scene". False if the file can't be read as a scene or written back.
bool mergeSceneSettings(const std::string& scenePath, const nlohmann::json& keys);

} // namespace projectio
