#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "Component.hpp"   // ComponentBase, Property
#include "EditMesh.hpp"

// --- Procedural graphs: a small Houdini --------------------------------------
// An object described by a network of nodes instead of by its faces: a tube, a
// torus, copies of a spoke round a hub, every hull face given a panel, the caps
// dressed in another material. Each node takes the geometry of the nodes wired
// into it and makes new geometry; the node with the OUTPUT flag is what the
// object shows. Change a number anywhere and everything downstream is made
// again, which is the whole point -- a space station's ring is one radius,
// not two hundred faces to move by hand.
//
// What comes out is an ordinary modelled mesh (MeshComponent). The graph lives
// beside it on the same object (ProcGraphComponent) and is "cooked" into that
// mesh whenever it changes, so everything that already knows meshes -- the
// renderer, picking, physics, the modifier stack, prefabs, export -- knows a
// procedural object without hearing of graphs. The scene file keeps both: the
// game never cooks, and an object whose graph is removed simply keeps the mesh
// it had, to be modelled by hand from there.
//
// Open by construction, like the modifier stack: a node kind is a class with
// its settings as Property rows (fields and file keys for free) and a cook(),
// plus one line in the registrar at the bottom of ProcNodes.cpp. The graph, the
// file format and the editor know nothing about any particular kind.
namespace proc {

// --- What flows along a wire ----------------------------------------------------
// A line through points of its own: a circle's rim, a path, a profile. Curves
// are not drawn in the game -- they are what faces are made FROM (swept into a
// pipe, turned into a dome, filled, copied onto) -- and the editor draws them
// over the scene while the graph is open.
//
// A `loose` curve is no line at all but a set of points -- what Mesh to points
// makes of a surface: copied onto, selected and deleted like a curve's points,
// but never swept, turned or resampled, and drawn as dots. Its `nrm` keeps
// which way the surface faced at each point, so a copy can still stand on it.
struct Curve {
    std::vector<glm::vec3> pts;
    std::vector<char>      sel;   // per point, parallel to pts (see Geo::hasSel)
    std::vector<glm::vec3> nrm;   // per point, parallel to pts; empty = straight up
    bool                   closed = false;
    bool                   loose  = false;

    // Which way is up at point `i`: the surface it came from, else +Y.
    glm::vec3 normalAt(int i) const {
        return (i >= 0 && i < static_cast<int>(nrm.size())) ? nrm[static_cast<std::size_t>(i)]
                                                           : glm::vec3(0.0f, 1.0f, 0.0f);
    }
};

// The geometry a node makes: faces (the EditMesh -- the only part the object
// finally shows), curves, and which of the points are selected.
//
// The selection is Houdini's point group, one per stream: a Select points node
// makes it, the nodes after it act on it (copy onto the selected points, move
// only them, delete them, dress the faces between them), and while no
// selection exists every point counts as selected -- a step that reads the
// selection does the whole thing when nobody has narrowed it down. A step that
// rebuilds the faces (subdivide, thicken, lattice) renumbers their corners, so
// the faces' part of the selection is gone after it; the curves' part stays.
// A prefab placed by the graph: which one (by name, as the project's prefabs
// folder lists it) and where, in the graph's space. A graph does not hold
// prefabs -- it says where they go; the editor puts real objects there (see
// ProcMadeComponent), so their models, lights and scripts are all theirs.
struct Instance {
    std::string prefab;
    glm::mat4   xf{1.0f};
};

struct Geo {
    EditMesh              mesh;
    std::vector<char>     meshSel;   // per mesh corner, parallel to mesh.verts
    std::vector<Curve>    curves;
    std::vector<Instance> instances; // moved, copied and merged like the rest
    bool                  hasSel = false;

    // Point `i` of the faces / of curve `c`: selected (or no selection made).
    bool meshPicked(int i) const {
        return !hasSel || (i >= 0 && i < static_cast<int>(meshSel.size()) && meshSel[static_cast<std::size_t>(i)]);
    }
    static bool curvePicked(const Curve& c, int i, bool hasSel) {
        return !hasSel || (i >= 0 && i < static_cast<int>(c.sel.size()) && c.sel[static_cast<std::size_t>(i)]);
    }
    // Bring the selection up to the point counts (new points unselected).
    void syncSel();
    // Forget the selection of the faces' corners (after their renumbering).
    void dropMeshSel() { if (hasSel) meshSel.assign(mesh.verts.size(), 0); }
    std::size_t pointCount() const;
    std::size_t selectedCount() const;   // 0 while no selection exists
};

// One node of a graph.
class Node {
public:
    virtual ~Node() = default;
    virtual std::unique_ptr<Node> clone() const = 0;
    virtual const char* typeId() const = 0;        // stable id (the scene file)
    virtual const char* displayName() const = 0;   // the editor's name for the kind
    // Its settings: the fields the editor draws and the keys the file keeps.
    virtual const std::vector<Property>& props() const = 0;

