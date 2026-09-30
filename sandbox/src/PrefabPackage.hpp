#pragma once

#include <string>
#include <utility>
#include <vector>

namespace fitzel { class AssetDatabase; }
namespace animgraph { struct Graph; }

// Prefabs that travel between projects: one .zip holding a prefab and everything
// it needs -- models and what a model reads beside it, materials and their
// textures, scripts, sounds, sprites, synth patches and MIDI files, the prefabs
// it spawns by name, and the animation graphs its objects run.
//
// WHY THE .META FILES GO ALONG. A prefab names its models and materials by GUID,
// and a GUID lives in the sidecar beside the file. Copied without it, the file
// arrives, the new project mints it a fresh GUID, and the prefab points at
// nothing. So every asset travels with its .meta and lands at the same relative
// path, and the references hold.
//
// WHAT STAYS BEHIND, AND IS SAID: the engine's own content (every fitzel has
// it), scenes a component names, Timeline clips (their tracks name a scene's
// objects by id) and a scene's changes to a model's OWN materials.
//
// IMPORT LOOKS BEFORE IT WRITES, and never overwrites a file it did not make:
//   - an asset whose GUID the project already has is left where it is;
//   - a material the project has under another GUID -- same name, same content,
//     which is what every project's "Default" is -- is used instead of a copy;
//   - a file whose name is taken by a DIFFERENT file comes in under a new name,
//     and the prefab is rewritten to use it;
//   - the one thing replaced is an older version of the SAME prefab (same GUID),
//     and the old file is kept beside it as .bak.
// Editor-only: the zip code (miniz) is linked into the editor alone.
namespace prefabpkg {

// --- Export ------------------------------------------------------------------
struct ExportSource {
    std::string            projectFolder;           // the project the prefab is in
    fitzel::AssetDatabase* assetDb = nullptr;       // GUIDs and file names
    // The open scene's graphs, for a prefab saved before graphs travelled with
    // it: its objects name graphs that only the scene has.
    const std::vector<animgraph::Graph>* sceneGraphs = nullptr;
};

struct Report {
    std::vector<std::string> files;   // what went in, project-relative
    std::vector<std::string> notes;   // what did not, and why
};

bool exportZip(const ExportSource& src, const std::string& prefabPath,
               const std::string& zipPath, Report& rep, std::string& err);

// --- Import ------------------------------------------------------------------
struct ImportTarget {
    std::string            projectFolder;           // the open project
    fitzel::AssetDatabase* assetDb = nullptr;       // what it already has
};

struct Item {
    enum class Action {
        Add,       // new to the project
        Present,   // the project has it already -- nothing is written
        Rename,    // the name is taken by a different file: comes in as `target`
        Update,    // an older version of the same prefab: replaced, old kept as .bak
    };
    std::string entry;                 // path inside the zip
    std::string target;                // project-relative path it lands on / is at
    Action      action = Action::Add;
    std::string why;                   // for anything but Add, in words
};

struct Plan {
    std::string       zipPath;
    std::string       prefabName;
    std::string       prefabEntry;     // the prefab's own file inside the zip
    std::vector<Item> items;           // one per file; a .meta rides with its asset
    std::vector<std::string> notes;    // the exporter's, plus the importer's own
    // What the prefab (and any material coming in) has to be rewritten to, so it
    // points at what the project has: file names that had to change, and GUIDs
    // of assets the project already holds under another one.
    std::vector<std::pair<std::string, std::string>> renamedFiles; // old name -> new
    std::vector<std::pair<std::string, std::string>> guidRemap;    // zip GUID -> project GUID
};

// Read the zip and work out what importing it into `dst` would do. Writes
// nothing. False with `err` for a file that is not a prefab package, or one that
// tries to write outside the project.
bool planImport(const ImportTarget& dst, const std::string& zipPath, Plan& plan,
                std::string& err);

struct Applied {
    std::string              prefabPath;   // the imported prefab, absolute
    std::vector<std::string> written;      // project-relative files written
};

// Carry out a plan from planImport (for the same zip and project).
bool applyImport(const ImportTarget& dst, const Plan& plan, Applied& out, std::string& err);

} // namespace prefabpkg