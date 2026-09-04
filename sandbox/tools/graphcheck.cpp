// graphcheck -- does the animation state machine go where its arrows say?
//
// An FSM fails quietly by definition: it sits in a state. A trigger that is
// never cleared sends the machine racing through every arrow that tests it; one
// that is cleared too early is a button press the game drops. An exit time
// measured against the wrong length holds a door shut forever, and an arrow with
// no conditions at all makes its own source state unreachable. None of that
// throws, none of it draws anything red, and all of it looks -- in the editor --
// like an author who has not finished drawing the graph.
//
// So the machine is played out here against graphs built in code: triggers fired
// and consumed, an Any-State arrow beating an ordinary one, exit time waiting for
// the clip and then letting go, ordering deciding between two ready arrows, and
// a graph saved and read back behaving identically.
//
// No GL, no window, no scene: this is a machine over clip NAMES, which is
// exactly why it can be measured without one.
//
//   build/release/bin/graphcheck.exe
// Exits non-zero if any check fails.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../src/AnimGraph.hpp"

namespace {

int g_fails = 0;

void fail(const char* what, const std::string& detail) {
    std::printf("[FAIL] %s: %s\n", what, detail.c_str());
    ++g_fails;
}
void pass(const char* what, const std::string& detail) {
    std::printf("  ok   %s -- %s\n", what, detail.c_str());
}
void check(bool ok, const char* what, const std::string& detail) {
    if (ok) pass(what, detail); else fail(what, detail);
}
void near(float got, float want, const char* what, float eps = 1e-3f) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "got %.4f, want %.4f", got, want);
    check(std::fabs(got - want) <= eps, what, buf);
}

using namespace animgraph;

// The state the machine is in, by name -- what every check below actually asks.
std::string where(const Graph& g, const Instance& in) {
    if (in.state < 0 || in.state >= static_cast<int>(g.states.size())) return "(none)";
    return g.states[static_cast<std::size_t>(in.state)].name;
}

// One frame. The caller says how long the current state's clip is, the way main
// does from the clip library.
std::string tick(const Graph& g, Instance& in, float dt, float len = 1.0f) {
    std::string clip;
    float t = 0.0f;
    step(g, in, dt, len, clip, t);
    return where(g, in);
}

// Step once and report where that left the machine.
//
// It exists because the obvious spelling is subtly wrong:
//     check(tick(...) == "Opening", "...", where(g, in));
// C++ does not order those two arguments, so `where` is free to run BEFORE the
// tick and the line prints the state the machine was in a moment ago. The
// ASSERTION was still right, which is the dangerous part: every one of these
// passed while reporting the wrong evidence, and a harness whose evidence lies
// is worse than one that says nothing.
void steps(const Graph& g, Instance& in, float dt, float len,
           const char* want, const char* what) {
    const std::string got = tick(g, in, dt, len);
    check(got == want, what, got + (got == want ? "" : std::string(", want ") + want));
}

State st(const char* name, const char* clip, bool loop = true) {
    State s;
    s.name = name; s.clip = clip; s.loop = loop;
    return s;
}

// Idle -(open)-> Opening -(exit time)-> Open -(close)-> Idle, plus an Any-State
// arrow to Broken. The door every one of these features exists for.
Graph makeDoor() {
    Graph g;
    g.states = {st("Idle", "idle"), st("Opening", "opening", false),
                st("Open", "open"), st("Broken", "broken")};
    g.params = {{"open", Param::Type::Trigger, 0.0f},
                {"close", Param::Type::Trigger, 0.0f},
                {"locked", Param::Type::Bool, 0.0f},
                {"force", Param::Type::Number, 0.0f}};

    Transition toOpening;                       // Idle -> Opening on `open`
    toOpening.from = 0; toOpening.to = 1;
    toOpening.conditions = {{"open", Condition::Op::Fired, 0.0f}};

    Transition toOpen;                          // Opening -> Open when it finishes
    toOpen.from = 1; toOpen.to = 2;
    toOpen.hasExitTime = true; toOpen.exitTime = 1.0f;

    Transition toIdle;                          // Open -> Idle on `close`
    toIdle.from = 2; toIdle.to = 0;
    toIdle.conditions = {{"close", Condition::Op::Fired, 0.0f}};

    Transition broken;                          // anywhere -> Broken on force
    broken.from = Transition::kAnyState; broken.to = 3;
    broken.conditions = {{"force", Condition::Op::Greater, 10.0f}};

    g.transitions = {toOpening, toOpen, toIdle, broken};
    return g;
}

} // namespace

