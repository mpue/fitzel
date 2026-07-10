#include "SculptPanel.hpp"

#include <imgui.h>

namespace sculptui {

void drawPanel(const PanelState& s) {
    if (!s.show) return;
    if (ImGui::Begin("Terrain Sculpt", &s.show)) {
        if (ImGui::Checkbox("Sculpt mode", &s.sculptMode) && s.sculptMode)
            s.grassPaintMode = s.roadEditMode = s.treePaintMode =
                s.flowerPaintMode = s.paintMode = false; // brush owns the LMB
        if (s.sculptMode)
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.6f, 1.0f),
                "Hold LMB to sculpt | Alt inverts raise/lower");
        else
            ImGui::TextDisabled("Enable to reshape the ground with the brush");

        const char* tools[] = {"Raise", "Lower", "Smooth", "Flatten",
                               "Erode", "Stamp", "Noise", "Carve"};
        ImGui::Combo("Tool", &s.tool, tools, IM_ARRAYSIZE(tools));
        ImGui::SliderFloat("Radius", &s.radius, 1.0f, 40.0f, "%.1f m");
        ImGui::SliderFloat("Strength", &s.strength, 0.05f, 1.0f);
        if (s.tool == 3)
            ImGui::SliderFloat("Flatten height", &s.flattenHeight,
                               -40.0f, 60.0f, "%.1f m");
        if (s.tool == 4)
            ImGui::TextDisabled("Weathers slopes: material slides downhill.");
        if (s.tool == 6)
            ImGui::SliderFloat("Feature size", &s.noiseFreq, 0.08f, 1.0f, "%.2f");
        if (s.tool == 7) {
            ImGui::SliderFloat("Valley depth", &s.carveDepth, 2.0f, 40.0f, "%.1f m");
            ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f),
                               "Drag to carve a valley | Alt raises a ridge");
        }
        if (s.tool == 5) {
            const char* shapes[] = {"Dome", "Cone", "Plateau", "Crater",
                                    "Ridge", "Range"};
            ImGui::Combo("Shape", &s.stampShape, shapes, IM_ARRAYSIZE(shapes));
            ImGui::SliderFloat("Stamp height", &s.stampHeight, -30.0f, 40.0f, "%.1f m");
            // Ridge (4) and mountain range (5) are directional -> offer rotation.
            if (s.stampShape == 4 || s.stampShape == 5) {
                float deg = glm::degrees(s.stampRot);
                if (ImGui::SliderFloat("Rotation", &deg, -180.0f, 180.0f, "%.0f deg"))
                    s.stampRot = glm::radians(deg);
            }
            ImGui::TextColored(ImVec4(0.8f, 0.6f, 1.0f, 1.0f),
                               "Click to stamp | Alt inverts (digs in)");
        }

        // Grid spacing only makes sense to change before any edits exist (the map
        // keys are cell indices at the current spacing).
        float grid = s.work.cell;
        ImGui::BeginDisabled(!s.work.deltas.empty());
        if (ImGui::SliderFloat("Grid", &grid, 0.5f, 4.0f, "%.2f m"))
            s.work.cell = grid;
        ImGui::EndDisabled();

        ImGui::Text("Edited cells: %d", static_cast<int>(s.work.deltas.size()));
        ImGui::BeginDisabled(s.work.deltas.empty());
        if (ImGui::Button("Clear sculpt")) {
            s.work.deltas.clear();
            s.publish();
            s.streamer.rebuild(); // drop all edits -> regenerate the terrain
            s.grassDirty = true;
        }
        ImGui::EndDisabled();
    }
    ImGui::End();
}

} // namespace sculptui