    // How many inputs it shows. A shape has none, a transform one, "copy to
    // points" two. A variadic node (Merge) takes as many as are wired into it.
    virtual int  inputSlots() const { return 0; }
    virtual bool variadic() const { return false; }
    virtual const char* inputName(int slot) const { return slot == 0 ? "Input" : "Input 2"; }

    // Make `out` (empty on entry) from the inputs, one per slot, null where a
    // slot is not wired. Returns "" or what went wrong, in a few words for the
    // node's tooltip; a node that fails gives empty geometry and the rest of
    // the graph goes on with it.
    virtual std::string cook(const std::vector<const Geo*>& in, Geo& out) const = 0;

    int              id = 0;
    std::string      name;      // unique within its graph: "tube1", "ring"
    std::vector<int> inputs;    // node ids, one per slot; -1 = not wired
    // Houdini's bypass flag: the node passes its first input through untouched
    // (a shape gives nothing). The quickest way to see what a step does.
    bool             bypass = false;
    // Where the node sits on the editor's canvas, in text heights (so it keeps
    // its place at every zoom) -- once someone has placed it. Until then the
    // canvas lays the graph out by itself. Not part of what the node makes.
    glm::vec2        pos{0.0f};
    bool             placed = false;
};

// The boilerplate half of a kind: T only declares its settings and cook().
template <class T>
class NodeOf : public Node {
public:
    std::unique_ptr<Node> clone() const override {
        return std::make_unique<T>(static_cast<const T&>(*this));
    }
};

struct TypeInfo {
    std::string typeId;
    std::string displayName;
    std::string category;   // the Add menu's section: "Shapes", "Copies", ...
    std::string tip;        // what it does, a line or two for the Add menu
    std::function<std::unique_ptr<Node>()> make;
};
void registerType(TypeInfo info);
const std::vector<TypeInfo>& registry();
const TypeInfo* typeInfo(const std::string& typeId);
// A fresh node of this kind with its default settings (no id, no name), or null.
std::unique_ptr<Node> make(const std::string& typeId);

// A network of nodes. Nodes are kept in the order they were made; the wiring
// is by id, so that order means nothing to the result.
struct Graph {
    std::vector<std::unique_ptr<Node>> nodes;
    int output = -1;   // the node the object shows (-1: none)
    int nextId = 1;

    Graph() = default;
    Graph(const Graph& o) { *this = o; }
    Graph& operator=(const Graph& o);
    Graph(Graph&&) = default;
    Graph& operator=(Graph&&) = default;

    Node*       find(int id);
    const Node* find(int id) const;
    // A name nobody in the graph has yet: "tube" -> "tube1", "tube2", ...
    std::string uniqueName(const std::string& base) const;
    // Take `n` in, give it an id and (if it has none) a name. Returns it.
    Node& add(std::unique_ptr<Node> n);
    // Take node `id` out. Whatever it fed is fed by its first input instead, so
    // deleting a step out of a chain closes the chain; the output flag moves the
    // same way.
    void remove(int id);
    // Would wiring `source` into `consumer` make a loop (consumer upstream of
    // source, or the same node)?
    bool wouldCycle(int consumer, int source) const;
    // Wire `source` (-1: nothing) into slot `slot` of `consumer`. Refuses a loop.
    // A variadic node's slot equal to its input count appends.
    bool connect(int consumer, int slot, int source);
    // Ids of every node `id` depends on, itself included.
    std::vector<int> upstream(int id) const;
    // Has anyone placed a node by hand? Then the canvas keeps every node where
    // it is; otherwise it arranges them all.
    bool placedByHand() const;

