#pragma once

#include <cstddef>
#include <functional>
#include <vector>

#include <glm/glm.hpp>

#include "SceneTypes.hpp"  // Entity, EntityType
#include "Selection.hpp"

class Document;

// The editor's "Hierarchy" panel: the scene tree. Roots first, children nested;
// single click selects (Ctrl+click extends), F2 or a double-click renames in
// place, drag a node onto another to reparent (onto empty space to unparent),
// right-click for the create/duplicate/delete menu.
//
// Editor-only -- the player has no scene tree to edit. Unlike most panels this
// one has no `show` flag: Hierarchy is always open, the same way Inspector is.
namespace hierarchyui {

// What the panel touches in main. References rather than a back-pointer to the
// editor, so the panel can be read on its own and can't quietly reach for more
// than this list -- the same shape roadui::PanelState uses.
struct PanelState {
    std::vector<Entity>& entities;
    Document&            document;
    // What is selected, and how to change it. The panel both reads this (to
    // mark rows, to enable the buttons) and drives it (click, Ctrl+click), so
    // it takes the selection itself rather than a handful of accessors.
    Selection&           sel;

    // --- Inline rename -------------------------------------------------------
    // The state lives in main because the viewport's key handling has to know a
    // rename is in progress (F2's field eats the movement keys).
    int&        renameId;    // entity being renamed, -1 = none
    char*       renameBuf;
    std::size_t renameCap;
    bool&       renameFocus; // grab the keyboard on the frame the field appears

    // --- Acting on a row -----------------------------------------------------
    // One undoable step each; main owns the history, so main does them.
    std::function<void(int)>                 duplicateEntity;   // by index
    std::function<void(int)>                 deleteEntity;
    std::function<void()>                    duplicateSelection;
    std::function<void()>                    deleteSelection;
    std::function<void(int)>                 addEmptyParent;
    std::function<void(int)>                 addEmptyChild;
    std::function<void(int, EntityType)>     addPrimitiveChild;
    std::function<void(int)>                 addShotCamera;     // "Shoot this"
    std::function<void(int)>                 addVehicleLights;
    std::function<void(int)>                 setMainCamera;     // by entity id

    // --- Reparenting ---------------------------------------------------------
    // Rejecting a cycle needs the ancestor test, and keeping the child put needs
    // its parent's world matrix -- both live in main next to the scene graph.
    std::function<bool(int, int)>                  isUnderId;   // (id, ancestorId)
    std::function<glm::mat4(const Entity&)>        worldOf;
    std::function<void(Entity&, const glm::mat4*)> rebaseLocal;

    // "Save as Prefab..." seeds the Prefabs panel's name field and opens it.
    char*       prefabNameBuf;
    std::size_t prefabNameCap;
    bool&       showPrefabs;
};

void drawPanel(const PanelState& s);

} // namespace hierarchyui
