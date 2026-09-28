#pragma once

// Which tool has the left mouse button in the scene viewport.
//
// One value, so two tools can never both have it. It used to be ten separate
// flags, and every tool that switched itself on had to switch the others off
// from its own list -- fifteen lists in main and in five panels, each missing
// a different sibling. A tool left off one of them got the same click as the
// tool that was on (road points used to drop a new object under every waypoint
// placed in Create mode). Now a panel knows only its own on/off, and main
// writes that back here with takeTool().
enum class ViewTool {
    None,
    Grass, Trees, Flowers, Scatter,   // vegetation and object brushes
    Sculpt, Paint,                     // the terrain's shape and its texture layers
    MeshPaint,                         // texture layers on a modelled object
    Road, Spline, River,               // the path editors' handles
};

// One tool's on/off as its panel left it, written back into the one value:
// switched on, it takes the button from whichever tool had it; switched off, it
// lets go of it only if it was the one holding it.
inline void takeTool(ViewTool& tool, ViewTool mine, bool on) {
    if (on) tool = mine;
    else if (tool == mine) tool = ViewTool::None;
}
