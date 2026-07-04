#include "TerrainPanel.hpp"

#include <imgui.h>

namespace terrainui {

void drawPanel(const PanelState& s) {
    if (!s.show) return;
    if (ImGui::Begin("Terrain", &s.show)) {
        ImGui::SeparatorText("Generator");
        ImGui::SliderFloat("Height",     &s.uiSettings.heightScale, 0.0f, 30.0f);
        ImGui::SliderFloat("Ridges",     &s.uiSettings.ridgeScale, 0.0f, 50.0f);
        ImGui::SliderFloat("Continents", &s.uiSettings.continentAmp, 0.0f, 3.0f);
        ImGui::SliderFloat("Biome size", &s.uiSettings.biomeFreq, 0.0005f, 0.004f, "%.4f");
        ImGui::SliderFloat("Terraces",   &s.uiSettings.terrace, 0.0f, 1.0f);
        ImGui::SliderFloat("Warp",       &s.uiSettings.warpStrength, 0.0f, 40.0f);
        ImGui::SliderFloat("Frequency",  &s.uiSettings.frequency, 0.003f, 0.05f, "%.3f");
        ImGui::SliderInt  ("Octaves",    &s.uiSettings.octaves, 1, 8);
        ImGui::SliderFloat("Seed",       &s.uiSettings.seed, 0.0f, 100.0f);
        if (ImGui::Button("Regenerate")) {
            s.streamer.settings() = s.uiSettings;
            s.streamer.rebuild();
            s.streamer.update(s.camera.position());
            s.grassDirty = true;            // regrow vegetation on the new terrain
            s.treeCenter = glm::vec2(1e9f);
            s.roadDirty  = true;            // re-drape roads on the new heights
        }
        ImGui::SeparatorText("Material (slope)");
        ImGui::SliderFloat("Texture scale",   &s.texScale, 0.02f, 0.2f, "%.3f");
        ImGui::SliderFloat("Normal strength", &s.normalStrength, 0.0f, 1.0f);
        ImGui::SliderFloat("Rock slope",      &s.look.rockSlope, 0.0f, 1.0f);
        ImGui::SliderFloat("Slope blend",     &s.look.slopeSharpness, 0.02f, 0.4f);
        ImGui::SliderFloat("Snow level",      &s.look.snowLevel, 0.0f, 40.0f);
        ImGui::SliderFloat("Detail bump",     &s.look.detailStrength, 0.0f, 4.0f);
    }
    ImGui::End();
}

} // namespace terrainui
