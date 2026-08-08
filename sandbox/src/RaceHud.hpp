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

// What the end-of-race question came back with. The HUD only reports the
// choice; restarting or leaving is the caller's business.
enum class EndAction { None, RaceAgain, StartScreen };

// End-of-race flow: the classification board comes up first, and only once the
// player has acknowledged it does the "race again / start screen" question
// appear -- so the summary can be read before anything asks for a decision.
// The caller owns one of these and clears it when a race starts.
struct EndPrompt {
    bool  asked  = false;   // board acknowledged -> the question is up
    int   choice = 0;       // 0 = race again, 1 = start screen
    float boardT = 0.0f;    // seconds the board has been up (arms the hint)
};

// Menu input, already edge-detected by the caller (which owns the keyboard and
// gamepad, exactly as it does for the scene UI overlay). The mouse is handled
// in here, where the button rectangles are.
struct EndInput {
    bool confirm = false;   // Enter / pad A
    bool prev    = false;   // left / up
    bool next    = false;   // right / down
};

// Draw the HUD into `dl`, anchored to the rendered viewport rect (`vmin` /
// `vsize`) -- the whole window in presentation mode, the viewport panel in the
// editor. `topInset` pushes the top-left block down past anything the caller
// already drew there (the script's HUD line). Call only while a glider is flown.
EndAction draw(ImDrawList* dl, const ImVec2& vmin, const ImVec2& vsize,
               const racesim::RaceState& st, float topInset,
               EndPrompt& prompt, const EndInput& in);

} // namespace racehud
