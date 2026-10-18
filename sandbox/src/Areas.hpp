#pragma once

#include <string>
#include <vector>

// The editor's screen as areas, Blender's way (AreaTree.hpp for the tree).
// Editor only.
//
// Every area carries a thin header: a drop-down that picks the editor it shows,
// and three buttons -- cut it side by side, cut it top/bottom, give it back to
// its neighbour. The strips between areas are dragged to resize them. A 3D view
// cut in two gets a second 3D view in the new half (while there is one left).
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
    // A 3D view. Splitting an area that shows one fills the new half with the
    // next 3D view no area shows yet, rather than leaving it empty.
    bool        view3d = false;
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

// Is the editor whose window is `title` on screen: shown by an area, and that
// area not behind another one blown up to the whole screen?
bool showing(const std::string& title);

// The area showing `instead` shows `title` from the next frame on (and stays
// the maximized one, if it was); an area that showed `title` takes `instead`.
// False when no area shows `instead`. `insteadKept` says whether `instead`
// still has an area -- if not, the caller closes its window (else it would come
// back floating). Play uses it to run where the user is looking (startPlay).
bool showInstead(const std::string& title, const std::string& instead, bool* insteadKept = nullptr);

} // namespace areas
