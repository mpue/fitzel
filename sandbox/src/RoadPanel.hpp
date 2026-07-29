#pragma once

#include <functional>

class RoadSystem;
namespace fitzel { class AssetDatabase; }

// The Roads panel: the road's build button, its shape/surface tunables, and the
// bridge list. Editor-only -- the panel widget lives in RoadPanel.cpp, which the
// player doesn't compile (same split as TerrainPanel).
namespace roadui {

// What the panel touches in main. References rather than a back-pointer to the
// editor, so the panel can be read on its own and can't quietly reach for more
// than this list -- the same shape terrainui::PanelState uses.
struct PanelState {
    bool&        show;      // the window's own open flag
    RoadSystem&  road;
    bool&        editMode;  // viewport handle editing (owns the left mouse button)
    int&         sel;       // selected control point, -1 = none
    int&         sel2;      // shift-clicked second point (a bridge's far end)
    // Every Model asset the database knows (engine + project), for the
    // side-object model picker -- not just the ones already imported.
    fitzel::AssetDatabase& assetDb;

    // Turning edit mode on has to switch the sibling brushes off, or two tools
    // fight over the left button. main owns those flags; it hands us the one call.
    std::function<void()> grabLMB;
    // Commit the road into the terrain (grades the corridor, lofts mesh+collider,
    // republishes the terrain). main owns the terrain field, so it owns this.
    std::function<void()> build;
    // Erase a control point, fixing up the bridges that name points by index.
    std::function<void(int)> removePoint;

    // Undo bracket for the edits the panel makes itself (bridges, Clear, the
    // height field, the loop toggle). Call beginEdit() before touching the road
    // and endEdit("label") once the interaction is over -- for a slider that is
    // when it is released, not on every frame it changes, or one drag would fill
    // the history. main owns the CommandStack, so it does the pushing; a change
    // that turns out to be a no-op is dropped there.
    std::function<void()>             beginEdit;
    std::function<void(const char*)>  endEdit;
};

void drawPanel(const PanelState& s);

} // namespace roadui
