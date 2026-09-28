#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "Command.hpp"
#include "Document.hpp"
#include "GameSettings.hpp"
#include "SceneTypes.hpp"
#include "Selection.hpp"

namespace fitzel { class Window; class Gui; }
namespace viewnav { class Nav; }

// The editor's menu bar and its default panel layout. Editor only.
//
// Each menu gets exactly the slice of main's state it touches, gathered once
// into a context of references and callbacks -- the same shape projectio's
// Context already uses -- and handed back every frame. That lets the menu bodies
// move out of main() unchanged, which is the point: a menu that had to be
// rewritten in order to be moved is a menu whose behaviour you can no longer
// diff against the one that worked.
namespace editormenu {

// First run (or after "Reset layout"): lay the panels out into a tidy right-hand
// column, split top/bottom, so they don't start as a heap of floating windows.
// Once arranged, ImGui persists it in imgui.ini.
void buildDefaultDockLayout(ImGuiID dockId);

using NameAndPath = std::vector<std::pair<std::string, std::string>>;

struct FileMenuCtx {
    fitzel::Window&                 window;
    const std::string&              currentProject;
    const std::string&              prefLocation;
    const std::vector<std::string>& recentProjects;
    const std::string&              exportStatus;
    const std::string&              autosaveStatus;
    const char*                     projNameBuf;
    char*                           wizName;
    std::size_t                     wizNameCap;
    char*                           wizLocation;
    std::size_t                     wizLocationCap;
    bool&                           wizardOpen;
    bool&                           wizardIsNew;
    game::Settings&                 gameSettings;
    bool&                           gameSettingsOpen;
    std::function<void()>                          saveCurrent;
    std::function<void(const std::string&)>        exportGame;
    std::function<bool(const std::string&)>        openProjectAsync;
    std::function<NameAndPath(const std::string&)> listProjectsIn;
};

struct SceneMenuCtx {
    const std::string& currentProject;
    char*              sceneNameBuf;
    std::size_t        sceneNameCap;
    bool&              sceneNewOpen;
    bool&              sceneRenameOpen;
    bool&              sceneDeleteOpen;
    std::function<void(const std::string&)>        saveSceneFile;
    std::function<bool(const std::string&)>        loadSceneAsync;
    std::function<NameAndPath(const std::string&)> listScenesIn;
};

struct EditMenuCtx {
    CommandStack&        history;
    Document&            document;
    std::vector<Entity>& entities;
    Selection&           sel;
    char*                prefabNameBuf;
    std::size_t          prefabNameCap;
    bool&                showPrefabs;
    std::function<void()>             clampRoadSel;
    std::function<void()>             clampSplineSel;
    // Also re-cuts the watercourse beds: an undo puts different PATHS back and
    // the terrain has to follow them. See main's clampRiverSel.
    std::function<void()>             clampRiverSel;
    std::function<void()>             duplicateSelection;
    std::function<void()>             deleteSelection;
};

// One row per entry in the View menu: the submenu it sits under, its label
// (nullptr = a separator within that submenu), the shortcut hint, and the bool
// it toggles. A table rather than twenty-eight MenuItem calls, because "Close
// all panels" has to clear exactly the set the menu opens -- kept as two
// hand-written lists they drift apart, and a panel you cannot close is worse
// than one you cannot open. `closeAll = false` holds an entry out of that sweep.
struct PanelEntry {
    const char* group;
    const char* label;
    const char* shortcut;
    bool*       flag;
    bool        closeAll = true;
};

void drawFileMenu(const FileMenuCtx& c);
void drawSceneMenu(const SceneMenuCtx& c);
void drawEditMenu(const EditMenuCtx& c);
void drawViewMenu(fitzel::Gui& gui, const std::vector<PanelEntry>& panels,
                  viewnav::Nav& viewNav, bool& prefsDirty, bool& requestDockRebuild);

} // namespace editormenu
