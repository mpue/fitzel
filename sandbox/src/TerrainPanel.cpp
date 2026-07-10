#include "TerrainPanel.hpp"

#include <cmath>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include <fitzel/asset/AssetDatabase.hpp>

namespace terrainui {

using fitzel::AssetId;

namespace {

// A one-click landscape flavour: a tuned bundle of generator params. The user's
// seed is deliberately left untouched, so applying a preset re-shapes the world
// they're already looking at instead of teleporting them to a new one.
struct GenPreset {
    const char* name;
    float heightScale, ridgeScale, continentAmp, biomeFreq, terrace,
          warpStrength, frequency, valleyDepth, peakSharpness, reliefGain;
    int   octaves;
};

constexpr GenPreset kPresets[] = {
    // name              hgt  ridge cont   biome   terr  warp  freq   valley peak relief oct
    {"Sanfte Hügel", 10.f,  6.f, 1.0f, 0.0018f, 0.0f, 10.f, 0.014f,  0.f, 0.8f, 1.0f, 5},
    {"Alpen",             16.f, 40.f, 1.6f, 0.0014f, 0.1f, 16.f, 0.013f,  8.f, 1.9f, 1.5f, 7},
    {"Canyon",            12.f, 10.f, 1.2f, 0.0016f, 0.7f, 12.f, 0.012f, 22.f, 1.0f, 1.2f, 6},
    {"Mesa / Plateau",    12.f, 14.f, 1.3f, 0.0016f, 0.9f, 10.f, 0.011f,  6.f, 1.1f, 1.1f, 6},
    {"Archipel",          14.f, 10.f, 2.6f, 0.0012f, 0.0f, 18.f, 0.015f,  4.f, 1.2f, 1.3f, 6},
    {"Fjorde",            16.f, 28.f, 2.2f, 0.0013f, 0.1f, 16.f, 0.013f, 26.f, 1.6f, 1.6f, 7},
};

void applyPreset(fitzel::TerrainSettings& u, const GenPreset& p) {
    u.heightScale  = p.heightScale;  u.ridgeScale    = p.ridgeScale;
    u.continentAmp = p.continentAmp; u.biomeFreq     = p.biomeFreq;
    u.terrace      = p.terrace;      u.warpStrength  = p.warpStrength;
    u.frequency    = p.frequency;    u.valleyDepth   = p.valleyDepth;
    u.peakSharpness= p.peakSharpness;u.reliefGain    = p.reliefGain;
    u.octaves      = p.octaves;
}

} // namespace

void drawPanel(const PanelState& s) {
    if (!s.show) return;
    // Live preview: regenerate as soon as a slider is released (see below). On by
    // default -- toggle off for surgical tweaks without a rebuild per drag.
    static bool autoPreview = true;

    if (ImGui::Begin("Buddel", &s.show)) {
        // Push new generator params into the streamer and rebuild the world.
        auto regenerate = [&] {
            s.streamer.settings() = s.uiSettings;
            s.streamer.rebuild();
            s.streamer.update(s.camera.position());
            s.grassDirty = true;            // regrow vegetation on the new terrain
            s.treeCenter = glm::vec2(1e9f);
            s.roadDirty  = true;            // re-drape roads on the new heights
        };

        ImGui::SeparatorText("Presets");
        ImGui::TextDisabled("One click = a whole landscape. Fine-tune below.");
        for (int i = 0; i < IM_ARRAYSIZE(kPresets); ++i) {
            if (i % 3) ImGui::SameLine();
            if (ImGui::Button(kPresets[i].name, ImVec2(112, 0))) {
                applyPreset(s.uiSettings, kPresets[i]);
                regenerate();
            }
        }

        ImGui::SeparatorText("Generator");
        // `released` fires once when a slider is let go after editing -- that's the
        // cue to rebuild (rebuilding every frame mid-drag would thrash the streamer).
        bool released = false;
        auto slid = [&] { released |= ImGui::IsItemDeactivatedAfterEdit(); };

        ImGui::SliderFloat("Height",     &s.uiSettings.heightScale, 0.0f, 30.0f);   slid();
        ImGui::SliderFloat("Relief boost", &s.uiSettings.reliefGain, 0.5f, 2.5f);   slid();
        ImGui::SliderFloat("Ridges",     &s.uiSettings.ridgeScale, 0.0f, 50.0f);    slid();
        ImGui::SliderFloat("Peak sharpness", &s.uiSettings.peakSharpness, 0.5f, 3.0f); slid();
        ImGui::SliderFloat("Valleys",    &s.uiSettings.valleyDepth, 0.0f, 30.0f, "%.1f m"); slid();
        ImGui::SliderFloat("Continents", &s.uiSettings.continentAmp, 0.0f, 3.0f);   slid();
        ImGui::SliderFloat("Biome size", &s.uiSettings.biomeFreq, 0.0005f, 0.004f, "%.4f"); slid();
        ImGui::SliderFloat("Terraces",   &s.uiSettings.terrace, 0.0f, 1.0f);        slid();
        ImGui::SliderFloat("Warp",       &s.uiSettings.warpStrength, 0.0f, 40.0f);  slid();
        ImGui::SliderFloat("Frequency",  &s.uiSettings.frequency, 0.003f, 0.05f, "%.3f"); slid();
        ImGui::SliderInt  ("Octaves",    &s.uiSettings.octaves, 1, 8);              slid();
        ImGui::SliderFloat("Seed",       &s.uiSettings.seed, 0.0f, 100.0f);         slid();

        if (autoPreview && released) regenerate();

        ImGui::Checkbox("Live preview", &autoPreview);
        ImGui::SameLine();
        if (ImGui::Button("Regenerate")) regenerate();
        ImGui::SameLine();
        if (ImGui::Button("Reroll")) {   // nudge the seed for a fresh variation
            s.uiSettings.seed = std::fmod(s.uiSettings.seed + 17.3f, 100.0f);
            regenerate();
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
                // Optional normal map: adds tangent-space surface relief for this
                // layer (triplanar Whiteout blend), dialled by "Normal strength".
                const std::string curN = L.normId.valid() ? "(assigned)" : "(none)";
                if (ImGui::BeginCombo("Normal", curN.c_str())) {
                    if (ImGui::Selectable("(none)", !L.normId.valid())) {
                        L.normId = AssetId{};
                        L.norm.reset();
                    }
                    for (const auto& [id, rel] : texAssets)
                        if (ImGui::Selectable(rel.c_str(), id == L.normId)) {
                            L.normId = id;
                            L.norm   = s.assetDb.loadTexture(id);
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
