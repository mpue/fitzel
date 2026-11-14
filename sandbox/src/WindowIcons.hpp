#pragma once

#include <imgui.h>

// A picture for every tool window, in front of its name: on its title bar and
// dock tab (through the WindowIconHook our ImGui patch adds), in the View menu
// and in an area's editor menu. Painted from lines like the toolbar strip's
// icons (ToolbarIcons.hpp), whose pictures the windows it opens share. Editor
// only.
namespace winicon {

// Puts the hook into the current ImGui context. Once, after it exists.
void install();

// Paints the picture of the window called `name` -- its ImGui::Begin title (the
// part shown, or else the ID after "###") or its menu label -- in the square of
// radius `r` round `c`. False, and nothing painted, when it has none.
bool draw(ImDrawList* dl, const char* name, ImVec2 c, float r, ImU32 col);
bool has(const char* name);

// ImGui::MenuItem with the window's picture in the menu's icon column. Toggles
// `*flag` when clicked, as MenuItem(label, shortcut, flag) does.
bool menuItem(const char* label, const char* shortcut, bool* flag, bool enabled = true);
bool menuItem(const char* label, const char* shortcut = nullptr, bool selected = false,
              bool enabled = true);

// ImGui::Selectable with the picture of `window` in front of `label`.
bool selectable(const char* label, const char* window, bool selected);

// The picture of `window` and `label` as a combo's preview: call right after
// the BeginCombo(..., ImGuiComboFlags_CustomPreview) / EndCombo block.
void comboPreview(const char* label, const char* window);

} // namespace winicon
