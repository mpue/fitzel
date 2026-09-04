#pragma once

#include <functional>
#include <vector>

#include "AnimGraph.hpp"
#include "AnimSystem.hpp"

struct Entity;
class Selection;

// The editor's "Animation graph" panel: the state machine, drawn.
//
// WHY IT IS A CANVAS AND NOT A TABLE. A graph IS a picture -- which state leads
// where is the whole content, and a list of "from / to / conditions" rows makes
// the reader rebuild that picture in their head every time they look. The one
// thing a node editor is for is seeing the shape.
//
// AND WHY EVERY GESTURE HAS A BUTTON BESIDE IT. A node editor is otherwise the
// most drag-dependent instrument in any tool: drag the node, drag the link from
// this small port to that small port, drag the canvas, drag to zoom. This editor
// cannot require any of that. So: nodes snap to a grid when dragged and can be
// laid out by a button instead; an arrow can be drawn by dragging from a port OR
// made from two combo boxes; the canvas pans with buttons and centres itself on
// demand. Dragging is offered everywhere and required nowhere.
namespace graphui {

struct PanelState {
    bool&                          show;
    std::vector<animgraph::Graph>& graphs;
    int&                           editGraph;
    // The scene's clips, for a state's clip picker -- a state is a clip plus how
    // to play it, so choosing one is the main thing a node says.
    const std::vector<anim::Clip>& clips;
    // The scene, for the live half: while the game runs, the node the selected
    // object is actually IN is lit, and its parameters can be fired by hand.
    // Watching the machine move is how you find out the graph is wrong.
    std::vector<Entity>&           entities;
    const Selection&               sel;
    bool                           playing = false;

    std::function<void()> markDirty;
};

void drawPanel(const PanelState& s);

} // namespace graphui
