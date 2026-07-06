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

// Serialization.
void saveScene(const Context& ctx, const std::string& path);
void writeProjectMaterials(const Context& ctx, const std::string& matsDir);
void loadProjectMaterials(Context& ctx, const std::string& matsDir);
bool loadScene(Context& ctx, const std::string& path);

// Project operations.
void saveProjectTo(Context& ctx, const std::string& folder);
void saveCurrent(Context& ctx);
void exportGame(Context& ctx, const std::string& outDir);
bool openProjectFolder(Context& ctx, const std::string& folder);
void newProject(Context& ctx);

} // namespace projectio
