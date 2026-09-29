#include "ModifierPanel.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>

#include <imgui.h>

#include "EditorContext.hpp"   // normalizeMeshEntity
#include "UiStyle.hpp"

namespace modifierui {

void card(ModifierStackComponent& ms, Entity& e, std::vector<Entity>& entities) {
    MeshComponent* mc = e.components.get<MeshComponent>();
    if (!mc)
        ui::hint("Modifiers change a modelled mesh. This object has none\n"
                 "yet: make it editable (Modeling, Tab) and they apply.");

    ImGui::Checkbox("Shade smooth", &ms.smooth);
    ImGui::SetItemTooltip("Share a normal across faces that meet flatter than the\n"
                          "angle: curved surfaces read as curved, edges stay sharp.");
    if (ms.smooth) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
        ImGui::SliderFloat("Angle", &ms.smoothAngle, 1.0f, 180.0f, "%.0f deg");
    }

    // What the buttons asked for, done after the list: each of them changes it.
    int up = -1, down = -1, remove = -1, apply = -1;
    const int n = static_cast<int>(ms.stack.size());
    const ImVec2 btn(0.0f, ImGui::GetFrameHeight());
    for (int i = 0; i < n; ++i) {
        modifiers::Modifier& md = *ms.stack[static_cast<std::size_t>(i)];
        ImGui::PushID(i);
        ImGui::Separator();
        // The name is the switch: a modifier switched off stays where it is,
        // settings and all, and the stack runs past it.
        ImGui::Checkbox(md.displayName(), &md.enabled);
        ImGui::SetItemTooltip("On: part of the result. Off: skipped, kept as it is.");
        ImGui::BeginDisabled(i == 0);
        if (ImGui::Button("Up", btn)) up = i;
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(i + 1 >= n);
        if (ImGui::Button("Down", btn)) down = i;
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!mc);
        if (ImGui::Button("Apply", btn)) apply = i;
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Bake this modifier -- and every one above it -- into the\n"
                              "mesh and take them off the list. The object keeps its shape.");
        ImGui::SameLine();
        if (ImGui::Button("Remove", btn)) remove = i;
        ImGui::BeginDisabled(!md.enabled);
        ImGui::Indent();
        for (const Property& pr : md.props()) drawProperty(pr, &md);
        ImGui::Unindent();
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    if (n > 0) ImGui::Separator();

    if (ImGui::Button("Add modifier", ImVec2(ImGui::GetFontSize() * 9.0f, ImGui::GetFrameHeight() * 1.3f)))
        ImGui::OpenPopup("##addModifier");
    if (ImGui::BeginPopup("##addModifier")) {
        for (const modifiers::TypeInfo& t : modifiers::registry()) {
            if (ImGui::Selectable(t.displayName.c_str()))
                if (auto m = t.make()) ms.stack.push_back(std::move(m));
            if (!t.tip.empty()) ImGui::SetItemTooltip("%s", t.tip.c_str());
        }
        ImGui::EndPopup();
    }

    if (up > 0) std::swap(ms.stack[static_cast<std::size_t>(up)],
                          ms.stack[static_cast<std::size_t>(up - 1)]);
    if (down >= 0 && down + 1 < n)
        std::swap(ms.stack[static_cast<std::size_t>(down)],
                  ms.stack[static_cast<std::size_t>(down + 1)]);
    if (remove >= 0) ms.stack.erase(ms.stack.begin() + remove);
    if (apply >= 0 && mc) {
        // Baked through the same transform it was drawn with, and then the
        // object is squared with its new mesh -- recentred, its box fitted --
        // so nothing on screen moves.
        const glm::vec3 scale = editmesh::fitScale(mc->mesh, e.half);
        mc->mesh = modifiers::evaluate(mc->mesh, ms, apply + 1);
        mc->mesh.hasFrame    = false;
        mc->mesh.smoothAngle = 0.0f;
        ms.stack.erase(ms.stack.begin(), ms.stack.begin() + apply + 1);
        normalizeMeshEntity(entities, e, *mc, scale);
    }

    // What came out, so a Decimate ratio or a subdivision level can be judged
    // by more than the look of it.
    if (mc) {
        const modifiers::Shown sh = modifiers::shown(e, *mc);
        std::size_t tris = 0;
        for (const std::vector<int>& f : sh.mesh->faces)
            if (f.size() >= 3) tris += f.size() - 2;
        ImGui::TextDisabled("Result: %zu faces, %zu corners, %zu triangles",
                            sh.mesh->faces.size(), sh.mesh->verts.size(), tris);
    }
}

} // namespace modifierui
