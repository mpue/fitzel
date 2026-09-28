#pragma once

#include <functional>

#include <glm/glm.hpp>
#include <imgui.h>

#include "ModelingKeys.hpp"   // modelkeys::Live
#include "ModelingTools.hpp"  // modeltools::Selection
#include "SceneTypes.hpp"     // Entity

class MeshComponent;
class SplineSystem;
struct EditorContext;
struct ViewportFrame;

// The modelling mode -- Blender's Edit Mode: the Modeling panel open on an
// editable mesh. This is its host side, on the editor's core: what an edit
// does to the entity around the mesh (one undo step, geometry re-centred,
// half-extents squared with it), the modal edits of the modelling keys, the
// viewport while it is on (keys, overlay, picking a corner, edge or face), and
// the Modeling and UV panels fed from the selection. The operations themselves
// are in EditMesh / ModelingPanel / ModelingKeys. Editor only.
namespace modelmode {

// What lives across frames besides the face selection the EditorContext
// carries (ed.meshFaceSel / meshFaceOwner). One object in main.
struct Session {
    // Corners and edges picked while modelling, and which of vertex / edge /
    // face the viewport is picking (the face itself stays in ed.meshFaceSel).
    modeltools::Selection sel;
    // A modal edit (G, E, Ctrl+B ...) in flight: the entity as it was when it
    // began, and the scale it applied to its mesh.
    Entity    liveBefore;
    glm::vec3 liveScale{1.0f};
};

// The editable mesh on the selected object, if it has one.
MeshComponent* selectedMesh(EditorContext& ed);

// Turn the selected solid into an editable mesh of exactly the same size, as
// one undo step. Nothing for a selection that is not a solid, or already is one.
void convertToMesh(EditorContext& ed);

// Run one face operation as one undoable step: `op` is handed the mesh and
// returns the face to keep selected; the changed edges flash where they now are.
void applyEdit(EditorContext& ed, const std::function<int(MeshComponent&)>& op,
               const char* label);

// A modal edit of the modelling keys: every frame starts again from the entity
// as it was, so the operation is always "base + what the pointer says now",
// never a pile-up. Commit banks one undo step; Cancel puts the entity back.
void live(EditorContext& ed, Session& s, modelkeys::Live phase,
          const std::function<void(EditMesh&)>& op, const char* label);

// What the viewport part needs from main.
struct ViewportHost {
    bool gizmoOut    = false;   // the transform gizmo is out (it may have the pointer)
    bool faceDragged = false;   // a face-gizmo drag is running
    // F while modelling: frame this sphere.
    std::function<void(const glm::vec3& centre, float radius)> frame;
};

// One frame in the Scene window while the modelling mode is on: the modelling
// keys, then the mesh's wireframe, its corners, the element under the pointer,
// the selection, the preview of a hovered button and the flash of the last
// edit, and the modal edit's read-out -- a 2D overlay, like the 3D cursor:
// authoring marks, not things in the scene.
void viewport(EditorContext& ed, const ViewportFrame& view, Session& s, const ViewportHost& h);

// A plain left-click in the viewport while modelling: picks the corner, edge or
// face under the pointer on the selected mesh. True when it hit the mesh -- the
// click is then the mesh's, not the object selection's.
bool click(EditorContext& ed, const ViewportFrame& view, Session& s);

// A face index belongs to one object's mesh and to one version of it: drop it
// when the selection moves, or when an undo left the mesh with fewer faces than
// the index. Returns the selected mesh (null for none).
MeshComponent* keepFaceSelection(EditorContext& ed);

struct PanelHost {
    bool& show;
    bool& showMaterials;              // "Edit" on a face's material opens it
    ImVec2 viewMin{0, 0}, viewMax{0, 0};   // the viewport: where the window opens first
    glm::vec3 cursor{0.0f};           // the 3D cursor, a pivot a spin can turn about
    const SplineSystem* splines = nullptr;   // paths to duplicate along
};
// The Modeling panel, for the selected object. Draws nothing when `show` is false.
void modelingPanel(EditorContext& ed, Session& s, const PanelHost& p);

// The UV panel: where the selected face's texture sits. Same face selection and
// the same one-undo-step edit as the Modeling panel -- the same mesh being
// shaped, looked at from the texture's side. Draws nothing when `show` is false.
void uvPanel(EditorContext& ed, bool& show);

} // namespace modelmode
