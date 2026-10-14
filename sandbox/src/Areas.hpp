#pragma once

#include <vector>

// The editor's screen as areas, Blender's way (AreaTree.hpp for the tree).
// Editor only.
//
// Every area carries a thin header: a drop-down that picks the editor it shows,
// and three buttons -- cut it side by side, cut it top/bottom, give it back to
// its neighbour. The strips between areas are dragged to resize them.
//
// The editors themselves are the ordinary ImGui windows they always were:
// nothing in a panel knows about areas. Each area holds a one-window dock space
// with no tab bar, and the window its header names is docked into it by title.
// A panel's own show flag still says whether it is open -- an area turns its
// editor's flag on, and a flag turned off (the View menu, "Close all panels")
// empties the area that showed it.
namespace areas {

// One editor an area can show.
struct Editor {
    const char* group;    // the heading it is listed under
    const char* label;    // what the menu calls it
    const char* window;   // its ImGui::Begin title, exactly
    // Its show flag; nullptr for the windows drawn every frame (the viewport,
    // Hierarchy, Inspector). Those are hidden while no area shows them.
    bool*       show;
};

// Hook the layout into imgui.ini. Once, after the ImGui context exists and
// before its first NewFrame (which is when the file is read).
void install();

// Back to the default arrangement on the next frame().
void requestReset();

// Every frame, after the menu bar and toolbar (they take their strips off the
// screen first) and before any editor's window: lays the areas out, draws
// their headers and dividers, and puts each editor's window where it belongs.
void frame(const std::vector<Editor>& editors);

} // namespace areas
