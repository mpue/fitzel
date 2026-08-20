#pragma once

#include <functional>

#include <glm/glm.hpp>
#include <imgui.h>

class SplineSystem;

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

    // The viewport, as the editor already has it: the camera's view-projection,
    // the image's top-left in screen space, its size, whether the cursor is over
    // it, and the cursor in NDC.
    glm::mat4 viewProj{1.0f};
    ImVec2    origin{0.0f, 0.0f};
    float     viewW = 1.0f, viewH = 1.0f;
    bool      hovered = false;
    glm::vec2 mouseNdc{0.0f};
    ImVec2    mousePos{0.0f, 0.0f};
    glm::vec3 cameraPos{0.0f};
    glm::vec3 cameraFront{0.0f, 0.0f, -1.0f};  // for the camera-relative nudge
    float     cameraFov = 60.0f;

    // Raycast the terrain under a viewport NDC point (main's roadPickTerrain),
    // and sample its height. Injected so this file stays free of the terrain.
    std::function<bool(glm::vec2, const glm::mat4&, glm::vec3&)> pickTerrain;
    std::function<float(float, float)>                           groundAt;

    // Undo bracket, same contract as the panel's: open on grab, commit on release.
    std::function<void()>            beginEdit;
    std::function<void(const char*)> endEdit;
    // True while an interaction is open, so a key-repeat burst can be closed once
    // the last key comes up rather than per repeat.
    std::function<bool()>            editOpen;
};

// Run one frame of the tool: picking, dragging, adding, deleting, nudging and
// drawing. Call it while the spline edit mode owns the left mouse button.
void handle(const Context& c);

} // namespace splineedit
