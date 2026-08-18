#pragma once

#include <string>
#include <vector>

// Turning a finished export folder into one setup.exe.
//
// The export already produces everything a player needs -- an exe, a boot
// game.json, an encrypted archive, the licence notices -- but a folder is not
// something you hand to somebody. This writes an Inno Setup script for that
// folder and compiles it, so what leaves the building is a single file that
// installs the game, puts it in the Start menu and can take itself back out.
//
// Inno Setup is a separate, free install (innosetup.com) and is found on the
// machine rather than shipped with the editor -- the alternative was writing our
// own installer, and an installer nobody else has audited is not a thing to hand
// to strangers. Without it the export still succeeds; only the setup is missing,
// and the message says so.
namespace installer {

struct Info {
    std::string name;      // product name: Start menu, wizard title, Programs list
    std::string exeName;   // the game's file name inside the folder ("race.exe")
    std::string version;   // "1.0.0" -- shown in the Programs list
    std::string publisher; // may be empty
};

// True if an Inno Setup compiler was found on this machine. The Game Settings
// dialog asks so it can say what will happen BEFORE somebody waits out an
// export to find out.
bool available();

// Where the compiler was found, for the dialog to show. Empty if there is none.
std::string compilerPath();

// Compile a setup for everything in `payloadDir` (recursively). `icon` -- the
// bytes from icoutil::fromImage, may be empty -- becomes the setup's own icon,
// so the file somebody downloads already looks like the game before it has been
// run once.
//
// The setup lands next to the export as "<name>-setup.exe"; its path comes back
// in `setupPath`. Returns false and fills `err` on failure.
bool build(const std::string& payloadDir, const Info& info,
           const std::vector<unsigned char>& icon, std::string& setupPath,
           std::string& err);

} // namespace installer
