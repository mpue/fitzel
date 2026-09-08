#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "Selection.hpp"
#include "SceneTypes.hpp" // Entity

// The editor's "Prefabs" panel: the reusable object templates in the project's
// prefabs/ folder. Make one from what is selected, or click a saved one to drop
// an instance into the scene.
//
// It needs a project: prefabs live in a folder, and with no project open there
// is nowhere to put them -- which the panel says rather than offering a button
// that would quietly do nothing.
namespace prefabsui {

struct PanelState {
    bool& show;

    // Where the prefabs live. Empty means no project is open, which is a state
    // this panel has to show rather than work around.
    std::function<std::string()> prefabDir;

    const std::vector<Entity>& entities;  // to know whether the selection can be one
    const Selection&           sel;

    char*       nameBuf;   // the new prefab's name, kept between frames
    std::size_t nameCap;

    std::function<void(const std::string&)> createFromSelection;
    std::function<void(const std::string&)> instantiate;  // by file path
    // Open one for editing on its own, away from the scene (see PrefabEdit.hpp).
    // Its own button per row rather than a right-click menu: this is the door to
    // a mode, and a door you have to know about is not a door.
    std::function<void(const std::string&)> edit;         // by file path
    // True while a session is up -- the whole list is shut then: the scene is
    // stashed, so there is nothing to drop an instance into and no second prefab
    // to open.
    std::function<bool()> editing;

    // --- Renaming and deleting -----------------------------------------------
    // Both act on FILES, so both are the caller's to do; the panel only asks. The
    // buffer and the two targets live with the caller for the same reason the
    // hierarchy's rename buffer does: they have to survive the frames a modal is
    // open, and the panel is redrawn from scratch every one of them.
    std::function<void(const std::string& path, const std::string& newName)> rename;
    std::function<void(const std::string& path)>                             remove;
    char*        renameBuf;
    std::size_t  renameCap;
    // Which prefab is picked: everything the panel can do acts on this one, and
    // it is what a double-click, a right-click and the button row all agree
    // about. Kept by the caller because it has to outlive the frames a modal is
    // open, exactly like the hierarchy's rename state.
    std::string& selPath;
    std::string& selName;
};

void drawPanel(const PanelState& s);

} // namespace prefabsui
