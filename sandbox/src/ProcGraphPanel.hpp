#pragma once

#include <functional>
#include <memory>
#include <string>

#include <glm/glm.hpp>
#include <imgui.h>

#include "ProcGraph.hpp"
#include "SceneTypes.hpp"   // Entity

struct EditorContext;
struct ViewportFrame;

// The editor's "Procedural" window (View > Objects): the node graph of the
// selected procedural object, drawn, and the settings of the node picked in it.
// Every change cooks the graph into the object's mesh at once (see
// ProcGraph.hpp) and is one undo step.
//
// The wiring is the author's: a new node stands on its own until it is wired
// (only the first node of an empty graph becomes the output by itself). The
// canvas lays the graph out by itself until a node is moved by hand; from then
// on every node stays where it is, and "Arrange" lays it all out afresh.
//
// Dragging is offered and never required (see parkinson-tauglicher Editor):
//   - a wire: drag from a dot to its partner -- or click the one, then the
//     other (an output dot, then the node or input dot it should feed, or the
//     other way round); every wire can also be set from a list in the
//     settings. Pulling an input's wire off onto nothing takes it out.
//   - a node: drag it (it snaps to the grid when let go) -- or press "Move"
//     and click where it should go;
//   - a number: a stepper, whose middle can be clicked to type a value.
//
// Nodes are picked like objects: a click takes one, Shift+click adds or takes
// one away, a drag on the empty canvas draws a box round several (A takes all,
// Alt+A none); dragging one of them moves them all. The Delete key (or X)
// removes the picked nodes -- while this window has the pointer or the
// keyboard, that key is its own and never removes an object of the scene.
// Shift+A (or a right-click on the empty canvas) opens the Add menu at the
// pointer; a right-click on a node, that node's menu. The middle button pans.
namespace procui {

class Panel {
public:
    struct Deps {
        EditorContext& ed;
        // Where a new object goes: a point about `dist` metres ahead, on the
        // ground (main's spawnPoint -- the 3D cursor when it is shown).
        std::function<glm::vec3(float)> spawnPoint;
        // The 3D cursor, when it is shown: where "Add point at the 3D cursor"
        // puts a curve's next point. False when there is none.
        std::function<bool(glm::vec3&)> cursor = {};
    };
    explicit Panel(Deps d);

    void draw(bool& show);
    // Into the Scene window, after the scene: the picked node's curves and
    // points over the object -- the selected points lit -- while this window
    // is open. Curves are never drawn by the renderer; this is where they are
    // seen.
    void viewport(const ViewportFrame& view);

    // A new procedural object made from preset `i` (procpreset::list()),
    // selected, as one undo step. Returns its id. The window's tiles call this;
    // so does the harness.
    int create(int preset);
    // Cook the graph of object `id` into its mesh -- what every change ends
    // with. Not an undo step by itself.
    void cookInto(Entity& e);
    // Show node `id` of the selected object's graph (its settings, its rim lit),
    // the canvas scrolled to it -- the one picked node.
    void pickNode(int id) {
        m_node = id;
        m_sel.clear();
        if (id >= 0) m_sel.push_back(id);
        m_scrollTo = true;
    }
    // The picked nodes (the one whose settings show is among them).
    const std::vector<int>& pickedNodes() const { return m_sel; }
    // Does this window own the keyboard this frame -- the pointer over it, or
    // it in focus? Then the Delete key is its own: main must leave the scene be.
    bool ownsKeys() const { return m_keys; }

    // The project's prefabs, for the Prefab node: their names (the picker),
    // and a fresh copy of one as scene entities, root first, ids taken from
    // `counter` (main: findPrefab + prefab::instantiate). Set after
    // construction, because main's prefab cache is made later than this panel.
    std::function<std::vector<std::string>()>                                 prefabNames;
    std::function<std::vector<Entity>(const std::string& name, int& counter)> spawnPrefab;

    // For the harness (procpanelcheck): told where each control it may click
    // was drawn this frame, by name -- "add", "add:tube", "node:hub", "out:hub",
    // "in:merge1:0", "radius+", "size.x=" ... Unset in the editor.
    std::function<void(const std::string&, ImVec2, ImVec2)> probe;

private:
    Entity* target();
    // Every change to a graph goes through here: the object as it was is kept
    // for undo (once per interaction), `fn` changes the graph, and it cooks --
    // unless `cook` is false, for what does not change the result (moving a
    // node on the canvas).
    void change(Entity& e, const std::function<void(proc::Graph&)>& fn, bool cook = true);
    void commit();

