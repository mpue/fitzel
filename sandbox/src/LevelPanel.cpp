#include "LevelPanel.hpp"

#include <cstdio>

#include <imgui.h>

#include "UiStyle.hpp"

namespace levelui {
namespace {

// A slider that re-lays the circuit when it is RELEASED, never while it is being
// dragged. Laying one out is milliseconds, but it is milliseconds against a
// report the eye is reading, and a number that flickers through twenty answers
// on the way to one is a number nobody trusts. Same rule TerrainPanel applies to
// its own regenerate.
struct Settled {
    const PanelState& s;
    bool released = false;
    void operator()() { released |= ImGui::IsItemDeactivatedAfterEdit(); }
};

void reportLine(const PanelState& s) {
    if (s.laying) {
        ui::hint("Laying it out...");
        return;
    }
    if (!s.reportValid) {
        ui::hint("Press Generate, or move a slider, to lay one out.");
        return;
    }
    const levelgen::Report& r = s.report;
    ui::hint("%.1f km \xC2\xB7 tightest corner %.0f m \xC2\xB7 steepest %.1f%% \xC2\xB7 "
             "%d bridge%s, %d tunnel%s, %d loop%s \xC2\xB7 %d checkpoints, %d slots",
             r.length / 1000.0f, r.minRadius, r.maxGradient * 100.0f,
             r.bridges, r.bridges == 1 ? "" : "s",
             r.tunnels, r.tunnels == 1 ? "" : "s",
             r.loops, r.loops == 1 ? "" : "s",
             s.params.checkpoints, s.params.gridSlots);
    if (r.crossings)
        ui::hint("%d crossing%s, %.1f m of clearance at the tightest \xC2\xB7 %s",
                 r.crossings, r.crossings == 1 ? "" : "s", r.crossingClearance,
                 s.params.crossing == levelgen::Crossing::Level ? "on the level"
                                                                : "flyovers");
    if (r.droppedFeatures)
        ui::hint("%d feature%s had nowhere to go on this circuit", r.droppedFeatures,
                 r.droppedFeatures == 1 ? "" : "s");
    if (!r.why.empty()) {
        const ImVec4 warn = r.ok ? ImVec4(0.80f, 0.80f, 0.55f, 1.0f)
                                 : ImVec4(1.00f, 0.55f, 0.30f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, warn);
        ImGui::TextWrapped("%s", r.why.c_str());
        ImGui::PopStyleColor();
    }
}

// The confirmation. It says what goes, what stays, and that this cannot be taken
// back -- because two of the things it throws away are not on the undo stack at
// all, and a dialog that only asks "are you sure?" is one people learn to click
// through.
void confirmPopup(const PanelState& s) {
    ImGui::SetNextWindowSize(ImVec2(470.0f, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Generate level", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::TextWrapped("This replaces the world in this scene.");
    ImGui::Spacing();
    ui::sectionText("Replaced");
    ImGui::BulletText("Terrain, regenerated from the seed.");
    ImGui::BulletText("Every road in this scene, by the generated circuit.");
    ImGui::BulletText("The start/finish line, the checkpoints and the grid markers.");
    if (s.sculptCells > 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.30f, 1.0f));
        ImGui::BulletText("Sculpting: %d edited cells, DISCARDED.", s.sculptCells);
        ImGui::PopStyleColor();
        ui::hint("The road grades its corridor into that layer, so a corridor cut\n"
                 "for the old track would stay behind as a trench under the new one.");
    }
    ImGui::Spacing();
    ui::sectionText("Kept");
    ImGui::BulletText("The sun, the sky, the water and your materials.");
    ImGui::BulletText("Pickups, boost pads, prefab instances, anything you placed.");
    ImGui::BulletText("Rivers, re-cut on the new ground. Vegetation regrows itself.");
    ImGui::Spacing();
    if (s.paintCells > 0)
        ImGui::Checkbox("Also discard texture painting", &s.discardPaint);
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.30f, 1.0f));
    ImGui::TextWrapped("Sculpting and painting are not on the undo stack. "
                       "Generate clears the undo history.");
    ImGui::PopStyleColor();
    if (s.reportValid && s.report.corridorCells > 400000)
        ui::hint("About %.1f million corridor cells to cut -- this will pause.",
                 s.report.corridorCells / 1.0e6f);
    ImGui::Separator();
    if (ImGui::Button("Generate", ImVec2(130.0f, 0.0f))) {
        s.generate();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(130.0f, 0.0f))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

} // namespace

