#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include <fitzel/graphics/Texture.hpp>

#include "PathTrace.hpp"
#include "PathTraceCapture.hpp"

namespace fitzel {
class Camera;
class Renderer;
}

// The Render panel: where an author asks for a still and watches it arrive.
//
// It is split from both halves it drives -- the tracer and the harvest -- for
// the usual reason (main.cpp stays out of it) and one specific one: the harvest
// must happen at an exact moment in the frame, after every submit() and before
// the next begin(), and that moment is nowhere near where a panel is drawn. So
// the panel only ever RAISES A REQUEST, and service() fulfils it from the right
// place in the loop. Doing the readback from inside the ImGui call would work
// by accident today and break the first time a panel moves.
//
// The preview refreshes while the render converges rather than appearing at the
// end, because "how many samples does this shot need" has no answer except
// watching one: the author stops it when the noise stops mattering, which is a
// judgement nobody can make from a number.
namespace pathpanel {

struct State {
    bool open = false;   // the View menu's flag for this panel

    // --- What the author set ---
    pathcapture::Options capture;
    pathtrace::Settings  settings;
    int  resolutionPreset = 1;      // index into the preset list in the .cpp
    bool saveExrToo       = true;   // write the linear image beside the PNG

    // Depth of field. `autoFocus` measures the distance to whatever the centre
    // of the frame is pointed at, which is what an author means by "focus on
    // the car" and saves them reading a number off the scene.
    float aperture      = 0.0f;     // metres; 0 = everything sharp
    bool  autoFocus     = true;
    float focusDistance = 10.0f;

    // --- The running render ---
    pathtrace::Job      job;
    pathcapture::Report report;
    std::string         reportLine;
    std::string         status;
    std::string         lastSaved;

    // Set by draw(), cleared by service(). See the note above about why the
    // harvest cannot happen where the button is.
    bool captureRequested = false;

    // The preview. The Texture is a member and not a local because ImGui draws
    // the frame's image list AFTER the panel code has returned: a texture
    // destroyed at the end of draw() leaves the draw command pointing at a
    // freed name, and what appears in its place is the font atlas.
    fitzel::Texture preview;
    int    previewW = 0, previewH = 0;
    int    previewSamples = -1;
    double previewStamp   = 0.0;

    State();
};

// Draw the panel. `projectDir` is where a saved render lands (in renders/).
// `now` is the editor's clock, used only to rate-limit the preview refresh.
void draw(State& state, const std::filesystem::path& projectDir, double now);

// The parts of the frame's look that the Renderer does not carry, because they
// belong to something else: the sky is a file the environment map was loaded
// from, and the grade lives in the post chain. Grouped rather than passed as
// loose arguments so that the next thing the picture depends on has an obvious
// place to go instead of lengthening a call.
struct SceneLook {
    std::string      hdriPath;        // "" = the scene has no environment map
    float            hdriIntensity = 1.0f;
    pathtrace::Grade grade;
};

// Fulfil a pending request. Call once per frame from the render loop, AFTER the
// frame's submit() calls and BEFORE the next begin() -- that is the only window
// in which the renderer's queue holds the scene. Cheap (a bool test) unless a
// render was actually asked for.
void service(State& state, const fitzel::Renderer& renderer,
             const fitzel::Camera& camera, const SceneLook& look);

} // namespace pathpanel
