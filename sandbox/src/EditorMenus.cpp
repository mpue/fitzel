#include "EditorMenus.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include <imgui_internal.h>   // DockBuilder

#include <fitzel/core/Window.hpp>
#include <fitzel/ui/Gui.hpp>

#include "FolderDialog.hpp"
#include "UiStyle.hpp"
#include "ViewportNav.hpp"

using namespace fitzel;

namespace editormenu {

// --- The default panel layout ----------------------------------------------

// First run (or after "Reset layout"): lay the panels out into a tidy right-hand
// column, split top/bottom, so they don't start as a heap of floating windows.
// Once arranged, ImGui persists it in imgui.ini.
void buildDefaultDockLayout(ImGuiID dockId) {
    ImGui::DockBuilderRemoveNode(dockId);
    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_PassthruCentralNode
                                    | ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockId, ImGui::GetMainViewport()->WorkSize);

    // Hierarchy (left) | Scene (centre) | Inspector over ONE tool dock
    // (right).
    //
    // Every tool panel is pre-docked into that single node, so opening
    // one adds a TAB rather than a window. Left to float, they get
    // dragged out into a tiled wall -- which is exactly what happened:
    // fourteen panels side by side and the viewport reduced to a tab
    // behind one of them. The panels are not the work; the scene is,
    // and it keeps the middle.
    //
    // This does not make the panels fewer, only stop them competing
    // for the same space. Fewer is a different job (merging them by
    // task rather than by source file).
    ImGuiID central = 0;
    ImGuiID left  = ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Left, 0.18f,
                                                nullptr, &central);
    ImGuiID right = ImGui::DockBuilderSplitNode(central, ImGuiDir_Right, 0.30f,
                                                nullptr, &central);
    // Inspector keeps its own strip above the tools: it is the one
    // panel wanted WHILE a tool is open (pick a road point, look at
    // what it is), so making it a peer tab would mean flipping back
    // and forth.
    ImGuiID inspector = 0;
    ImGuiID tools = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.62f,
                                                nullptr, &inspector);

    ImGui::DockBuilderDockWindow("Scene", central);
    ImGui::DockBuilderDockWindow("Hierarchy", left);
    ImGui::DockBuilderDockWindow("Inspector", inspector);
    // Titles must match the ImGui::Begin() strings exactly -- a typo
    // costs a floating window and nothing else, which is why they are
    // in one list rather than scattered over the call sites.
    for (const char* w : {
            "Terrain", "Terrain Sculpt", "Terrain Paint", "Water",
            "Sky & atmosphere", "Weather & audio", "Colour grade",
            "Environment", "Advanced nature",
            "Vegetation", "Scatter",
            "Roads", "City", "Buildings",
            "Materials", "Models", "Prefabs", "Assets",
            "UI Overlay", "Camera path", "Timeline", "Animation graph",
            "Camera", "3D Cursor", "Render",
            "Vehicle", "Glider", "Voxels", "Mixer", "Scripts",
            "Performance", "Stats"})
        ImGui::DockBuilderDockWindow(w, tools);
    ImGui::DockBuilderFinish(dockId);
}

