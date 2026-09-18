#pragma once

#include <functional>
#include <vector>

#include <glm/glm.hpp>
#include <imgui.h>

#include <fitzel/asset/AssetId.hpp>

#include "ModelingTools.hpp" // the selection the viewport is picking
#include "SceneTypes.hpp"    // MaterialDef (a face is dressed from the library)

class MeshComponent;
class SplineSystem;

// The editor's Modeling panel: a small box modeller in one FLOATING window over
// the viewport, next to the thing being shaped rather than docked at the edge of
// the screen. It carries everything -- what is being picked (corners, edges or a
// face), the operations for that, the amounts they apply, and the material a
// face wears -- so shaping something is never a trip between two panels.
//
// Every operation is a BUTTON WITH A NUMBER, never a drag. That is not a
// simplification of a gizmo-based tool that would otherwise be better -- it is
// the point. Pulling a face out by exactly 0.4 m is a thing you can ask for and
// get; doing it by dragging is a thing you can only approach, and only with a
// steady hand. The numbers are also what makes the result repeatable across the
// six faces of a building. Same for the amounts themselves: a step is picked
// with a pair of big -/+ buttons, not by dragging a slider.
//
// Picking happens in the viewport (click a corner, an edge or a face); this
// panel acts on whatever is picked and knows nothing about how it got there --
// see ModelingTools.hpp for that half.
namespace modelui {

struct PanelState {
    bool&                   show;
    MeshComponent*          mesh;      // null: the selection has no editable mesh
    int&                    faceSel;   // selected face, -1 for none
    modeltools::Selection&  sel;       // corners / edges, and which mode
    const std::vector<MaterialDef>& materials; // the library a face is dressed from
    bool                    haveSelection = false; // an entity is selected at all
    bool                    canConvert    = false; // ...and it could become a mesh

    // Mesh space -> world, so the nudge arrows can move a selection along the
    // WORLD axes whatever the object's own rotation is.
    glm::mat4 model{1.0f};
    // The viewport's rect: only for where the window opens the first time.
    ImVec2 viewMin{0, 0}, viewMax{0, 0};

    // Turn the selected entity into an editable mesh (Box -> its own geometry).
    std::function<void()> convert;
    // Run an edit: the callback is handed the mesh and returns the face to keep
    // selected. The host wraps it in one undo step, re-centres the geometry and
    // squares the entity's half-extents with it -- so an operation here is just
    // the operation, and every one of them gets that treatment.
    std::function<void(const std::function<int(MeshComponent&)>&, const char*)> edit;

    // Open the Materials panel on a material, for editing the surface itself
    // rather than which one the face wears.
    std::function<void(fitzel::AssetId)> editMaterial;

    // Live read-outs for the header (0 when nothing is selected).
    int faceCount = 0, vertCount = 0;

    // The 3D cursor (world), one of the points a spin can turn about.
    glm::vec3 cursor{0.0f};
    // The scene's spline paths, for duplicating along one. Null: none offered.
    const SplineSystem* splines = nullptr;
};

void drawPanel(const PanelState& s);

} // namespace modelui
