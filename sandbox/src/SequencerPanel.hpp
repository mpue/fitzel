#pragma once

#include <functional>
#include <string>
#include <vector>

#include <imgui.h>

#include "AnimSystem.hpp"
#include "Sequencer.hpp"

struct Entity;

// The editor's "Sequencer" panel: the scene's sequences, drawn as an edit.
//
// IT LOOKS LIKE AN EDITOR AND WORKS LIKE THIS ONE. The layout is the one every
// video editor and DAW has taught people -- track headers on the left, strips on
// lanes, a ruler and a playhead over them, waveforms in the audio -- because that
// picture is understood before anything is explained. But the editor's rule
// holds here as it does in the Timeline: nothing needs a steady hand. Every
// drag has a button or a number beside it -- a strip is placed at the playhead
// from its track's + menu, moved and trimmed a frame at a time from the strip
// bar under the lanes, and every time it lands on is rounded to the grid (or,
// within a few pixels, onto the playhead or another strip's edge, so two
// strips meant to butt up against each other do). Dragging is offered, never
// required.
//
// The panel edits and previews; it does not run the game. main ticks the
// runner, because it owns the frame's time and the camera the view is on.
namespace sequi {

struct Host {
    bool& show;                              // View > Presentation > Sequencer
    std::vector<seq::Sequence>& seqs;
    int&  editSeq;                           // which sequence the panel shows
    seq::Runner& runner;
    std::vector<anim::Clip>& clips;          // what animation strips can place
    std::vector<Entity>& entities;
    int&  activeCam;                         // the camera that is the view
    bool  gameRunning = false;               // Play: the runner is the game's

    // Sound files the project can place, by the name a strip stores.
    std::function<std::vector<std::string>()>              soundNames;
    // A strip's sound name to a path the waveform can be read from.
    std::function<std::string(const std::string&)>          resolveSound;
    // An asset dragged in from the Assets browser (its GUID) to a sound name;
    // "" when it is not a sound.
    std::function<std::string(const std::string& guid)>     soundOfAsset;
    // The mixer's channel names, for an audio track's output.
    std::function<std::vector<std::string>()>              channels;
    // The recorded fly-camera path as a new camera entity and a clip that moves
    // it. Fills both; false when there is no path worth the name.
    std::function<bool(std::string& clip, int& camera)>     cameraFromPath;
    bool hasCameraPath = false;
    // A new camera where the viewport's view is (an Empty + Camera); its id,
    // or -1. Lets a shot be set up the way it is in a film: stand where the
    // camera goes, and press the button.
    std::function<int()>                                   cameraFromView;
    // Select an object in the scene (a shot's camera, from its strip).
    std::function<void(int entityId)>                      selectEntity;
    // The scene changed: mark the document dirty.
    std::function<void()>                                  markDirty;
    // Called just before a preview starts, so the Timeline can end its own
    // first: two previews capturing each other's poses would hand the wrong
    // values back when they end.
    std::function<void()>                                  beforePreview;

    // Optional: told where the panel drew its parts ("strip:<uid>",
    // "lane:<track uid>", "play", "add:<track uid>"), so seqpanelcheck can
    // press them the way a person would.
    std::function<void(const std::string&, ImVec2, ImVec2)> probe;
};

void drawPanel(const Host& h);

// The sequencer window has the keyboard: Del, Space and the arrows are its own
// and must not also reach the scene (deleting the selected object, say).
bool ownsKeys();

// Select a strip (its uid; -1 = none), as a click on it would.
void selectStrip(int stripUid);

} // namespace sequi
