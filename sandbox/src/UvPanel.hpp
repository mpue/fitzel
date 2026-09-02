#pragma once

#include <functional>
#include <vector>

#include <fitzel/asset/AssetId.hpp>

#include "SceneTypes.hpp"   // MaterialDef (the texture the panel draws under the face)

class MeshComponent;

// The editor's "UV" panel: where a face's texture sits on it.
//
// Only for meshes fitzel made itself -- a MeshComponent, i.e. a box somebody
// made editable and then shaped. An imported glTF arrives with the UVs its
// author unwrapped, and second-guessing those from here would be an editor
// quietly rewriting somebody else's work.
//
// It is not a corner-dragging unwrapper, and that is deliberate twice over.
// Once because this mesh kind is a box modeller: its faces are flat convex
// polygons, and everything you actually want on box architecture -- "the brick
// is too big", "turn the planks ninety degrees", "line the course up across all
// four walls" -- is a placement of the projection, not a rearrangement of
// islands. And once because dragging individual points is the one interaction
// this editor is built to avoid: every control here is a number you can ask for
// and get, and the drag inside the preview aims at the whole face at once.
namespace uvui {

struct PanelState {
    bool&           show;
    MeshComponent*  mesh;        // null: the selection has no editable mesh
    int&            faceSel;     // face selected in the viewport, -1 for none
    const std::vector<MaterialDef>& materials;
    // The material the OBJECT wears, for a face that wears none of its own --
    // the panel draws the texture the face will actually be seen through, and
    // most faces are seen through this one.
    fitzel::AssetId objectMaterial;

    bool haveSelection = false;  // an entity is selected at all
    bool canConvert    = false;  // ...and it could become a mesh

    std::function<void()> convert;
    // One undo step, exactly as the Modeling panel takes it: the callback is
    // handed the mesh and returns the face to keep selected.
    std::function<void(const std::function<int(MeshComponent&)>&, const char*)> edit;

    int faceCount = 0;
};

void drawPanel(const PanelState& s);

} // namespace uvui
