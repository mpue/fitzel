#pragma once

#include <imgui.h>

// Buttons that show a picture instead of a word.
//
// The words do not go away -- every one of these carries its name and what it
// does in a tooltip -- but the thing you aim at is a shape: a floppy disk, a
// folder, a curved arrow, a sine wave. A shape is read before a word is, it
// reads the same in every language, and it stays readable at a size where a
// label has to be abbreviated into nonsense.
//
// Same technique as the main toolbar's icons (icon:: in main.cpp): drawn as
// vectors into the window's draw list on top of a blank button, so they scale
// with the UI font and follow the theme's text colour, and there is no image
// atlas to load or keep in step.
namespace picto {

enum class Icon {
    // files and history
    New, Open, Save, Undo, Redo, Tidy,
    // sound
    Play, Stop, Speaker, Keys, Lock, LockOpen,
    // editing
    AddDial, Trash,
    // oscillator waves
    WaveSine, WaveSaw, WavePulse, WaveNoise,
    // filter responses
    LowPass, HighPass, BandPass,
    // distortion curves
    ShapeSoft, ShapeHard, ShapeAtan, ShapeFold,
    // modelling: what is picked
    ModeVertex, ModeEdge, ModeFace, SelectAdd, SelectAll, SelectNone, MakeEditable,
    // modelling: what is done to it
    Extrude, MoveNormal, Inset, ScaleFace, LoopCutH, LoopCutV, Subdivide,
    Merge, SplitEdge, Collapse, Dissolve, Pencil,
    Bevel, MakeFace, FillHole, Connect, Flip, Weld, Grow,
    Spin, Duplicate, DuplicatePath, AlignPath,
    // modelling: what a spin turns about
    PivotOrigin, PivotSelection, PivotCursor,
    // directions
    Nudge, ArrowLeft, ArrowRight, ArrowUp, ArrowDown, ArrowIn, ArrowOut,
};

// Paint `icon` centred on `c`, fitting a circle of radius `r`.
void draw(ImDrawList* dl, Icon icon, ImVec2 c, float r, ImU32 col);

// A square button of `size` (0 = the editor's standard, about two text lines
// tall -- a target to aim at, not to hit precisely). `active` puts it on the
// accent, for the chosen one of a group. Returns the click.
bool button(const char* id, Icon icon, const char* tip, bool enabled = true,
            bool active = false, float size = 0.0f);

// The same, at any size -- for a button that has to line up with a column
// wider than it is tall. The picture is centred and sized to the shorter side.
bool buttonSized(const char* id, Icon icon, const char* tip, bool enabled, bool active,
                 ImVec2 size);

// The standard size, for laying a row out around them.
float size();

} // namespace picto
