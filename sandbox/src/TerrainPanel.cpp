#include "TerrainPanel.hpp"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include <fitzel/asset/AssetDatabase.hpp>

namespace terrainui {

using fitzel::AssetId;

void drawPanel(const PanelState& s) {
    if (!s.show) return;
    if (ImGui::Begin("Buddel", &s.show)) {
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
        ImGui::SeparatorText("Texture layers");
        ImGui::TextDisabled("A layer paints where BOTH the height and the slope "
                            "fall in its band; overlapping bands cross-fade.");

        // Available texture assets (engine + project), for the per-layer picker.
        std::vector<std::pair<AssetId, std::string>> texAssets;
        for (const AssetId id : s.assetDb.allAssets())
            if (s.assetDb.typeForId(id) == fitzel::AssetType::Texture)
                if (const auto* e = s.assetDb.entry(id))
                    texAssets.push_back({id, e->relPath});

        int removeIdx = -1;
        for (int i = 0; i < static_cast<int>(s.look.layers.size()); ++i) {
            TerrainLayer& L = s.look.layers[i];
            ImGui::PushID(i);
            const std::string header =
                (L.name.empty() ? ("Layer " + std::to_string(i)) : L.name);
            if (ImGui::TreeNodeEx(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                const std::string cur = L.texId.valid()
                    ? (L.name.empty() ? L.texId.toString() : L.name) : "(none)";
                if (ImGui::BeginCombo("Texture", cur.c_str())) {
                    for (const auto& [id, rel] : texAssets)
                        if (ImGui::Selectable(rel.c_str(), id == L.texId)) {
                            L.texId = id;
                            L.tex   = s.assetDb.loadTexture(id);
                            L.name  = std::filesystem::path(rel).stem().string();
                        }
                    ImGui::EndCombo();
                }
                ImGui::DragFloatRange2("Height", &L.heightStart, &L.heightEnd,
                                       0.2f, -200.0f, 200.0f, "%.1f", "%.1f");
                ImGui::DragFloatRange2("Slope", &L.slopeStart, &L.slopeEnd,
                                       0.5f, 0.0f, 90.0f, "%.0f deg", "%.0f deg");
                ImGui::SliderFloat("Scale", &L.scale, 0.01f, 0.5f, "%.3f");
                if (ImGui::SmallButton("Remove layer")) removeIdx = i;
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (removeIdx >= 0)
            s.look.layers.erase(s.look.layers.begin() + removeIdx);
        ImGui::BeginDisabled(static_cast<int>(s.look.layers.size()) >= kMaxTerrainLayers);
        if (ImGui::Button("Add layer")) s.look.layers.push_back(TerrainLayer{});
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("%d / %d",
                            static_cast<int>(s.look.layers.size()), kMaxTerrainLayers);

        ImGui::SeparatorText("Surface detail");
        ImGui::SliderFloat("Normal strength", &s.normalStrength, 0.0f, 1.0f);
        ImGui::SliderFloat("Detail bump",     &s.look.detailStrength, 0.0f, 4.0f);
    }
    ImGui::End();
}

} // namespace terrainui
