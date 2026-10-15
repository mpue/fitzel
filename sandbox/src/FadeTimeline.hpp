#pragma once

#include "AnimGraph.hpp"

// The fade of one transition, drawn on a time line: the state being left as one
// bar, the state being entered as another overlapping it, and between them the
// curve that hands the pose from one to the other.
//
// WHY A PICTURE OF A FADE. A transition is four numbers -- when it may go (exit
// time), how long the hand-over takes, along which curve, and where in the new
// clip it lands -- and as four numbers they say nothing about how they meet. Is
// a 0.3 s fade a long one for this walk? Does the new clip start mid-step? Two
// bars that overlap answer that at a glance; four fields never do.
//
// Every handle is a drag AND has a stepper beside it in the transition panel:
// the editor's rule (see GraphPanel.hpp) that dragging is offered everywhere and
// required nowhere. The drags snap to the steppers' steps, so the two never
// disagree about what a value can be.
namespace graphui {

// One side of the fade: what the time line needs to know about a state that the
// transition itself does not say.
struct FadeSide {
    const char* name   = "";
    float       length = 0.0f;  // its clip, in clip seconds (0 = unknown)
    float       rate   = 1.0f;  // how fast it plays (speed x speed parameter)
    bool        loop   = true;
    bool        model  = false; // has a model animation -- only those mix
};

struct FadeView {
    FadeSide from, to;
    // The selected object's machine, if it is running this transition, so the
    // play head can be watched crossing it: seconds spent in the state being
    // left, or seconds into the fade itself. -1 = not there.
    float liveInFrom = -1.0f;
    float liveInFade = -1.0f;
};

// A preview of the fade, played or scrubbed in the editor. Owned by the caller
// so it outlives the frame; the time line moves `at` and draws it.
struct FadePreview {
    bool  playing = false;
    float at      = -1.0f;  // seconds along the line (-1 = no preview)
    float speed   = 1.0f;   // 1, 0.5, 0.25: a fade is judged in slow motion
    const char* target = nullptr;  // whose figure shows it ("" / null = nobody's)
};

// The two states at one moment of the line: how far each clip has played (a
// phase, see animgraph::Instance -- the caller turns it into a clip time with
// animgraph::clipTime) and the leaving state's share of the pose.
struct FadePose {
    float fromPhase = 0.0f;
    bool  toOn      = false;  // false before the exit: only the old state plays
    float toPhase   = 0.0f;
    float fromShare = 1.0f;   // 1 before the exit, 0 once the fade is over
};

// Where the exit sits on the line, in seconds -- drawn half-way along a clip
// for an arrow that waits for nothing (see fadeTimeline).
float fadeExitAt(const animgraph::Transition& t, const FadeView& v);
FadePose fadePoseAt(const animgraph::Transition& t, const FadeView& v, float at);

// Draws the time line at the cursor, full width, and lets its handles be
// dragged. With `preview`, also its play button and a play head that can be
// scrubbed. Returns true on the frames it changed `t`.
bool fadeTimeline(const char* id, animgraph::Transition& t, const FadeView& v,
                  FadePreview* preview = nullptr);

} // namespace graphui
