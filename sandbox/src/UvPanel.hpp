#pragma once

#include <functional>
#include <string>
#include <vector>

#include <imgui.h>

#include <fitzel/asset/AssetId.hpp>

#include "SceneTypes.hpp"   // MaterialDef (the texture the panel draws under the faces)
#include "UvOps.hpp"

class MeshComponent;

// The editor's UV editor, laid out and driven the way Blender's is.
//
// The texture fills the window; over it the faces of the mesh as they sit on
// it -- the faces picked in the viewport (all of them while none are, or with
// "All faces" on). Select corners, edges, faces or whole islands (1 2 3 4),
// welded corners coming along (sticky); move, turn and scale with G, R and S;
// unwrap with U (along the seams, conformal), smart-project, project onto a
// cube, a cylinder, a sphere or in metres; pack islands, even out their
// density, pin, merge, align, snap to texels. Every operation is also in the
// menus and on a button, and none needs a drag: G R S follow the pointer from
// one click to the next (Blender's own way), a box is click and click, and a
// number can be typed for any of them. The operations themselves are in
// UvOps.hpp; seams are marked in the modelling mode's edge mode, or from the
// islands here.
//
// Only for meshes fitzel made (a MeshComponent): an imported model keeps the
// UVs its author unwrapped.
namespace uvui {

// A modal edit through the host: Begin remembers the entity, each Set starts
// again from it and applies the edit, Commit makes it one undo step, Cancel
// puts it back.
enum class Live { Begin, Set, Commit, Cancel };

struct PanelState {
    bool&           show;
    MeshComponent*  mesh;        // null: the selection has no editable mesh
    int&            faceSel;     // the active face (kept across edits)
    int             ownerId = -1;    // the entity the mesh belongs to: another one starts afresh
    std::vector<int> picked;     // faces picked in the viewport (empty: none)
    const std::vector<MaterialDef>& materials;
    // The material the OBJECT wears, for faces that wear none of their own.
    fitzel::AssetId objectMaterial;

    bool haveSelection = false;  // an entity is selected at all
    bool canConvert    = false;  // ...and it could become a mesh

    std::function<void()> convert;
    // One undo step: handed the mesh, returns the face to keep active.
    std::function<void(const std::function<int(MeshComponent&)>&, const char*)> edit;
    std::function<void(Live, const std::function<void(EditMesh&)>&, const char*)> live;
    // The mesh again, after an edit may have replaced the component.
    std::function<MeshComponent*()> meshNow;
    // Ctrl+Z / Ctrl+Y while the window has the keyboard (it claims it, as the
    // image editor does: 1 2 3 4, X and the rest are its own here).
    std::function<void(bool redo)> undo;

    int faceCount = 0;
};

void drawPanel(const PanelState& s);

// The window had the keyboard last frame.
bool hasKeyboard();

// --- For uvpanelcheck ----------------------------------------------------------------
uvops::Selection&  selection();
uvops::SelectMode& selectMode();
// Where a UV point was on screen in the last frame drawn.
ImVec2 toScreen(glm::vec2 uv);
// Is a G / R / S running.
bool   transforming();
// The last thing an operation said.
const std::string& message();

} // namespace uvui
