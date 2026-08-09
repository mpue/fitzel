#pragma once

#include <functional>
#include <string>

class RoadSystem;

// The "City" panel: the UI over CityGen. A list of biomes, each owning a stretch
// of the road, and the numbers that give that stretch its character -- how far
// back the facades stand, how wide a plot is, how tall the towers get, which
// styles are in the mix, whether the street carries skyways and neon.
//
// The panel owns no state: everything it edits lives on the RoadSystem (which
// saves it and puts it on the undo stack), so an undo/redo or a project load
// can't leave it stale. Same shape as roadui::PanelState. Editor-only -- the
// player builds the same city from the same rules without ever compiling this.
namespace cityui {

struct PanelState {
    bool&       show;
    RoadSystem& road;
    bool        hasRoad;   // the road has been built (a centreline to build along)

    // Re-derive the district. main owns it because the buildings' shared
    // materials come out of the project's material library.
    std::function<void()> rebuild;
    // Lift the building nearest the camera out of the district and into the scene
    // as a real, editable entity subtree (see city::bake).
    std::function<void()> bakeNearest;

    // Undo bracket for the edits the panel makes. Call beginEdit() before
    // touching the road and endEdit("label") once the interaction is over -- for
    // a slider that is when it is released, not every frame it moves, or one drag
    // would fill the history.
    std::function<void()>            beginEdit;
    std::function<void(const char*)> endEdit;

    const std::string& status;   // last action's outcome (footer line)
};

void drawPanel(const PanelState& s);

} // namespace cityui
