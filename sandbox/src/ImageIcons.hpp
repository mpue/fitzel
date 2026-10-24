#pragma once

#include <imgui.h>

// The Image editor's icons: drawn, not typed (the UI font has no tool glyphs),
// each one a few primitives in a square -- the way SequencerPanel draws its
// transport. They follow Photoshop's pictures where Photoshop has one, so a hand
// that knows where the lasso is finds it here too.
namespace imageui {

enum class Icon {
    // tools
    Move, Marquee, MarqueeEllipse, Lasso, PolyLasso, Wand, Crop, Eyedropper, Brush, Clone, Eraser,
    Gradient, RadialGradient, Bucket, Blur, Sharpen, Dodge, Burn, Sponge, Saturate, Text, Shape, Hand, Zoom,
    // shapes
    Line, RectOutline, RectFilled, EllipseOutline, EllipseFilled,
    // selection modes
    SelReplace, SelAdd, SelSubtract, SelIntersect,
    // actions
    Undo, Redo, ZoomIn, ZoomOut, Fit, OneToOne, Swap, Defaults,
    Eye, EyeOff, Lock, Unlock, LayerNew, Duplicate, Trash, Up, Down, Left, Right, Merge,
    Check, Cancel, FlipH, FlipV, RotateCW, RotateCCW, LineMode, Target, Transform, Chain, Tile, Grid,
    Count
};

// `c` the centre, `s` the side of the square it fills.
void drawIcon(ImDrawList* dl, Icon ic, ImVec2 c, float s, ImU32 col);

// A square button with an icon. `on` lights it (a switch that is on, the tool
// in hand); the tooltip says what the icon cannot. `side` 0 = the frame height.
bool iconButton(const char* id, Icon ic, const char* tip, bool on = false, float side = 0.0f);

} // namespace imageui
