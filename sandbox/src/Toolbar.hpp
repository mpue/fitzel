#pragma once

#include <functional>
#include <vector>

#include <imgui.h>      // ImGuizmo.h leans on ImGui's types; must come first
#include <ImGuizmo.h>

#include "SceneTypes.hpp" // EntityType
#include "ToolbarIcons.hpp"
#include "ViewTool.hpp"

// The toolbar strip under the menu bar. A viewport side bar reserves space at
// the top of the work area, so the dockspace below shifts down by itself. It
// starts with Save, then the Select/Create pair (what a viewport click does),
// the shapes (clicking one makes that type the active one), the terrain, the
// gizmo's Move/Rotate/Scale and its space, how the viewport draws the scene,
// and then the tools: the road editor and the windows most work happens in
// (State::panels). The pictures are painted by icon:: (ToolbarIcons.hpp).
// Editor only.
namespace toolbar {

struct State {
    bool&       placeMode;       // Create: a click on empty ground drops the shape
    EntityType& newType;         // the shape a click (or its button) makes
    std::function<void(EntityType)> addShape;   // Select mode: one now, at the spawn point
    bool        terrainOn = false;              // the scene has ground already
    std::function<void()> addTerrain;
    ImGuizmo::OPERATION& gizmoOp;
    ImGuizmo::MODE&      gizmoMode;
    bool  playMode = false;      // the shading buttons are off in Play
    int&  viewShade;             // ViewShade.hpp
    std::function<void()> refreshTrace;         // Pathtraced pressed again: look again
    ViewTool& viewTool;
    bool&     showRoads;         // the road editor opens its panel with it
    // Save: the project and the scene that is open, as File > Save Project.
    // Greyed out while there is no project to save into.
    std::function<void()> save;
    bool canSave = false;
    // The tools, in the order they are drawn: a button per tool window that
    // opens and closes it (lit while it is open), Tool::None for the gap
    // between two groups, Tool::Road for the road editor.
    struct Panel {
        icon::Tool  tool;
        const char* tip;
        bool*       open;
    };
    std::vector<Panel> panels;
};

void draw(const State& s);

} // namespace toolbar
