#pragma once

#include <string>
#include <vector>

// Per-project "game" settings: the handful of knobs that describe the shipped
// game rather than the scene being edited -- the exe name, the splash image, the
// scene the game boots into, and which scenes an export bundles. They live in a
// `game.json` at the project root (separate from the runtime boot game.json the
// export writes next to the player), loaded/saved by the editor's Game Settings
// dialog and consumed by projectio::exportGame.
namespace game {

struct Settings {
    std::string exeName;                    // exported exe base name ("" = project name)
    std::string splash;                     // splash image, project-relative ("" = engine default)
    std::string startScene;                 // scene stem the game boots into ("" = default scene)
    std::vector<std::string> exportScenes;  // scene stems to bundle (empty = all scenes)
    bool trimAssets = false;                 // export only assets the project references
};

// Read <projectFolder>/game.json. Missing file / parse error -> a default Settings
// (all empty), so callers always get a usable value.
Settings load(const std::string& projectFolder);

// Write <projectFolder>/game.json. Empty fields are still written so the file is a
// complete, editable record of the current choices.
void save(const std::string& projectFolder, const Settings& s);

} // namespace game
