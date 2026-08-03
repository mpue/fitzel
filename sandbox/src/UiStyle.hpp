#pragma once

#include <imgui.h>

// Small presentation helpers shared by every editor panel. The theme itself
// lives in the engine (fitzel::Gui), which owns the colours, metrics and fonts;
// this is only the typographic layer on top of it -- the handful of places where
// a panel says "this is a heading" or "this is a footnote" rather than plain
// body text. Keeping it here (and not in each panel) means the editor's voice is
// set in one file.
namespace ui {

// Hand the semibold UI font to the helpers below. Call once at startup with
// fitzel::Gui::boldFont(); passing null is fine and simply keeps everything at
// the regular weight.
void setBoldFont(ImFont* bold);

// A collapsing section heading, drawn semibold. Drop-in replacement for
// ImGui::CollapsingHeader(label, flags) -- returns true when open.
bool header(const char* label, ImGuiTreeNodeFlags flags = 0);

// An inline section rule with a semibold caption, for grouping controls inside
// an already-open panel. Drop-in replacement for ImGui::SeparatorText.
void sectionText(const char* label);

// A plain semibold caption with no rule -- for titling a boxed group such as an
// Inspector component card, where a full-width separator line would be redundant
// with the box's own border.
void title(const char* fmt, ...) IM_FMTARGS(1);

// Secondary text: dimmed and a touch smaller, for hints, units and counts that
// should be readable without competing with the controls they describe.
void hint(const char* fmt, ...) IM_FMTARGS(1);

} // namespace ui
