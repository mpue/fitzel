#pragma once

#include <cstdint>
#include <functional>

#include <glm/glm.hpp>
#include <imgui.h>

#include <fitzel/world/Terrain.hpp>

struct ViewportFrame; // EditorContext.hpp

// The editor's "Terrain Sculpt" tool: the panel with the brush controls, and the
// brush itself in the viewport (picking, the per-frame dabs, the anchored Pull
// and Rain gestures, the cursor ring) for the manual deformation layer
// (raise/lower/smooth/flatten/erode/stamp/noise/carve/pull/rain). It edits the
// live TerrainEditField owned by main. Editor only.
namespace sculptui {

// The brush: its settings, which the panel edits, and the gesture in flight,
// which the viewport keeps from frame to frame. One object in main.
struct Brush {
    int   tool          = 0;     // 0 raise 1 lower 2 smooth 3 flatten 4 erode 5 stamp 6 noise 7 carve 8 pull 9 rain
    float radius        = 8.0f;  // world units
    float strength      = 0.5f;  // 0..1 brush intensity
    float flattenHeight = 0.0f;  // flatten target height (grabbed on press)
    int   stampShape    = 0;     // 0 dome 1 cone 2 plateau 3 crater 4 ridge 5 range
    float stampHeight   = 12.0f; // stamp peak height (m); negative digs in
    float stampRot      = 0.0f;  // ridge orientation (radians)
    float noiseFreq     = 0.35f; // roughen feature size
    float noiseSeed     = 0.0f;  // advanced per dab so detail layers up
    float carveDepth    = 12.0f; // valley depth (m); Alt raises a ridge

    // --- Proportional pull ------------------------------------------------
    // Press on the ground and the point under the cursor follows it up or
    // down, with the disc around it coming along less and less out to the rim.
    //
    // It is the one sculpt tool here that is not a dab. Every other brush
    // applies a step per frame and the ground ends up wherever holding the
    // button for that long put it -- which means the result depends on the
    // steadiness of a hand, and getting a hill to a particular height means
    // creeping up on it. This one is ABSOLUTE: the height is a function of
    // where the mouse is now, not of how long it has been down, so overshoot
    // costs nothing, wobble leaves nothing behind, and letting go early
    // leaves exactly what was on screen. That is the whole reason it exists
    // next to Raise rather than instead of it.
    float pullFalloff = 1.0f;    // skirt shape (see TerrainEditField::pull)
    // What one CLICK is worth. The drag is a convenience on top of it, not
    // the way in: press-and-move is exactly the gesture this editor exists to
    // not require (see the Parkinson note in the project's UI rules), so the
    // tool has to be complete without it -- set the number with the panel's
    // steppers, click the ground, done. A drag ends by writing what it
    // arrived at back into here, so the next click repeats it.
    float     pullHeight  = 4.0f;    // metres per click; negative pushes in
    bool      pullActive  = false;   // a pull gesture is in progress
    glm::vec2 pullCenter{0.0f};      // where it was anchored
    float     pullRadius  = 8.0f;    // ...and the radius it was anchored with
    float     pullShape   = 1.0f;    // ...and the shape, both frozen for the drag
    float     pullApplied = 0.0f;    // metres already written to the field
    float     pullStartY  = 0.0f;    // mouse Y at the press, in screen pixels
    float     pullScale   = 0.05f;   // world metres per screen pixel, at the anchor

    // --- Rain: hydraulic erosion ------------------------------------------
    // Anchored the way Pull is: the press decides where it rains, and holding
    // keeps it raining THERE, so a hand that drifts or shakes does not smear
    // the gullies across the slope. A click alone is one shower; held, the
    // showers come at a fixed pace rather than one per frame.
    bool          rainActive = false;
    glm::vec2     rainCenter{0.0f};
    float         rainRadius = 8.0f;
    float         rainClock  = 0.0f;   // seconds to the next shower while held
    std::uint32_t rainSeed   = 1;      // advanced per shower, so no two fall alike
};

struct PanelState {
    bool& show;
    // Only this tool's own switch: which tool has the left button is main's
    // (ViewTool.hpp), so turning this on switches the others off there.
    bool& sculptMode;

    Brush& brush;

    fitzel::TerrainEditField& work;      // live, main-thread-only edit field
    fitzel::TerrainStreamer&  streamer;
    bool&                     grassDirty; // set true to regrow grass after a change
    std::function<void()>     publish;    // snapshot `work` as the live terrain layer
};

// Draws nothing when `show` is false.
void drawPanel(const PanelState& s);

// One frame of the brush while sculpt mode owns the left mouse button: raise,
// lower, smooth or flatten the ground under a 3D disc that hugs the surface
// (hold LMB to apply; Alt inverts raise/lower), the anchored Pull and Rain
// gestures, and the cursor ring. Every change is published and the touched
// terrain chunks rebuilt.
void brushViewport(Brush& brush, const ViewportFrame& view, fitzel::TerrainEditField& work,
                   fitzel::TerrainStreamer& streamer, const std::function<void()>& publish,
                   bool& grassDirty, float dt);

} // namespace sculptui
