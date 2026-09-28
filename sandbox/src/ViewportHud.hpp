#pragma once

#include <string>

#include <imgui.h>

class CommandStack;

// What the Scene window shows over the picture besides the tools: small
// read-outs and one or two controls in its corners. Each one is about the view
// or the next Play, not about an object in the scene -- which is why they sit
// in the viewport and not in a panel. Editor only.
//
// `vmin` / `vmax` are the scene IMAGE's rect (see the Scene window: never the
// last item's, which is whichever overlay was drawn last).
namespace viewhud {

// The selected camera's own view, bottom right: a sixth of the viewport's width
// in the preview's aspect, clamped so it stays a corner and never grows into a
// second view. Drawn into the window's draw list, not a floating window: it
// belongs to the viewport, moves with it, and cannot be dragged away and lost.
void cameraPreview(ImVec2 vmin, ImVec2 vmax, ImTextureID texture, int texW, int texH,
                   const std::string& name);

// Top right: what Play will start as in this scene (-1: the game's own start).
// Scene data, so a change touches `history` for the autosave. Amber while it
// forces something. True while the pointer is on it -- it is then NOT on the
// scene, or a click would open the combo and pick an object behind it at once.
bool playAsPicker(ImVec2 vmin, ImVec2 vmax, int& sceneStartMode, CommandStack& history);

// Top left, over the path tracer's picture: what it is doing. A progressive
// render that says nothing looks exactly like a stuck one.
void traceStatus(ImVec2 vmin, const std::string& status);

// A camera preview owns the viewport: "Exit camera: <name>" top left gives the
// free camera back (`activeCam` = -1). True while the pointer is on the button,
// so a click on it does not also select what is behind it.
bool exitCamera(ImVec2 vmin, const std::string& cameraName, int& activeCam);

// Top left: which way we are looking when it is a standard view ("Front"), and
// the lens when it is orthographic. Nothing for a free perspective view.
void viewLabel(ImVec2 vmin, const char* standardView, bool ortho);

} // namespace viewhud
