#pragma once

#include <string>
#include <vector>

#include "LoadingScreen.hpp"

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
    // The application icon, as a picture -- a PNG the artist already has, not an
    // .ico nobody owns a tool for. The export scales it to every size Windows
    // asks for and writes it into the exe's resources, so there is no icon
    // format and no build step for anyone to get wrong.
    std::string icon;                       // icon source image, project-relative ("" = none)
    std::string startScene;                 // scene stem the game boots into ("" = default scene)
    std::vector<std::string> exportScenes;  // scene stems to bundle (empty = all scenes)
    bool trimAssets = false;                 // export only assets the project references
    // Bundle content/, project/ and assets/ into one encrypted game.fpak next to
    // the exe instead of shipping them as folders. On by default: a shipped game
    // whose art sits in an open folder is a shipped game whose art is already
    // copied. Off is for debugging an export.
    bool packContent = true;

    // --- Installer -----------------------------------------------------------
    // Build a setup.exe next to the export (needs Inno Setup on the machine).
    // Off by default: compressing a few gigabytes with LZMA costs minutes, and
    // most exports during development are made to be run, not to be shipped.
    bool        makeInstaller = false;
    std::string productName;                 // shown in the wizard ("" = exe name)
    std::string version = "1.0.0";           // shown in the Programs list
    std::string publisher;                   // may be empty

    // How the loading screen looks -- the picture the game sits on while a level
    // streams in. It lives here rather than in the scene because it belongs to
    // the GAME, not to any one level: the screen shown BETWEEN two scenes cannot
    // sensibly be owned by either of them.
    loadingscreen::Style loading;
};

// Read <projectFolder>/game.json. Missing file / parse error -> a default Settings
// (all empty), so callers always get a usable value.
Settings load(const std::string& projectFolder);

// Write <projectFolder>/game.json. Empty fields are still written so the file is a
// complete, editable record of the current choices.
void save(const std::string& projectFolder, const Settings& s);

} // namespace game
