#pragma once

#include <functional>

#include "LevelGen.hpp"

// The level generator's panel: a seed, a few groups of sliders, and a report.
//
// The panel owns no state -- everything it edits lives on the Params main holds,
// and the world it makes is made by main's apply step. Same shape as
// terrainui::PanelState and cityui::PanelState, and for the same reason: an undo
// or a project load must not be able to leave it stale.
//
// The two buttons both go through a confirmation. Generating replaces the
// terrain, the roads and the race objects in this scene, and throws away the
// sculpt layer -- which is not on the undo stack, here or anywhere else in the
// editor (see TerrainPanel's reset, which asks for the same reason). A Reroll
// beside a guarded Generate would be the unguarded way to do the same damage,
// and that is the button people press.
namespace levelui {

struct PanelState {
    bool&                   show;
    levelgen::Params&       params;
    // The last circuit the generator was asked for, WITHOUT it being applied.
    // Laying one out costs a few milliseconds and touches nothing, so the panel
    // can answer "is this reroll worth keeping" before the world is replaced --
    // which is the whole point of the report existing.
    const levelgen::Report& report;
    bool&                   reportValid;
    bool&                   discardPaint;

    // How much the confirmation has to warn about, counted by main.
    int sculptCells = 0;
    int paintCells  = 0;

    // A circuit is being laid out on another thread, or one is being put into
    // the world. The panel greys out and says so rather than pretending the
    // number on screen is the one for the sliders as they now stand.
    bool laying  = false;
    bool applying = false;

    std::function<void()> preview;   // re-run the generator; the world is untouched
    std::function<void()> generate;  // ...and then put it in the world
};

void drawPanel(const PanelState& s);

} // namespace levelui
