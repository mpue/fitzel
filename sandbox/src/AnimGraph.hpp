#pragma once

#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

// An animation state machine: which clip an object is playing, and what makes it
// play another one.
//
// WHY A GRAPH AND NOT A LIST OF RULES. A door that opens, waits, closes and can
// be interrupted half-way is four states and six ways between them, and written
// as conditions in a script it becomes a pile of booleans nobody can read back.
// Drawn as states with arrows it is the picture the author already has in their
// head. That is the whole argument for this file: the behaviour was always
// expressible, it was just never legible.
//
// WHAT IT DELIBERATELY IS NOT. There is no blending between states and no layers
// -- a transition cuts. Blending needs a second clip evaluated alongside the
// first and a weight, and every one of those decisions is easier to make once
// there are real graphs to look at. Cutting is honest and it is what a door, a
// lift, a lamp and a signal all want. Sub-graphs, mirroring and IK are not here
// either, and this comment is the record that they were left out rather than
// forgotten.
namespace animgraph {

// A knob the graph reacts to. The VALUE does not live here -- see Instance --
// because two doors share one graph and must not share their state.
struct Param {
    enum class Type { Trigger, Bool, Number };
    std::string name;
    Type        type = Type::Trigger;
    float       def  = 0.0f;   // starting value (a Trigger always starts unset)
};

// One test on one parameter. A transition holds several and they must ALL hold:
// AND is what an author means by listing conditions, and OR is spelled by
// drawing a second arrow -- which is also the version you can see.
struct Condition {
    enum class Op { IsTrue, IsFalse, Greater, Less, Equals, Fired };
    std::string param;
    Op          op    = Op::Fired;
    float       value = 0.0f;   // compared against, for the number ops
};

// A state: the clip the object plays while it is in it.
struct State {
    std::string name = "State";
    std::string clip;                 // an anim::Clip name ("" = hold still)
    bool        loop  = true;
    float       speed = 1.0f;
    glm::vec2   pos{0.0f};            // where its node sits on the editor canvas
};

// An arrow. `from` is a state index, or kAnyState for one that can be taken from
// wherever the machine happens to be -- the "hit" or "die" case, which otherwise
// needs an arrow out of every state and is unreadable the moment there are five.
struct Transition {
    static constexpr int kAnyState = -1;
    int  from = 0;
    int  to   = 0;
    std::vector<Condition> conditions;
    // Wait until the state's clip has played this far before allowing it (1.0 =
    // to the end). Without it a "walk then stop" arrow fires on the first frame
    // the parameter says so, and the walk never finishes a step.
    bool  hasExitTime = false;
    float exitTime    = 1.0f;   // fraction of the clip's length
};

struct Graph {
    std::string name = "Graph";
    std::vector<State>      states;
    std::vector<Transition> transitions;
    std::vector<Param>      params;
    int entry = 0;              // the state entered when the machine starts
};

// One object's run of a graph: where it is and what its parameters say. Held by
// the component, not by the graph, so two objects on the same graph are two
// independent machines.
struct Instance {
    int   state = -1;           // -1 = not started
    float time  = 0.0f;         // seconds spent in the current state
    std::vector<float> values;  // one per graph param; a Trigger is 0 or 1
    // The state the last step() moved into, for anyone who wants to react to a
    // change (a sound on the door starting to open). -1 when nothing changed.
    int   entered = -1;
};

int findState(const Graph& g, const std::string& name);   // -1 if absent
int findParam(const Graph& g, const std::string& name);   // -1 if absent
int findGraph(const std::vector<Graph>& gs, const std::string& name);

// --- Driving ----------------------------------------------------------------
// Put the machine in its entry state and its parameters at their defaults.
void start(const Graph& g, Instance& in);

// Set a parameter. `fire` is the trigger case: it stays set until a transition
// that tests it is taken, so a trigger sent from a script on one frame is not
// missed by a machine that steps on the next.
void setBool(const Graph& g, Instance& in, const std::string& param, bool v);
void setNumber(const Graph& g, Instance& in, const std::string& param, float v);
void fire(const Graph& g, Instance& in, const std::string& param);

// Advance by dt: take a transition if one is ready, then report what should be
// playing. `outClip` is the state's clip name and `outTime` where in it -- the
// caller looks the clip up and applies it, because the graph knows nothing about
// tracks or entities.
//
// The clip's LENGTH is needed to loop and to answer exit time, and only the
// caller has the clip library, so it comes in as `clipLength` (0 = unknown,
// which makes exit time pass immediately and looping a no-op).
void step(const Graph& g, Instance& in, float dt,
          float clipLength, std::string& outClip, float& outTime);

// Would this transition be taken right now? Used by step(), and by the editor to
// draw an arrow that is currently satisfied.
bool ready(const Graph& g, const Instance& in, const Transition& t,
           float clipLength);

// --- Persistence ------------------------------------------------------------
void save(nlohmann::json& j, const std::vector<Graph>& graphs);
void load(const nlohmann::json& j, std::vector<Graph>& graphs);

// Names for the editor's combos, and for reading a saved graph back.
const char* opName(Condition::Op op);
const char* paramTypeName(Param::Type t);

} // namespace animgraph
