#pragma once

#include <functional>
#include <vector>

#include "AnimSystem.hpp"

struct Entity;
class Selection;

// The editor's "Timeline" panel: the scene's keyframes, laid out in time.
//
// WHY IT IS BUILT OUT OF BUTTONS. A conventional timeline is a drag-only
// instrument -- you drag the playhead, you drag keys, you drag to zoom -- and
// every one of those asks for a steady hand on a small target. This editor's
// whole point is that it must not. So: the playhead moves with transport buttons
// and a number field as readily as by dragging; every time the editor writes is
// rounded to the clip's grid, so a shaky drag still lands on a frame and two
// keys meant to line up actually do; keys are selected by a click and then
// nudged a frame at a time by buttons; and the row that says what is selected
// carries its time and value as editable numbers. Dragging still works for
// anyone who wants it -- it is simply never the only way to do anything.
//
// The panel edits; it does not tick. main advances a running clip, because it
// owns the frame's time and the play mode this preview has to stay out of.
namespace timelineui {

struct PanelState {
    bool&                show;      // View > Presentation > Timeline
    // The scene's animations and which of them this panel is editing. A list
    // rather than one clip because an object picks a clip by name with an
    // Animator component -- a scene needs more than one for that to mean
    // anything. Never empty; main keeps one.
    std::vector<anim::Clip>& clips;
    int&                 editClip;
    anim::Player&        player;
    std::vector<Entity>& entities;
    const Selection&     sel;

    // Whether an inspector edit writes a key by itself. Lives in main because
    // the Inspector reads it too -- it is the same switch on both panels.
    bool& autoKey;

    // The scene has changed (a key added, moved, deleted). Marks the document
    // dirty so the animation is not the one edit that closes without asking.
    std::function<void()> markDirty;
};

void drawPanel(const PanelState& s);

// The key diamond the Inspector draws beside an animatable property: hollow when
// the property is not animated, amber when it is, filled white when there is a
// key at the playhead. Returns true when it is clicked.
//
// It lives here because it IS the timeline's key, drawn small -- one look for
// the same thing in both panels, and the Inspector does not grow its own idea of
// what a keyframe looks like.
bool keyDiamond(const char* id, bool animated, bool keyedHere);

} // namespace timelineui
