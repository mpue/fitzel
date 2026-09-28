#pragma once

#include <functional>
#include <vector>

#include <glm/glm.hpp>
#include <imgui.h>

struct EditorContext;
struct ViewportFrame;

// Selecting in the scene viewport with the left button: a click picks the
// object under the cursor, Ctrl+click adds or removes one, Ctrl+drag draws a
// box, and in Create mode a click on empty ground places a new object. Editor
// only.
namespace viewpick {

// The gesture in flight, kept from frame to frame. One object in main.
struct Picker {
    // Box-select (Ctrl + left-drag in the viewport): the rectangle in progress.
    bool   boxSelecting = false;
    ImVec2 boxStart{0.0f, 0.0f};
    // Round-robin picking: the entity ids the last click's ray passed through
    // (nearest first) and which one is currently selected, so repeated clicks
    // at the same spot cycle to the next overlapping entity (a parent group's
    // bounding box no longer permanently swallows clicks meant for a child).
    std::vector<int> pickStack;
    int              pickIdx = -1;
};

// The entity ids the ray under the cursor passes through, nearest first. What
// is not shown is not pickable.
std::vector<int> rayPick(const EditorContext& ed, const ViewportFrame& view);

struct Host {
    // The left button is the selection's this frame: the cursor is over the
    // image, no viewport tool owns the button and the gizmo is not under it.
    bool canPick   = false;
    bool placeMode = false;                        // Create mode...
    std::function<void(const glm::vec3&)> place;   // ...a click on empty ground places here
    // While modelling, a plain click that lands on the selected mesh picks one
    // of its elements instead; true when it took the click.
    std::function<bool()> meshClick;
};

// One frame of it. Ctrl+left starts a gesture: released without moving it
// toggles the object clicked, dragged it adds every object whose centre ends up
// inside the box. A plain click selects the nearest object under the cursor --
// the next one behind it when clicked again at the same spot -- places a new one
// on empty ground in Create mode, and otherwise clears the selection.
void click(EditorContext& ed, const ViewportFrame& view, Picker& p, const Host& h);

} // namespace viewpick
