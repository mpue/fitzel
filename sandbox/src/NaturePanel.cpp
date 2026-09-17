#include "NaturePanel.hpp"

#include <imgui.h>

#include "UiStyle.hpp"

namespace natureui {

void drawPanel(const PanelState& s) {
    if (!s.show) return;
    if (!ImGui::Begin("Advanced nature", &s.show)) { ImGui::End(); return; }

    // --- The mountains on the horizon ---------------------------------------
    ui::sectionText("Horizon");
    ImGui::Checkbox("Horizon terrain", &s.farTerrain);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Draw the terrain past the streamed ring out\n"
                          "to 30 km, coarse and in its own depth range:\n"
                          "mountains on the horizon instead of fog.\n"
                          "Their shape is the Terrain component's\n"
                          "Backdrop and Valley settings.");
    ImGui::BeginDisabled(!s.farTerrain);
    ImGui::SliderFloat("Snow line", &s.snowLine, 0.0f, 4000.0f, "%.0f m");
    ImGui::SliderFloat("Tree line", &s.treeLine, 0.0f, 3000.0f, "%.0f m");
    ImGui::EndDisabled();
    ImGui::SliderFloat("Meadow colour", &s.meadowTint, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Past the grass's radius the ground takes the\n"
                          "field's own colour, so a meadow stays a\n"
                          "meadow at any distance instead of turning\n"
                          "into the soil texture under it.");
    ImGui::SliderFloat("Dry grass", &s.dryGrass, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Ground too dry for the meadow grows a thin,\n"
                          "straw-coloured sward instead of bare earth.");

    // --- The forest -------------------------------------------------------------
    ui::sectionText("Forest");
    ImGui::Checkbox("Forest field", &s.eco.enabled);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Procedural trees from one ecology rule\n"
                          "(stands, clearings, tree line), meshes near\n"
                          "you and impostors out to the forest radius.");
    ImGui::BeginDisabled(!s.eco.enabled);
    ImGui::SliderFloat("Forest cover", &s.eco.cover, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Stand size", &s.eco.standSize, 60.0f, 1500.0f, "%.0f m");
    ImGui::SliderFloat("Lone trees", &s.eco.solitary, 0.0f, 0.3f, "%.3f");
    ImGui::SliderFloat("Slopes wooded", &s.eco.slopeLove, 0.0f, 3.0f, "%.2f");
    ImGui::SliderFloat("Impostors from", &s.impostorStart, 40.0f, 400.0f, "%.0f m");
    ImGui::SliderFloat("Impostor shadows", &s.impostorShadows, 0.0f, 1200.0f,
                       s.impostorShadows <= 0.0f ? "off" : "%.0f m");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The far trees cast shadows too, out to this far:\n"
                          "one card a tree, turned to the sun. Worth it where\n"
                          "the forest is seen from high above.");
    ImGui::SliderFloat("Forest radius", &s.forestRadius, 200.0f, 4000.0f, "%.0f m");
    ImGui::SliderInt("Forest floor layer", &s.forestFloorLayer, -1, 5);
    ImGui::EndDisabled();

    // --- Wind and sun -----------------------------------------------------------
    ui::sectionText("Wind & sun");
    ImGui::SliderFloat("Wind", &s.windStrength, 0.0f, 1.5f, "%.2f");
    ImGui::SliderFloat("Wind towards", &s.windAngle, -180.0f, 180.0f, "%.0f deg");
    ImGui::SliderFloat("Gusts", &s.windGust, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Latitude", &s.sunLatitude, -60.0f, 70.0f, "%.0f deg");
    ImGui::SliderFloat("Season", &s.sunDeclination, -23.4f, 23.4f, "%.1f deg");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The sun's declination: +23 midsummer,\n"
                          "0 equinox, -23 midwinter (north).");
    ImGui::Checkbox("Cloud shadows", &s.cloudShadows);

    // --- What lives in it -------------------------------------------------------
    ui::sectionText("Life");
    ImGui::Checkbox("Wildlife", &s.wildlife);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Starling flocks, swallows, raptors in the\n"
                          "thermals, butterflies at the flowers,\n"
                          "fish rising and jumping in the lake\n"
                          "(needs Birds on in the Vegetation panel).");
    ImGui::Checkbox("Pollen in the light", &s.motes);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Pollen, dust and seeds drifting on the wind,\n"
                          "glittering where the sun shines towards you.");
    ImGui::Checkbox("Soundscape", &s.soundscape);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Birdsong from the trees by the hour,\n"
                          "grasshoppers, crickets, leaves in the wind.\n"
                          "Heard in Play.");
    ImGui::Checkbox("Day runs in Play", &s.timeFlows);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The sun moves while playing, at the Sky\n"
                          "panel's day length -- dawn to dusk to the\n"
                          "fireflies and crickets of the night.");

    ImGui::End();
}

} // namespace natureui