void drawFileMenu(const FileMenuCtx& c) {
    if (!ImGui::BeginMenu("File")) return;
    if (ImGui::MenuItem("New Project...")) {
        c.wizardIsNew = true;
        c.wizName[0] = '\0';
        std::snprintf(c.wizLocation, c.wizLocationCap, "%s",
                      c.prefLocation.c_str());
        c.wizardOpen = true;
    }
    if (ImGui::MenuItem("Save Project", nullptr, false,
                        !c.currentProject.empty()))
        c.saveCurrent();
    if (ImGui::MenuItem("Save Project As...")) {
        c.wizardIsNew = false;
        std::snprintf(c.wizName, c.wizNameCap, "%s", c.projNameBuf);
        std::snprintf(c.wizLocation, c.wizLocationCap, "%s",
                      c.prefLocation.c_str());
        c.wizardOpen = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Game Settings...", nullptr, false,
                        !c.currentProject.empty())) {
        // Load the project's current settings, then open the modal.
        c.gameSettings = game::load(
            std::filesystem::path(c.currentProject)
                .parent_path().generic_string());
        c.gameSettingsOpen = true;
    }
    if (ImGui::MenuItem("Export Game...", nullptr, false,
                        !c.currentProject.empty())) {
        std::string picked;
        if (ed::pickFolder(picked, c.prefLocation)) c.exportGame(picked);
    }
    if (!c.exportStatus.empty())
        ImGui::TextDisabled("%s", c.exportStatus.c_str());
    // When the last crash snapshot was taken. Not an action, just proof the
    // safety net is there -- which is the only thing anyone wants to know about
    // it until the day they need it.
    if (!c.autosaveStatus.empty())
        ImGui::TextDisabled("%s", c.autosaveStatus.c_str());
    ImGui::Separator();
    if (ImGui::BeginMenu("Open Project")) {
        if (ImGui::MenuItem("Browse folder...")) {
            std::string picked;
            if (ed::pickFolder(picked, c.prefLocation) &&
                !c.openProjectAsync(picked))
                std::fprintf(stderr,
                    "No project (.fitzel) in %s\n", picked.c_str());
        }
        if (!c.recentProjects.empty()) {
            ui::sectionText("Recent");
            int ri = 0;
            for (const std::string& folder : c.recentProjects) {
                // Scope each item by index so two entries can never
                // share an ImGui id, even if a duplicate path slips
                // into the list.
                ImGui::PushID(ri++);
                const std::string lbl =
                    std::filesystem::path(folder).filename().string();
                if (ImGui::MenuItem(lbl.c_str()))
                    c.openProjectAsync(folder);
                ImGui::PopID();
            }
        }
        ui::sectionText("In default location");
        const auto projs = c.listProjectsIn(c.prefLocation);
        if (projs.empty()) ImGui::TextDisabled("(none)");
        for (const auto& [n, folder] : projs)
            if (ImGui::MenuItem((n + "##d" + folder).c_str()))
                c.openProjectAsync(folder);
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Exit")) c.window.requestClose();
    ImGui::EndMenu();
}

void drawSceneMenu(const SceneMenuCtx& c) {
    if (!ImGui::BeginMenu("Scene")) return;
    if (c.currentProject.empty()) {
        ImGui::TextDisabled("Open or create a project first.");
    } else {
        const std::string projFolder =
            std::filesystem::path(c.currentProject).parent_path().generic_string();
        if (ImGui::MenuItem("New Scene...")) {
            c.sceneNameBuf[0] = '\0';
            c.sceneNewOpen = true;
        }
        if (ImGui::MenuItem("Save Scene"))
            c.saveSceneFile(c.currentProject);
        if (ImGui::MenuItem("Rename Scene...")) {
            std::snprintf(c.sceneNameBuf, c.sceneNameCap, "%s",
                std::filesystem::path(c.currentProject).stem().string().c_str());
            c.sceneRenameOpen = true;
        }
        const auto scenes = c.listScenesIn(projFolder);
        ImGui::BeginDisabled(scenes.size() < 2); // keep at least one scene
        if (ImGui::MenuItem("Delete Scene..."))
            c.sceneDeleteOpen = true;
        ImGui::EndDisabled();
        ui::sectionText("Switch to");
        for (const auto& [n, path] : scenes) {
            const bool active = (path == c.currentProject);
            if (ImGui::MenuItem((n + "##sc" + path).c_str(), nullptr, active) &&
                !active) {
                c.saveSceneFile(c.currentProject); // don't lose current edits
                c.loadSceneAsync(path);
            }
        }
    }
    ImGui::EndMenu();
}

