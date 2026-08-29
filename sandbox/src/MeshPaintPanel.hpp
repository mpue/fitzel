#pragma once

#include <functional>
#include <vector>

#include <fitzel/asset/AssetId.hpp>

#include "SceneTypes.hpp" // MaterialDef (the paint slots pick from the library)

class MeshComponent;

// The editor's "Mesh Paint" panel: a brush that puts textures on a modelled
// object. Same gesture as the terrain's paint brush and the same four weights
// per corner -- but what those four weights MEAN is the object's own business
// (MeshComponent::paintSlots), picked here from the scene's material library.
// The terrain's layers have nothing to do with it: painting a wall with brick
// must not put brick on the ground.
//
// Only an editable mesh can be painted. A box becomes one in a click (the same
// "Make editable" the Modeling panel offers); imported models cannot, because
// their geometry is shared by every copy in the scene and painting one would
// paint them all.
namespace meshpaintui {

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
    bool& paintMode;

    // Other viewport brushes -- switched off when this one grabs the left button.
    bool& terrainPaintMode;
    bool& grassPaintMode;
    bool& roadEditMode;
    bool& treePaintMode;
    bool& flowerPaintMode;
    bool& sculptMode;
    bool& scatterMode;

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

    std::function<void()> convert;    // Box -> editable mesh
    std::function<void()> clearPaint; // drop this mesh's paint (one undo step)
    // Open the Materials panel on a slot's material, for editing the texture
    // itself rather than which one it is.
    std::function<void(int)> editMaterial;
};

void drawPanel(const PanelState& s);

} // namespace meshpaintui
