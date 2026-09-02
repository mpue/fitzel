#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "WeatherPreset.hpp"

// The Weather & audio panel: the preset shelf, the storm dial and the weather's
// own mixer. Editor-only -- the widget lives in WeatherPanel.cpp, which the
// player does not compile, while WeatherPreset.cpp (the presets themselves) is
// runtime, because a loaded scene has to be able to be a weather without a panel
// in the room.
namespace weatherui {

// What the panel touches in main. References rather than a back-pointer to the
// editor, the same shape riverui::PanelState uses.
struct PanelState {
    bool& show;                  // the window's own open flag

    // The live weather, as WeatherPreset.hpp binds it.
    weather::Live live;

    // The project's presets and which one is showing. Owned by main so they
    // survive the frame and are reloaded when the project changes.
    std::vector<weather::Preset>& presets;
    std::string&                  current;  // "" = nothing applied / hand-edited

    // What the NEXT save should record about a preset's reach: whether it moves
    // the clock and whether it speaks about the world mist. They are the two
    // things capture() cannot read off the live state -- "the mist is off" and
    // "this weather has no opinion about mist" look identical from here.
    bool& savesTime;
    bool& savesMist;

    // Where weather.json lives. Empty when no project is open, which is the one
    // state where a preset can be applied but not kept.
    std::string projectFolder;

    // The "Save as" name field. main owns the buffer so the half-typed name
    // survives a frame in which the panel was not drawn.
    char*       nameBuf = nullptr;
    std::size_t nameCap = 0;

    // Read-outs the panel shows and does not own: what the dial is currently
    // producing, and the ground under the camera (a preset with mist in it sits
    // its volume on that).
    float rainIntensity = 0.0f;
    float roadWetness   = 0.0f;
    float groundY       = 0.0f;

    // The master end of the mixer, which lives here because this is the panel
    // people open when the rain is too loud.
    bool&  muted;
    float& masterVolume;
    bool   audioOk = true;
};

void drawPanel(const PanelState& s);

} // namespace weatherui
