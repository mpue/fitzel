#include "EditorMenus.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include <imgui_internal.h>   // DockBuilder

#include <fitzel/Version.hpp>
#include <fitzel/core/Window.hpp>
#include <fitzel/ui/Gui.hpp>

#include "FolderDialog.hpp"
#include "ProjectIO.hpp"
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
    if (ImGui::MenuItem("Save Project", "Ctrl+S", false,
                        !c.currentProject.empty() && !c.playMode))
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

void drawAbout(bool& show) {
    if (!show) return;
    ImGui::SetNextWindowSize(ImVec2(360.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::Begin("About Fitzel", &show,
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::Text("Fitzel %d.%d.%d", fitzel::kVersionMajor, fitzel::kVersionMinor,
                    fitzel::kVersionPatch);
        ImGui::TextDisabled("3D vegetation & road engine");
        ImGui::Separator();
        // The four-part version alone can't tell two builds of one commit
        // apart, so show what identifies this binary exactly.
        ImGui::Text("Build %d", fitzel::kVersionBuild);
        if (fitzel::kGitHash[0])
            ImGui::Text("Commit %s%s", fitzel::kGitHash,
                        fitzel::kGitDirty ? " (uncommitted changes)" : "");
        ImGui::Spacing();
        if (ImGui::Button("Copy version")) ImGui::SetClipboardText(fitzel::kVersionFull);
    }
    ImGui::End();
}

void drawProjectWizard(const FileMenuCtx& c, const std::function<void()>& newProject,
                       const std::function<void(const std::string&)>& saveProjectTo) {
    if (c.wizardOpen) { ImGui::OpenPopup("Project Wizard"); c.wizardOpen = false; }
    ImGui::SetNextWindowSize(ImVec2(520.0f, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Project Wizard", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::TextUnformatted(c.wizardIsNew ? "Create a new project" : "Save project as");
    ImGui::Separator();
    const float fieldW = 340.0f;
    ImGui::SetNextItemWidth(fieldW);
    ImGui::InputText("Name", c.wizName, c.wizNameCap);
    ImGui::SetNextItemWidth(fieldW);
    ImGui::InputText("Location", c.wizLocation, c.wizLocationCap);
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        std::string picked;
        if (ed::pickFolder(picked, c.wizLocation[0] ? std::string(c.wizLocation) : c.prefLocation))
            std::snprintf(c.wizLocation, c.wizLocationCap, "%s", picked.c_str());
    }

    const std::string safe   = projectio::safeName(c.wizName);
    const std::string loc(c.wizLocation);
    const std::string target = loc.empty() ? std::string() : (loc + "/" + safe);
    std::error_code vec;
    const bool nameOk = c.wizName[0] != '\0';
    const bool locOk  = !loc.empty() && std::filesystem::is_directory(loc, vec);
    const bool exists = nameOk && locOk && std::filesystem::exists(target, vec);

    ImGui::Spacing();
    if (!target.empty()) {
        // Bound the wrap so a long path can't stretch the modal wide.
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 460.0f);
        ImGui::TextDisabled("Folder: %s", target.c_str());
        ImGui::PopTextWrapPos();
    }
    const ImVec4 warn(1.0f, 0.55f, 0.3f, 1.0f);
    if (!nameOk)     ImGui::TextColored(warn, "Enter a project name.");
    else if (!locOk) ImGui::TextColored(warn, "Location does not exist.");
    else if (exists) ImGui::TextColored(warn, "A folder with that name already exists here.");
    ImGui::Spacing();

    const bool canGo = nameOk && locOk && !exists;
    ImGui::BeginDisabled(!canGo);
    if (ImGui::Button(c.wizardIsNew ? "Create" : "Save", ImVec2(120.0f, 0.0f))) {
        if (c.wizardIsNew && newProject) newProject();
        if (saveProjectTo) saveProjectTo(target);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void drawSceneDialogs(const SceneMenuCtx& c, const SceneDialogActions& a) {
    if (c.sceneNewOpen)    { ImGui::OpenPopup("New Scene");    c.sceneNewOpen = false; }
    if (c.sceneRenameOpen) { ImGui::OpenPopup("Rename Scene"); c.sceneRenameOpen = false; }
    if (c.sceneDeleteOpen) { ImGui::OpenPopup("Delete Scene"); c.sceneDeleteOpen = false; }
    const std::string sceneFolder =
        c.currentProject.empty()
            ? std::string()
            : std::filesystem::path(c.currentProject).parent_path().generic_string();
    // 0 = ok, 1 = empty, 2 = a scene with that name already exists. `allowSelf`
    // lets the current scene's own file match (used by Rename).
    auto sceneNameState = [&](bool allowSelf) -> int {
        if (c.sceneNameBuf[0] == '\0') return 1;
        const std::string target =
            sceneFolder + "/" + projectio::safeName(c.sceneNameBuf) + ".fitzel";
        std::error_code ec;
        if (std::filesystem::exists(target, ec) && !(allowSelf && target == c.currentProject))
            return 2;
        return 0;
    };
    const ImVec4 sceneWarn(1.0f, 0.55f, 0.3f, 1.0f);

    ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("New Scene", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("New scene in this project");
        ImGui::TextDisabled("Shares the project's materials; starts from the "
                            "current world with no objects.");
        ImGui::Separator();
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(300.0f);
        ImGui::InputText("Name##newscene", c.sceneNameBuf, c.sceneNameCap);
        const int st = sceneNameState(false);
        if (st == 1)      ImGui::TextColored(sceneWarn, "Enter a scene name.");
        else if (st == 2) ImGui::TextColored(sceneWarn, "A scene with that name already exists.");
        ImGui::Spacing();
        ImGui::BeginDisabled(st != 0);
        if (ImGui::Button("Create", ImVec2(120.0f, 0.0f))) {
            if (a.create) a.create(sceneFolder, c.sceneNameBuf);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Rename Scene", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Rename the current scene");
        ImGui::Separator();
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(300.0f);
        ImGui::InputText("Name##renscene", c.sceneNameBuf, c.sceneNameCap);
        const int st = sceneNameState(true); // its own file may match
        if (st == 1)      ImGui::TextColored(sceneWarn, "Enter a scene name.");
        else if (st == 2) ImGui::TextColored(sceneWarn, "A scene with that name already exists.");
        ImGui::Spacing();
        ImGui::BeginDisabled(st != 0);
        if (ImGui::Button("Rename", ImVec2(120.0f, 0.0f))) {
            if (a.rename) a.rename(c.sceneNameBuf);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Delete Scene", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete scene \"%s\"?",
                    std::filesystem::path(c.currentProject).stem().string().c_str());
        ImGui::TextDisabled("This permanently removes the .fitzel file from disk.");
        ImGui::Spacing();
        if (ImGui::Button("Delete", ImVec2(120.0f, 0.0f))) {
            const std::string gone = c.currentProject;
            std::string next; // switch to another scene before removing this one
            if (c.listScenesIn)
                for (const auto& [n, p] : c.listScenesIn(sceneFolder))
                    if (p != gone) { next = p; break; }
            if (!next.empty() && a.removeCurrent) a.removeCurrent(next, gone);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace editormenu
