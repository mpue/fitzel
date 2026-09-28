#pragma once

#include <imgui.h>
#include <ImGuizmo.h>

#include "SceneTypes.hpp"

// The toolbar strip's icons: every button in the strip paints its own picture
// into the window's draw list (see ToolbarIcons.cpp for why). Editor only.
namespace icon {

// This tool/shape is active: the theme's accent, read back from the style so the
// strip follows fitzel::Gui -- the bright cut (CheckMark), since a 2 px line needs
// more luminance than a filled button to read as the same colour.
ImU32 on();
constexpr ImU32 kOff = IM_COL32(215, 215, 220, 255);
constexpr ImU32 kDim = IM_COL32(130, 132, 140, 255);  // offered but not available

// The pictures: draw list, centre, radius, colour.
void shape(ImDrawList* dl, EntityType t, ImVec2 c, float r, ImU32 col);
void pointer(ImDrawList* dl, bool create, ImVec2 c, float r, ImU32 col);
void terrain(ImDrawList* dl, ImVec2 c, float r, ImU32 col);
void gizmo(ImDrawList* dl, ImGuizmo::OPERATION op, ImVec2 c, float r, ImU32 col);
void gizmoSpace(ImDrawList* dl, bool local, ImVec2 c, float r, ImU32 col);
void road(ImDrawList* dl, ImVec2 c, float r, ImU32 col);
void shade(ImDrawList* dl, int mode, ImVec2 c, float r, ImU32 col);

// One button in the strip: a blank fixed-size button with a tooltip, whose
// picture the caller paints afterwards at `center` -- afterwards, so it lands on
// top of the button rather than under it. A disabled button still draws itself:
// greyed out is a state worth showing, missing is not. An `active` button (the
// current tool, shape, mode) sits on a wash of the accent, so what is on reads
// from the button's shape and not only from the thin lines of its picture.
bool button(const char* id, ImVec2 size, const char* tip, bool disabled,
            ImVec2& center, bool active = false);

} // namespace icon
