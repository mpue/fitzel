#pragma once

#include <cstddef>
#include <functional>
#include <string>

#include "StairGen.hpp"

// The editor's "Stairs" panel: the UI over StairGen. Pick a preset or a type,
// set the height, watch the plan and the side view redraw with the figures a
// stair is judged by, then Generate -- the stair lands at the 3D cursor (or in
// front of the camera), climbing away from the view, as an ordinary entity
// subtree you can walk up; "Save as prefab" turns it into a reusable asset.
//
// The panel owns no state: everything it edits lives in main (like every other
// panel here), so an undo/redo or a project load can't leave it stale.
namespace stairui {

struct PanelState {
    bool&             show;
    stairgen::Params& cfg;
    bool              hasProject;       // prefabs need an open project
    bool              hasLive;          // a generated stair is still in the scene
    bool              selectionIsStair; // the selection is (inside) a generated stair
    char*             nameBuf;          // prefab / root name (edited in place)
    std::size_t       nameBufSize;
    bool&             autoRebuild;      // re-generate the live stair on every edit
    bool&             pendingRebuild;   // set by an edit, cleared once rebuilt
    std::function<void()> generate;     // place a new stair in the scene
    std::function<void()> rebuild;      // re-generate the live one in place
    std::function<void()> loadSelected; // take the selected stair's parameters
    std::function<void()> saveAsPrefab; // write it to the project's prefabs/
    const std::string&    status;       // last action's outcome (footer line)
};

void drawPanel(const PanelState& s);

} // namespace stairui