void drawEditMenu(const EditMenuCtx& c) {
    if (!ImGui::BeginMenu("Edit")) return;
    const std::string undoLbl = c.history.canUndo()
        ? std::string("Undo ") + c.history.undoName() : "Undo";
    const std::string redoLbl = c.history.canRedo()
        ? std::string("Redo ") + c.history.redoName() : "Redo";
    if (ImGui::MenuItem(undoLbl.c_str(), "Ctrl+Z", false, c.history.canUndo())) {
        c.history.undo(c.document); c.sel.clear(); c.clampRoadSel(); c.clampSplineSel();
        c.clampRiverSel();
    }
    if (ImGui::MenuItem(redoLbl.c_str(), "Ctrl+Y", false, c.history.canRedo())) {
        c.history.redo(c.document); c.sel.clear(); c.clampRoadSel(); c.clampSplineSel();
        c.clampRiverSel();
    }
    ImGui::Separator();
    const bool hasSel = c.sel.valid() &&
        c.entities[c.sel.index()].type != EntityType::Sun;
    const int selCount = static_cast<int>(c.sel.count());
    const char* dupLbl = selCount > 1 ? "Duplicate selection" : "Duplicate";
    const char* delLbl = selCount > 1 ? "Delete selection"    : "Delete";
    if (ImGui::MenuItem(dupLbl, nullptr, false, hasSel))
        c.duplicateSelection();
    if (ImGui::MenuItem(delLbl, nullptr, false, hasSel))
        c.deleteSelection();
    if (ImGui::MenuItem("Save as Prefab...", nullptr, false, hasSel)) {
        // Seed the name field from the selection and open the panel;
        // the panel's "Create" button does the actual save.
        const std::string nm = c.entities[c.sel.index()].name;
        std::snprintf(c.prefabNameBuf, c.prefabNameCap, "%s",
                      nm.c_str());
        c.showPrefabs = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Clear objects")) {
        c.entities.erase(std::remove_if(c.entities.begin(), c.entities.end(),
            [](const Entity& e){ return e.type != EntityType::Sun; }),
            c.entities.end());
        c.sel.clear();
        c.history.clear(); // bulk reset -> drop history
    }
    ImGui::EndMenu();
}

void drawViewMenu(Gui& gui, const std::vector<PanelEntry>& panels,
                  viewnav::Nav& viewNav,
                  bool& prefsDirty, bool& requestDockRebuild) {
    if (!ImGui::BeginMenu("View")) return;
    // Where the camera looks from, before which windows are open: it is the one
    // entry here that changes the picture rather than the furniture, and it is
    // the way to reach a standard view without a numpad -- or without holding
    // anything steady, which is the point (see the editor's aims in README).
    viewNav.drawMenu();
    ImGui::Separator();
    // Grouped by the JOB, not by which file draws it. A flat list of twenty-eight
    // entries is a list you read start to finish every time; "where do I set fog"
    // has an obvious answer only once the entries are sorted the way the work is.
    const char* group = nullptr;
    bool        open  = false;
    for (const PanelEntry& e : panels) {
        if (!group || std::strcmp(group, e.group) != 0) {
            if (open) ImGui::EndMenu();
            group = e.group;
            open  = ImGui::BeginMenu(group);
        }
        if (!open)    continue;
        if (!e.label) ImGui::Separator();
        else          ImGui::MenuItem(e.label, e.shortcut, e.flag);
    }
    if (open) ImGui::EndMenu();
    ImGui::Separator();
    // Text size/typeface are a comfort setting, not a scene one: they live in the
    // editor prefs and apply immediately.
    if (ImGui::BeginMenu("Interface")) {
        float px = gui.fontSize();
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::SliderFloat("Text size", &px, 14.0f, 28.0f, "%.0f pt")) {
            gui.setFontSize(px);
            prefsDirty = true;
        }
        if (gui.fontFamilyCount() > 1) {
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::BeginCombo("Typeface",
                                  gui.fontFamilyName(gui.fontFamily()))) {
                for (int i = 0; i < gui.fontFamilyCount(); ++i)
                    if (ImGui::Selectable(gui.fontFamilyName(i),
                                          i == gui.fontFamily())) {
                        gui.setFontFamily(i);
                        ui::setBoldFont(gui.boldFont());
                        prefsDirty = true;
                    }
                ImGui::EndCombo();
            }
        }
        ui::hint("Verdana and Tahoma have the largest x-height "
                 "-- easiest to read at small sizes.");
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Close all panels")) {
        // One click back to scene + hierarchy + inspector. The panels are cheap
        // to reopen and expensive to look past.
        for (const PanelEntry& e : panels)
            if (e.flag && e.closeAll) *e.flag = false;
    }
    if (ImGui::MenuItem("Reset layout")) requestDockRebuild = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Put the panels back into one docked tab\n"
                          "strip on the right, with the scene in the\n"
                          "middle. Needed once after an update: your\n"
                          "own arrangement is remembered in imgui.ini\n"
                          "and wins until you ask for this.");
    ImGui::EndMenu();
}

} // namespace editormenu
