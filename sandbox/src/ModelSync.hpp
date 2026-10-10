#pragma once
// Keeps a scene's structured models in line with their files.
//
// A model imported as a hierarchy (one entity per mesh node, under a group)
// used to find each part by its POSITION in the file. Export the file again
// from Blender with an object added or removed and every part after it pointed
// at its neighbour's mesh; resize one and the scene squeezed it back into the
// old box. So editing a level in Blender was not a way at all.
//
// Here a part is found by its NAME, and the file is read as the source of the
// layout: a part moved or resized in Blender moves and resizes in the scene,
// an object added in Blender appears, one deleted disappears. What the editor
// did to a part survives, because each part remembers where it sat and how
// big it was in the file the last time (ModelComponent::fileCenter/fileHalf):
// only the CHANGE in the file is applied on top of the scene's own placement.
//
// Runs after every scene load. A part that is no longer in the file is
// removed when it is a plain part; one that carries more (a Door, a script,
// children) is kept, drawing nothing, so nothing built on it is lost.

#include <string>
#include <vector>

struct Entity;
struct MaterialDef;
class ModelLibrary;
namespace fitzel { class AssetDatabase; }

namespace modelsync {

struct Report {
    int groups    = 0;  // structured model groups looked at
    int moved     = 0;  // parts whose place the file changed
    int resized   = 0;  // parts whose size the file changed
    int relinked  = 0;  // parts found again under another position in the file
    int baselined = 0;  // parts that got their first file reading (older scenes)
    std::vector<std::string> added, removed, kept;
    bool changedScene() const {
        return moved || resized || relinked || !added.empty() || !removed.empty();
    }
};

Report syncAll(std::vector<Entity>& entities, int& entityCounter, ModelLibrary& models,
               fitzel::AssetDatabase& assetDb, std::vector<MaterialDef>& materials);

// One line for the console / the HUD ("" when nothing happened).
std::string describe(const Report& r);

} // namespace modelsync
