#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include <glm/glm.hpp>

#include <fitzel/graphics/Texture.hpp>

#include "PathTrace.hpp"
#include "PathTraceCapture.hpp"
#include "PathTracePanel.hpp"

namespace fitzel {
class Camera;
class Renderer;
}

// The path-traced viewport: the Render panel's tracer, pointed at the viewport
// and left running.
//
// It is a MODE OF LOOKING, not a render. The difference decides everything
// here. A render is asked for, takes as long as it takes and is worth waiting
// for; this one nobody asked for -- it has to appear while the author is doing
// something else, get out of the way the moment they move, and never be the
// reason the editor feels slow. So it runs at a fraction of the viewport's
// resolution, at a sample count that reaches "good enough to judge by" in
// seconds rather than "clean" in minutes, and it starts only once the camera
// has been still for a moment. Dragging a camera through a scene while a
// tracer restarts on every frame is not a live preview, it is a stutter.
//
// The harvest is the expensive half (it reads every drawn mesh back off the
// GPU), so it is kept: moving the camera re-aims the SAME scene, and only an
// actual edit pays for a new one.
namespace viewtrace {

struct State {
    // --- What the author gets, and what it costs ---
    // Half resolution: the tracer is on the CPU, and four times the pixels is
    // four times the wait for the same judgement about the same light.
    float scale        = 0.5f;
    int   samples      = 128;   // the ceiling; it is watchable long before it
    int   maxBounces   = 4;
    float settleTime   = 0.25f; // seconds of stillness before a restart

    // --- The running preview ---
    std::shared_ptr<pathtrace::Scene> scene; // the harvest, reused across moves
    pathtrace::Job      job;
    pathcapture::Report report;
    fitzel::Texture     image;
    int         texW = 0, texH = 0;
    int         shownSamples = -1;
    double      shownStamp   = 0.0;
    std::string status;

    // What a restart is watching, and all it watches: the eye and the size of
    // the picture. See service() for why an edit is not on that list.
    glm::mat4   lastView{0.0f};
    int         lastW = 0, lastH = 0;
    bool        needCapture = true;  // harvest afresh on the next restart
    bool        restartDue  = false;
    double      restartAt   = 0.0;
    bool        wasEnabled  = false;
};

// Drive the preview for this frame. Call from the render loop at the SAME
// moment pathpanel::service() is called and for the same reason: the harvest
// can only happen while the renderer's queue still holds the frame.
//
// `enabled` false is the whole switch-off: a running trace is cancelled and its
// scene dropped, so a mode nobody is looking at costs nothing and holds no
// memory. Cheap (one digest of the queue) while it is on and idle.
void service(State& st, bool enabled, fitzel::Renderer& renderer,
             const fitzel::Camera& camera, const pathpanel::SceneLook& look,
             int viewW, int viewH, double now);

// Harvest the scene again and re-trace. The preview follows the CAMERA on its
// own; this is how an edit reaches it -- the editor hangs it off clicking the
// mode's own button again, so the way to say "look again" is to press what you
// are already looking through.
void refresh(State& st);

// The texture to put in the viewport, or 0 while nothing has been traced yet
// (the caller shows its raster frame until then). Top-down, like every other
// image the tracer hands over -- so it is drawn with ImGui's default UVs, NOT
// with the flip a GL render target needs.
unsigned int texture(const State& st);

} // namespace viewtrace
