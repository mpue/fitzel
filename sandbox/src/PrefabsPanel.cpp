#include "PrefabsPanel.hpp"

#include <cstdio>
#include <string>

#include <imgui.h>

#include "PrefabSystem.hpp"

namespace prefabsui {

namespace {

// How tall a button in this panel is. Deliberately half again the default: these
// open, rename and delete a file, they are pressed by someone whose hand may not
// hold still, and a 19-pixel target is one you have to aim at (see the
// tremor-friendly goal).
float rowHeight() { return ImGui::GetFrameHeight() * 1.7f; }

} // namespace

void drawPanel(const PanelState& s) {
    if (!s.show) return;
    ImGui::SetNextWindowSize(ImVec2(330.0f, 420.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Prefabs", &s.show)) {
const std::string dir = s.prefabDir();
if (dir.empty()) {
    ImGui::TextWrapped(
        "Open or create a project first -- prefabs are saved in "
        "the project's prefabs/ folder.");
} else {
    const bool hasSel =
        s.sel.valid() &&
        s.entities[s.sel.index()].type != EntityType::Sun;
    ImGui::TextDisabled("New prefab from the selected object:");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##prefabName", s.nameBuf,
                     s.nameCap);
    ImGui::BeginDisabled(!hasSel || s.nameBuf[0] == '\0');
    if (ImGui::Button("Create from selection", ImVec2(-1.0f, rowHeight())))
        s.createFromSelection(s.nameBuf);
    ImGui::EndDisabled();
    if (!hasSel)
        ImGui::TextDisabled("(select an object in the scene first)");

    ImGui::Separator();
    const bool busy = s.editing && s.editing();
    ImGui::TextDisabled(busy
        ? "A prefab is open for editing -- close it to use these:"
        : "Double-click to edit; right-click for the rest.");

    // The list is a LIST: one row a prefab, a click picks it, and what can be
    // done to it is said once underneath rather than four times per row. The
    // first shape of this panel put every action on every row, which turned a
    // dozen prefabs into fifty buttons and buried the names between them.
    //
    // Three ways in, because they suit different hands: double-click (the
    // convention -- opening a thing is what a double-click means), the context
    // menu (where a mouse user looks), and the row of buttons below (a big,
    // stationary target that needs no double anything).
    const auto items = prefab::list(dir);
    bool openRename = false, openDelete = false;
    ImGui::BeginDisabled(busy);
    const float rowsH = ImGui::GetContentRegionAvail().y -
                        (rowHeight() + ImGui::GetStyle().ItemSpacing.y * 2.0f);
    ImGui::BeginChild("##prefabList", ImVec2(0.0f, glm::max(rowsH, 60.0f)), true);
    for (const auto& it : items) {
        ImGui::PushID(it.second.c_str());
        const bool selected = s.selPath == it.second;
        if (ImGui::Selectable(it.first.c_str(), selected,
                              ImGuiSelectableFlags_AllowDoubleClick)) {
            s.selPath = it.second;
            s.selName = it.first;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && s.edit)
                s.edit(it.second);
        }
        // Right-click picks the row it was aimed at first, so the menu can never
        // act on something other than what is under the pointer.
        if (ImGui::BeginPopupContextItem("##ctx")) {
            s.selPath = it.second;
            s.selName = it.first;
            if (ImGui::MenuItem("Edit") && s.edit)  s.edit(it.second);
            if (ImGui::MenuItem("Add to scene"))    s.instantiate(it.second);
            ImGui::Separator();
            if (ImGui::MenuItem("Rename...")) {
                std::snprintf(s.renameBuf, s.renameCap, "%s", it.first.c_str());
                openRename = true;
            }
            if (ImGui::MenuItem("Delete...")) openDelete = true;
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (items.empty())
        ImGui::TextDisabled("(no prefabs yet)");
    ImGui::EndChild();

    // ...and the same four things for whatever is picked, in one row of targets
    // that do not move as the list scrolls.
    const bool picked = !s.selPath.empty();
    ImGui::BeginDisabled(!picked);
    const float sp = ImGui::GetStyle().ItemSpacing.x;
    const float bw = (ImGui::GetContentRegionAvail().x - sp * 3.0f) / 4.0f;
    const ImVec2 bs(bw, rowHeight());
    if (ImGui::Button("Edit", bs) && s.edit) s.edit(s.selPath);
    if (ImGui::IsItemHovered() && picked)
        ImGui::SetTooltip("Open \"%s\" on its own, away from the scene",
                          s.selName.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Add", bs)) s.instantiate(s.selPath);
    if (ImGui::IsItemHovered() && picked)
        ImGui::SetTooltip("Drop an instance into the scene, in front of the camera");
    ImGui::SameLine();
    if (ImGui::Button("Rename", bs)) {
        std::snprintf(s.renameBuf, s.renameCap, "%s", s.selName.c_str());
        openRename = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete", bs)) openDelete = true;
    ImGui::EndDisabled();

    // The two questions. At panel level, not inside a row: both can now be asked
    // from three places, and a modal that lived in one row's ID scope would only
    // answer to one of them.
    if (openRename) ImGui::OpenPopup("Rename prefab");
    if (openDelete) ImGui::OpenPopup("Delete prefab");
    if (ImGui::BeginPopupModal("Rename prefab", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Rename \"%s\" to:", s.selName.c_str());
        ImGui::SetNextItemWidth(320.0f);
        const bool entered =
            ImGui::InputText("##newName", s.renameBuf, s.renameCap,
                             ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);
        // What a rename does NOT carry, said where it is being done: the file
        // keeps its GUID, so instances already placed still find it, but anything
        // that NAMES it -- a script's spawnPrefab, a Grid Position marker's rival
        // -- is now naming something that is not there.
        ImGui::TextDisabled(
            "Instances already in a scene are unaffected.\n"
            "Scripts and Grid Position markers that name this\n"
            "prefab will have to be pointed at the new name.");
        ImGui::Separator();
        const bool ok = s.renameBuf[0] != '\0';
        ImGui::BeginDisabled(!ok);
        if (ImGui::Button("Rename", ImVec2(140.0f, rowHeight())) || (entered && ok)) {
            if (s.rename) s.rename(s.selPath, s.renameBuf);
            s.selPath.clear(); s.selName.clear();   // the path just moved
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(140.0f, rowHeight())))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Delete prefab", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete \"%s\"?", s.selName.c_str());
        ImGui::TextDisabled(
            "The file is removed from the project. This cannot be undone.\n"
            "Instances already in a scene stay where they are; scripts and\n"
            "Grid Position markers that name it will find nothing.");
        ImGui::Separator();
        // The dangerous button is not the one the hand is already on: Cancel
        // first, and the other one has to be moved to.
        if (ImGui::Button("Cancel", ImVec2(140.0f, rowHeight())))
            ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.16f, 0.14f, 1.0f));
        if (ImGui::Button("Delete", ImVec2(140.0f, rowHeight()))) {
            if (s.remove) s.remove(s.selPath);
            s.selPath.clear(); s.selName.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();
        ImGui::EndPopup();
    }
    ImGui::EndDisabled();
}
    }
    ImGui::End();
}

} // namespace prefabsui
