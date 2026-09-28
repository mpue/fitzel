#pragma once

#include <functional>
#include <vector>

#include <fitzel/asset/AssetId.hpp>

#include "SceneTypes.hpp" // MaterialDef (the paint slots pick from the library)

class MeshComponent;
struct EditorContext;
struct ViewportFrame;

// The editor's "Mesh Paint" panel: a brush that puts textures on a modelled
// object. Same gesture as the terrain's paint brush and the same four weights
// per corner -- but what those four weights MEAN is the object's own business
// (MeshComponent::paintSlots), picked here from the scene's material library.
// The terrain's layers have nothing to do with it: painting a wall with brick
// must not put brick on the ground.
//
// Only an editable mesh can be painted. A built-in shape becomes one in a click (the same
// "Make editable" the Modeling panel offers); imported models cannot, because
// their geometry is shared by every copy in the scene and painting one would
// paint them all.
namespace meshpaintui {

// The brush: its settings, which the panel edits, and the stroke in flight,
// which the viewport keeps from frame to frame. One object in main.
//
// The weights it lays down live on the mesh's own corners (EditMesh::paint) and
// what they MEAN lives there too (MeshComponent::paintSlots), so a painted
// object is self-contained: it travels, it copies, it becomes a prefab, and none
// of that depends on what the terrain happens to be textured with. The brush
// splits the faces it crosses, because paint on four corners is not a stroke.
struct Brush {
    int   slot     = 0;      // which of the mesh's own four slots
    float radius   = 0.6f;   // world units -- an object-sized brush
    float strength = 0.5f;
    float detail   = 0.25f;  // split faces down to this edge length
    bool  erase    = false;
    // One held button = one undo step: the entity as it was when the stroke
    // started, banked when it ends.
    bool   stroking = false;
    Entity before;
};

// A slot change the panel WANTS, applied by the host after the panel is done
// drawing. Not applied on the spot, because banking it as an undo step assigns
// the entity's "after" snapshot over it and replaces its components -- and the
// MeshComponent* this panel is drawing from would be dangling for the rest of
// the frame. `slot` < 0 means nothing was asked for.
struct SlotEdit {
    int             slot = -1;
    bool            setMaterial = false;
    fitzel::AssetId material;      // invalid = clear the slot
    bool            setScale = false;
    float           scale = 0.0f;
};

struct PanelState {
    bool& show;
    // Only this tool's own switch: which tool has the left button is main's
    // (ViewTool.hpp), so turning this on switches the others off there.
    bool& paintMode;

    const std::vector<MaterialDef>& materials; // the library the slots pick from
    int&   slot;               // which of the mesh's four slots the brush paints
    float& radius;             // brush radius in metres
    float& strength;
    float& detail;             // split faces until their edges are this short
    bool&  erase;              // paint vs back to the object's own material

    MeshComponent* mesh          = nullptr; // null: the selection has no mesh
    bool           haveSelection = false;   // an entity is selected at all
    bool           canConvert    = false;   // ...and it could become a mesh

    int faceCount    = 0;
    int paintedCount = 0;   // corners carrying any weight

    // What the panel wants done to the slots this frame; the host applies it as
    // one undo step once drawPanel has returned. See SlotEdit.
    SlotEdit& edit;

    std::function<void()> convert;    // built-in shape -> editable mesh
    std::function<void()> clearPaint; // drop this mesh's paint (one undo step)
    // Open the Materials panel on a slot's material, for editing the texture
    // itself rather than which one it is.
    std::function<void(int)> editMaterial;
};

void drawPanel(const PanelState& s);

// What the panel needs from main beyond the editor's core.
struct Host {
    bool& show;
    bool& paintMode;               // this tool's own switch (see PanelState)
    bool& showMaterials;           // "Edit" on a slot opens the Materials panel
    std::function<void()> convert; // built-in shape -> editable mesh
};

// The panel for the selected object: drawPanel fed from the editor's core, and
// what it asks for (a slot filled or rescaled, the paint cleared) applied as one
// undo step once it has drawn.
void panel(EditorContext& ed, Brush& brush, const Host& h);

// One frame of the brush: while `paintMode` (mesh paint has the left mouse
// button), hold it over the selected mesh to paint the chosen slot, Alt (or
// Erase) takes the paint back off; the cursor ring lies in the face under it.
// Call it too while a stroke is still open after the button went to another
// tool: with `paintMode` off it banks that stroke and does nothing else -- as
// it does, letting go of the button, when the selection moves off the mesh.
void brushViewport(EditorContext& ed, const ViewportFrame& view, Brush& brush,
                   bool& paintMode, float dt);

} // namespace meshpaintui
