#pragma once

#include <functional>

#include <glm/glm.hpp>
#include <imgui.h>

#include "ViewportFrame.hpp"

class RoadSet;
class RoadSystem;

// The road tool's viewport half: the selected road's draggable control-point
// handles, click-to-add, the drawn preview with its bridges, tunnels, loops and
// junctions, the loop ghost, and the keyboard nudge. Editor-only.
//
// Out of main() for the same reason the spline tool is (SplineEdit.hpp, whose
// shape this copies): five hundred lines of viewport gesture had no business
// there. Everything it needs comes in through Context -- it never reaches for
// the camera, the terrain or the scene itself.
namespace roadedit {

// --- The point list's bookkeeping ---------------------------------------------
// A bridge, a tunnel and a loop name their ends by control-point index, so a
// point erased or inserted has to shift them with it -- a structure left naming
// a deleted point would silently move to whatever slid into its place.

// Erase control point `k` of `road`; any bridge, tunnel or loop ending on it goes
// with it, later points shift down. False (and nothing done) when out of range.
bool removePoint(RoadSystem& road, int k);
// Insert a control point at index `at` (clamped), inheriting the height and the
// cross-fall of its neighbours so a raised or banked stretch keeps its shape.
// Returns the index it went in at.
int insertPoint(RoadSystem& road, int at, glm::vec2 p);
// Best index to insert a new waypoint at world XZ `p`: between the two control
// points whose segment lies nearest, so clicking on an existing road adds a
// point in the middle instead of always at the tail. A click that projects past
// an open end extends the road there instead of splitting the end segment.
int insertIndex(const RoadSystem& road, glm::vec2 p);

// --- The viewport tool ---------------------------------------------------------
struct Context {
    RoadSet& roads;      // the handles belong to its active road
    int&  sel;           // selected control point, -1 = none
    int&  sel2;          // shift-clicked second point (a bridge's far end), -1 = none
    bool& dragging;      // a handle is being dragged (state persists across frames)
    bool& dragHeight;    // ...vertically (Ctrl held on grab) rather than across the ground

    // The viewport this frame (ViewportFrame.hpp): the camera, the image on
    // screen, the cursor over it, and the ground under a point of it.
    ViewportFrame view;

    // Undo bracket: open on grab, commit on release; editOpen says one is open,
    // so a key-repeat burst closes once the last key comes up.
    std::function<void()>            beginEdit;
    std::function<void(const char*)> endEdit;
    std::function<bool()>            editOpen;
    // Adding and deleting a point, each one undo step of its own.
    std::function<void(int, glm::vec2)> addPoint;
    std::function<void(int)>            deletePoint;
};

// Run one frame of the tool: picking, dragging, adding, deleting, nudging and
// drawing. Call it while the road edit mode owns the left mouse button.
void handle(const Context& c);

} // namespace roadedit
