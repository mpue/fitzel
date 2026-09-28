#pragma once

#include <functional>
#include <vector>

#include <glm/glm.hpp>
#include <imgui.h>

#include "ViewportFrame.hpp"

class SplineSystem;
namespace splineplace { struct Spot; }

// The spline tool's viewport half: draggable control-point handles, click-to-add,
// the drawn path, and the keyboard nudge. Editor-only.
//
// It lives here rather than in main() because it is the same two hundred lines
// the road handles already cost there, and main is long enough. Everything it
// needs comes in through Context -- it never reaches for the camera, the
// terrain or the scene itself.
//
// Tremor-friendly by construction, which is the point of the whole tool: a
// generous pixel grab radius, a click on empty ground extends the run rather
// than demanding a drag, Delete removes the selected point, and the arrow keys
// nudge it in whole steps -- so a path can be laid and corrected without a
// single precise drag.
namespace splineedit {

struct Context {
    SplineSystem& splines;
    int&  sel;         // selected path, -1 = none
    int&  ptSel;       // selected control point of that path, -1 = none
    bool& dragging;    // a handle is being dragged (state persists across frames)
    bool& dragHeight;  // ...vertically (Ctrl held on grab) rather than across the ground

    // The viewport this frame (ViewportFrame.hpp): the camera, the image on
    // screen, the cursor over it, and the ground under a point of it.
    ViewportFrame view;

    // Undo bracket, same contract as the panel's: open on grab, commit on release.
    std::function<void()>            beginEdit;
    std::function<void(const char*)> endEdit;
    // True while an interaction is open, so a key-repeat burst can be closed once
    // the last key comes up rather than per repeat.
    std::function<bool()>            editOpen;

    // Where "Place along path" would put its copies, drawn as markers; null when
    // the panel is not offering a placement.
    const std::vector<splineplace::Spot>* preview = nullptr;
};

// Run one frame of the tool: picking, dragging, adding, deleting, nudging and
// drawing. Call it while the spline edit mode owns the left mouse button.
void handle(const Context& c);

// Draw only -- the paths and the placement preview, no handles and no input.
// For while the Splines panel is open but edit mode is off: a bare path has no
// geometry of its own, so without this it would be invisible exactly when you
// are about to place things along it. Uses only `c.view`.
void draw(const Context& c);

} // namespace splineedit
