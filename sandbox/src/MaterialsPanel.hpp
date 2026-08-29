#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

#include <fitzel/asset/AssetId.hpp>

#include "Document.hpp"
#include "SceneTypes.hpp" // MaterialDef, Entity

namespace fitzel {
class AssetDatabase;
class Texture;
}
class VideoLibrary;

// The editor's "Materials" panel: the scene's material library, and the editor
// for whichever one is selected -- colour, the surface numbers, and the texture
// slots you drop assets onto.
//
// Two kinds of material live in the same list and the difference shows here. A
// LIBRARY material is a .fmat file in the project and owns itself. A MODEL
// material came in with an imported model, cannot be deleted from this list, and
// keeps its edits in the scene rather than in a file of its own -- which is why
// the texture slots offer "Model" beside "Clear": one empties the slot, the
// other puts back what the model shipped.
//
// Unlike the viewport tools, this one edits the library in place instead of
// reporting what it wants. That is safe precisely because material edits do not
// go through the undo stack: nothing here pushes a command, so nothing here
// assigns an entity's snapshot over it and invalidates the references the panel
// is holding (see docs/invariants.md, "push oder pushApplied"). A material edit
// that ever becomes undoable has to move to the deferred shape the mesh paint
// panel uses.
namespace materialsui {

struct PanelState {
    bool& show;

    std::vector<MaterialDef>& materials;  // the library, edited in place
    Document&                 document;   // addMaterial mints the GUID
    std::vector<Entity>&      entities;   // a deleted material has users to rehome
    int&                      sel;        // index into `materials`, -1 for none
    char*                     filter;     // the name search box's buffer
    std::size_t               filterCap;

    fitzel::AssetDatabase& assetDb;       // resolves dropped asset GUIDs
    VideoLibrary&          videos;        // reports what a bound .fvid holds

    // The little square in front of a texture slot. Kept with the host because
    // it draws out of the thumbnail cache, which is the editor's and not this
    // panel's business.
    std::function<void(const std::shared_ptr<fitzel::Texture>&, fitzel::AssetId)> texSwatch;
};

void drawPanel(const PanelState& s);

} // namespace materialsui
