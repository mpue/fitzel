#include "HierarchyPanel.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>

#include <glm/glm.hpp>
#include <imgui.h>

#include "Component.hpp"
#include "Document.hpp"
#include "UiStyle.hpp"

namespace hierarchyui {

void drawPanel(const PanelState& s) {
    if (!ImGui::Begin("Hierarchy")) { ImGui::End(); return; }

    ImGui::BeginDisabled(!s.sel.valid());
    if (ImGui::Button("Duplicate")) s.duplicateEntity(s.sel.index());
    ImGui::SameLine();
    if (ImGui::Button("Delete")) s.deleteEntity(s.sel.index());
    ImGui::EndDisabled();

    ui::sectionText("Scene");
    // A proper tree control: roots first, children nested; the tree
    // fills the panel and scrolls. Single click selects, arrow/double-
    // click expands; drag a node onto another to reparent (onto empty
    // space to unparent); right-click for Duplicate/Delete.
    int reparentSrc = -1, reparentTo = -2; // -2 = none, -1 = root
    int dupReq = -1, delReq = -1;
    // Deferred context-menu creation requests (applied after the tree
    // is drawn, so entities isn't mutated mid-iteration).
    int emptyParentReq = -1, emptyChildReq = -1, primChildReq = -1;
    int shotCamReq = -1;   // "Shoot this": a multishot camera on this object
    int vehicleLightsReq = -1;
    EntityType primChildType = EntityType::Box;
    auto typeColor = [](EntityType t) -> ImU32 {
        switch (t) {
            case EntityType::Light:    return IM_COL32(255, 224, 130, 255);
            case EntityType::Sun:      return IM_COL32(255, 200,  90, 255);
            case EntityType::Model:    return IM_COL32(150, 200, 255, 255);
            case EntityType::Sphere:   return IM_COL32(190, 230, 200, 255);
            case EntityType::Plane:    return IM_COL32(205, 225, 210, 255);
            case EntityType::Cylinder: return IM_COL32(200, 210, 235, 255);
            case EntityType::Empty:    return IM_COL32(170, 175, 185, 255);
            default:                   return IM_COL32(220, 220, 225, 255);
        }
    };
    std::function<void(int)> drawNode = [&](int i) {
        ImGui::PushID(s.entities[i].id);           // stable id
        bool hasChildren = false;
        for (const Entity& c : s.entities)
            if (c.parent == s.entities[i].id) { hasChildren = true; break; }
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
                                 | ImGuiTreeNodeFlags_OpenOnDoubleClick
                                 | ImGuiTreeNodeFlags_SpanFullWidth
                                 | ImGuiTreeNodeFlags_DefaultOpen;
        if (s.sel.contains(s.entities[i].id)) flags |= ImGuiTreeNodeFlags_Selected;
        if (!hasChildren)   flags |= ImGuiTreeNodeFlags_Leaf;
        const char* nm = s.entities[i].name.empty() ? "(unnamed)"
                                                  : s.entities[i].name.c_str();
        // Start renaming this node: seed the buffer and grab focus.
        auto beginRename = [&] {
            s.renameId = s.entities[i].id;
            std::snprintf(s.renameBuf, s.renameCap, "%s",
                          s.entities[i].name.c_str());
            s.renameFocus = true;
        };
        const bool renaming = (s.entities[i].id == s.renameId);

        // Dim the label when the object is effectively off (itself or an
        // ancestor deactivated), so a hidden subtree reads at a glance.
        // The Active toggle itself lives in the Inspector.
        ImU32 col = typeColor(s.entities[i].type);
        if (!s.entities[i].activeInHierarchy)
            col = (col & 0x00FFFFFF) | 0x66000000; // ~40% alpha
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        // While renaming, draw the row with a blank label and overlay an
        // edit field, keeping the tree's arrow + indentation intact.
        const bool open = ImGui::TreeNodeEx("##n", flags, "%s",
                                            renaming ? "" : nm);
        ImGui::PopStyleColor();

        if (renaming) {
            ImGui::SameLine();
            if (s.renameFocus) { ImGui::SetKeyboardFocusHere(); s.renameFocus = false; }
            ImGui::SetNextItemWidth(-1.0f);
            const bool enter = ImGui::InputText("##rename", s.renameBuf,
                s.renameCap, ImGuiInputTextFlags_EnterReturnsTrue |
                                   ImGuiInputTextFlags_AutoSelectAll);
            const bool esc = ImGui::IsKeyPressed(ImGuiKey_Escape);
            // Commit on Enter or when the field loses focus; Escape cancels.
            if (enter || (ImGui::IsItemDeactivated() && !esc)) {
                s.entities[i].name = s.renameBuf;
                s.renameId = -1;
            } else if (esc) {
                s.renameId = -1;
            }
        } else {
            // Ctrl+click toggles this row in/out of the selection; a plain
            // click selects just it.
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                if (ImGui::GetIO().KeyCtrl) s.sel.toggle(s.entities[i].id);
                else                        s.sel.select(s.entities[i].id);
            }
            // F2 on the active node (or a double-click on its label)
            // starts an inline rename, Unity-style.
            if (i == s.sel.index() && ImGui::IsWindowFocused() &&
                ImGui::IsKeyPressed(ImGuiKey_F2))
                beginRename();
            if (ImGui::IsItemHovered() && !ImGui::IsItemToggledOpen() &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                beginRename();
            if (ImGui::BeginPopupContextItem()) {
                // Right-clicking an unselected row selects just it; if it's
                // already part of a multi-selection, keep the whole set so
                // Duplicate/Delete act on all of it.
                if (!s.sel.contains(s.entities[i].id)) s.sel.select(s.entities[i].id);
                if (ImGui::MenuItem("Rename", "F2")) beginRename();
                if (ImGui::MenuItem("Duplicate")) dupReq = i;
                if (ImGui::MenuItem("Save as Prefab...")) {
                    std::snprintf(s.prefabNameBuf, s.prefabNameCap,
                                  "%s", s.entities[i].name.c_str());
                    s.showPrefabs = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Create Empty Parent")) emptyParentReq = i;
                if (ImGui::MenuItem("Create Empty Child"))  emptyChildReq = i;
                if (ImGui::BeginMenu("Add Primitive")) {
                    if (ImGui::MenuItem("Box"))
                        { primChildReq = i; primChildType = EntityType::Box; }
                    if (ImGui::MenuItem("Ramp"))
                        { primChildReq = i; primChildType = EntityType::Ramp; }
                    if (ImGui::MenuItem("Cylinder"))
                        { primChildReq = i; primChildType = EntityType::Cylinder; }
                    if (ImGui::MenuItem("Sphere"))
                        { primChildReq = i; primChildType = EntityType::Sphere; }
                    if (ImGui::MenuItem("Plane"))
                        { primChildReq = i; primChildType = EntityType::Plane; }
                    ImGui::EndMenu();
                }
                if (const auto* cc = s.entities[i].components.get<CameraComponent>()) {
                    ImGui::Separator();
                    if (ImGui::MenuItem("Set as Main Camera", nullptr,
                                        cc->activeOnStart))
                        s.setMainCamera(s.entities[i].id);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Shoot this (Multishot camera)"))
                    shotCamReq = i;
                if (s.entities[i].components.get<VehicleComponent>()) {
                    ImGui::Separator();
                    if (ImGui::MenuItem("Add headlights")) vehicleLightsReq = i;
                }
                ImGui::Separator();
                ImGui::BeginDisabled(s.entities[i].type == EntityType::Sun);
                if (ImGui::MenuItem("Delete")) delReq = i;
                ImGui::EndDisabled();
                ImGui::EndPopup();
            }
            if (ImGui::BeginDragDropSource()) {
                const int sid = s.entities[i].id;
                ImGui::SetDragDropPayload("SOLID_ID", &sid, sizeof(int));
                ImGui::Text("%s", nm);
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("SOLID_ID")) {
                    reparentSrc = *static_cast<const int*>(pl->Data);
                    reparentTo  = s.entities[i].id;
                }
                ImGui::EndDragDropTarget();
            }
        }
        if (open) {
            if (hasChildren)
                for (int c = 0; c < static_cast<int>(s.entities.size()); ++c)
                    if (s.entities[c].parent == s.entities[i].id) drawNode(c);
            ImGui::TreePop();
        }
        ImGui::PopID();
    };
    ImGui::BeginChild("##tree", ImVec2(0.0f, 0.0f), true);
    for (int i = 0; i < static_cast<int>(s.entities.size()); ++i)
        if (s.entities[i].parent < 0) drawNode(i);
    // Empty space in the tree unparents a node dropped onto it.
    ImGui::Dummy(ImVec2(-1.0f, ImGui::GetContentRegionAvail().y));
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("SOLID_ID")) {
            reparentSrc = *static_cast<const int*>(pl->Data);
            reparentTo  = -1;
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::EndChild();
    // Duplicate/Delete act on the whole selection when the acted-on row
    // is part of a multi-selection, otherwise on just that row.
    auto reqIsMultiSelected = [&](int idx) {
        return idx >= 0 && idx < static_cast<int>(s.entities.size()) &&
               s.sel.contains(s.entities[idx].id) && s.sel.ids().size() > 1;
    };
    if (dupReq >= 0) {
        if (reqIsMultiSelected(dupReq)) s.duplicateSelection();
        else                            s.duplicateEntity(dupReq);
    }
    else if (delReq >= 0) {
        if (reqIsMultiSelected(delReq)) s.deleteSelection();
        else                            s.deleteEntity(delReq);
    }
    else if (emptyParentReq >= 0) s.addEmptyParent(emptyParentReq);
    else if (emptyChildReq >= 0)  s.addEmptyChild(emptyChildReq);
    else if (primChildReq >= 0)   s.addPrimitiveChild(primChildReq, primChildType);
    else if (shotCamReq >= 0)     s.addShotCamera(shotCamReq);
    else if (vehicleLightsReq >= 0) s.addVehicleLights(vehicleLightsReq);
    // Apply a requested reparent (rejecting cycles).
    if (reparentSrc >= 0 && reparentTo != -2) {
        int si = -1;
        for (int k = 0; k < static_cast<int>(s.entities.size()); ++k)
            if (s.entities[k].id == reparentSrc) { si = k; break; }
        if (si >= 0 && reparentSrc != reparentTo &&
            (reparentTo < 0 || !s.isUnderId(reparentTo, reparentSrc))) {
            s.entities[si].parent = reparentTo;
            // Keep the child put: rebase its local onto the new parent.
            Entity* np = (reparentTo >= 0) ? s.document.find(reparentTo) : nullptr;
            const glm::mat4 pw = np ? s.worldOf(*np) : glm::mat4(1.0f);
            s.rebaseLocal(s.entities[si], np ? &pw : nullptr);
        }
    }

    ImGui::End();
}

} // namespace hierarchyui
