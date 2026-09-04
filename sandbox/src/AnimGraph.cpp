#include "AnimGraph.hpp"

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

namespace animgraph {

int findState(const Graph& g, const std::string& name) {
    for (int i = 0; i < static_cast<int>(g.states.size()); ++i)
        if (g.states[i].name == name) return i;
    return -1;
}

int findParam(const Graph& g, const std::string& name) {
    for (int i = 0; i < static_cast<int>(g.params.size()); ++i)
        if (g.params[i].name == name) return i;
    return -1;
}

int findGraph(const std::vector<Graph>& gs, const std::string& name) {
    for (int i = 0; i < static_cast<int>(gs.size()); ++i)
        if (gs[i].name == name) return i;
    return -1;
}

const char* opName(Condition::Op op) {
    switch (op) {
        case Condition::Op::IsTrue:  return "is true";
        case Condition::Op::IsFalse: return "is false";
        case Condition::Op::Greater: return ">";
        case Condition::Op::Less:    return "<";
        case Condition::Op::Equals:  return "==";
        case Condition::Op::Fired:   break;
    }
    return "fired";
}

const char* paramTypeName(Param::Type t) {
    switch (t) {
        case Param::Type::Bool:   return "Bool";
        case Param::Type::Number: return "Number";
        case Param::Type::Trigger: break;
    }
    return "Trigger";
}

// --- Driving ----------------------------------------------------------------

void start(const Graph& g, Instance& in) {
    in.values.assign(g.params.size(), 0.0f);
    for (std::size_t i = 0; i < g.params.size(); ++i)
        // A Trigger always starts unset. Its default would mean "this machine
        // begins with the button already pressed", which is never what anybody
        // drew, and it would fire once on every scene load.
        in.values[i] = g.params[i].type == Param::Type::Trigger ? 0.0f : g.params[i].def;
    in.state   = g.states.empty() ? -1
               : std::clamp(g.entry, 0, static_cast<int>(g.states.size()) - 1);
    in.time    = 0.0f;
    in.entered = in.state;
}

namespace {

// The instance's slot for a parameter, growing the value list if the graph has
// gained a parameter since this instance started -- which it does the moment the
// author adds one while the game is running.
float* slot(const Graph& g, Instance& in, const std::string& param) {
    const int i = findParam(g, param);
    if (i < 0) return nullptr;
    if (in.values.size() != g.params.size()) {
        std::vector<float> grown(g.params.size(), 0.0f);
        for (std::size_t k = 0; k < std::min(grown.size(), in.values.size()); ++k)
            grown[k] = in.values[k];
        in.values = std::move(grown);
    }
    return &in.values[static_cast<std::size_t>(i)];
}

} // namespace

void setBool(const Graph& g, Instance& in, const std::string& param, bool v) {
    if (float* f = slot(g, in, param)) *f = v ? 1.0f : 0.0f;
}

void setNumber(const Graph& g, Instance& in, const std::string& param, float v) {
    if (float* f = slot(g, in, param)) *f = v;
}

void fire(const Graph& g, Instance& in, const std::string& param) {
    if (float* f = slot(g, in, param)) *f = 1.0f;
}

bool ready(const Graph& g, const Instance& in, const Transition& t, float clipLength) {
    if (t.hasExitTime) {
        // No clip, or one with no length, cannot be waited on -- treat the wait
        // as already over rather than wedging the machine in a state it can
        // never leave.
        const float wait = clipLength > 1e-4f ? clipLength * std::max(t.exitTime, 0.0f)
                                              : 0.0f;
        if (in.time < wait) return false;
    }
    for (const Condition& c : t.conditions) {
        const int pi = findParam(g, c.param);
        if (pi < 0) return false;              // names a parameter that is gone
        const float v = pi < static_cast<int>(in.values.size()) ? in.values[pi] : 0.0f;
        switch (c.op) {
            case Condition::Op::Fired:
            case Condition::Op::IsTrue:  if (v < 0.5f) return false; break;
            case Condition::Op::IsFalse: if (v >= 0.5f) return false; break;
            case Condition::Op::Greater: if (!(v > c.value)) return false; break;
            case Condition::Op::Less:    if (!(v < c.value)) return false; break;
            case Condition::Op::Equals:
                if (std::fabs(v - c.value) > 1e-4f) return false;
                break;
        }
    }
    // An arrow with no conditions and no exit time would be taken on the frame
    // it is drawn, which makes its source state unreachable and is almost always
    // a half-finished edit rather than an intent.
    return t.hasExitTime || !t.conditions.empty();
}

void step(const Graph& g, Instance& in, float dt,
          float clipLength, std::string& outClip, float& outTime) {
    outClip.clear();
    outTime = 0.0f;
    in.entered = -1;
    if (g.states.empty()) return;
    if (in.state < 0 || in.state >= static_cast<int>(g.states.size())) start(g, in);

    in.time += dt;

    // Transitions are tried IN ORDER, and the first that is ready wins. Order is
    // the author's, so a graph with two arrows that can both fire resolves the
    // way the list reads rather than the way the states happen to be numbered.
    // Any-State arrows are tried first: "you were hit" outranks "you finished
    // your step", which is the point of having them.
    for (int pass = 0; pass < 2; ++pass) {
        for (const Transition& t : g.transitions) {
            const bool any = (t.from == Transition::kAnyState);
            if (any != (pass == 0)) continue;
            if (!any && t.from != in.state) continue;
            if (t.to < 0 || t.to >= static_cast<int>(g.states.size())) continue;
            if (any && t.to == in.state) continue;   // don't restart where we are
            if (!ready(g, in, t, clipLength)) continue;

            // Taking it CONSUMES the triggers it tested. A trigger is a doorbell:
            // it rings once. Leaving it set would send the machine straight on
            // through the next arrow that tests the same one.
            for (const Condition& c : t.conditions) {
                const int pi = findParam(g, c.param);
                if (pi >= 0 && pi < static_cast<int>(in.values.size()) &&
                    g.params[pi].type == Param::Type::Trigger)
                    in.values[pi] = 0.0f;
            }
            in.state   = t.to;
            in.time    = 0.0f;
            in.entered = t.to;
            pass = 2;      // one transition per step: a machine that walks three
            break;         // states in one frame is a loop nobody can see
        }
    }

    const State& s = g.states[static_cast<std::size_t>(in.state)];
    outClip = s.clip;
    float t = in.time * s.speed;
    if (clipLength > 1e-4f) {
        if (s.loop) t = std::fmod(t, clipLength);
        else        t = std::min(t, clipLength);
    }
    outTime = t;
}

// --- Persistence ------------------------------------------------------------

namespace {

Condition::Op opFromName(const std::string& s) {
    if (s == "is true")  return Condition::Op::IsTrue;
    if (s == "is false") return Condition::Op::IsFalse;
    if (s == ">")        return Condition::Op::Greater;
    if (s == "<")        return Condition::Op::Less;
    if (s == "==")       return Condition::Op::Equals;
    return Condition::Op::Fired;
}

Param::Type typeFromName(const std::string& s) {
    if (s == "Bool")   return Param::Type::Bool;
    if (s == "Number") return Param::Type::Number;
    return Param::Type::Trigger;
}

} // namespace

void save(nlohmann::json& j, const std::vector<Graph>& graphs) {
    nlohmann::json arr = nlohmann::json::array();
    for (const Graph& g : graphs) {
        nlohmann::json gj;
        gj["name"]  = g.name;
        gj["entry"] = g.entry;

        nlohmann::json ps = nlohmann::json::array();
        for (const Param& p : g.params)
            ps.push_back({{"name", p.name}, {"type", paramTypeName(p.type)},
                          {"def", p.def}});
        gj["params"] = std::move(ps);

        nlohmann::json ss = nlohmann::json::array();
        for (const State& s : g.states)
            ss.push_back({{"name", s.name}, {"clip", s.clip}, {"loop", s.loop},
                          {"speed", s.speed}, {"x", s.pos.x}, {"y", s.pos.y}});
        gj["states"] = std::move(ss);

        nlohmann::json ts = nlohmann::json::array();
        for (const Transition& t : g.transitions) {
            nlohmann::json cs = nlohmann::json::array();
            for (const Condition& c : t.conditions)
                cs.push_back({{"param", c.param}, {"op", opName(c.op)},
                              {"value", c.value}});
            ts.push_back({{"from", t.from}, {"to", t.to},
                          {"exit", t.hasExitTime}, {"exitTime", t.exitTime},
                          {"conds", std::move(cs)}});
        }
        gj["transitions"] = std::move(ts);
        arr.push_back(std::move(gj));
    }
    j["animGraphs"] = std::move(arr);
}

void load(const nlohmann::json& j, std::vector<Graph>& graphs) {
    graphs.clear();
    const auto it = j.find("animGraphs");
    if (it == j.end() || !it->is_array()) return;
    for (const nlohmann::json& gj : *it) {
        Graph g;
        g.name  = gj.value("name", std::string{"Graph"});
        g.entry = gj.value("entry", 0);

        if (const auto ps = gj.find("params"); ps != gj.end() && ps->is_array())
            for (const nlohmann::json& pj : *ps) {
                Param p;
                p.name = pj.value("name", std::string{});
                p.type = typeFromName(pj.value("type", std::string{"Trigger"}));
                p.def  = pj.value("def", 0.0f);
                if (!p.name.empty()) g.params.push_back(std::move(p));
            }

        if (const auto ss = gj.find("states"); ss != gj.end() && ss->is_array())
            for (const nlohmann::json& sj : *ss) {
                State s;
                s.name  = sj.value("name", std::string{"State"});
                s.clip  = sj.value("clip", std::string{});
                s.loop  = sj.value("loop", true);
                s.speed = sj.value("speed", 1.0f);
                s.pos   = glm::vec2(sj.value("x", 0.0f), sj.value("y", 0.0f));
                g.states.push_back(std::move(s));
            }

        if (const auto ts = gj.find("transitions"); ts != gj.end() && ts->is_array())
            for (const nlohmann::json& tj : *ts) {
                Transition t;
                t.from        = tj.value("from", 0);
                t.to          = tj.value("to", 0);
                t.hasExitTime = tj.value("exit", false);
                t.exitTime    = tj.value("exitTime", 1.0f);
                if (const auto cs = tj.find("conds"); cs != tj.end() && cs->is_array())
                    for (const nlohmann::json& cj : *cs) {
                        Condition c;
                        c.param = cj.value("param", std::string{});
                        c.op    = opFromName(cj.value("op", std::string{"fired"}));
                        c.value = cj.value("value", 0.0f);
                        if (!c.param.empty()) t.conditions.push_back(std::move(c));
                    }
                g.transitions.push_back(std::move(t));
            }
        g.entry = g.states.empty() ? 0
                : std::clamp(g.entry, 0, static_cast<int>(g.states.size()) - 1);
        graphs.push_back(std::move(g));
    }
}

} // namespace animgraph
