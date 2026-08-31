#pragma once

#include <functional>

#include <glm/glm.hpp>
#include <imgui.h>

class RiverSystem;

// The water tool's viewport half: draggable control-point handles, click-to-add,
// the drawn course with its banks and flow arrows, and the keyboard nudge.
// Editor-only.
//
// The same two hundred lines the road and the spline handles already cost main,
// and here for the same reason. Everything it needs comes in through Context --
// it never reaches for the camera, the terrain or the scene itself.
//
// Tremor-friendly by construction: a generous pixel grab radius, a click on empty
// ground extends the run rather than demanding a drag, Delete removes the
// selected point, and the arrow keys nudge in whole steps.
//
// What it draws is deliberately more than a line, because this tool decides
// things the author did not type. It shows which way the water ended up flowing,
// where the channel's edges are, and which stretches came out white -- so the
// answer to "why is there a waterfall there" is on screen rather than in the
// terrain.
namespace riveredit {

struct Context {
    RiverSystem& rivers;
    int&  sel;         // selected watercourse, -1 = none
    int&  ptSel;       // selected control point of it, -1 = none
    bool& dragging;    // a handle is being dragged (persists across frames)
    bool& dragHeight;  // ...vertically (Ctrl held on grab): the water level here

    // The viewport, as the editor already has it.
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

    // Undo bracket, same contract as the panel's: open on grab, commit on
    // release -- and the commit is also what re-cuts the bed.
    std::function<void()>            beginEdit;
    std::function<void(const char*)> endEdit;
    // True while an interaction is open, so a key-repeat burst can be closed once
    // the last key comes up rather than per repeat.
    std::function<bool()>            editOpen;
};

// Run one frame of the tool: picking, dragging, adding, deleting, nudging and
// drawing. Call it while the water edit mode owns the left mouse button.
void handle(const Context& c);

} // namespace riveredit
