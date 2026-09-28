#pragma once

#include <functional>

#include <glm/glm.hpp>

struct EditorContext;
struct ViewportFrame;

// The Blender-style 3D cursor: a world-space reference point, placed with
// Shift+Right-click in the viewport, that new objects are put on while it is
// shown and that the snap operations move things to and from -- plus the snap
// steps Ctrl rasters a gizmo drag to. The Cursor is plain data the scene saves,
// so the player compiles it too; everything else here is editor only.
namespace cursor3d {

struct Cursor {
    glm::vec3 pos{0.0f};
    bool      visible = true;   // shown: new objects are placed on it
    // The step of the snap-to-grid operations -- and of a Ctrl'd gizmo move,
    // and of the construction grid drawn on the cursor's plane.
    float grid = 1.0f;
    // Holding Ctrl while dragging the gizmo rasters it: a move lands on `grid`,
    // a turn goes in snapAngle steps, a scale in snapScale steps of the size it
    // started at. A hand that shakes then cannot nudge a value it has already
    // found -- the next step is a whole step away.
    float snapAngle = 15.0f;
    float snapScale = 0.1f;
};

// `p` rounded onto a grid of `step` (unchanged for a step <= 0).
glm::vec3 snapToGrid(const glm::vec3& p, float step);

// The snap operations, the same in the panel and in the Shift+S menu.
enum class Snap {
    CursorToOrigin, CursorToGrid, CursorToTerrain, CursorToSelection,
    SelectionToCursor, SelectionToGrid,
};
// `groundAt` is the terrain's height (for CursorToTerrain). The selection ones
// move the active object through its local transform, so a parented one stays
// where its parent puts it, as one undo step; with nothing selected they do
// nothing.
void snap(Snap op, EditorContext& ed, Cursor& c,
          const std::function<float(float, float)>& groundAt);

// In the Scene window: Shift+Right-click puts the cursor on the ground under
// the pointer, and its mark is drawn while it is shown. `editing`: not in Play.
void viewport(const ViewportFrame& view, Cursor& c, bool editing);
// ...and Shift+S there opens the snap menu (Ctrl+S stays Save).
void snapMenu(EditorContext& ed, const ViewportFrame& view, Cursor& c, bool editing);

struct Panel {
    bool&  show;
    bool&  showGrid;   // the construction grid, drawn on the cursor's plane
    float& gridFade;   // ...and how far out it fades
    std::function<float(float, float)>    groundAt;   // "To terrain"
    std::function<void(const glm::vec3&)> addAt;      // "Add object at cursor"
};
// The "3D Cursor" panel: where it is and whether it is shown, the snap steps,
// the grid drawn with them, the snap operations and "Add object at cursor".
// Draws nothing when `show` is false.
void panel(EditorContext& ed, Cursor& c, const Panel& p);

} // namespace cursor3d
