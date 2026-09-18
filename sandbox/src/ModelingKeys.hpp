#pragma once

#include <functional>

#include <glm/glm.hpp>
#include <imgui.h>

#include "ModelingTools.hpp"

class MeshComponent;

// The modelling mode: Blender's Edit Mode, keymap and all, over the modelling
// tools. Tab goes in and out (the host does that part); while in, the pointer
// over the viewport means what it means in Blender --
//
//   1 2 3            corner / edge / face picking
//   click, Shift     pick, add or take away;  drag: box;  Alt+click: loop
//   A, Alt+A, Ctrl+I all, none, invert;  L / Ctrl+L linked;  Ctrl+Num+/- grow, shrink
//   G R S            move / rotate / scale with the mouse; X Y Z (Shift: plane)
//                    lock an axis, pressed again local, again free; type a value;
//                    Ctrl snaps, Shift slows; Enter/click confirm, Esc/right cancel
//   E  I  Ctrl+B     extrude, inset, bevel (wheel: segments)
//   Ctrl+R           loop cut: point at an edge, click, slide, click
//   Shift+D          duplicate and move;   F fill / make face;   J connect
//   M  X/Del  Alt+N  merge, delete and normals menus;  Shift+N recalculate outside
//   Numpad .         frame what is picked
//
// -- and every other key the editor has (F to frame, G for the glider, Q/W/E for
// the gizmo, Delete for the object) is the host's again the moment the mode is
// left. Outside it nothing here runs.
//
// The modal operations change the REAL mesh while they run, so what you see is
// the result and not a ghost of it: the host keeps the entity as it was when
// the operation began, and every frame puts it back before the operation is
// applied again from there (Live::Set). Confirming banks one undo step; Esc puts
// the mesh back as if nothing had happened.
//
// For the tremor-friendly side of the editor none of this is required: every
// operation here is also a button with a number on the Modeling panel. What the
// modal versions add for a steady hand is speed, and for any hand the typed
// value -- G Z 0.5 Enter moves by exactly half a metre, no dragging at all.
namespace modelkeys {

enum class Live { Begin, Set, Commit, Cancel };

struct Host {
    MeshComponent*         mesh    = nullptr;  // the mesh being modelled
    modeltools::Selection* sel     = nullptr;
    int*                   faceSel = nullptr;  // the active face
    modeltools::View       view;               // mesh -> world, the camera, the image rect
    bool hovered   = false;   // pointer over the viewport image
    bool keysFree  = true;    // no text field has the keyboard
    bool gizmoOver = false;   // the gizmo has the pointer: a click is its
    // One undoable edit, as the Modeling panel runs them (returns the active face).
    std::function<void(const std::function<int(MeshComponent&)>&, const char*)> edit;
    // A modal edit in flight: Begin snapshots the entity, Set restores it and
    // applies the op (and re-centres), Commit banks one undo step under `label`,
    // Cancel restores.
    std::function<void(Live, const std::function<void(EditMesh&)>&, const char*)> live;
    // Bring a world-space sphere into view (Numpad .).
    std::function<void(const glm::vec3& centre, float radius)> frame;
};

// Keys, clicks and the operation in flight. Call once a frame while in the mode,
// from inside the viewport's window, before the overlay is drawn.
void update(const Host& h);

// The box being dragged, the axis guides, the operation's read-out and the key
// strip -- over the overlay.
void drawHud(ImDrawList* dl, const Host& h);

// An operation (or a box drag) owns the pointer: the host keeps its hands off
// right-button look, wheel and pan, the gizmo and the panel's buttons.
bool busy();

// Drop an operation in flight, restoring the mesh (leaving the mode mid-drag).
void cancel(const Host& h);

} // namespace modelkeys
