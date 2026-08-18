#include "BuildingPanel.hpp"

#include <cfloat>

#include <imgui.h>

#include "UiStyle.hpp"

namespace buildingui {

namespace {

using buildings::Crown;
using buildings::Footprint;
using buildings::Style;

constexpr int kStyleCount     = static_cast<int>(Style::Count);
constexpr int kFootprintCount = static_cast<int>(Footprint::Count);
constexpr int kCrownCount     = static_cast<int>(Crown::Count);

// An enum combo driven by the module's own name function, so adding a shape or
// crown in BuildingGen shows up here with no second list to maintain.
template <class E, int N, const char* (*NameFn)(E)>
bool enumCombo(const char* label, E& value) {
    const char* names[N];
    for (int i = 0; i < N; ++i) names[i] = NameFn(static_cast<E>(i));
    int cur = static_cast<int>(value);
    if (!ImGui::Combo(label, &cur, names, N)) return false;
    value = static_cast<E>(cur);
    return true;
}

// A whole-number knob with -/+ buttons: precise without a drag, which matters
// more here than saving a few pixels (see the editor's accessibility goal).
bool stepInt(const char* label, int& v, int lo, int hi) {
    const bool changed = ImGui::InputInt(label, &v);
    v = v < lo ? lo : (v > hi ? hi : v);
    return changed;
}

// One-click looks: the accent (neon/beacon) plus a facade tint that suits it.
struct ColorPreset {
    const char* name;
    glm::vec3   accent;
    glm::vec3   glass;
};
const ColorPreset kColorPresets[] = {
    {"Cyan",    {0.20f, 0.85f, 1.00f}, {0.09f, 0.13f, 0.18f}},
    {"Magenta", {1.00f, 0.25f, 0.75f}, {0.13f, 0.09f, 0.16f}},
    {"Amber",   {1.00f, 0.62f, 0.15f}, {0.16f, 0.13f, 0.10f}},
    {"Toxic",   {0.55f, 1.00f, 0.25f}, {0.10f, 0.14f, 0.11f}},
    {"Ice",     {0.75f, 0.90f, 1.00f}, {0.16f, 0.19f, 0.23f}},
};

} // namespace

void drawPanel(const PanelState& s) {
    if (!s.show) return;
    ImGui::SetNextWindowSize(ImVec2(360.0f, 620.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Buildings", &s.show)) {
        buildings::Params& c = s.cfg;
        bool changed = false;

        ui::hint("Procedural skyscrapers. Generate, tune, then save the result "
                 "as a prefab to place it along the track.");

        ui::sectionText("Style");
        {   // two columns of big preset buttons -- one click, whole new building
            const float gap = ImGui::GetStyle().ItemSpacing.x;
            const float w   = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
            for (int i = 0; i < kStyleCount; ++i) {
                const Style st = static_cast<Style>(i);
                if (i % 2) ImGui::SameLine();
                if (ImGui::Button(buildings::styleName(st), ImVec2(w, 28.0f))) {
                    buildings::applyStyle(c, st);
                    changed = true;
                }
            }
        }
        if (ImGui::Button("Reroll shape", ImVec2(-FLT_MIN, 0.0f))) {
            c.seed = c.seed * 1664525u + 1013904223u; // LCG: a new building, same knobs
            changed = true;
        }
        ImGui::SetItemTooltip("Same settings, different proportions.");

        ui::sectionText("Massing");
        changed |= enumCombo<Footprint, kFootprintCount, buildings::footprintName>(
            "Footprint", c.shape);
        changed |= ImGui::SliderFloat("Width", &c.width, 4.0f, 200.0f, "%.1f m");
        changed |= ImGui::SliderFloat("Depth", &c.depth, 4.0f, 200.0f, "%.1f m");
        changed |= stepInt("Floors", c.floors, 1, 250);
        changed |= ImGui::SliderFloat("Floor height", &c.floorHeight, 2.0f, 8.0f,
                                      "%.1f m");
        ui::hint("Height: %.0f m", c.floors * c.floorHeight);
        changed |= stepInt("Setbacks", c.sections, 1, 12);
        ImGui::SetItemTooltip("Stacked masses -- each one steps in from the last.");
        changed |= ImGui::SliderFloat("Taper", &c.taper, 0.15f, 1.2f);
        ImGui::SetItemTooltip("Top footprint as a fraction of the base.");
        changed |= ImGui::SliderFloat("Irregularity", &c.jitter, 0.0f, 1.0f);
        changed |= ImGui::SliderFloat("Twist", &c.twist, -360.0f, 360.0f, "%.0f deg");
        ImGui::SetItemTooltip("Total rotation over the height. Raise Setbacks to "
                              "8+ for a smooth spiral.");
        changed |= stepInt("Podium floors", c.podiumFloors, 0, 40);
        changed |= ImGui::SliderFloat("Podium spread", &c.podiumSpread, 1.0f, 3.0f,
                                      "x%.2f");
        changed |= ImGui::SliderFloat("Sink", &c.sink, 0.0f, 5.0f, "%.1f m");
        ImGui::SetItemTooltip("Push the base into the ground so no gap shows on "
                              "uneven terrain.");

        ui::sectionText("Detail");
        changed |= stepInt("Band every N floors", c.bandEvery, 0, 50);
        ImGui::SetItemTooltip("Horizontal mechanical ledges (0 = none).");
        changed |= ImGui::SliderFloat("Band depth", &c.bandOverhang, 0.0f, 3.0f,
                                      "%.2f m");
        changed |= stepInt("Fins per mass", c.fins, 0, 16);
        changed |= ImGui::SliderFloat("Fin depth", &c.finDepth, 0.1f, 4.0f, "%.2f m");
        changed |= ImGui::Checkbox("Neon strips", &c.neon);
        changed |= ImGui::Checkbox("Corner strips", &c.edgeStrips);
        ImGui::SetItemTooltip("Lit lines up the four corners of every mass. The "
                              "bands count a building's floors and make it read "
                              "wide; this is the vertical the eye runs up, and it "
                              "is most of what makes a tower feel tall. Follows "
                              "the setbacks and the twist.");
        ImGui::BeginDisabled(!c.edgeStrips);
        changed |= ImGui::SliderFloat("Strip width", &c.edgeWidth, 0.1f, 3.0f,
                                      "%.2f m");
        ImGui::EndDisabled();
        changed |= enumCombo<Crown, kCrownCount, buildings::crownName>("Crown",
                                                                      c.crown);
        changed |= ImGui::SliderFloat("Crown height", &c.crownHeight, 0.0f, 1.0f,
                                      "%.2f of body");
        changed |= ImGui::Checkbox("Beacon light", &c.beaconLight);
        ImGui::SetItemTooltip("One point light in the crown. Keep it on for a "
                              "night skyline, off for a dense city (cost).");
        changed |= ImGui::Checkbox("Solid (collider)", &c.collider);
        ImGui::SetItemTooltip("Static physics on the main masses, so a car or "
                              "glider crashes into the building instead of "
                              "flying through it.");

        ui::sectionText("Windows");
        changed |= ImGui::Checkbox("Lit windows", &c.windows);
        ImGui::SetItemTooltip("Procedural lit windows on the glazing. Costs no "
                              "geometry and no texture -- the shader hashes them "
                              "out of the world position, so the rows line up "
                              "across the setbacks of a stack.");
        ImGui::BeginDisabled(!c.windows);
        changed |= ImGui::SliderFloat("Window pitch", &c.windowWidth, 1.0f, 12.0f,
                                      "%.1f m");
        ImGui::SetItemTooltip("Metres of facade per window. The row height is the "
                              "floor height -- a window belongs to a storey.");
        changed |= ImGui::SliderFloat("Lit fraction", &c.windowLit, 0.0f, 1.0f, "%.2f");
        ImGui::SetItemTooltip("0 = a dead, unoccupied tower. Around 0.5 reads as "
                              "an evening; 0.9 as an office block on a deadline.");
        changed |= ImGui::ColorEdit3("Window light", &c.windowColor.x);
        changed |= ImGui::SliderFloat("Window glow", &c.windowGlow, 0.0f, 8.0f, "%.2f");
        ImGui::EndDisabled();

        ui::sectionText("Wear");
        changed |= ImGui::SliderFloat("Weathering", &c.weathering, 0.0f, 1.0f, "%.2f");
        ImGui::SetItemTooltip("0 = new curtain wall, 1 = derelict concrete. Drops "
                              "the mirror finish (glass reflectivity 0.70 -> "
                              "almost none) and soils the colours. Acts on the "
                              "shared palette, so it re-skins every building on "
                              "this slot.");
        changed |= ImGui::SliderFloat("Bare concrete", &c.grime, 0.0f, 1.0f, "%.2f");
        ImGui::SetItemTooltip("Chance a mass is raw structural concrete instead of "
                              "glazing -- a glass shaft on a concrete stump.");
        changed |= stepInt("Roof clutter", c.clutter, 0, 60);
        ImGui::SetItemTooltip("Tanks, vents and ducts on the setback roofs.");

        ui::sectionText("Look");
        {
            const char* slots[] = {"A", "B", "C", "D", "E", "F", "G", "H"};
            changed |= ImGui::Combo("Palette", &c.palette, slots,
                                    IM_ARRAYSIZE(slots));
            ImGui::SetItemTooltip("Buildings on the same slot share four "
                                  "materials -- editing the colours re-skins all "
                                  "of them at once.");
        }
        for (int i = 0; i < IM_ARRAYSIZE(kColorPresets); ++i) {
            if (i) ImGui::SameLine();
            if (ImGui::SmallButton(kColorPresets[i].name)) {
                c.accentColor = kColorPresets[i].accent;
                c.glassTint   = kColorPresets[i].glass;
                changed = true;
            }
        }
        changed |= ImGui::ColorEdit3("Glass", &c.glassTint.x);
        changed |= ImGui::ColorEdit3("Frame", &c.frameTint.x);
        changed |= ImGui::ColorEdit3("Neon", &c.accentColor.x);
        changed |= ImGui::ColorEdit3("Podium", &c.baseTint.x);
        changed |= ImGui::SliderFloat("Glow", &c.accentStrength, 0.0f, 10.0f);

        // --- Actions ---------------------------------------------------------
        ImGui::Separator();
        ui::hint("%d objects per building", s.objectCount);
        if (ImGui::Button("Generate building", ImVec2(-FLT_MIN, 34.0f)))
            s.generate();
        ImGui::SetItemTooltip("Places a new building on the ground in front of "
                              "the camera (one undo step).");

        ImGui::BeginDisabled(!s.hasLive);
        if (ImGui::Button("Rebuild in place", ImVec2(-FLT_MIN, 0.0f))) s.rebuild();
        ImGui::EndDisabled();
        ImGui::Checkbox("Live update", &s.autoRebuild);
        ImGui::SetItemTooltip("Re-generate the last building whenever a setting "
                              "changes.");
        if (!s.hasLive)
            ImGui::TextDisabled("(generate one first)");

        ui::sectionText("Save as prefab");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##bldName", "Name", s.nameBuf, s.nameBufSize);
        ImGui::BeginDisabled(!s.hasLive || !s.hasProject || s.nameBuf[0] == '\0');
        if (ImGui::Button("Save as prefab", ImVec2(-FLT_MIN, 0.0f)))
            s.saveAsPrefab();
        ImGui::EndDisabled();
        if (!s.hasProject)
            ImGui::TextDisabled("Open a project first (prefabs live in it).");
        else
            ui::hint("The materials are saved with the PROJECT -- save it too, "
                     "or the prefab loads untinted.");
        if (!s.status.empty()) ImGui::TextWrapped("%s", s.status.c_str());

        // A slider drag would otherwise push one rebuild (and one undo step) per
        // frame, so the edit is applied once the widget is released.
        if (changed) s.pendingRebuild = true;
        if (s.pendingRebuild && !ImGui::IsAnyItemActive()) {
            s.pendingRebuild = false;
            if (s.autoRebuild && s.hasLive) s.rebuild();
        }
    }
    ImGui::End();
}

} // namespace buildingui
