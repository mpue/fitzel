#pragma once

#include <vector>

#include <glm/glm.hpp>
#include <imgui.h>      // ImGuizmo.h leans on ImGui's types; must come first
#include <ImGuizmo.h>

#include "SceneTypes.hpp" // Entity

struct EditorContext;
struct ViewportFrame;
namespace modeltools { struct Selection; }

// The transform gizmo in the scene viewport: Move / Rotate / Scale handles on
// the selected object -- or, while modelling with a face (or corners, or edges)
// picked, on those, the same handles applied to the corners instead of a
// transform. Every drag is one undo step. Editor only.
namespace gizmo {

// A drag in flight, kept from frame to frame. One object in main.
struct Drag {
    // --- An object drag -----------------------------------------------------
    bool                active = false;
    std::vector<int>    ids;      // everything it moves: the selected roots and their subtrees
    std::vector<Entity> before;   // ...as they were when it began (one undo step)
    // Multi-select: the selected roots being moved together and the active
    // object's world transform last frame, so each other root gets the same
    // incremental delta applied (individual-origins style).
    std::vector<int> roots;
    glm::vec3        prevT{0.0f}, prevR{0.0f}, prevS{1.0f};
    glm::vec3        startT{0.0f};   // where the drag began (Ctrl grid snap)

    // --- A face drag ----------------------------------------------------------
    // The entity as it was when the drag began (one undo step for the whole
    // drag) and the scale it applies to its mesh.
    bool      faceActive = false;
    Entity    faceBefore;
    glm::vec3 faceScale{1.0f};
    // The scale the gizmo has reported so far in this drag (see the SCALE
    // branch: ImGuizmo measures that one from the start of the drag, not from
    // the last frame).
    glm::vec3 faceAccScale{1.0f};
    // Where the pivot was in the world when the drag began, for the distance
    // read-out next to the pointer.
    glm::vec3 faceStartPivot{0.0f};
};

// How the gizmo is set up this frame -- main's toolbar, keys and modes.
struct Settings {
    ImGuizmo::OPERATION op   = ImGuizmo::TRANSLATE;
    ImGuizmo::MODE      mode = ImGuizmo::WORLD;
    bool editMode   = true;   // the gizmo is out (Esc puts it away)
    // Nothing else has the left button for the object: no vehicle handle, no
    // modal mesh edit.
    bool objectFree = true;
    // Modelling: picked faces / corners / edges take the gizmo (and no modal
    // mesh edit is running).
    bool faceMode   = false;
    const modeltools::Selection* modelSel = nullptr;  // which, besides ed.meshFaceSel
    // What Ctrl rasters a drag to: the grid (metres), degrees, a scale step.
    float grid = 1.0f, snapAngle = 15.0f, snapScale = 0.1f;
};

// One frame of the gizmo, inside the Scene window (ImGuizmo::BeginFrame has run).
// A finished drag is banked as one undo step on the frame after the button is
// let go.
void frame(EditorContext& ed, const ViewportFrame& view, Drag& d, const Settings& s);

} // namespace gizmo
