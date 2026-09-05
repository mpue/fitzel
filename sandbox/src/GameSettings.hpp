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

// What the player IS on the first frame of the game -- which is a decision about
// the GAME, not about a level, and had until now been two per-scene checkboxes
// ("Start Play in vehicle mode" / "...in glider mode") sitting in the Vehicle and
// Glider panels.
//
// Two bools in two panels could not express this list, and that is why it moved:
// they can say on-foot-or-driving-or-flying and nothing else, they can disagree
// with each other, and neither of them is anywhere near the dialog that decides
// what the shipped game is. A game that opens on a camera -- an attract screen,
// a title card, a reel circling the car -- was not expressible at all.
enum class StartMode {
    Fps = 0,     // on foot at the PlayerStart: the walking player
    Vehicle,     // behind the wheel of the scene's vehicle
    Glider,      // flying the scene's glider
    MainCamera,  // watching, through the camera marked Main Camera
    Multishot,   // watching, through the scene's multishot camera (the attract reel)
};

// "On foot", "Behind the wheel", ... -- for the dialog.
const char* startModeName(StartMode m);
// The key written to game.json. A NAME rather than the enum's number, so the file
// reads as what it means and so inserting a mode in the middle of the list cannot
// silently turn every configured game into a different one.
const char* startModeKey(StartMode m);
StartMode   startModeFromKey(const std::string& key);

struct Settings {
    std::string exeName;                    // exported exe base name ("" = project name)
    std::string splash;                     // splash image, project-relative ("" = engine default)
    // The application icon, as a picture -- a PNG the artist already has, not an
    // .ico nobody owns a tool for. The export scales it to every size Windows
    // asks for and writes it into the exe's resources, so there is no icon
    // format and no build step for anyone to get wrong.
    std::string icon;                       // icon source image, project-relative ("" = none)
    std::string startScene;                 // scene stem the game boots into ("" = default scene)
    StartMode   startMode = StartMode::Fps; // ...and what the player is when it does
    // Did the file actually SAY startMode, or is the field just its default?
    // The two are not the same question: "on foot" is the default value, so
    // without this a game deliberately set to on foot is indistinguishable from
    // a project that never opened the dialog -- and the per-scene flags this
    // setting replaced would keep overriding it forever (see startPlay).
    bool        startModeSet = false;
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
