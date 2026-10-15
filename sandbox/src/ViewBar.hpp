#pragma once

#include <functional>

namespace viewnav { class Nav; }

// The strip across the top of a 3D view: how THIS view draws the scene (the
// shading modes and Post FX) and where it looks from (the standard viewpoints
// and the orthographic lens). Every 3D view has its own, because every 3D view
// has its own of all of these -- a wireframe side view next to the textured
// perspective is the reason to have two views at all. Editor only.
//
// The pictures are the toolbar's (ToolbarIcons.hpp); this strip used to be a
// group of the main toolbar, back when there was one view to set.
namespace viewbar {

struct State {
    int&          shade;          // ViewShade.hpp
    // Post FX: the scene's post look, or the bare frame. Picking a mode sets it
    // (on with Textured and Pathtraced, off with the others); the button
    // overrides that until the next pick. A right-click opens the Post
    // processing panel.
    bool&         postFx;
    bool&         showPost;
    viewnav::Nav& nav;            // the viewpoint menu writes its requests here
    bool          ortho = false;  // the lens as it stands (the label shows it)
    bool          playMode = false;   // the game draws itself: no modes in Play
    std::function<void()> refreshTrace;  // Pathtraced pressed again: look again
};

// The strip's height, for laying the picture out under it.
float height();

// Draw it at the cursor, across the available width. `id` keeps two views'
// strips apart.
void draw(const char* id, const State& s);

} // namespace viewbar
