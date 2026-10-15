#pragma once

#include <memory>

#include <fitzel/scene/Camera.hpp>

#include "ViewShade.hpp"
#include "ViewTargets.hpp"
#include "ViewportNav.hpp"
#include "ViewportTrace.hpp"

// The editor's 3D views. Editor only.
//
// The first one is the "Scene" window the editor always had, drawn with main's
// own targets (hdrRT, the post chain, viewportRT ...). Up to three more can be
// opened in any area; each has its own camera, its own viewpoint and lens, its
// own shading mode, and its own targets -- everything sized to the view or
// carrying history from one of its frames to the next. What is shared is the
// frame's scene: submitted once, its shadows re-fitted per view, its streaming
// done once.
//
// ONE view is active at a time: the one the pointer was last over. Its camera,
// viewpoint, mode and Post FX live in main's `camera`, `viewNav`, `viewShade`
// and `postFx` -- which is what every tool, the gizmo, the picking and the
// keys already read -- and in its Slot only while it is not active. Making
// another view active swaps the two (main's activateView). That is the whole
// trick: none of the tools had to learn that there is more than one view.
namespace edview {

constexpr int kMaxViews = 4;

// What a view looks with, kept here while the view is not the active one.
struct Look {
    fitzel::Camera cam;
    viewnav::Nav   nav;
    int            shade  = kShadeTextured;
    bool           postFx = true;
};

struct Slot {
    bool open    = false;   // its window is up (slot 0's always is)
    bool started = false;   // opened at least once: `look` holds a real view
    bool visible = false;   // its window drew this frame
    bool failed  = false;   // its targets could not be made; it is not drawn
    Look look;
    int  w = 1, h = 1;      // its picture's size, from the last UI pass
    std::unique_ptr<ViewTargets> gl;   // slots 1.. only, made on first draw
    viewtrace::State trace;        // slots 1.. only (slot 0 uses main's)
};

// fitzel::Camera keeps its world-up as a const member, so it cannot be
// assigned: a view's camera is handed over by its settings instead -- the
// pose (with the exact basis, which a look through a rolled scene camera
// sets), the lens and the fly speeds.
inline void copyCamera(fitzel::Camera& to, const fitzel::Camera& from) {
    to.setPosition(from.position());
    to.setYaw(from.yaw());
    to.setPitch(from.pitch());
    to.setBasis(from.front(), from.up());
    to.setFov(from.fov());
    to.setFarPlane(from.farPlane());
    to.setOrthographic(from.orthographic());
    to.setOrthoHalfHeight(from.orthoHalfHeight());
    to.moveSpeed = from.moveSpeed;
    to.mouseSens = from.mouseSens;
}

// The window title of view `slot` -- what the areas dock by.
inline const char* title(int slot) {
    static const char* const kTitles[kMaxViews] = {"Scene", "Scene 2", "Scene 3", "Scene 4"};
    return kTitles[slot];
}

} // namespace edview
