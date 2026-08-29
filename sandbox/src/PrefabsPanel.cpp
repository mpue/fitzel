#include "PrefabsPanel.hpp"

#include <string>

#include <imgui.h>

#include "PrefabSystem.hpp"

namespace prefabsui {

void drawPanel(const PanelState& s) {
    if (!s.show) return;
    ImGui::SetNextWindowSize(ImVec2(300.0f, 380.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Prefabs", &s.show)) {
const std::string dir = s.prefabDir();
if (dir.empty()) {
    ImGui::TextWrapped(
        "Open or create a project first -- prefabs are saved in "
        "the project's prefabs/ folder.");
} else {
    const bool hasSel =
        s.entitySel >= 0 &&
        s.entitySel < static_cast<int>(s.entities.size()) &&
        s.entities[s.entitySel].type != EntityType::Sun;
    ImGui::TextDisabled("New prefab from the selected object:");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##prefabName", s.nameBuf,
                     s.nameCap);
    ImGui::BeginDisabled(!hasSel || s.nameBuf[0] == '\0');
    if (ImGui::Button("Create from selection", ImVec2(-1.0f, 0.0f)))
        s.createFromSelection(s.nameBuf);
    ImGui::EndDisabled();
    if (!hasSel)
        ImGui::TextDisabled("(select an object in the scene first)");

    ImGui::Separator();
    ImGui::TextDisabled("Click a prefab to add it to the scene:");
    ImGui::BeginChild("##prefabList", ImVec2(0.0f, 0.0f), true);
    const auto items = prefab::list(dir);
    for (const auto& it : items)
        if (ImGui::Selectable((it.first + "##" + it.second).c_str()))
            s.instantiate(it.second);
    if (items.empty())
        ImGui::TextDisabled("(no prefabs yet)");
    ImGui::EndChild();
}
    }
    ImGui::End();
}

} // namespace prefabsui
