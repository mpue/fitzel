#pragma once

#include <functional>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/asset/AssetId.hpp>

#include "SceneTypes.hpp"   // MaterialDef (a face is dressed from the library)

class MeshComponent;

// The editor's "Modeling" panel: the face operations of a small box modeller.
//
// Every operation is a BUTTON WITH A NUMBER, never a drag. That is not a
// simplification of a gizmo-based tool that would otherwise be better -- it is
// the point. Pulling a face out by exactly 0.4 m is a thing you can ask for and
// get; doing it by dragging is a thing you can only approach, and only with a
// steady hand. The numbers are also what makes the result repeatable across the
// six faces of a building.
//
// Selection happens in the viewport (click a face); this panel acts on whatever
// is selected and knows nothing about how it got there.
namespace modelui {

struct PanelState {
    bool&           show;
    MeshComponent*  mesh;        // null: the selection has no editable mesh
    int&            faceSel;     // index of the selected face, -1 for none
    const std::vector<MaterialDef>& materials; // the library a face is dressed from
    bool            haveSelection = false; // an entity is selected at all
    bool            canConvert    = false; // ...and it could become a mesh

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
};

void drawPanel(const PanelState& s);

} // namespace modelui
