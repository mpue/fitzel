#pragma once

#include <cstddef>
#include <functional>
#include <string>

#include "BuildingGen.hpp"

// The editor's "Buildings" panel: the UI over BuildingGen. Pick a style, tune
// the massing, hit Generate -- the building lands in front of the camera as an
// ordinary entity subtree, and "Save as prefab" turns it into a reusable asset
// for the racing track.
//
// The panel owns no state: everything it edits lives in main (like every other
// panel here), so an undo/redo or a project load can't leave it stale.
namespace buildingui {

struct PanelState {
    bool&              show;
    buildings::Params& cfg;
    int                objectCount;   // objects the current parameters produce
    bool               hasProject;    // prefabs need an open project
    bool               hasLive;       // a generated building is still in the scene
    char*              nameBuf;       // prefab / root name (edited in place)
    std::size_t        nameBufSize;
    bool&              autoRebuild;   // re-generate the live building on every edit
    bool&              pendingRebuild;// set by an edit, cleared once rebuilt
    std::function<void()> generate;     // place a new building in the scene
    std::function<void()> rebuild;      // re-generate the live one in place
    std::function<void()> saveAsPrefab; // write it to the project's prefabs/
    const std::string&    status;       // last action's outcome (footer line)
};

void drawPanel(const PanelState& s);

} // namespace buildingui
