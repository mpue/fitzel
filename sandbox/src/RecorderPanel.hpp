#pragma once

#include <functional>

#include "RecorderSession.hpp"

// The editor's "Recorder" panel: the game recorder's controls (GameRecorder.hpp,
// RecorderSession.hpp). Record a Play run (or every one), keep the take as a
// .frec file in the project's recordings/ folder, load one back, watch it over
// the scene with a scrubbable playhead, and render it to a video.
namespace recorderui {

struct PanelState {
    bool&            show;
    gamerec::Session& session;
    bool             playMode;
    std::function<void()> startPlay;   // "Record" outside Play: start Play, recording
};

void drawPanel(const PanelState& s);

} // namespace recorderui
