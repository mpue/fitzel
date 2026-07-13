#include "PaintPanel.hpp"

#include <string>

#include <imgui.h>

namespace paintui {

void drawPanel(const PanelState& s) {
    if (!s.show) return;
    if (ImGui::Begin("Terrain Paint", &s.show)) {
        if (ImGui::Checkbox("Paint mode", &s.paintMode) && s.paintMode)
            s.grassPaintMode = s.roadEditMode = s.treePaintMode =
                s.flowerPaintMode = s.sculptMode = s.scatterMode = false; // brush owns the LMB
        if (s.paintMode)
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.85f, 1.0f),
                "Hold LMB to paint | Alt (or Erase) reverts to the auto blend");
        else
            ImGui::TextDisabled("Enable to paint textures onto the ground");

        ImGui::SeparatorText("Layer to paint");
        ImGui::TextDisabled("The first four textured layers are paintable.");
        // Present the paintable layers in the same order the renderer binds them
        // (textured layers only), so the chosen slot matches the shader.
        int slot = 0;
        for (int i = 0; i < static_cast<int>(s.look.layers.size()) && slot < 4; ++i) {
            const TerrainLayer& L = s.look.layers[i];
            if (!L.tex) continue; // untextured layers are skipped when binding
            const std::string label =
                (L.name.empty() ? ("Layer " + std::to_string(i)) : L.name);
            ImGui::PushID(slot);
            if (ImGui::RadioButton(label.c_str(), s.layer == slot)) s.layer = slot;
            ImGui::PopID();
            ++slot;
        }
        if (slot == 0)
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.4f, 1.0f),
                "Add a textured layer in the Buddel panel first.");
        else if (s.layer >= slot)
            s.layer = 0; // a layer was removed -> fall back to the first slot

        ImGui::SeparatorText("Brush");
        ImGui::SliderFloat("Radius", &s.radius, 1.0f, 40.0f, "%.1f m");
        ImGui::SliderFloat("Strength", &s.strength, 0.05f, 1.0f);
        ImGui::Checkbox("Erase (revert to auto)", &s.erase);

        ImGui::Text("Painted cells: %d", static_cast<int>(s.work.weights.size()));
        ImGui::BeginDisabled(s.work.weights.empty());
        if (ImGui::Button("Clear paint")) {
            s.work.weights.clear();
            s.publish();
            s.streamer.rebuild(); // drop all paint -> rebuild with the auto blend
        }
        ImGui::EndDisabled();
    }
    ImGui::End();
}

} // namespace paintui