    void save(nlohmann::json& j) const;
    void load(const nlohmann::json& j);
};

// What a cook found out about each node it ran, for the editor: the size of
// what each one made and anything that went wrong.
struct CookInfo {
    std::unordered_map<int, std::string> errors;   // node id -> message
    std::unordered_map<int, std::size_t> faces;    // node id -> faces it made
    std::unordered_map<int, std::size_t> corners;  // node id -> points it made (faces' and curves')
    std::unordered_map<int, std::size_t> curves;   // node id -> curves it made
    std::unordered_map<int, std::size_t> selected; // node id -> points selected (with a selection)
    std::unordered_map<int, std::size_t> prefabs;  // node id -> prefabs it places
    double ms = 0.0;
};

// Everything node `id` makes, cooking whatever it needs upstream once each.
// An unknown id, or a loop that a hand-edited file smuggled in, gives nothing
// (and an error in `info`).
Geo cookGeo(const Graph& g, int id, CookInfo* info = nullptr);
// ...and the faces of it: what an object cooked from the graph shows.
EditMesh cook(const Graph& g, int id, CookInfo* info = nullptr);
// The faces of `g` as an object shows them: corners no face uses dropped,
// parallel arrays nobody filled left empty.
EditMesh facesOf(const Geo& g);

// One number for everything that decides the result: the graph as the file
// keeps it, less where the nodes sit on the canvas -- moving a node is not a
// reason to cook. What the editor compares to know a cook is out of date.
std::size_t hashOf(const Graph& g);

// Make what node `id` makes part of the object (the editor's "Merge into
// output" button): wired into the output when that is a Merge, else a new
// Merge of the old output and it becomes the output. With no output yet, `id`
// simply becomes it.
void mergeIntoOutput(Graph& g, int id);

// --- Shared helpers for the node kinds and the editor -------------------------

// Which faces a node acts on. The same choice on every node that picks faces
// (extrude, panels, material, delete), so it reads the same everywhere. "Away
// from axis" is the outside of anything round the Y axis -- a ring's outer
// wall, a hub's skin -- and "toward axis" its inside.
// "Between selected points" is every face all of whose corners are selected:
// how a Select points node reaches the faces. Stored by index, so new choices
// are only ever appended.
enum class FaceSet {
    All = 0, Up, Down, Sides, PosX, NegX, PosZ, NegZ, AwayFromAxis, TowardAxis,
    SelectedPoints
};
const std::vector<std::string>& faceSetLabels();
// The faces of `g.mesh` in `set`, then a random `share` of them (0..1, by `seed`).
std::vector<int> pickFaces(const Geo& g, FaceSet set, float share, int seed);

// The normal at each corner of `m`: the faces round it, weighted by area.
// Straight up for a corner no face uses.
std::vector<glm::vec3> cornerNormals(const EditMesh& m);

// A Curve node's points as its file keeps them -- "x y z; x y z; ..." -- and back.
std::vector<glm::vec3> parsePoints(const std::string& s);
std::string formatPoints(const std::vector<glm::vec3>& pts);

// The die every random choice in a graph rolls: the same index and seed give
// the same number in [0, 1) on every cook, so a panel that stuck out stays out.
float random01(int index, int seed);

} // namespace proc

// The tag on a prefab the graph of its parent placed (a Prefab node). Every
// cook that changes the list of placed prefabs takes the tagged objects away
// and puts new ones down; `sig` says which list a tagged object came from, so
// a cook that changes only the faces leaves them alone. Untagged children are
// the author's and are never touched.
class ProcMadeComponent : public ComponentBase {
public:
    std::string sig;

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<ProcMadeComponent>(*this);
    }
    const char* typeId() const override { return "procmade"; }
    const char* displayName() const override { return "Placed by a graph"; }
    const std::vector<Property>& props() const override {
        static const std::vector<Property> none; return none;
    }
    void save(nlohmann::json& j) const override;
    void load(const nlohmann::json& j) override;
};

// The component that carries an object's graph. The cooked result is the
// object's MeshComponent; this keeps what made it.
class ProcGraphComponent : public ComponentBase {
public:
    proc::Graph graph;
    // Where the graph's origin sits in the object's mesh. A mesh is kept
    // centred on its object (normalizeMeshEntity), so a cook whose result
    // grew to one side moves the object by that much and shifts the mesh back;
    // remembering the shift is what lets the next cook put its geometry in the
    // same place instead of creeping.
    glm::vec3   pivot{0.0f};

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<ProcGraphComponent>(*this);
    }
    const char* typeId() const override { return "procgraph"; }
    const char* displayName() const override { return "Procedural"; }
    // Bespoke editor (the Procedural window); the graph is not a property list.
    const std::vector<Property>& props() const override {
        static const std::vector<Property> none; return none;
    }
    void save(nlohmann::json& j) const override;
    void load(const nlohmann::json& j) override;
};