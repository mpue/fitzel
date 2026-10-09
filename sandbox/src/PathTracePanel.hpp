#pragma once

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>

#include <atomic>
#include <thread>

#include <fitzel/graphics/Texture.hpp>
#include <fitzel/graphics/Texture3D.hpp>

#include <functional>
#include <vector>

#include "LightGrid.hpp"
#include "GrassTrace.hpp"
#include "PathTrace.hpp"
#include "PathTraceCapture.hpp"
#include "ShotList.hpp"
#include "WorldTrace.hpp"

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
    // What of the world beyond the render queue goes in, and how far out.
    worldtrace::Options  world;
    int  resolutionPreset = 1;      // index into the preset list in the .cpp
    bool saveExrToo       = true;   // write the linear image beside the PNG

    // Depth of field. `autoFocus` measures the distance to whatever the centre
    // of the frame is pointed at, which is what an author means by "focus on
    // the car" and saves them reading a number off the scene.
    float aperture      = 0.0f;     // metres; 0 = everything sharp
    bool  autoFocus     = true;
    float focusDistance = 10.0f;
    // The job measures the focus once its accelerator is built; the panel
    // picks the answer up from it so the slider shows where the lens went.
    bool  focusPending  = false;

    // --- The running render ---
    pathtrace::Job      job;
    // The scene the job was started on, kept so a shot list can render the
    // very same harvest on the GPU as well (attachToShots' `gpuToo`).
    std::shared_ptr<const pathtrace::Scene> lastScene;
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

    // --- Baked light -------------------------------------------------------
    // The grid shares this panel because it shares everything that makes it
    // possible: the same harvest of the render queue, the same tracer, the same
    // moment in the frame. Splitting it into a panel of its own would duplicate
    // all three to separate two buttons.
    // The settings and the worker live here; the GRID does not. It belongs to
    // the application (lightgrid::Runtime), because a shipped game loads and
    // lights from one without there being an editor anywhere near it.
    lightgrid::Settings gridSettings;
    std::string         gridStatus;
    int                 bakeLamps = 0, bakeLampsBaked = 0;   // what the last bake heard

    // The bake runs for minutes, so it runs on its own thread and the editor
    // stays usable. The atomics are the only things the two threads share.
    std::thread        bakeThread;
    std::atomic<bool>  bakeRunning{false};
    std::atomic<bool>  bakeCancel{false};
    std::atomic<float> bakeProgress{0.0f};
    std::atomic<bool>  bakeDone{false};   // finished: join, upload, hand over
    bool bakeRequested = false;

    State();
    ~State();
    State(const State&)            = delete;
    State& operator=(const State&) = delete;
};

// Draw the panel. `scenePath` is the open scene's file: renders land beside it
// in renders/, and its baked light in lightgrids/. Empty means no scene is
// open, and the panel says so rather than writing somewhere arbitrary.
// `now` is the editor's clock, used only to rate-limit the preview refresh.
void draw(State& state, lightgrid::Runtime& light,
          const std::filesystem::path& scenePath, double now);

// The parts of the frame's look that the Renderer does not carry, because they
// belong to something else: the sky is a file the environment map was loaded
// from, and the grade lives in the post chain. Grouped rather than passed as
// loose arguments so that the next thing the picture depends on has an obvious
// place to go instead of lengthening a call.
struct SceneLook {
    std::string      hdriPath;        // "" = the scene has no environment map
    float            hdriIntensity = 1.0f;
    pathtrace::Grade grade;

    // The grass field, or null when the scene has none / it is switched off.
    // Grass reaches the tracer through here rather than through the harvest,
    // because there is nothing in the render queue to harvest: the blades are
    // instance data plus a vertex shader. It is regenerated from these
    // parameters instead -- see GrassTrace.hpp. A pointer because the field owns
    // its road and brook lists and a render is not worth copying them for; the
    // capture is synchronous, so it only has to outlive the call.
    const grassfield::Field* grass = nullptr;
    float grassRadius = 40.0f;  // metres of field around the camera
    float grassWindTime = 0.0f; // the instant of wind the still is taken at
    // The blades painted by hand (7 floats each, relative height) and the
    // field's blade height that scales them. Null = none.
    const std::vector<float>* paintedGrass = nullptr;
    float grassHeight = 0.35f;

    // Everything else the render queue does not carry -- the forest, flowers,
    // water, far terrain, wildlife, particles, rain, the clouds' shadow and the
    // sky (WorldTrace.hpp). Handed in as a call rather than as the systems
    // themselves, so neither the panel nor the preview has to know what a
    // forest is. `preview` asks for the near world only. Empty: none of it.
    std::function<void(pathtrace::Scene&, const worldtrace::Options&, bool preview,
                       std::vector<std::string>& notes)> world;
};

// Fulfil a pending request. Call once per frame from the render loop, AFTER the
// frame's submit() calls and BEFORE the next begin() -- that is the only window
// in which the renderer's queue holds the scene. Cheap (a bool test) unless a
// render was actually asked for.
// Also hands the renderer this frame's baked light, loads the grid belonging to
// `scenePath` when the scene changes, and finishes a bake that has completed on
// its worker thread -- all of which need a current GL context and the render
// loop's own moment, which is why they live here rather than in draw().
// Let a shot list (`--shots-trace <samples>`) take a traced still of each of its
// views through this panel: the same harvest, settings and file format as the
// Render button, at the size of the window and `samples` per pixel.
// With `gpuToo`, the same harvest is rendered by the GPU tracer as well and
// written as <name>_gpu.png -- the viewport preview's renderer, on the scene the
// still was made from, so the two can be held against each other.
void attachToShots(State& state, shotlist::Runner& shots, int samples,
                   const std::function<void(int&, int&)>& windowSize,
                   bool gpuToo = false);

// The meadow's colour (GroundLook) fades in where the viewport's blades fade
// out: from 30% to 95% of the grass radius. The traced field can be cut shorter
// than the viewport's (the triangle ceiling, the preview's smaller disc), and
// the fade then follows the TRACED edge -- or there is bare soil between the
// last blade and the first of the meadow's colour.
inline void followGrass(pathtrace::GroundLook& g, float tracedRadius) {
    if (tracedRadius <= 0.0f || !g.meadowOn()) return;
    g.meadowNear = std::min(g.meadowNear, 0.3f * tracedRadius);
    g.meadowFar  = std::min(g.meadowFar, 0.95f * tracedRadius);
}

void service(State& state, lightgrid::Runtime& light,
             fitzel::Renderer& renderer, const fitzel::Camera& camera,
             const SceneLook& look, const std::filesystem::path& scenePath);

} // namespace pathpanel
