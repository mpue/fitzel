#include "RoadPanel.hpp"

#include <fstream>
#include <vector>

#include <glm/glm.hpp>
#include <imgui.h>

#include "RoadBridge.hpp"
#include "RoadSystem.hpp"

namespace roadui {
namespace {

// The bridge list + its style sliders. A bridge names the two control points the
// user picked; the road flies straight between them and the ground below is left
// alone. Returns true when something changed that the road must be rebuilt for.
bool bridgeSection(const PanelState& s) {
    bool rc = false;
    if (!ImGui::CollapsingHeader("Bridges", ImGuiTreeNodeFlags_DefaultOpen)) return rc;

    const bool pair = s.sel >= 0 && s.sel2 >= 0 && s.sel != s.sel2;
    // Already bridged? Then offer to take it away again instead.
    int existing = -1;
    for (int i = 0; pair && i < static_cast<int>(s.road.bridges.size()); ++i)
        if ((s.road.bridges[i].a == s.sel && s.road.bridges[i].b == s.sel2) ||
            (s.road.bridges[i].a == s.sel2 && s.road.bridges[i].b == s.sel))
            existing = i;

    ImGui::BeginDisabled(!pair || existing >= 0);
    if (ImGui::Button("Create bridge", ImVec2(-1.0f, 0.0f))) {
        s.road.bridges.push_back({s.sel, s.sel2});
        rc = true;
    }
    ImGui::EndDisabled();
    if (!pair)
        ImGui::TextDisabled("Select a handle, then shift+click another");
    else if (existing >= 0)
        ImGui::TextDisabled("#%d \xE2\x86\x92 #%d is already a bridge", s.sel, s.sel2);

    for (int i = 0; i < static_cast<int>(s.road.bridges.size()); ++i) {
        ImGui::PushID(i);
        ImGui::Text("Bridge #%d \xE2\x86\x92 #%d", s.road.bridges[i].a,
                    s.road.bridges[i].b);
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
            s.road.bridges.erase(s.road.bridges.begin() + i);
            rc = true;
            ImGui::PopID();
            break; // the list just shifted under us
        }
        ImGui::PopID();
    }
    if (s.road.bridges.empty()) ImGui::TextDisabled("No bridges");