int main() {
    std::printf("graphcheck\n");

    // --- Starting -----------------------------------------------------------
    {
        Graph g = makeDoor();
        g.entry = 0;
        Instance in;
        start(g, in);
        check(where(g, in) == "Idle", "a machine starts in its entry state", where(g, in));
        check(in.values.size() == g.params.size(), "with a slot per parameter",
              std::to_string(in.values.size()));

        // A Trigger with a default of 1 would mean "starts with the button already
        // pressed" -- it would fire on the first frame of every scene load.
        Graph h = makeDoor();
        h.params[0].def = 1.0f;
        Instance hi;
        start(h, hi);
        near(hi.values[0], 0.0f, "a trigger starts unset whatever its default says");
        steps(h, hi, 0.016f, 1.0f, "Idle",
              "so nothing fires on the first frame");
    }

    // --- A trigger fires once ------------------------------------------------
    {
        Graph g = makeDoor();
        Instance in;
        start(g, in);
        steps(g, in, 0.1f, 1.0f, "Idle",
              "nothing happens without the trigger");
        fire(g, in, "open");
        steps(g, in, 0.1f, 1.0f, "Opening", "firing it takes the arrow");
        near(in.values[0], 0.0f, "and the trigger is consumed by the arrow that read it");
        near(in.time, 0.0f, "the new state's clock starts at zero");
    }

    // --- Exit time waits for the clip, then lets go --------------------------
    {
        Graph g = makeDoor();
        Instance in;
        start(g, in);
        fire(g, in, "open");
        tick(g, in, 0.1f);                       // -> Opening
        check(where(g, in) == "Opening", "in Opening", where(g, in));
        // The opening clip is two seconds long; the arrow waits for all of it.
        steps(g, in, 1.0f, 2.0f, "Opening",
              "half way through: still opening");
        steps(g, in, 1.0f, 2.0f, "Open", "at the end: through");

        // A state whose clip has no length cannot be waited for -- the machine
        // must move on rather than wedge. (A missing clip is exactly this case.)
        Graph h = makeDoor();
        Instance hi;
        start(h, hi);
        fire(h, hi, "open");
        tick(h, hi, 0.1f, 0.0f);
        steps(h, hi, 0.016f, 0.0f, "Open", "an exit time on a clip with no length does not wedge");
    }

    // --- Any State beats an ordinary arrow ----------------------------------
    {
        Graph g = makeDoor();
        Instance in;
        start(g, in);
        fire(g, in, "open");
        tick(g, in, 0.1f);                       // Opening
        setNumber(g, in, "force", 25.0f);
        // Opening's own arrow (exit time, satisfied at 2s) and the Any-State one
        // are both ready; the Any-State one is the emergency and wins.
        steps(g, in, 5.0f, 2.0f, "Broken",
              "Any State outranks the state's own");
        // ...and it does not re-enter the state it is already in, which would
        // restart the clip every frame the condition held.
        steps(g, in, 0.1f, 1.0f, "Broken", "and does not restart itself");
        near(in.time, 0.1f, "so its clock keeps running");
    }

    // --- One transition per step --------------------------------------------
    {
        // Idle -> A -> B, both arrows always ready (exit time 0). A machine that
        // walked both in one frame would skip A entirely and nobody could see it.
        Graph g;
        g.states = {st("Idle", ""), st("A", ""), st("B", "")};
        Transition t1; t1.from = 0; t1.to = 1; t1.hasExitTime = true; t1.exitTime = 0.0f;
        Transition t2; t2.from = 1; t2.to = 2; t2.hasExitTime = true; t2.exitTime = 0.0f;
        g.transitions = {t1, t2};
        Instance in;
        start(g, in);
        steps(g, in, 0.016f, 1.0f, "A", "one step, one transition");
        steps(g, in, 0.016f, 1.0f, "B", "the next one on the next step");
    }

    // --- Order decides between two ready arrows ------------------------------
    {
        Graph g;
        g.states = {st("Idle", ""), st("Left", ""), st("Right", "")};
        g.params = {{"go", Param::Type::Bool, 1.0f}};
        Transition a; a.from = 0; a.to = 1; a.conditions = {{"go", Condition::Op::IsTrue, 0.0f}};
        Transition b; b.from = 0; b.to = 2; b.conditions = {{"go", Condition::Op::IsTrue, 0.0f}};
        g.transitions = {a, b};
        Instance in;
        start(g, in);
        steps(g, in, 0.016f, 1.0f, "Left", "the first arrow listed wins");

        // ...and with them the other way round, the other one does. (Otherwise
        // "first listed" would only be true of this particular numbering.)
        Graph h = g;
        std::swap(h.transitions[0], h.transitions[1]);
        Instance hi;
        start(h, hi);
        steps(h, hi, 0.016f, 1.0f, "Right",
              "reversing the list reverses the answer");
    }

    // --- An arrow with nothing on it is not taken ---------------------------
    {
        Graph g;
        g.states = {st("Idle", ""), st("Nowhere", "")};
        Transition empty; empty.from = 0; empty.to = 1;   // no conditions, no exit time
        g.transitions = {empty};
        Instance in;
        start(g, in);
        steps(g, in, 1.0f, 1.0f, "Idle", "a half-drawn arrow does not make its source unreachable");
    }

    // --- Conditions ----------------------------------------------------------
    {
        Graph g;
        g.states = {st("Idle", ""), st("Go", "")};
        g.params = {{"speed", Param::Type::Number, 0.0f},
                    {"ready", Param::Type::Bool, 0.0f}};
        Transition t; t.from = 0; t.to = 1;
        t.conditions = {{"speed", Condition::Op::Greater, 5.0f},
                        {"ready", Condition::Op::IsTrue, 0.0f}};
        g.transitions = {t};

        Instance in;
        start(g, in);
        setNumber(g, in, "speed", 9.0f);
        steps(g, in, 0.1f, 1.0f, "Idle",
              "every condition must hold, not any");
        setBool(g, in, "ready", true);
        steps(g, in, 0.1f, 1.0f, "Go", "and then it goes");

        // A condition naming a parameter that has been deleted must block the
        // arrow, not pass by default -- a graph missing a knob is broken, and
        // firing anyway would move objects the author never asked to move.
        Graph h = g;
        h.params.erase(h.params.begin());       // "speed" is gone
        Instance hi;
        start(h, hi);
        setBool(h, hi, "ready", true);
        steps(h, hi, 0.1f, 1.0f, "Idle",
              "a condition on a missing parameter blocks");
    }

    // --- What plays, and where in it ----------------------------------------
    {
        Graph g = makeDoor();
        Instance in;
        start(g, in);
        std::string clip;
        float t = 0.0f;
        step(g, in, 0.5f, 2.0f, clip, t);
        check(clip == "idle", "the current state names the clip", clip);
        near(t, 0.5f, "and time in the state is time in the clip");

        // Looping wraps; not looping holds at the end rather than running past it.
        step(g, in, 2.0f, 2.0f, clip, t);
        near(t, 0.5f, "a looping state wraps");

        Instance ni;
        start(g, ni);
        fire(g, ni, "open");
        step(g, ni, 0.1f, 2.0f, clip, t);        // -> Opening (not looping)
        step(g, ni, 9.0f, 2.0f, clip, t);
        check(clip == "open" || clip == "opening", "a finished clip stays put", clip);

        // Speed scales the clip's clock, not the state's.
        Graph h = makeDoor();
        h.states[0].speed = 2.0f;
        Instance hi;
        start(h, hi);
        step(h, hi, 0.5f, 100.0f, clip, t);
        near(t, 1.0f, "speed scales where in the clip a state is");
    }

    // --- A parameter added while the machine runs ---------------------------
    {
        Graph g = makeDoor();
        Instance in;
        start(g, in);
        g.params.push_back({"late", Param::Type::Bool, 0.0f});   // the author adds one
        setBool(g, in, "late", true);
        check(in.values.size() == g.params.size(),
              "an instance grows to fit a parameter added under it",
              std::to_string(in.values.size()));
        near(in.values.back(), 1.0f, "and the new one takes its value");
    }

    // --- Round trip through JSON --------------------------------------------
    {
        Graph g = makeDoor();
        g.name = "Door";
        g.entry = 0;
        g.states[1].pos = glm::vec2(120.0f, -40.0f);
        nlohmann::json j;
        save(j, {g});
        std::vector<Graph> back;
        load(j, back);

        check(back.size() == 1, "the graph survives the save", std::to_string(back.size()));
        if (!back.empty()) {
            const Graph& b = back[0];
            check(b.name == "Door", "with its name", b.name);
            check(b.states.size() == g.states.size(), "its states",
                  std::to_string(b.states.size()));
            check(b.transitions.size() == g.transitions.size(), "its arrows",
                  std::to_string(b.transitions.size()));
            check(b.params.size() == g.params.size(), "and its parameters",
                  std::to_string(b.params.size()));
            near(b.states[1].pos.x, 120.0f, "node positions come back too");
            check(b.transitions[3].from == Transition::kAnyState,
                  "an Any State arrow is still one",
                  std::to_string(b.transitions[3].from));
            check(b.transitions[0].conditions.size() == 1 &&
                  b.transitions[0].conditions[0].op == Condition::Op::Fired,
                  "and a condition keeps its operator", "");

            // The real question: does the reloaded graph BEHAVE the same?
            Instance in;
            start(b, in);
            fire(b, in, "open");
            steps(b, in, 0.1f, 1.0f, "Opening",
                  "the reloaded graph runs the same");
        }
    }

    // --- A scene with no graphs ---------------------------------------------
    {
        nlohmann::json j;
        std::vector<Graph> gs{Graph{}};
        load(j, gs);
        check(gs.empty(), "a scene with no graphs loads none", std::to_string(gs.size()));

        // ...and an empty graph is steppable without crashing.
        Graph g;
        Instance in;
        start(g, in);
        std::string clip;
        float t = 0.0f;
        step(g, in, 0.1f, 1.0f, clip, t);
        check(clip.empty(), "an empty graph plays nothing", "no crash");
    }

    std::printf(g_fails ? "\n%d check(s) failed\n" : "\nall checks passed\n", g_fails);
    return g_fails ? 1 : 0;
}
