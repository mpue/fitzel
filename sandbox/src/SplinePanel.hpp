#pragma once

#include <functional>
#include <vector>

#include <fitzel/asset/AssetId.hpp>

#include "SceneTypes.hpp"   // MaterialDef

class SplineSystem;

// The Splines panel: the path list, the viewport edit-mode toggle, the preset
// picker and the style + material controls for the selected fence / wall / track.
// Editor-only -- the widget lives in SplinePanel.cpp, which the player doesn't
// compile (same split as RoadPanel).
namespace splineui {

// What the panel touches in main. References rather than a back-pointer to the
// editor, so the panel can be read on its own and can't quietly reach for more
// than this list -- the same shape roadui::PanelState uses.
struct PanelState {
    bool&         show;      // the window's own open flag
    SplineSystem& splines;
    bool&         editMode;  // viewport handle editing (owns the left mouse button)
    int&          sel;       // selected path, -1 = none
    int&          ptSel;     // selected control point of that path, -1 = none

    // The project's material library, for the per-element material pickers. A
    // path can point any of its three elements at any material in here -- which
    // is how a wall gets a brick texture and its coping a stone one.
    std::vector<MaterialDef>& materials;

    // Turning edit mode on has to switch the sibling brushes off, or two tools
    // fight over the left button. main owns those flags; it hands us the one call.
    std::function<void()> grabLMB;
    // Show a material in the Materials panel (main owns the selection + that
    // window's flag), so "give this brick a texture" is one click from here
    // instead of a hunt through a list of look-alike names.
    std::function<void(fitzel::AssetId)> editMaterial;

    // Undo bracket for the edits the panel makes itself. Call beginEdit() before
    // touching a path and endEdit("label") once the interaction is over -- for a
    // slider that is when it is released, not on every frame it changes, or one
    // drag would fill the history. main owns the CommandStack, so it does the
    // pushing; a change that turns out to be a no-op is dropped there.
    std::function<void()>            beginEdit;
    std::function<void(const char*)> endEdit;
};

void drawPanel(const PanelState& s);

} // namespace splineui