    ImGui::Separator();
    rc |= roadbridge::panel(s.road.bridgeStyle);
    return rc;
}

// The road.txt scratch save/load. Predates scenes carrying roads and is kept as a
// quick way to move a spline between scenes by hand.
void scratchFileSection(const PanelState& s) {
    ImGui::Separator();
    if (ImGui::Button("Save")) {
        std::ofstream f("road.txt");
        for (const glm::vec2& p : s.road.roadPts) f << p.x << ' ' << p.y << '\n';
    }
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        std::ifstream f("road.txt");
        if (f) {
            s.road.roadPts.clear();
            glm::vec2 p;
            while (f >> p.x >> p.y) s.road.roadPts.push_back(p);
            s.sel = -1;
            s.road.needsBuild = true;
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(road.txt)");
}

} // namespace

void drawPanel(const PanelState& s) {
    if (!s.show) return;
    if (ImGui::Begin("Roads", &s.show)) {
        ImGui::Checkbox("Show roads", &s.road.enabled);
        if (ImGui::Checkbox("Edit mode", &s.editMode) && s.editMode) s.grabLMB();
        if (s.editMode) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f),
                               "Click ground = add | drag handle = move | Del = delete");
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f),
                               "Shift+click a second handle = pick a bridge");
        } else {
            ImGui::TextDisabled("Enable edit mode to place and drag handles");
        }
        ImGui::Text("Points: %d", static_cast<int>(s.road.roadPts.size()));
        ImGui::SameLine();
        if (s.sel >= 0 && s.sel2 >= 0)
            ImGui::Text("| selected #%d \xE2\x86\x92 #%d", s.sel, s.sel2);
        else if (s.sel >= 0) ImGui::Text("| selected #%d", s.sel);
        else                 ImGui::TextDisabled("| none selected");

        // --- Build: commit the previewed spline into the terrain --------------
        // Editing only updates the preview; Build grades the road into the ground
        // (flush + smooth) and lofts the drivable mesh + collider.
        ImGui::Separator();
        if (s.road.needsBuild)
            ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.30f, 1.0f),
                               "Preview \xE2\x80\x93 not built yet");
        else if (s.road.verts() > 0)
            ImGui::TextColored(ImVec4(0.45f, 0.90f, 0.55f, 1.0f),
                               "Built \xE2\x80\x93 embedded in terrain");
        else
            ImGui::TextDisabled("No road built");
        ImGui::BeginDisabled(s.road.roadPts.size() < 2);
        if (ImGui::Button(s.road.needsBuild ? "Build road into terrain" : "Rebuild road",
                          ImVec2(-1.0f, 0.0f)))
            s.build();
        ImGui::EndDisabled();
        ImGui::Separator();

        // Everything that changes the road's shape or the corridor it grades feeds
        // `rc`, and `rc` is what marks it dirty -- so a tunable can't quietly take
        // effect on some frames and not others.
        bool rc = false;
        rc |= ImGui::Checkbox("Closed loop", &s.road.closed);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Join the last control point back to the first\n"
                              "(needs at least 3 points).");
        rc |= ImGui::SliderFloat("Width", &s.road.width, 1.0f, 20.0f, "%.1f m");
        rc |= ImGui::SliderFloat("Smoothing", &s.road.grade, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How much the road grade is flattened along its\n"
                              "length (higher = smoother/steadier slope).");
        rc |= ImGui::SliderFloat("Shoulder", &s.road.shoulder, 0.0f, 12.0f, "%.1f m");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Width of the terrain blend beyond the road edge\n"
                              "that eases back into the natural ground.");
        rc |= ImGui::SliderFloat("Texture tile", &s.road.texTile, 2.0f, 24.0f, "%.1f m");

        ImGui::Separator();
        rc |= bridgeSection(s);

        if (rc) s.road.needsBuild = true;

        // Edge fade is a pure shader effect (alpha taper at the ribbon edges) -- it
        // needs no rebuild, so it's deliberately kept out of `rc`.
        ImGui::SliderFloat("Edge fade", &s.road.fadeWidth, 0.0f, s.road.width * 0.5f,
                           "%.1f m");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Fade the road to transparent over this many metres\n"
                              "at each edge, blending it into the terrain (0 = off).");

        // Also a pure shader effect -- no rebuild, so out of `rc` like the fade.
        ImGui::SliderFloat("Rain rings", &s.road.rainRings, 0.0f, 3.0f, "%.2fx");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How hard rain drops dent the wet road (0 = off).\n"
                              "Only shows while it is actually raining AND the\n"
                              "surface is wet -- turn the storm up to judge it.");

        // Surface texture picker (any diffuse texture in textures/). Picking one
        // brings its matching normal map along, so the common case needs no second
        // trip to the combo below -- but the combo is there to override or clear it.
        if (!s.road.texFiles.empty() &&
            ImGui::BeginCombo("Surface", s.road.texFiles[s.road.texSel].c_str())) {
            for (int i = 0; i < static_cast<int>(s.road.texFiles.size()); ++i) {
                const bool sel = (i == s.road.texSel);
                if (ImGui::Selectable(s.road.texFiles[i].c_str(), sel)) {
                    s.road.setSurface(s.road.texFiles[i]);
                    s.road.texSel = i;
                    const std::string n = s.road.normalFor(s.road.texFiles[i]);
                    s.road.setNormal(n); // "" clears it: a pack without one stays flat
                    s.road.normSel = -1;
                    for (int k = 0; k < static_cast<int>(s.road.normFiles.size()); ++k)
                        if (s.road.normFiles[k] == n) s.road.normSel = k;
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        // Normal map: the asphalt's grain. Without one the ribbon is lit by its
        // geometry normal alone and reads as painted on, especially under a low sun.
        const char* curN = (s.road.normSel >= 0 &&
                            s.road.normSel < static_cast<int>(s.road.normFiles.size()))
                               ? s.road.normFiles[s.road.normSel].c_str()
                               : "(none)";
        if (ImGui::BeginCombo("Normal", curN)) {
            if (ImGui::Selectable("(none)", s.road.normSel < 0)) {
                s.road.setNormal(std::string());
                s.road.normSel = -1;
            }
            for (int i = 0; i < static_cast<int>(s.road.normFiles.size()); ++i) {
                const bool sel = (i == s.road.normSel);
                if (ImGui::Selectable(s.road.normFiles[i].c_str(), sel)) {
                    s.road.setNormal(s.road.normFiles[i]);
                    s.road.normSel = i;
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Follows the surface by name when you pick one\n"
                              "(asphalt_02_diff_4k -> asphalt_02_nor_gl_4k).\n"
                              "Strength is the global \"Normal strength\" in Terrain.");

        ImGui::BeginDisabled(s.sel < 0 || s.sel >= static_cast<int>(s.road.roadPts.size()));
        if (ImGui::Button("Delete selected")) s.removePoint(s.sel);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(s.road.roadPts.empty());
        if (ImGui::Button("Undo point"))
            s.removePoint(static_cast<int>(s.road.roadPts.size()) - 1);
        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
            s.road.roadPts.clear();
            s.road.bridges.clear();
            s.sel = s.sel2 = -1;
            s.road.needsBuild = true;
        }
        ImGui::EndDisabled();

        scratchFileSection(s);
    }
    ImGui::End();
}

} // namespace roadui
