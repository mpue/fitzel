#pragma once

#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "Behaviour.hpp"
#include "ScriptSystem.hpp"

struct EditorContext;

// The editor's "Behaviour" window (View > Presentation): the behaviour graph of
// the selected object, drawn -- events on the left, what they set off to the
// right, the values they need wired in from below (see Behaviour.hpp).
//
// Dragging is offered and never required (the editor's rule):
//   - a wire: drag from a pin to a pin -- or click the one, then the other. A
//     wire dragged out onto empty canvas opens the node menu with only the
//     nodes that fit, and the new node arrives wired. Pulling a wire off an
//     input onto nothing takes it out; a right-click on a pin takes its wires
//     out. Every wire can also be made from a list in the node's settings.
//   - a node: drag it (it snaps when let go), or "Lay out" places them all.
//     A node comes from the menu (right-click on the canvas, or "Add node"),
//     or from the list on the left with one click.
//   - a value: typed or stepped in the node's settings below the canvas.
// The Delete key removes the picked node while this window has the pointer or
// the keyboard. With nothing selected the window stays on the object it showed
// last -- Play clears the selection, and that is when the graph is watched.
//
// While the game runs the graph is a readout: each node lights up as it runs
// and the variables show their values. It cannot be edited then -- what runs
// was compiled when Play started, and an edit to a world Play throws away
// afterwards would be lost.
//
// Every change is one undo step, banked when the mouse is let go (a drag is
// one step, not one per frame).
namespace behaviourui {

class Panel {
public:
    struct Deps {
        EditorContext& ed;
        std::function<bool()> playing;
        // The running behaviour of an object (ScriptSystem::live) -- for the
        // lit nodes and the variables while the game runs.
        std::function<bool(int id, ScriptSystem::Live&)> live;
        // The last script error, which is where a behaviour that stopped says why.
        std::function<std::string()> lastError;
    };
    explicit Panel(Deps d);

    void draw(bool& show);
    // Pick the first node of `type` on the shown object's graph, as a click on it
    // would (FITZEL_BEHAVIOUR's pick=, for a screenshot of its settings).
    void pick(const std::string& type);
    // While true, the keyboard is this window's: main leaves the Delete key be.
    bool ownsKeys() const { return m_keys; }

private:
    struct PinRef {
        int  node  = -1;
        int  pin   = -1;
        bool input = false;
        bool valid() const { return node >= 0; }
    };

    Entity* target();
    void    commit();
    void    changed() { m_changed = true; }

    void toolbar(Entity& e, BehaviourComponent& bc);
    void sidebar(behaviour::Graph& g);
    void canvas(behaviour::Graph& g, const ScriptSystem::Live* live);
    void settings(behaviour::Graph& g, const ScriptSystem::Live* live);
    void addMenu(behaviour::Graph& g);
    void luaWindow(const Entity& e, const behaviour::Graph& g);
    void startTiles(Entity& e);

    int  addNode(behaviour::Graph& g, const std::string& type, glm::vec2 at);
    bool tryConnect(behaviour::Graph& g, PinRef a, PinRef b);

    Deps m_d;
    bool m_keys     = false;
    bool m_showLua  = false;

    // The object the window shows and the undo step being collected for it.
    int              m_of = -1;
    bool             m_changed = false;   // something changed this frame
    bool             m_pending = false;   // ...and it has not been banked yet
    behaviour::Graph m_before;            // the graph as it was before the pending step

    // The canvas.
    glm::vec2 m_pan{40.0f, 40.0f};
    float     m_zoom = 1.0f;
    int       m_sel  = -1;                // picked node id
    PinRef    m_link;                     // a wire being drawn: where it started
    bool      m_linkClick = false;        // ...by a click, waiting for the second click
    PinRef    m_pulled;                   // an input whose wire is being pulled off
    bool      m_bgDragged = false;
    bool      m_fit = true;               // frame the whole graph on the next draw
    bool      m_openMenu = false;         // open the node menu in the canvas this frame
    glm::vec2 m_viewCentre{0.0f};         // the middle of the canvas, graph units
    glm::vec2 m_menuAt{0.0f};             // where the node menu puts its node (graph units)
    PinRef    m_menuLink;                 // the wire the menu's node is to take, if any
    char      m_menuFilter[64] = {};
    char      m_paletteFilter[64] = {};
    std::string m_message;                // the last refusal ("these pins do not fit")
    float       m_messageAt = -1.0f;
};

} // namespace behaviourui
