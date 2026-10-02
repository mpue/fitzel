#pragma once

#include <string>

// Blender as the editor's universal motion reader.
//
// fitzel reads glTF itself. Motions also come as FBX (Mixamo, ActorCore, the
// asset stores), BVH (mocap databases) and .blend files -- and Blender opens
// all of them well, better than any importer this project could carry. So for
// those, the editor runs the Blender that is installed on the machine in the
// background: import, export as .glb, done. The user never sees Blender; the
// result lands in a cache keyed by the source file, so each file is converted
// once.
//
// Finding Blender is automatic: a path the user chose wins, then the newest of
// every installation the search turns up (the .blend file association, the
// uninstall entries, Program Files, Steam, the PATH). Editor only.
namespace blender {

struct Install {
    std::string exe;        // "" = none found
    std::string version;    // "5.1.0", "" if unknown
    bool        chosen = false;  // the user's own pick rather than the search
};

// Where Blender is. Searched once and remembered; `again` searches anew.
const Install& find(bool again = false);
// Use this blender.exe from now on ("" goes back to searching). Kept in
// blender.json beside the editor. False if the file is not there.
bool choose(const std::string& exe);

// Can Blender read this kind of file for us (extension with the dot, any case)?
bool canConvert(const std::string& ext);
// Would the editor need Blender for this file at all (anything but glTF)?
bool needsBlender(const std::string& file);

struct Result {
    bool        ok = false;
    std::string glb;        // the converted file
    std::string message;    // one line: what went wrong, or what was found
    std::string log;        // Blender's own output, for the curious
};

// The converted .glb for `src` -- from the cache when it was converted before
// and the source has not changed since, else through Blender (seconds; runs
// one Blender at a time, safe to call from a worker thread).
Result toGlb(const std::string& src);
// Where the cache keeps the conversion of `src` (whether or not it exists yet).
std::string cachePath(const std::string& src);

} // namespace blender