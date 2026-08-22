#pragma once

#include <cstddef>

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

// Case-insensitive substring test, the one every list filter in the editor uses.
// An empty (or null) needle matches everything, so a search box nobody has typed
// in hides nothing.
bool icontains(const char* hay, const char* needle);

// A full-width search field for filtering the list that follows. `buf` is the
// caller's filter text (it keeps it between frames); `cap` its size. Returns
// true while a filter is actually in force, so the caller can say what it hid.
// Clearing it is one click on the x, which matters more here than saving the
// widget: retyping a name to get the full list back is exactly the kind of
// fiddly the editor tries not to ask for.
bool searchBox(const char* id, char* buf, std::size_t cap,
               const char* placeholder = "Search...");

} // namespace ui
