#pragma once

#include <imgui.h>

namespace racesim { struct RaceState; }

// The racing HUD: speed readout, lap + times, the field's running order, the
// start countdown, and the final classification.
//
// Everything it draws comes out of one racesim::RaceState, so it needs no
// plumbing of its own -- and it draws straight into an ImDrawList (no ImGui
// windows), because a game HUD has to sit on the rendered image without the
// editor's panel chrome.
namespace racehud {

// Draw the HUD into `dl`, anchored to the rendered viewport rect (`vmin` /
// `vsize`) -- the whole window in presentation mode, the viewport panel in the
// editor. `topInset` pushes the top-left block down past anything the caller
// already drew there (the script's HUD line). Call only while a glider is flown.
void draw(ImDrawList* dl, const ImVec2& vmin, const ImVec2& vsize,
          const racesim::RaceState& st, float topInset);

} // namespace racehud
