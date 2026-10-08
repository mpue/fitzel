#pragma once

// Where this user's own state lives -- the editor's settings, its window layout,
// its crash recovery, every game's saves. Never beside the executable: an
// installed editor sits in Program Files or /opt and may not write there, and
// two users of one machine should not share one recent-projects list.
//
//   Windows   %APPDATA%\fitzel\
//   elsewhere $XDG_DATA_HOME/fitzel/, by default ~/.local/share/fitzel/
//
// The player is the exception: an exported game keeps graphics.json,
// difficulty.json and scores.json beside its own exe, as it always has. Its
// setup installs per user, and the folder is the game's -- the editor's
// settings have no business there, nor the other way round.
//
// Inline, because a dozen check harnesses link the panels that use it (Blender,
// Retarget, the camera path) and none of them should have to list one more .cpp.
// The parts only main() needs -- creating the folder, the documents folder,
// moving an older install's settings over -- are in UserDir.cpp.

#include <cstdlib>
#include <filesystem>
#include <string>

namespace userdir {

// The per-user base folder, for every program built from this tree. Not created
// here; "." when the environment names no home at all.
inline std::filesystem::path root() {
    std::filesystem::path base;
#ifdef _WIN32
    if (const char* appData = std::getenv("APPDATA")) base = appData;
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
        base = xdg;
    else if (const char* home = std::getenv("HOME"))
        base = std::filesystem::path(home) / ".local/share";
#endif
    if (base.empty()) return ".";
    return base / "fitzel";
}

// The folder this program's own settings go in: root() for the editor, the
// working directory -- the exe's folder, see startup::setWorkingDirToExe -- for
// the player.
inline std::filesystem::path stateDir() {
#ifdef FITZEL_PLAYER
    return ".";
#else
    return root();
#endif
}

// stateDir()/name, as the string the file APIs here take.
inline std::string file(const std::string& name) {
    return (stateDir() / name).generic_string();
}

// --- UserDir.cpp ---------------------------------------------------------------

// Once at startup, before anything reads a settings file: creates stateDir(),
// and on the editor's first start in it copies over what an older build left
// beside the exe (editor.json, imgui.ini, recovery/ ...). Copied, not moved --
// that folder may be read-only, and an older version installed beside this one
// still wants its own.
void prepare();

// Where the New Project wizard puts projects until the user picks somewhere
// else: "Fitzel" in the user's documents folder (Documents, Dokumente, or what
// the desktop calls it).
std::filesystem::path defaultProjectsDir();

} // namespace userdir
