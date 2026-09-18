#pragma once

#include <functional>

#include <imgui.h>

#include "ModelingPanel.hpp"
#include "Pictogram.hpp"

// The pieces the Modeling panel is laid out from, shared with the files that
// draw a section of it (ModelingSweep.cpp). Not for anything else: the sizes
// and the column shape are this panel's own conventions.
namespace modelui::parts {

// Sizes follow the UI font, never fixed pixels (see ModelingPanel.cpp).
float em();
float btnH();
float smallH();
void  gap();

// One operation: a picture of what it does and, under it, the amount it
// applies (null for none). Hovering it shows `previewOp` as a ghost on the
// mesh; `badge` is a small number in the button's corner.
bool opColumn(const char* id, picto::Icon icon, bool enabled, const char* tip,
              const modeltools::Op& previewOp, float* amount, float step, float lo, float hi,
              const char* fmt, const char* badge = nullptr);

// A setting that belongs to the operation beside it rather than being one: a
// caption where the button would be, and the stepper under it, so it lines up
// with the operation columns.
void captionColumn(const char* id, const char* caption, const char* tip, float& value,
                   float step, float lo, float hi, const char* fmt);

// The Spin, Duplicate and Duplicate-along-a-path rows. Clicks are handed back
// through `pending`, which the panel runs after its last widget.
void sweepTools(const PanelState& s, std::function<void()>& pending);

} // namespace modelui::parts
