#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

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
    int                        entitySel;

    char*       nameBuf;   // the new prefab's name, kept between frames
    std::size_t nameCap;

    std::function<void(const std::string&)> createFromSelection;
    std::function<void(const std::string&)> instantiate;  // by file path
};

void drawPanel(const PanelState& s);

} // namespace prefabsui
