#pragma once

#include <functional>

class RiverSystem;

// The Water panel: the watercourse list, the viewport edit-mode toggle, the
// preset picker and the channel + look controls for the selected brook, river or
// canal. Editor-only -- the widget lives in RiverPanel.cpp, which the player does
// not compile (same split as RoadPanel / SplinePanel).
namespace riverui {

// What the panel touches in main. References rather than a back-pointer to the
// editor, so it can be read on its own and cannot quietly reach for more than
// this list -- the same shape splineui::PanelState uses.
//
// There is no material library in here, and that is not an omission: running
// water is drawn by its own shader from the numbers in the channel's style, so
// there is no slot for a material to go in.
struct PanelState {
    bool&        show;      // the window's own open flag
    RiverSystem& rivers;
    bool&        editMode;  // viewport handle editing (owns the left mouse button)
    int&         sel;       // selected watercourse, -1 = none
    int&         ptSel;     // selected control point of it, -1 = none

    // Turning edit mode on has to switch the sibling brushes off, or two tools
    // fight over the left button. main owns those flags; it hands us the one call.
    std::function<void()> grabLMB;

    // Undo bracket for the edits the panel makes itself. beginEdit() before
    // touching a path, endEdit("label") when the interaction is over -- for a
    // slider that is on release, not every frame it changes, or one drag would
    // fill the history.
    //
    // endEdit is also what triggers the CUT: the bed is re-cut when a gesture
    // ends, never while it is running. That is the whole reason this panel does
    // every edit through the helpers below rather than touching a float directly.
    std::function<void()>            beginEdit;
    std::function<void(const char*)> endEdit;
};

void drawPanel(const PanelState& s);

} // namespace riverui
