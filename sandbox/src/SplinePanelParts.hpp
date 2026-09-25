#pragma once

#include "SplinePanel.hpp"
#include "SplineGen.hpp"

// The Splines panel's building blocks, shared with the kind sections that live
// in their own files (BridgePanel.cpp). Editor-only, like the panel.
namespace splineui {

// A slider that brackets itself into one undo step and marks path `path` for
// regeneration. Every control in the panel goes through one of these.
bool slider(const PanelState& s, int path, const char* label, float* v,
            float lo, float hi, const char* fmt = "%.2f m");
bool sliderInt(const PanelState& s, int path, const char* label, int* v, int lo, int hi);

// The Shape section of a bridge (BridgePanel.cpp).
void bridgeStyle(const PanelState& s, int path, splinegen::Style& st);

} // namespace splineui
