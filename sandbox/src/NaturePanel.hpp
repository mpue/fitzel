#pragma once

#include "Ecology.hpp"

// The Advanced nature panel: everything that makes the landscape more than its
// terrain -- the mountains on the horizon, the forest rule, the wind, where the
// sun runs, and what lives in it. Editor-only (NaturePanel.cpp is not compiled
// into the player); every value it edits is a scene setting main already owns.
namespace natureui {

// What the panel touches in main. References rather than a back-pointer to the
// editor, the same shape weatherui::PanelState uses.
struct PanelState {
    bool& show;                    // the window's own open flag

    // The ground past the streamed ring (FarTerrain.hpp).
    bool&  farTerrain;
    float& snowLine;
    float& treeLine;
    // The meadow past the grass's radius, and the dry ground's sward.
    float& meadowTint;
    float& dryGrass;

    // The forest (Ecology.hpp) and how far it is drawn.
    ecology::Params& eco;
    float& impostorStart;
    float& forestRadius;
    int&   forestFloorLayer;

    // The air and the sun.
    float& windStrength;
    float& windAngle;
    float& windGust;
    float& sunLatitude;
    float& sunDeclination;
    bool&  cloudShadows;

    // What lives in it.
    bool& wildlife;
    bool& motes;
    bool& soundscape;
    bool& timeFlows;
};

void drawPanel(const PanelState& s);

} // namespace natureui