void drawPanel(const PanelState& s) {
    if (!s.show) return;
    if (!ImGui::Begin("Level generator", &s.show)) { ImGui::End(); return; }

    levelgen::Params& p = s.params;
    Settled slid{s};

    ui::hint("A whole race scene from one seed: the land, one circuit built into\n"
             "it, and the race logic that makes it a race.");

    // --- Circuit --------------------------------------------------------------
    ui::sectionText("Circuit");
    {
        int seed = static_cast<int>(p.seed);
        if (ImGui::DragInt("Seed", &seed, 1.0f, 1, 1000000)) p.seed = std::max(seed, 1);
        slid();
        ImGui::SameLine();
        if (ImGui::Button("Reroll")) {
            // The same decorrelation the rest of the codebase uses on a reroll:
            // a seed stepped by one is a seed that looks like it did nothing.
            p.seed = p.seed * 1664525u + 1013904223u;
            if (p.seed == 0) p.seed = 1;
            s.preview();
        }
    }
    ImGui::SliderFloat("Length", &p.length, 600.0f, 12000.0f, "%.0f m");   slid();
    ImGui::SliderInt("Corners", &p.corners, 6, 24);                        slid();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How many corners the lap has. NOT how many control\n"
                          "points the road gets -- those are re-spaced every\n"
                          "twenty metres so the profile has room to work.");
    ImGui::SliderFloat("Irregularity", &p.irregular, 0.0f, 1.0f, "%.2f");  slid();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("0 is a plain oval. The generator takes this back on\n"
                          "its own if the corners come out tighter than the road\n"
                          "width and the pace allow, and says so in the report.");
    ImGui::SliderFloat("Width", &p.width, 8.0f, 40.0f, "%.0f m");          slid();
    ImGui::SliderFloat("Corner pace", &p.cornerPace, 12.0f, 45.0f, "%.0f m/s"); slid();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The speed the opponents must be able to hold in the\n"
                          "tightest corner. This is what sets the minimum radius:\n"
                          "they corner at sqrt(grip / curvature) and know nothing\n"
                          "about how the track was made.");

    // --- Land -----------------------------------------------------------------
    ui::sectionText("Land");
    ImGui::SliderFloat("Relief", &p.relief, 0.2f, 2.0f, "%.2f");           slid();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Rolling plain to alpine. Asking for alpine AND a gentle\n"
                          "gradient asks for two things at once; the generator\n"
                          "softens the land until both fit and tells you it did.");
    {
        float g = p.maxGradient * 100.0f;
        if (ImGui::SliderFloat("Gradient limit", &g, 3.0f, 20.0f, "%.0f%%"))
            p.maxGradient = g * 0.01f;
        slid();
    }
    ImGui::SliderFloat("Bank limit", &p.maxBank, 0.0f, 18.0f, "%.0f\xC2\xB0"); slid();

    // --- Features -------------------------------------------------------------
    ui::sectionText("Features");
    {
        int shape = static_cast<int>(p.shape);
        if (ImGui::BeginCombo("Shape", levelgen::shapeName(p.shape))) {
            for (int i = 0; i < static_cast<int>(levelgen::Shape::Count); ++i) {
                const auto cand = static_cast<levelgen::Shape>(i);
                if (ImGui::Selectable(levelgen::shapeName(cand), i == shape) &&
                    cand != p.shape) {
                    p.shape = cand;
                    s.preview();
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Ring: an irregular natural circuit.\n"
                              "Speedway: two long straights, two banked turns.\n"
                              "Street circuit: four straights, four hard corners.\n"
                              "Figure of eight: one self-crossing.\n"
                              "Trefoil: three of them -- the shape junctions are for.");
    }
    const bool crosses = (p.shape == levelgen::Shape::Eight ||
                          p.shape == levelgen::Shape::Knot);
    ImGui::BeginDisabled(!crosses);
    {
        int kind = (p.crossing == levelgen::Crossing::Level) ? 1 : 0;
        if (ImGui::Combo("Crossing", &kind, "Flyover\0On the level\0"))
            p.crossing = kind ? levelgen::Crossing::Level : levelgen::Crossing::Flyover;
        slid();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A flyover lifts one branch over the other and puts a\n"
                              "bridge under it. On the level, both branches are held\n"
                              "to one height and the road system lays a junction\n"
                              "apron where they meet.");
    }
    ImGui::EndDisabled();
    ImGui::SliderInt("Bridges", &p.bridges, 0, 6);   slid();
    ImGui::SliderInt("Tunnels", &p.tunnels, 0, 4);   slid();
    ImGui::SliderInt("Loops",   &p.loops,   0, 3);   slid();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Opponents drive the flat road under a loop -- only the\n"
                          "player rides it. A loop is a thing to look at and to\n"
                          "fly, not part of the racing line.");
    ImGui::SliderFloat("City", &p.cityAmount, 0.0f, 1.0f, "%.2f");  slid();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How much of the lap has a district beside it. Past the\n"
                          "halfway mark it becomes a canyon, hard against both\n"
                          "kerbs -- which is the expensive one to draw.");

    // --- The race -------------------------------------------------------------
    ui::sectionText("Race");
    ImGui::SliderInt("Checkpoints", &p.checkpoints, 3, 20);  slid();
    ImGui::SliderInt("Grid slots",  &p.gridSlots,   1, 24);  slid();
    ImGui::SliderFloat("Laps", &p.laps, 1.0f, 20.0f, "%.0f");
    ImGui::Checkbox("Player on pole", &p.pinPlayerPole);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("With no slot claiming the player there is no craft to\n"
                          "build, and a circuit opened straight from the editor\n"
                          "has nothing to fly. Untick it once the scene has its\n"
                          "own glider and the start becomes a draw again.");

    // --- Prefabs --------------------------------------------------------------
    if (ui::header("Prefabs")) {
        ui::hint("By name, as in the project's prefabs folder. A name that is not\n"
                 "there costs the LOOK of that object and nothing else -- the\n"
                 "marker still carries the component, and the race still runs.");
        auto nameField = [](const char* label, std::string& v) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s", v.c_str());
            if (ImGui::InputText(label, buf, sizeof(buf))) v = buf;
        };
        nameField("Player craft", p.playerPrefab);
        nameField("Rival craft",  p.rivalPrefab);
        nameField("Start line",   p.finishPrefab);
        nameField("Checkpoint",   p.checkPrefab);
        nameField("Guard rail",   p.railModel);
        nameField("Start decal",  p.startDecal);
    }

    // --- What it comes out as -------------------------------------------------
    ImGui::Separator();
    reportLine(s);
    ImGui::Spacing();
    ImGui::BeginDisabled(s.applying);
    if (ImGui::Button(s.applying ? "Generating..." : "Generate...",
                      ImVec2(-1.0f, 0.0f)))
        ImGui::OpenPopup("Generate level");
    ImGui::EndDisabled();
    confirmPopup(s);

    // Re-lay it on release, so the report answers for the sliders as they stand.
    if (slid.released) s.preview();

    ImGui::End();
}

} // namespace levelui
