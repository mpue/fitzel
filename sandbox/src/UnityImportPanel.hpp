#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/world/Model.hpp> // UnityTexMatch

namespace fitzel { class AssetDatabase; }

// The "Import Unity asset" panel: browse an asset folder, preview which textures
// map by Unity's naming conventions, then import the FBX as a hierarchy with
// those maps assigned (the matching also runs on reload). Unity FBX files do not
// reference their textures, so a plain import leaves them unmapped. Editor only.
namespace unityimportui {

// The panel's own state: the browsed folder, the chosen FBX and a cached
// texture-match preview, recomputed when the selection changes.
struct State {
    std::string dir;         // asset folder being browsed (default: models/)
    std::string fbx;         // selected .fbx (absolute path), "" = none
    std::vector<std::pair<std::string, std::string>> fbxList; // (relative, absolute)
    std::string scanDir;     // folder fbxList was scanned for ("" = stale)
    std::vector<fitzel::UnityTexMatch> preview;
    std::vector<std::string> nearby;   // image files near the selected FBX
    std::string previewFor;  // path `preview` was computed for
    bool        flipV = true;          // mirror V on import (FBX UV convention)
    std::string status;      // last import result, shown in the panel
};

struct Host {
    bool&                  show;
    const std::string&     modelDir;   // the project's models/ folder
    fitzel::AssetDatabase& assetDb;
    std::function<glm::vec3()> spawnAt; // where an import lands
    // Import `path` as one entity per part at a point, V flipped or not.
    std::function<void(glm::vec3, const std::string&, bool)> importHierarchy;
};

void panel(State& s, const Host& h);

// Every .fbx under `dir` as (path relative to it, absolute path), sorted. A
// manual directory stack, so one unreadable or over-long subfolder cannot abort
// the whole listing (recursive_directory_iterator stops at the first error);
// capped at 2000 files and 40000 entries scanned.
std::vector<std::pair<std::string, std::string>> scanFbx(const std::string& dir);

} // namespace unityimportui
