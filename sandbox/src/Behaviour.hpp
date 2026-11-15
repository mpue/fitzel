#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include "Component.hpp"

// A behaviour graph: what an object DOES, drawn instead of written.
//
// WHY A GRAPH WHEN THERE IS LUA. Everything a behaviour does could be written
// as a script, and for a programmer that stays the better tool. But "when the
// player comes near, open, wait two seconds, close" is a sentence, and most of
// the people who build a scene think in sentences, not in `if` and `local`. A
// graph makes that sentence the program: an event on the left, the steps it
// sets off to the right, the numbers they need wired in from below. The graph
// IS legible where the same thing as code is not -- the argument the animation
// graph makes for states (AnimGraph.hpp), made for behaviour.
//
// HOW IT RUNS. Not interpreted: at Play the graph is COMPILED to Lua and runs
// through ScriptSystem like any script -- its own environment on the object,
// the same `game` API, an error reported once and only this behaviour stopped.
// That decides most of what follows. A node is a piece of Lua with holes in it
// (the wires), so a node can do exactly what a script can and nothing more,
// the exported game needs nothing new to run it, and "Show Lua" in the editor
// shows precisely what will run -- the way from a graph to a hand-written
// script when a behaviour outgrows the graph.
//
// THE WIRING, as in every event graph:
//   - EXEC wires (white) say what happens next. An event fires, its exec
//     output runs the node it is wired to, that node runs the next. An exec
//     output feeds ONE input (what comes next is one thing; "several" is a
//     Sequence node), an exec input can be reached from many.
//   - DATA wires (coloured by kind) bring a value in. A data input takes ONE
//     wire; unwired, it uses the value typed into the node. Data nodes (maths,
//     "position of", "distance") have no exec pins: they are worked out at the
//     moment the node that reads them runs.
//
// WHAT IT DELIBERATELY IS NOT. No functions or macros, no loops other than
// Repeat (a loop that waits is a Delay wired back), no arrays. A behaviour that
// needs them has outgrown the graph -- that is what "Show Lua" is for.
namespace behaviour {

// What travels along a pin. Key and Choice are SETTINGS, not pins: they are
// shown and typed on the node but never wired (a key to listen for is part of
// what the node is, not a value that changes). Var names a variable of the
// graph, also never wired.
enum class Kind { Exec, Number, Bool, Vector, Text, Entity, Any, Key, Choice, Var };

bool isSetting(Kind k);                      // Key, Choice, Var
bool canConnect(Kind from, Kind to);         // may an output of `from` feed an input of `to`?
const char* kindName(Kind k);

// A value typed into a node's unwired input (or a variable's start value).
// One struct for every kind, so a node's inputs are one vector and an input
// whose kind changes keeps what it can. Entity is by NAME: "" is the object the
// behaviour runs on, anything else the first object of that name -- a name
// survives a copy into another scene where an id would not.
struct Value {
    float       num = 0.0f;
    bool        b   = false;
    glm::vec3   vec{0.0f};
    std::string text;
    int         choice = 0;   // Key: index into keys(); Choice: into the pin's labels
};

struct PinDef {
    std::string name;
    Kind        kind = Kind::Exec;
    Value       def;
    std::vector<std::string> choices;   // Choice only: the labels
};

enum class Category { Event, Flow, Action, Data };
const char* categoryName(Category c);

class Gen;   // see Behaviour.cpp: the compiler's view of a graph
struct Node;

// One kind of node: its pins and the Lua it compiles to. The catalogue is
// fixed (catalogue()); a graph stores only the type name, so a saved graph
// keeps working as long as the type does.
struct NodeDef {
    std::string type;        // stable id, stored in the graph ("move_toward")
    std::string title;       // what the node says on the canvas
    Category    cat = Category::Action;
    std::string help;        // one sentence, for the palette and the node's tooltip
    std::vector<PinDef> ins, outs;
    // Exactly one of these is set, by category:
    //   Event  -- `event`: adds its hook to start() / update() via the Gen.
    //   Flow, Action -- `exec`: the Lua statements run when exec input `pin`
    //             is reached (they call on to the next node through the Gen).
    //   Data   -- `expr`: the Lua expression for data output `pin`.
    std::function<void(Gen&, const Node&)>                    event;
    std::function<std::string(Gen&, const Node&, int pin)>    exec;
    std::function<std::string(Gen&, const Node&, int pin)>    expr;
};

const std::vector<NodeDef>& catalogue();
const NodeDef* findDef(const std::string& type);   // nullptr for an unknown type

// The keys a Key setting can name: what the node shows, and the `game.KEY_*`
// constant it compiles to.
struct KeyName { const char* label; const char* lua; };
const std::vector<KeyName>& keys();

// --- The graph ----------------------------------------------------------------
struct Node {
    int         id = 0;              // unique in its graph, never reused
    std::string type;
    glm::vec2   pos{0.0f};           // top-left on the canvas, graph units
    std::vector<Value> values;       // one per input pin (exec slots unused)
};

// A wire from output `fromPin` of one node to input `toPin` of another (pin
// numbers index the def's `outs` and `ins`).
struct Link {
    int fromNode = 0, fromPin = 0;
    int toNode   = 0, toPin   = 0;
};

// A value the behaviour keeps between frames: a counter, a "door is open". Its
// kind is fixed at Number, Bool, Text or Vector.
struct Variable {
    std::string name;
    Kind        kind = Kind::Number;
    Value       def;
};

struct Graph {
    std::vector<Node>     nodes;
    std::vector<Link>     links;
    std::vector<Variable> vars;
    int nextId = 1;
};

// A node of `type` with its inputs at their defaults, given the next id.
Node makeNode(Graph& g, const std::string& type, glm::vec2 pos);
Node*       findNode(Graph& g, int id);
const Node* findNode(const Graph& g, int id);
int         findVar(const Graph& g, const std::string& name);   // -1 if absent

// The kind a pin carries on THIS node: an Any pin of a variable node takes the
// kind of the variable it names. Exec/data kinds otherwise come from the def.
Kind pinKind(const Graph& g, const Node& n, bool input, int pin);

// Wire `from` -> `to`, keeping the rules: kinds must fit, a data input or an
// exec OUTPUT loses the wire it had, and a data wire that would close a loop
// of data nodes (a value made of itself) is refused. False = refused, with
// the reason in `why` when given.
bool connect(Graph& g, const Link& l, std::string* why = nullptr);
// Take a node out, and every wire that touches it.
void removeNode(Graph& g, int id);
// Every wire into input `pin` of node `id` (or out of output `pin`).
void disconnect(Graph& g, int id, bool input, int pin);

// Lay the nodes out left to right by how far they are from an event: the way
// out of a knot, and the way to use the editor without dragging nodes at all.
void autoLayout(Graph& g);

// --- Compiling ------------------------------------------------------------------
struct Compiled {
    std::string lua;                       // the whole script
    std::vector<std::string> warnings;     // things that will not do what they look like
};
// `label` names the behaviour in the script's comments and log lines (the
// object's name).
Compiled compile(const Graph& g, const std::string& label);

// --- Persistence ----------------------------------------------------------------
void save(nlohmann::json& j, const Graph& g);
void load(const nlohmann::json& j, Graph& g);

// Ready-made graphs to start from -- also the shapes behaviourcheck runs.
struct Template { const char* name; const char* help; std::function<Graph()> make; };
const std::vector<Template>& templates();

} // namespace behaviour

// --- Component: Behaviour (a graph that runs on the object while playing) ------
// The graph lives ON the component, not in a scene list as the animation graph
// does: a behaviour is usually this one object's -- this door, this guard --
// and a graph that is part of the object travels with it for free, into a
// duplicate, a prefab, another scene, an undo step. Two objects that should
// behave alike get a copy ("Copy" / "Paste" in the editor, or a prefab).
//
// Registered from Behaviour.cpp rather than Component.cpp, so the many tools
// that link Component.cpp do not have to carry the compiler too.
class BehaviourComponent : public ComponentBase {
public:
    behaviour::Graph graph;

    std::unique_ptr<ComponentBase> clone() const override {
        return std::make_unique<BehaviourComponent>(*this);
    }
    const char* typeId() const override { return "behaviour"; }
    const char* displayName() const override { return "Behaviour"; }
    const std::vector<Property>& props() const override { return properties(); }
    static const std::vector<Property>& properties();
    void save(nlohmann::json& j) const override;
    void load(const nlohmann::json& j) override;
};