    void toolbar(Entity& e, ProcGraphComponent& pg);
    void canvas(Entity& e, ProcGraphComponent& pg);
    void settings(Entity& e, ProcGraphComponent& pg);
    void newObjectTiles();
    void addNode(Entity& e, const std::string& kind);
    // Take the picked nodes out, as one undo step.
    void removePicked(Entity& e);
    // Put the prefabs the last cook of object `owner` asked for into the scene
    // (see ProcMadeComponent), unless the ones there already are those.
    void placePrefabs(int owner);
    // The objects a graph placed under `owner`, whole subtrees, in scene order.
    std::vector<Entity> madeUnder(int owner) const;
    bool drawProps(proc::Node& n);
    void refreshInfo(const Entity& e, const ProcGraphComponent& pg);

    Deps m_d;
    int  m_node = -1;          // the node whose settings are shown
    int  m_nodeOf = -1;        // ...in this object's graph
    // The interaction being edited: whose, and how it was before it -- the
    // object and the prefabs its graph had placed under it.
    int    m_editId = -1;
    Entity m_before;
    std::vector<Entity> m_madeBefore;
    // What the last cook asked to be placed, for placePrefabs at the end of
    // the frame (owner -1: nothing waiting).
    int                         m_placeFor = -1;
    std::vector<proc::Instance> m_placeList;
    glm::vec3                   m_placePivot{0.0f};
    // What the last cook said about each node (faces, errors), for the graph
    // it was made from.
    proc::CookInfo m_info;
    int            m_infoFor = -1;
    std::size_t    m_infoHash = 0;
    // A wire being made by two clicks: from an output (node id), or into an
    // input (node id + slot). -1 when none.
    int m_wireFrom = -1;
    int m_wireTo = -1, m_wireToSlot = 0;
    char m_name[64] = {};
    int  m_nameFor = -1;
    std::string m_note;        // the last refusal ("that would make a loop")
    // A preset asked for this frame, made once the drawing is done: a new
    // object grows the scene's entity list, and the object being drawn lives
    // in that list.
    int  m_pendingCreate = -1;
    // The canvas: its zoom (the node text size, relative to the editor's), a
    // request to fit the whole graph in, and one to bring the picked node into
    // view -- set by everything that picks a node somewhere other than on the
    // canvas itself (opening an object, Add, Delete, a list).
    float m_zoom = 1.0f;
    bool  m_fit = false;
    bool  m_scrollTo = true;
    // The picked nodes, and whether the keyboard is this window's.
    std::vector<int> m_sel;
    bool             m_keys = false;
    // A node being dragged: which, where the picked nodes were when the press
    // began (in text heights), and whether it has moved yet (a press that never
    // moved is a click). A wire being dragged out of an output dot, or into an
    // input dot. A box being drawn round nodes: where it started (canvas units).
    int       m_dragNode = -1;
    std::vector<std::pair<int, glm::vec2>> m_dragFrom;
    bool      m_dragMoved = false;
    bool      m_boxing = false;
    glm::vec2 m_boxFrom{0.0f};
    // The Add menu at the pointer: where the new node goes (text heights) and
    // what is typed into its search field.
    glm::vec2 m_addAt{0.0f};
    char      m_addFilter[48] = {};
    bool      m_addFocus = false;
    int       m_menuNode = -1;   // the node a right-click opened its menu on
    int       m_wireDragFrom = -1;
    int       m_wireDragTo = -1, m_wireDragToSlot = 0;
    // "Move" pressed for this node: the next click on the canvas puts it there.
    int       m_moveArmed = -1;
    // Was the window open this frame (the overlay shows only then), and the
    // object whose settings are being drawn (a Curve's "at the 3D cursor").
    bool      m_open = false;
    Entity*   m_drawing = nullptr;
    // What the overlay draws: the picked node's geometry, cooked again only
    // when the graph or the pick changed.
    proc::Geo   m_overlay;
    std::size_t m_overlayHash = 0;
    int         m_overlayNode = -1, m_overlayFor = -1;
    char m_matFilter[64] = {};
};

} // namespace procui