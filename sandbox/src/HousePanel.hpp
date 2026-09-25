#pragma once

#include <cstddef>
#include <functional>
#include <string>

#include "HouseGen.hpp"

// The editor's "Houses" panel: the UI over HouseGen. Write the room programme
// storey by storey, watch the floor plan redraw under it, then Generate -- the
// house lands in front of the camera as an ordinary entity subtree you can walk
// through, and "Save as prefab" turns it into a reusable asset.
//
// The panel owns no state: everything it edits lives in main (like every other
// panel here), so an undo/redo or a project load can't leave it stale.
namespace houseui {

struct PanelState {
    bool&             show;
    housegen::Params& cfg;
    int&              level;          // storey tab (programme + plan preview)
    bool              hasProject;     // prefabs need an open project
    bool              hasLive;        // a generated house is still in the scene
    bool              selectionIsHouse; // the selection is (inside) a generated house
    char*             nameBuf;        // prefab / root name (edited in place)
    std::size_t       nameBufSize;
    bool&             autoRebuild;    // re-generate the live house on every edit
    bool&             pendingRebuild; // set by an edit, cleared once rebuilt
    std::function<void()> generate;     // place a new house in the scene
    std::function<void()> rebuild;      // re-generate the live one in place
    std::function<void()> loadSelected; // take the selected house's parameters
    std::function<void()> saveAsPrefab; // write it to the project's prefabs/
    const std::string&    status;       // last action's outcome (footer line)
};

void drawPanel(const PanelState& s);

} // namespace houseui
