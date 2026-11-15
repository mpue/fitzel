#include "Behaviour.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

#include <nlohmann/json.hpp>

namespace behaviour {

bool isSetting(Kind k) { return k == Kind::Key || k == Kind::Choice || k == Kind::Var; }

bool canConnect(Kind from, Kind to) {
    if (isSetting(from) || isSetting(to)) return false;
    if (from == Kind::Exec || to == Kind::Exec) return from == to;
    if (from == to || from == Kind::Any || to == Kind::Any) return true;
    // Anything can be SHOWN: a number, a position, a yes/no all read as text.
    if (to == Kind::Text) return true;
    // An object is its id, and a yes/no counts as 1 or 0 -- the conversions an
    // author expects without having to ask for them.
    auto numberish = [](Kind k) { return k == Kind::Number || k == Kind::Entity || k == Kind::Bool; };
    return numberish(from) && numberish(to) && !(from == Kind::Entity && to == Kind::Bool) &&
           !(from == Kind::Bool && to == Kind::Entity);
}

const char* kindName(Kind k) {
    switch (k) {
        case Kind::Exec:   return "Exec";
        case Kind::Number: return "Number";
        case Kind::Bool:   return "Bool";
        case Kind::Vector: return "Vector";
        case Kind::Text:   return "Text";
        case Kind::Entity: return "Object";
        case Kind::Any:    return "Any";
        case Kind::Key:    return "Key";
        case Kind::Choice: return "Choice";
        case Kind::Var:    return "Variable";
    }
    return "?";
}

const char* categoryName(Category c) {
    switch (c) {
        case Category::Event:  return "Events";
        case Category::Flow:   return "Flow";
        case Category::Action: return "Actions";
        case Category::Data:   return "Values";
    }
    return "?";
}

const std::vector<KeyName>& keys() {
    // Stored by INDEX in a graph, so this order is part of the file format: new
    // keys go at the end. The Lua names live in a list of their own, built in
    // full before anything points into it.
    static const std::vector<std::string> luas = [] {
        std::vector<std::string> v;
        for (char c = 'A'; c <= 'Z'; ++c) v.push_back(std::string("game.KEY_") + c);
        for (char c = '0'; c <= '9'; ++c) v.push_back(std::string("game.KEY_") + c);
        return v;
    }();
    static const std::vector<std::string> labels = [] {
        std::vector<std::string> v;
        for (char c = 'A'; c <= 'Z'; ++c) v.push_back(std::string(1, c));
        for (char c = '0'; c <= '9'; ++c) v.push_back(std::string(1, c));
        return v;
    }();
    static const std::vector<KeyName> k = [] {
        std::vector<KeyName> v;
        for (std::size_t i = 0; i < luas.size(); ++i) v.push_back({labels[i].c_str(), luas[i].c_str()});
        v.push_back({"Space", "game.KEY_SPACE"});
        v.push_back({"Enter", "game.KEY_ENTER"});
        v.push_back({"Escape", "game.KEY_ESCAPE"});
        v.push_back({"Tab", "game.KEY_TAB"});
        v.push_back({"Left", "game.KEY_LEFT"});
        v.push_back({"Right", "game.KEY_RIGHT"});
        v.push_back({"Up", "game.KEY_UP"});
        v.push_back({"Down", "game.KEY_DOWN"});
        v.push_back({"Shift", "game.KEY_LSHIFT"});
        v.push_back({"Ctrl", "game.KEY_LCTRL"});
        v.push_back({"Alt", "game.KEY_LALT"});
        v.push_back({"Backspace", "game.KEY_BACKSPACE"});
        v.push_back({"Delete", "game.KEY_DELETE"});
        return v;
    }();
    return k;
}

namespace {

// The input of a variable node that names its variable (-1 = none), and the
// name in it.
int varPin(const NodeDef& d) {
    for (int i = 0; i < static_cast<int>(d.ins.size()); ++i)
        if (d.ins[static_cast<std::size_t>(i)].kind == Kind::Var) return i;
    return -1;
}

std::string varOf(const Node& n) {
    const NodeDef* d = findDef(n.type);
    const int      p = d ? varPin(*d) : -1;
    return p >= 0 && p < static_cast<int>(n.values.size()) ? n.values[static_cast<std::size_t>(p)].text : std::string();
}

} // namespace

namespace {

// --- Lua text -------------------------------------------------------------------

std::string num(float v) {
    if (!std::isfinite(v)) v = 0.0f;
    char b[32];
    std::snprintf(b, sizeof(b), "%.7g", static_cast<double>(v));
    return b;
}

std::string quote(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '\\': o += "\\\\"; break;
            case '"':  o += "\\\""; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            default:
                if (c < 32) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\%03d", c);
                    o += b;
                } else {
                    o += static_cast<char>(c);
                }
        }
    }
    return o + "\"";
}

std::string indent(const std::string& code, int levels) {
    if (code.empty()) return code;
    const std::string pad(static_cast<std::size_t>(levels) * 4, ' ');
    std::string o;
    std::size_t start = 0;
    while (start < code.size()) {
        std::size_t end = code.find('\n', start);
        if (end == std::string::npos) end = code.size();
        if (end > start) o += pad + code.substr(start, end - start);
        o += '\n';
        start = end + 1;
    }
    return o;
}

std::string literal(Kind k, const Value& v, const PinDef* pd) {
    switch (k) {
        case Kind::Number: return num(v.num);
        case Kind::Bool:   return v.b ? "true" : "false";
        case Kind::Vector: return "V(" + num(v.vec.x) + ", " + num(v.vec.y) + ", " + num(v.vec.z) + ")";
        case Kind::Text:   return quote(v.text);
        case Kind::Entity: return v.text.empty() ? std::string("E.id") : "FIND(" + quote(v.text) + ")";
        case Kind::Key: {
            const auto& ks = keys();
            const int i = std::clamp(v.choice, 0, static_cast<int>(ks.size()) - 1);
            return ks[static_cast<std::size_t>(i)].lua;
        }
        case Kind::Choice: {
            const int n = pd ? static_cast<int>(pd->choices.size()) : 1;
            return std::to_string(std::clamp(v.choice, 0, std::max(0, n - 1)));
        }
        case Kind::Var: return quote(v.text);
        case Kind::Any:
        case Kind::Exec: break;
    }
    return "nil";
}

} // namespace

// --- The compiler's view of a graph --------------------------------------------
// One per compile. The node definitions call back into it for the three things
// a piece of Lua needs from the rest of the graph: the value on a data input,
// the call that runs whatever an exec output leads to, and somewhere to hook an
// event into start() and update().
class Gen {
public:
    explicit Gen(const Graph& g) : g(g) {}
    const Graph& g;

    std::string startCode;     // inside start()
    std::string updateCode;    // inside update(), after timers and mail
    std::string mailCode;      // inside the loop over this frame's messages (m)
    std::vector<std::string> warnings;

    void warn(const std::string& w) {
        if (std::find(warnings.begin(), warnings.end(), w) == warnings.end()) warnings.push_back(w);
    }

    static std::string slot(int id, int pin) { return "O.n" + std::to_string(id) + "_" + std::to_string(pin); }
    static std::string st(const Node& n) { return "ST[" + std::to_string(n.id) + "]"; }
    static std::string id(const Node& n) { return std::to_string(n.id); }
    // Where action / event outputs are kept: written when the node runs, read
    // by whoever is wired to them afterwards (this frame or later ones).
    static std::string out(const Node& n, int pin) { return slot(n.id, pin); }

    // The Lua expression for data input `pin` of `n`: whatever is wired to it,
    // converted to the kind the input wants, or the value typed into the node.
    std::string in(const Node& n, int pin) {
        const NodeDef* d = findDef(n.type);
        if (!d || pin < 0 || pin >= static_cast<int>(d->ins.size())) return "nil";
        const PinDef& pd   = d->ins[static_cast<std::size_t>(pin)];
        const Kind    want = pinKind(g, n, true, pin);
        const Value&  v    = pin < static_cast<int>(n.values.size()) ? n.values[static_cast<std::size_t>(pin)] : pd.def;
        for (const Link& l : g.links) {
            if (l.toNode != n.id || l.toPin != pin) continue;
            const Node*    src = findNode(g, l.fromNode);
            const NodeDef* sd  = src ? findDef(src->type) : nullptr;
            if (!sd || l.fromPin < 0 || l.fromPin >= static_cast<int>(sd->outs.size())) break;
            const Kind have = pinKind(g, *src, false, l.fromPin);
            if (sd->cat == Category::Data && sd->expr) {
                // connect() refuses data loops; a graph edited by hand might not.
                if (std::find(m_stack.begin(), m_stack.end(), src->id) != m_stack.end() || m_stack.size() > 64) {
                    warn("A value is made of itself (a loop of value nodes): it reads as its typed value.");
                    break;
                }
                m_stack.push_back(src->id);
                const std::string e = "(" + sd->expr(*this, *src, l.fromPin) + ")";
                m_stack.pop_back();
                return coerce(e, have, want, false);
            }
            return coerce(out(*src, l.fromPin), have, want, true);
        }
        if (want == Kind::Any) {
            // An unwired Any input (Set Variable's value): typed as its variable.
            const int vi = findVar(g, varOf(n));
            return vi >= 0 ? literal(g.vars[static_cast<std::size_t>(vi)].kind, v, &pd) : "nil";
        }
        return literal(want, v, &pd);
    }

    // A setting (Key, Choice) as it is stored -- settings are never wired, so
    // the node can decide at compile time.
    int choice(const Node& n, int pin) const {
        const NodeDef* d = findDef(n.type);
        if (!d || pin < 0 || pin >= static_cast<int>(d->ins.size())) return 0;
        return pin < static_cast<int>(n.values.size()) ? n.values[static_cast<std::size_t>(pin)].choice
                                                       : d->ins[static_cast<std::size_t>(pin)].def.choice;
    }
    std::string text(const Node& n, int pin) const {
        const NodeDef* d = findDef(n.type);
        if (!d || pin < 0 || pin >= static_cast<int>(d->ins.size())) return {};
        return pin < static_cast<int>(n.values.size()) ? n.values[static_cast<std::size_t>(pin)].text
                                                       : d->ins[static_cast<std::size_t>(pin)].def.text;
    }

    // The statement that runs what exec output `pin` of `n` leads to ("" when
    // it leads nowhere).
    std::string next(const Node& n, int pin) const {
        for (const Link& l : g.links)
            if (l.fromNode == n.id && l.fromPin == pin)
                return "F.n" + std::to_string(l.toNode) + "_" + std::to_string(l.toPin) + "()\n";
        return {};
    }
    // An event firing through output `pin`: lit in the editor, then onward.
    std::string fire(const Node& n, int pin) const { return "PULSE[" + id(n) + "] = T\n" + next(n, pin); }

private:
    static std::string coerce(const std::string& e, Kind have, Kind want, bool stored) {
        // A stored output nobody has written yet is nil: read it as nothing of
        // its kind rather than stopping the behaviour on "arithmetic on nil".
        auto safe = [&](const char* dflt) { return stored ? "(" + e + " or " + dflt + ")" : e; };
        if (want == Kind::Text) return "TXT(" + e + ")";
        if (want == Kind::Number && have == Kind::Bool) return "((" + e + ") and 1 or 0)";
        if (want == Kind::Bool && have == Kind::Number) return "((" + e + ") ~= 0)";
        switch (want) {
            case Kind::Number: return safe("0");
            case Kind::Bool:   return stored ? "(" + e + " == true)" : e;
            case Kind::Vector: return safe("V()");
            case Kind::Entity: return safe("-2");
            default:           return e;
        }
    }
    std::vector<int> m_stack;   // value nodes being expanded, to stop a loop
};

namespace {

// --- Building the catalogue -------------------------------------------------------

PinDef X(const char* name = "") { return {name, Kind::Exec, {}, {}}; }
PinDef N(const char* name, float d = 0.0f) {
    PinDef p{name, Kind::Number, {}, {}};
    p.def.num = d;
    return p;
}
PinDef B(const char* name, bool d = false) {
    PinDef p{name, Kind::Bool, {}, {}};
    p.def.b = d;
    return p;
}
PinDef Vc(const char* name, glm::vec3 d = glm::vec3(0.0f)) {
    PinDef p{name, Kind::Vector, {}, {}};
    p.def.vec = d;
    return p;
}
PinDef T(const char* name, const char* d = "") {
    PinDef p{name, Kind::Text, {}, {}};
    p.def.text = d;
    return p;
}
PinDef En(const char* name = "Object") { return {name, Kind::Entity, {}, {}}; }
PinDef K(const char* name, int keyIndex) {
    PinDef p{name, Kind::Key, {}, {}};
    p.def.choice = keyIndex;
    return p;
}
PinDef C(const char* name, std::vector<std::string> labels, int d = 0) {
    PinDef p{name, Kind::Choice, {}, std::move(labels)};
    p.def.choice = d;
    return p;
}
PinDef Vr(const char* name = "Variable") { return {name, Kind::Var, {}, {}}; }
PinDef A(const char* name) { return {name, Kind::Any, {}, {}}; }

constexpr int kKeyE     = 4;
constexpr int kKeySpace = 36;

using ExecFn = std::function<std::string(Gen&, const Node&, int)>;

NodeDef event(const char* type, const char* title, const char* help, std::vector<PinDef> ins,
              std::vector<PinDef> outs, std::function<void(Gen&, const Node&)> fn) {
    NodeDef d;
    d.type = type; d.title = title; d.cat = Category::Event; d.help = help;
    d.ins = std::move(ins); d.outs = std::move(outs); d.event = std::move(fn);
    return d;
}
NodeDef flow(const char* type, const char* title, const char* help, std::vector<PinDef> ins,
             std::vector<PinDef> outs, ExecFn fn) {
    NodeDef d;
    d.type = type; d.title = title; d.cat = Category::Flow; d.help = help;
    d.ins = std::move(ins); d.outs = std::move(outs); d.exec = std::move(fn);
    return d;
}
// An action: one exec in, one exec out ("then"), and the Lua that runs between.
NodeDef action(const char* type, const char* title, const char* help, std::vector<PinDef> dataIns,
               std::function<std::string(Gen&, const Node&)> body, std::vector<PinDef> dataOuts = {}) {
    NodeDef d;
    d.type = type; d.title = title; d.cat = Category::Action; d.help = help;
    d.ins.push_back(X());
    for (PinDef& p : dataIns) d.ins.push_back(std::move(p));
    d.outs.push_back(X());
    for (PinDef& p : dataOuts) d.outs.push_back(std::move(p));
    d.exec = [body = std::move(body)](Gen& g, const Node& n, int) { return body(g, n) + g.next(n, 0); };
    return d;
}
NodeDef data(const char* type, const char* title, const char* help, std::vector<PinDef> ins,
             std::vector<PinDef> outs, ExecFn fn) {
    NodeDef d;
    d.type = type; d.title = title; d.cat = Category::Data; d.help = help;
    d.ins = std::move(ins); d.outs = std::move(outs); d.expr = std::move(fn);
    return d;
}

std::vector<NodeDef> buildCatalogue() {
    std::vector<NodeDef> c;

    // ===== Events ==================================================================
    c.push_back(event("on_start", "On Start", "Once, when the game starts.", {}, {X()},
                      [](Gen& g, const Node& n) { g.startCode += g.fire(n, 0); }));
    c.push_back(event("on_update", "On Update", "Every frame while the game runs. Delta is the frame's length in seconds.",
                      {}, {X(), N("Delta")}, [](Gen& g, const Node& n) {
                          g.updateCode += Gen::out(n, 1) + " = DT\n" + g.fire(n, 0);
                      }));
    c.push_back(event("every", "Every", "Again and again, this many seconds apart.", {N("Seconds", 1.0f)}, {X()},
                      [](Gen& g, const Node& n) {
                          const std::string s = Gen::st(n);
                          g.updateCode += "do\n    local every = math.max(" + g.in(n, 0) + ", 0.01)\n    " + s +
                                          " = (" + s + " or 0) + DT\n    if " + s + " >= every then\n        " + s +
                                          " = math.min(" + s + " - every, every)\n" + indent(g.fire(n, 0), 2) +
                                          "    end\nend\n";
                      }));
    c.push_back(event("on_key", "On Key", "When a key goes down, comes up, or for every frame it is held.",
                      {K("Key", kKeyE), C("When", {"pressed", "released", "held"})}, {X()},
                      [](Gen& g, const Node& n) {
                          const std::string key = g.in(n, 0);
                          switch (g.choice(n, 1)) {
                              case 1:
                                  g.updateCode += "do\n    local down = game.keyDown(" + key + ")\n    if " + Gen::st(n) +
                                                  " and not down then\n" + indent(g.fire(n, 0), 2) + "    end\n    " +
                                                  Gen::st(n) + " = down\nend\n";
                                  break;
                              case 2:
                                  g.updateCode += "if game.keyDown(" + key + ") then\n" + indent(g.fire(n, 0), 1) + "end\n";
                                  break;
                              default:
                                  g.updateCode += "if game.keyPressed(" + key + ") then\n" + indent(g.fire(n, 0), 1) + "end\n";
                          }
                      }));
    c.push_back(event("on_click", "On Click", "When this object (or a part of it) is clicked with the left mouse button.",
                      {}, {X()}, [](Gen& g, const Node& n) {
                          g.updateCode += "if CLICKED() then\n" + indent(g.fire(n, 0), 1) + "end\n";
                      }));
    c.push_back(event("player_near", "Player Near",
                      "When the player comes within the radius (Enter) and goes out of it again (Leave).",
                      {N("Radius", 3.0f)}, {X("Enter"), X("Leave"), N("Distance"), B("Inside")},
                      [](Gen& g, const Node& n) {
                          const std::string s = Gen::st(n);
                          g.updateCode += "do\n    local d = VLEN(VSUB(PLAYER(), WPOS(E.id)))\n    local inside = d <= " +
                                          g.in(n, 0) + "\n    local was = " + s + "\n    " + s + " = inside\n    " +
                                          Gen::out(n, 2) + ", " + Gen::out(n, 3) + " = d, inside\n" +
                                          "    if inside and not was then\n" + indent(g.fire(n, 0), 2) +
                                          "    elseif was and not inside then\n" + indent(g.fire(n, 1), 2) +
                                          "    end\nend\n";
                      }));
    c.push_back(event("on_message", "On Message",
                      "When another behaviour sends this one the message (Send Message). From is who sent it.",
                      {T("Message", "open")}, {X(), En("From")}, [](Gen& g, const Node& n) {
                          g.mailCode += "if m[1] == " + g.in(n, 0) + " then\n    " + Gen::out(n, 1) + " = m[2]\n" +
                                        indent(g.fire(n, 0), 1) + "end\n";
                      }));

    // ===== Flow ====================================================================
    c.push_back(flow("branch", "Branch", "One way if the condition holds, the other if it does not.",
                     {X(), B("Condition", true)}, {X("True"), X("False")}, [](Gen& g, const Node& n, int) {
                         const std::string t = g.next(n, 0), f = g.next(n, 1);
                         return "if " + g.in(n, 1) + " then\n" + indent(t, 1) + (f.empty() ? "" : "else\n" + indent(f, 1)) +
                                "end\n";
                     }));
    c.push_back(flow("sequence", "Sequence", "Several things one after the other, top to bottom.", {X()},
                     {X("Then 1"), X("Then 2"), X("Then 3"), X("Then 4")}, [](Gen& g, const Node& n, int) {
                         return g.next(n, 0) + g.next(n, 1) + g.next(n, 2) + g.next(n, 3);
                     }));
    c.push_back(flow("delay", "Delay", "Waits, then goes on. Reached again while it waits, it keeps the first wait.",
                     {X(), N("Seconds", 1.0f)}, {X("Done")}, [](Gen& g, const Node& n, int) {
                         return "AFTER(" + Gen::id(n) + ", " + g.in(n, 1) + ", function()\n    PULSE[" + Gen::id(n) +
                                "] = T\n" + indent(g.next(n, 0), 1) + "end)\n";
                     }));
    c.push_back(flow("do_once", "Do Once", "Lets through the first time only -- until Reset is reached.",
                     {X(), X("Reset")}, {X()}, [](Gen& g, const Node& n, int pin) -> std::string {
                         if (pin == 1) return Gen::st(n) + " = nil\n";
                         return "if not " + Gen::st(n) + " then\n    " + Gen::st(n) + " = true\n" +
                                indent(g.next(n, 0), 1) + "end\n";
                     }));
    c.push_back(flow("flip_flop", "Flip Flop", "Goes to A, then B, then A again, each time it is reached.", {X()},
                     {X("A"), X("B"), B("Is A")}, [](Gen& g, const Node& n, int) {
                         const std::string s = Gen::st(n);
                         return s + " = not " + s + "\n" + Gen::out(n, 2) + " = " + s + "\nif " + s + " then\n" +
                                indent(g.next(n, 0), 1) + "else\n" + indent(g.next(n, 1), 1) + "end\n";
                     }));
    c.push_back(flow("gate", "Gate", "Lets through while open. Open and Close change it; it starts as Start open says.",
                     {X("In"), X("Open"), X("Close"), B("Start open", true)}, {X()},
                     [](Gen& g, const Node& n, int pin) -> std::string {
                         const std::string s = Gen::st(n);
                         if (pin == 1) return s + " = true\n";
                         if (pin == 2) return s + " = false\n";
                         return "if " + s + " == nil then " + s + " = " + g.in(n, 3) + " end\nif " + s + " then\n" +
                                indent(g.next(n, 0), 1) + "end\n";
                     }));
    c.push_back(flow("chance", "Chance", "Yes with this probability (0..1), otherwise No.",
                     {X(), N("Chance", 0.5f)}, {X("Yes"), X("No")}, [](Gen& g, const Node& n, int) {
                         const std::string no = g.next(n, 1);
                         return "if math.random() < " + g.in(n, 1) + " then\n" + indent(g.next(n, 0), 1) +
                                (no.empty() ? "" : "else\n" + indent(no, 1)) + "end\n";
                     }));
    c.push_back(flow("repeat", "Repeat", "Runs Each this many times at once (Index counts 1, 2, ...), then Done.",
                     {X(), N("Times", 3.0f)}, {X("Each"), X("Done"), N("Index")}, [](Gen& g, const Node& n, int) {
                         return "for i = 1, math.floor(" + g.in(n, 1) + ") do\n    " + Gen::out(n, 2) + " = i\n" +
                                indent(g.next(n, 0), 1) + "end\n" + g.next(n, 1);
                     }));
    c.push_back(flow("cooldown", "Cooldown", "Lets through, then ignores everything for this many seconds.",
                     {X(), N("Seconds", 1.0f)}, {X()}, [](Gen& g, const Node& n, int) {
                         const std::string s = Gen::st(n);
                         return "if not " + s + " or T - " + s + " >= " + g.in(n, 1) + " then\n    " + s + " = T\n" +
                                indent(g.next(n, 0), 1) + "end\n";
                     }));

    // ===== Actions =================================================================
    c.push_back(action("move_by", "Move By",
                       "Moves the object by the offset -- per second (from On Update), or at once. "
                       "\"own\" turns the offset with the object's heading: +Z is its front.",
                       {En(), Vc("Offset", {0.0f, 0.0f, 1.0f}), B("Per second", true), C("Along", {"world", "own"})},
                       [](Gen& g, const Node& n) {
                           std::string s = "do\n    local id = " + g.in(n, 1) + "\n    local o = " + g.in(n, 2) + "\n";
                           if (g.choice(n, 4) == 1) s += "    o = YAW(o, ROT(id).y)\n";
                           s += "    if " + g.in(n, 3) + " then o = VMUL(o, DT) end\n    SETPOS(id, VADD(POS(id), o))\nend\n";
                           return s;
                       }));
    c.push_back(action("set_position", "Set Position", "Puts the object at a position.",
                       {En(), Vc("Position")}, [](Gen& g, const Node& n) {
                           return "SETPOS(" + g.in(n, 1) + ", " + g.in(n, 2) + ")\n";
                       }));
    {
        NodeDef d;
        d.type = "move_toward"; d.title = "Move Toward"; d.cat = Category::Action;
        d.help = "Moves the object toward the target at this speed (metres a second) -- from On Update. "
                 "Moving while it is under way, Arrived once it is there.";
        d.ins  = {X(), En(), Vc("Target"), N("Speed", 2.0f), B("Keep height", true)};
        d.outs = {X("Moving"), X("Arrived")};
        d.exec = [](Gen& g, const Node& n, int) {
            return "do\n    local id = " + g.in(n, 1) + "\n    local p, t = POS(id), " + g.in(n, 2) + "\n    if " +
                   g.in(n, 4) +
                   " then t = V(t.x, p.y, t.z) end\n    local d = VSUB(t, p)\n    local l = VLEN(d)\n    local step = " +
                   g.in(n, 3) + " * DT\n    if l <= step then\n        SETPOS(id, t)\n" + indent(g.next(n, 1), 2) +
                   "    else\n        SETPOS(id, VADD(p, VMUL(d, step / l)))\n" + indent(g.next(n, 0), 2) +
                   "    end\nend\n";
        };
        c.push_back(std::move(d));
    }
    c.push_back(action("rotate_by", "Rotate By", "Turns the object by these degrees -- per second, or at once.",
                       {En(), Vc("Degrees", {0.0f, 90.0f, 0.0f}), B("Per second", true)}, [](Gen& g, const Node& n) {
                           return "do\n    local id = " + g.in(n, 1) + "\n    local r = " + g.in(n, 2) + "\n    if " +
                                  g.in(n, 3) + " then r = VMUL(r, DT) end\n    SETROT(id, VADD(ROT(id), r))\nend\n";
                       }));
    c.push_back(action("look_at", "Look At",
                       "Turns the object's front (+Z) toward the target, round the up axis. Turn speed in degrees "
                       "a second; 0 turns at once.",
                       {En(), Vc("Target"), N("Turn speed", 0.0f)}, [](Gen& g, const Node& n) {
                           return "LOOK(" + g.in(n, 1) + ", " + g.in(n, 2) + ", " + g.in(n, 3) + ")\n";
                       }));
    c.push_back(action("set_rotation", "Set Rotation", "Sets the object's rotation, in degrees.",
                       {En(), Vc("Degrees")}, [](Gen& g, const Node& n) {
                           return "SETROT(" + g.in(n, 1) + ", " + g.in(n, 2) + ")\n";
                       }));
    c.push_back(action("set_scale", "Set Size", "Sets the object's half size (from its middle to each side), in metres.",
                       {En(), Vc("Half size", glm::vec3(0.5f))}, [](Gen& g, const Node& n) {
                           return "SETSCALE(" + g.in(n, 1) + ", " + g.in(n, 2) + ")\n";
                       }));
    c.push_back(action("set_visible", "Show / Hide",
                       "Shows or hides the object, with its children. A hidden object's own behaviour stops "
                       "until something shows it again.",
                       {En(), B("Visible", false)}, [](Gen& g, const Node& n) {
                           return "game.setActive(" + g.in(n, 1) + ", " + g.in(n, 2) + ")\n";
                       }));
    c.push_back(action("set_color", "Set Colour", "Gives the object a colour (red, green, blue, 0..1).",
                       {En(), Vc("Colour", {1.0f, 0.3f, 0.2f})}, [](Gen& g, const Node& n) {
                           return "do\n    local c = " + g.in(n, 2) + "\n    game.setColor(" + g.in(n, 1) +
                                  ", c.x, c.y, c.z)\nend\n";
                       }));
    c.push_back(action("destroy", "Destroy", "Removes the object from the game -- at once, or after so many seconds.",
                       {En(), N("After", 0.0f)}, [](Gen& g, const Node& n) {
                           return "DESTROY(" + g.in(n, 1) + ", " + g.in(n, 2) + ")\n";
                       }));
    c.push_back(action("spawn_prefab", "Spawn Prefab",
                       "Makes a new copy of a prefab of the project (by name) at a position, turned by Yaw degrees.",
                       {T("Prefab", ""), Vc("Position"), N("Yaw", 0.0f)}, [](Gen& g, const Node& n) {
                           return "do\n    local p = " + g.in(n, 2) + "\n    " + Gen::out(n, 1) + " = game.spawnPrefab(" +
                                  g.in(n, 1) + ", p.x, p.y, p.z, " + g.in(n, 3) + ") or -2\nend\n";
                       },
                       {En("Spawned")}));
    c.push_back(action("play_sound", "Play Sound",
                       "Plays a sound from the project's sounds -- at the object (heard from where it is) or flat.",
                       {T("Sound", "click.wav"), N("Volume", 1.0f), B("At the object", true)},
                       [](Gen& g, const Node& n) {
                           return "SOUND(" + g.in(n, 1) + ", " + g.in(n, 2) + ", " + g.in(n, 3) + ")\n";
                       }));
    c.push_back(action("add_score", "Add Score", "Adds points to the game's score (shared by every object).",
                       {N("Points", 1.0f)}, [](Gen& g, const Node& n) {
                           return "game.addScore(math.floor(" + g.in(n, 1) + "))\n";
                       }));
    c.push_back(action("hud_text", "Show Text", "Puts a line of text on the screen (the game's one HUD line).",
                       {T("Text", "Hello")}, [](Gen& g, const Node& n) { return "game.setHud(" + g.in(n, 1) + ")\n"; }));
    c.push_back(action("log", "Log", "Writes a line to the log -- to see what is happening, and when.",
                       {T("Text", "here")}, [](Gen& g, const Node& n) {
                           return "game.log(LABEL .. \": \" .. " + g.in(n, 1) + ")\n";
                       }));
    c.push_back(action("push", "Push", "Gives a physics object a shove (an impulse, in newton-seconds).",
                       {En(), Vc("Impulse", {0.0f, 5.0f, 0.0f})}, [](Gen& g, const Node& n) {
                           return "do\n    local j = " + g.in(n, 2) + "\n    game.applyImpulse(" + g.in(n, 1) +
                                  ", j.x, j.y, j.z)\nend\n";
                       }));
    c.push_back(action("set_velocity", "Set Velocity", "Sets how fast a physics object moves (metres a second).",
                       {En(), Vc("Velocity")}, [](Gen& g, const Node& n) {
                           return "do\n    local v = " + g.in(n, 2) + "\n    game.setVelocity(" + g.in(n, 1) +
                                  ", v.x, v.y, v.z)\nend\n";
                       }));
    c.push_back(action("anim_trigger", "Animation Trigger",
                       "Fires a trigger of the object's Animation Graph (its arrows decide what happens).",
                       {En(), T("Trigger", "open")}, [](Gen& g, const Node& n) {
                           return "game.animTrigger(" + g.in(n, 1) + ", " + g.in(n, 2) + ")\n";
                       }));
    c.push_back(action("anim_bool", "Animation Bool", "Sets a yes/no parameter of the object's Animation Graph.",
                       {En(), T("Parameter", "moving"), B("Value", true)}, [](Gen& g, const Node& n) {
                           return "game.animBool(" + g.in(n, 1) + ", " + g.in(n, 2) + ", " + g.in(n, 3) + ")\n";
                       }));
    c.push_back(action("anim_number", "Animation Number", "Sets a number parameter of the object's Animation Graph.",
                       {En(), T("Parameter", "speed"), N("Value", 1.0f)}, [](Gen& g, const Node& n) {
                           return "game.animNumber(" + g.in(n, 1) + ", " + g.in(n, 2) + ", " + g.in(n, 3) + ")\n";
                       }));
    c.push_back(action("send_message", "Send Message",
                       "Sends a message to another object's behaviour (its On Message), or to every behaviour "
                       "that listens. It arrives by the next frame.",
                       {T("Message", "open"), C("To", {"one object", "everyone"}), En()}, [](Gen& g, const Node& n) {
                           const std::string to = g.choice(n, 2) == 1 ? std::string("-1") : g.in(n, 3);
                           return "SEND(" + to + ", " + g.in(n, 1) + ")\n";
                       }));
    c.push_back(action("set_variable", "Set Variable", "Gives a variable of this behaviour a new value.",
                       {Vr(), A("Value")}, [](Gen& g, const Node& n) {
                           if (findVar(g.g, g.text(n, 1)) < 0)
                               g.warn("Set Variable names \"" + g.text(n, 1) + "\", which this behaviour does not have.");
                           return "VARS[" + quote(g.text(n, 1)) + "] = " + g.in(n, 2) + "\n";
                       }));
    c.push_back(action("add_variable", "Add To Variable", "Adds to a number variable (a counter, a timer).",
                       {Vr(), N("Amount", 1.0f)}, [](Gen& g, const Node& n) {
                           if (findVar(g.g, g.text(n, 1)) < 0)
                               g.warn("Add To Variable names \"" + g.text(n, 1) +
                                      "\", which this behaviour does not have.");
                           const std::string v = "VARS[" + quote(g.text(n, 1)) + "]";
                           return v + " = (tonumber(" + v + ") or 0) + " + g.in(n, 2) + "\n";
                       }));
    c.push_back(action("teleport_player", "Teleport Player",
                       "Puts the walking player somewhere else, facing Yaw degrees (0 = +X, 90 = +Z).",
                       {Vc("Position"), N("Yaw", 0.0f)}, [](Gen& g, const Node& n) {
                           return "do\n    local p = " + g.in(n, 1) + "\n    game.teleport(p.x, p.y, p.z, " + g.in(n, 2) +
                                  ")\nend\n";
                       }));
    c.push_back(action("load_scene", "Load Scene", "Loads another scene of the project (by name).",
                       {T("Scene", "")}, [](Gen& g, const Node& n) { return "game.loadScene(" + g.in(n, 1) + ")\n"; }));

    // ===== Values ==================================================================
    c.push_back(data("self", "Self", "The object this behaviour runs on.", {}, {En("Self")},
                     [](Gen&, const Node&, int) { return std::string("E.id"); }));
    c.push_back(data("object", "Object", "Another object of the scene, by name.", {T("Name", "")}, {En()},
                     [](Gen& g, const Node& n, int) { return "FIND(" + g.in(n, 0) + ")"; }));
    c.push_back(data("player_pos", "Player Position", "Where the player is (the eye of the player's camera).", {},
                     {Vc("Position")}, [](Gen&, const Node&, int) { return std::string("PLAYER()"); }));
    c.push_back(data("position_of", "Position Of", "Where an object is.", {En()}, {Vc("Position")},
                     [](Gen& g, const Node& n, int) { return "POS(" + g.in(n, 0) + ")"; }));
    c.push_back(data("rotation_of", "Rotation Of", "How an object is turned, in degrees.", {En()}, {Vc("Degrees")},
                     [](Gen& g, const Node& n, int) { return "ROT(" + g.in(n, 0) + ")"; }));
    c.push_back(data("distance", "Distance", "How far apart two positions are, in metres.",
                     {Vc("A"), Vc("B")}, {N("Metres")},
                     [](Gen& g, const Node& n, int) { return "VLEN(VSUB(" + g.in(n, 0) + ", " + g.in(n, 1) + "))"; }));
    c.push_back(data("direction", "Direction", "The way from one position to another, one metre long.",
                     {Vc("From"), Vc("To")}, {Vc("Direction")}, [](Gen& g, const Node& n, int) {
                         return "VNORM(VSUB(" + g.in(n, 1) + ", " + g.in(n, 0) + "))";
                     }));
    c.push_back(data("get_variable", "Get Variable", "The value a variable of this behaviour has now.", {Vr()},
                     {A("Value")}, [](Gen& g, const Node& n, int) {
                         if (findVar(g.g, g.text(n, 0)) < 0)
                             g.warn("Get Variable names \"" + g.text(n, 0) + "\", which this behaviour does not have.");
                         return "VARS[" + quote(g.text(n, 0)) + "]";
                     }));
    c.push_back(data("number", "Number", "A number, typed once and wired to as many inputs as want it.",
                     {N("Value", 1.0f)}, {N("")}, [](Gen& g, const Node& n, int) { return g.in(n, 0); }));
    c.push_back(data("text", "Text", "A piece of text.", {T("Value", "")}, {T("")},
                     [](Gen& g, const Node& n, int) { return g.in(n, 0); }));
    c.push_back(data("make_vector", "Make Vector", "A position or direction from three numbers.",
                     {N("X"), N("Y"), N("Z")}, {Vc("")}, [](Gen& g, const Node& n, int) {
                         return "V(" + g.in(n, 0) + ", " + g.in(n, 1) + ", " + g.in(n, 2) + ")";
                     }));
    c.push_back(data("break_vector", "Break Vector", "The three numbers of a position or direction.",
                     {Vc("Vector")}, {N("X"), N("Y"), N("Z")}, [](Gen& g, const Node& n, int pin) {
                         static const char* f[] = {".x", ".y", ".z"};
                         return g.in(n, 0) + f[std::clamp(pin, 0, 2)];
                     }));
    c.push_back(data("math", "Math", "Two numbers worked into one.",
                     {N("A"), C("Op", {"+", "-", "x", "/", "min", "max", "power", "remainder"}), N("B", 1.0f)}, {N("")},
                     [](Gen& g, const Node& n, int) -> std::string {
                         const std::string a = g.in(n, 0), b = g.in(n, 2);
                         switch (g.choice(n, 1)) {
                             case 1: return a + " - " + b;
                             case 2: return a + " * " + b;
                             case 3: return "DIV(" + a + ", " + b + ")";
                             case 4: return "math.min(" + a + ", " + b + ")";
                             case 5: return "math.max(" + a + ", " + b + ")";
                             case 6: return a + " ^ " + b;
                             case 7: return "MOD(" + a + ", " + b + ")";
                             default: return a + " + " + b;
                         }
                     }));
    c.push_back(data("vector_math", "Vector Math", "Two positions or directions added or taken apart.",
                     {Vc("A"), C("Op", {"+", "-"}), Vc("B")}, {Vc("")}, [](Gen& g, const Node& n, int) {
                         return (g.choice(n, 1) == 1 ? "VSUB(" : "VADD(") + g.in(n, 0) + ", " + g.in(n, 2) + ")";
                     }));
    c.push_back(data("vector_scale", "Scale Vector", "A position or direction times a number.",
                     {Vc("Vector"), N("Times", 1.0f)}, {Vc("")},
                     [](Gen& g, const Node& n, int) { return "VMUL(" + g.in(n, 0) + ", " + g.in(n, 1) + ")"; }));
    c.push_back(data("compare", "Compare", "Two numbers compared: yes or no.",
                     {N("A"), C("Op", {">", "<", "=", ">=", "<=", "not ="}), N("B")}, {B("")},
                     [](Gen& g, const Node& n, int) -> std::string {
                         const std::string a = g.in(n, 0), b = g.in(n, 2);
                         switch (g.choice(n, 1)) {
                             case 1: return a + " < " + b;
                             case 2: return "math.abs(" + a + " - " + b + ") < 1e-4";
                             case 3: return a + " >= " + b;
                             case 4: return a + " <= " + b;
                             case 5: return "math.abs(" + a + " - " + b + ") >= 1e-4";
                             default: return a + " > " + b;
                         }
                     }));
    c.push_back(data("logic", "And / Or", "Two yes/no answers into one.", {B("A"), C("Op", {"and", "or"}), B("B")},
                     {B("")}, [](Gen& g, const Node& n, int) {
                         return g.in(n, 0) + (g.choice(n, 1) == 1 ? " or " : " and ") + g.in(n, 2);
                     }));
    c.push_back(data("not", "Not", "Yes becomes no, no becomes yes.", {B("")}, {B("")},
                     [](Gen& g, const Node& n, int) { return "not " + g.in(n, 0); }));
    c.push_back(data("choose", "Choose", "One number if the condition holds, the other if not.",
                     {B("Condition"), N("If yes", 1.0f), N("If no")}, {N("")}, [](Gen& g, const Node& n, int) {
                         return "PICK(" + g.in(n, 0) + ", " + g.in(n, 1) + ", " + g.in(n, 2) + ")";
                     }));
    c.push_back(data("random", "Random", "A number somewhere between Min and Max, new every time it is read.",
                     {N("Min", 0.0f), N("Max", 1.0f)}, {N("")}, [](Gen& g, const Node& n, int) {
                         const std::string a = g.in(n, 0);
                         return a + " + math.random() * (" + g.in(n, 1) + " - " + a + ")";
                     }));
    c.push_back(data("time", "Time", "Seconds since the game started, and the length of this frame.", {},
                     {N("Time"), N("Delta")},
                     [](Gen&, const Node&, int pin) { return std::string(pin == 1 ? "DT" : "T"); }));
    c.push_back(data("wave", "Wave", "A number that swings from -Size to +Size and back, Speed times a second.",
                     {N("Speed", 1.0f), N("Size", 1.0f)}, {N("")}, [](Gen& g, const Node& n, int) {
                         return "math.sin(T * " + g.in(n, 0) + " * 6.2831853) * " + g.in(n, 1);
                     }));
    c.push_back(data("key_held", "Key Held", "Yes while the key is held down.", {K("Key", kKeySpace)}, {B("")},
                     [](Gen& g, const Node& n, int) { return "game.keyDown(" + g.in(n, 0) + ")"; }));
    c.push_back(data("join_text", "Join Text", "Two pieces of text one after the other (numbers become text).",
                     {T("A", "Score: "), T("B", "")}, {T("")},
                     [](Gen& g, const Node& n, int) { return g.in(n, 0) + " .. " + g.in(n, 1); }));
    c.push_back(data("score", "Score", "The game's score.", {}, {N("Score")},
                     [](Gen&, const Node&, int) { return std::string("game.getScore()"); }));
    c.push_back(data("is_visible", "Is Visible", "Yes if the object is shown (it and everything above it).", {En()},
                     {B("")}, [](Gen& g, const Node& n, int) { return "game.isActive(" + g.in(n, 0) + ") == true"; }));
    c.push_back(data("anim_state", "Animation State", "The state the object's Animation Graph is in now.", {En()},
                     {T("State")}, [](Gen& g, const Node& n, int) { return "game.animState(" + g.in(n, 0) + ") or \"\""; }));
    return c;
}

} // namespace

const std::vector<NodeDef>& catalogue() {
    static const std::vector<NodeDef> c = buildCatalogue();
    return c;
}

const NodeDef* findDef(const std::string& type) {
    for (const NodeDef& d : catalogue())
        if (d.type == type) return &d;
    return nullptr;
}

// --- The graph ----------------------------------------------------------------------

Node makeNode(Graph& g, const std::string& type, glm::vec2 pos) {
    Node n;
    n.id   = g.nextId++;
    n.type = type;
    n.pos  = pos;
    if (const NodeDef* d = findDef(type))
        for (const PinDef& p : d->ins) n.values.push_back(p.def);
    return n;
}

Node* findNode(Graph& g, int id) {
    for (Node& n : g.nodes)
        if (n.id == id) return &n;
    return nullptr;
}
const Node* findNode(const Graph& g, int id) {
    for (const Node& n : g.nodes)
        if (n.id == id) return &n;
    return nullptr;
}

int findVar(const Graph& g, const std::string& name) {
    for (int i = 0; i < static_cast<int>(g.vars.size()); ++i)
        if (g.vars[static_cast<std::size_t>(i)].name == name) return i;
    return -1;
}

Kind pinKind(const Graph& g, const Node& n, bool input, int pin) {
    const NodeDef* d = findDef(n.type);
    if (!d) return Kind::Any;
    const auto& pins = input ? d->ins : d->outs;
    if (pin < 0 || pin >= static_cast<int>(pins.size())) return Kind::Any;
    const Kind k = pins[static_cast<std::size_t>(pin)].kind;
    if (k != Kind::Any) return k;
    // A variable node's value is of the variable's kind.
    const int vi = findVar(g, varOf(n));
    if (vi >= 0) return g.vars[static_cast<std::size_t>(vi)].kind;
    return Kind::Any;
}

namespace {
// Would a data wire from `from` into `to` make a value depend on itself?
// Follows the data inputs of value nodes upstream from `from`.
bool dataLoop(const Graph& g, int from, int to) {
    std::vector<int> todo{from};
    std::set<int>    seen;
    while (!todo.empty()) {
        const int id = todo.back();
        todo.pop_back();
        if (id == to) return true;
        if (!seen.insert(id).second) continue;
        const Node*    n = findNode(g, id);
        const NodeDef* d = n ? findDef(n->type) : nullptr;
        if (!d || d->cat != Category::Data) continue;   // an action's output is stored, not recomputed
        for (const Link& l : g.links)
            if (l.toNode == id) todo.push_back(l.fromNode);
    }
    return false;
}
} // namespace

bool connect(Graph& g, const Link& l, std::string* why) {
    auto refuse = [&](const char* w) {
        if (why) *why = w;
        return false;
    };
    const Node* a = findNode(g, l.fromNode);
    const Node* b = findNode(g, l.toNode);
    const NodeDef* da = a ? findDef(a->type) : nullptr;
    const NodeDef* db = b ? findDef(b->type) : nullptr;
    if (!da || !db) return refuse("No such node.");
    if (l.fromNode == l.toNode) return refuse("A node cannot feed itself.");
    if (l.fromPin < 0 || l.fromPin >= static_cast<int>(da->outs.size()) || l.toPin < 0 ||
        l.toPin >= static_cast<int>(db->ins.size()))
        return refuse("No such pin.");
    const Kind ka = pinKind(g, *a, false, l.fromPin);
    const Kind kb = pinKind(g, *b, true, l.toPin);
    if (!canConnect(ka, kb)) {
        if (ka == Kind::Exec || kb == Kind::Exec) return refuse("A white (what happens next) pin connects only to another white pin.");
        return refuse("These two pins carry different kinds of value.");
    }
    if (kb != Kind::Exec && db->cat == Category::Data && dataLoop(g, l.fromNode, l.toNode))
        return refuse("That would make a value out of itself.");
    // An exec output leads to ONE place; a data input takes ONE value.
    g.links.erase(std::remove_if(g.links.begin(), g.links.end(),
                                 [&](const Link& o) {
                                     if (ka == Kind::Exec) return o.fromNode == l.fromNode && o.fromPin == l.fromPin;
                                     return o.toNode == l.toNode && o.toPin == l.toPin;
                                 }),
                  g.links.end());
    g.links.push_back(l);
    return true;
}

void removeNode(Graph& g, int id) {
    g.links.erase(std::remove_if(g.links.begin(), g.links.end(),
                                 [&](const Link& l) { return l.fromNode == id || l.toNode == id; }),
                  g.links.end());
    g.nodes.erase(std::remove_if(g.nodes.begin(), g.nodes.end(), [&](const Node& n) { return n.id == id; }),
                  g.nodes.end());
}

void disconnect(Graph& g, int id, bool input, int pin) {
    g.links.erase(std::remove_if(g.links.begin(), g.links.end(),
                                 [&](const Link& l) {
                                     return input ? (l.toNode == id && l.toPin == pin)
                                                  : (l.fromNode == id && l.fromPin == pin);
                                 }),
                  g.links.end());
}

void autoLayout(Graph& g) {
    // Columns: an event at 0, each step it sets off one further right (first
    // reached wins, so a Delay wired back to the start does not push the graph
    // off to infinity); value nodes one column left of the first node reading
    // them, so a value stands in front of what it feeds.
    std::map<int, int> col;
    std::vector<int>   queue;
    for (const Node& n : g.nodes)
        if (const NodeDef* d = findDef(n.type); d && d->cat == Category::Event) {
            col[n.id] = 0;
            queue.push_back(n.id);
        }
    for (std::size_t qi = 0; qi < queue.size(); ++qi) {
        const int from = queue[qi];
        for (const Link& l : g.links) {
            if (l.fromNode != from || col.count(l.toNode)) continue;
            const Node* to = findNode(g, l.toNode);
            const NodeDef* d = to ? findDef(to->type) : nullptr;
            if (!d || d->cat == Category::Data) continue;
            col[l.toNode] = col[from] + 1;
            queue.push_back(l.toNode);
        }
    }
    for (const Node& n : g.nodes)
        if (const NodeDef* d = findDef(n.type); !col.count(n.id) && d && d->cat != Category::Data) col[n.id] = 0;
    for (int pass = 0; pass < 8; ++pass)
        for (const Node& n : g.nodes) {
            const NodeDef* d = findDef(n.type);
            if (!d || d->cat != Category::Data) continue;
            int best = 1 << 20;
            for (const Link& l : g.links)
                if (l.fromNode == n.id && col.count(l.toNode)) best = std::min(best, col[l.toNode] - 1);
            col[n.id] = best == (1 << 20) ? 0 : best;
        }
    int lo = 0;
    for (const auto& [id, c] : col) lo = std::min(lo, c);
    // Rows: in each column, in the order the nodes stood before, stacked.
    std::map<int, std::vector<Node*>> byCol;
    for (Node& n : g.nodes) byCol[col[n.id] - lo].push_back(&n);
    for (auto& [c, list] : byCol) {
        std::stable_sort(list.begin(), list.end(), [](const Node* a, const Node* b) { return a->pos.y < b->pos.y; });
        float y = 0.0f;
        for (Node* n : list) {
            const NodeDef* d = findDef(n->type);
            const std::size_t rows = d ? std::max(d->ins.size(), d->outs.size()) : 1;
            n->pos = glm::vec2(static_cast<float>(c) * 264.0f, y);
            y += 40.0f + static_cast<float>(rows) * 22.0f + 36.0f;
        }
    }
}

// --- Compiling --------------------------------------------------------------------

namespace {

// The part of every compiled behaviour that is the same: the frame's context,
// the tables the nodes keep things in, and the helpers they call. Kept short
// and plain on purpose -- it is the first thing "Show Lua" shows.
const char* kPrelude = R"LUA(local E, DT, T, STEPS = nil, 0, 0, 0
local O, F, ST, TIMERS, FOUND = {}, {}, {}, {}, {}
_pulse = {}                          -- node id -> when it last ran (the editor lights it)
local PULSE = _pulse

local function V(x, y, z) return {x = x or 0, y = y or 0, z = z or 0} end
local function VADD(a, b) return V(a.x + b.x, a.y + b.y, a.z + b.z) end
local function VSUB(a, b) return V(a.x - b.x, a.y - b.y, a.z - b.z) end
local function VMUL(a, s) return V(a.x * s, a.y * s, a.z * s) end
local function VLEN(a) return math.sqrt(a.x * a.x + a.y * a.y + a.z * a.z) end
local function VNORM(a) local l = VLEN(a) if l < 1e-6 then return V() end return VMUL(a, 1 / l) end
local function YAW(v, deg)           -- turned round the up axis: +Z becomes the heading
    local r = math.rad(deg)
    local c, s = math.cos(r), math.sin(r)
    return V(v.x * c + v.z * s, v.y, -v.x * s + v.z * c)
end
local function DIV(a, b) if b == 0 then return 0 end return a / b end
local function MOD(a, b) if b == 0 then return 0 end return a % b end
local function PICK(c, a, b) if c then return a end return b end
local function TXT(v)
    if v == nil then return "" end
    if type(v) == "table" then return string.format("%.2f, %.2f, %.2f", v.x or 0, v.y or 0, v.z or 0) end
    if type(v) == "number" then
        if v == math.floor(v) and math.abs(v) < 1e15 then return string.format("%d", math.floor(v)) end
        return string.format("%.2f", v)
    end
    return tostring(v)
end

local function STEP(id)
    STEPS = STEPS + 1
    if STEPS > 10000 then error("more than 10000 steps in one frame -- a loop with no Delay in it?", 0) end
    PULSE[id] = T
end

-- Objects. The behaviour's own object is read and written through E, the
-- table Play hands it (its LOCAL transform); any other through game.*.
local function FIND(name)
    local id = FOUND[name]
    if id and id >= 0 and game.getPos(id) then return id end
    id = game.find(name) or -2
    FOUND[name] = id
    return id
end
local function MINE(id) return id == nil or id == E.id end
local function POS(id) if MINE(id) then return V(E.x, E.y, E.z) end return V(game.getPos(id)) end
local function WPOS(id) local x, y, z = game.getPos(id) if x then return V(x, y, z) end return POS(id) end
local function SETPOS(id, p) if MINE(id) then E.x, E.y, E.z = p.x, p.y, p.z elseif id >= 0 then game.setPos(id, p.x, p.y, p.z) end end
local function ROT(id) if MINE(id) then return V(E.rx, E.ry, E.rz) end return V(game.getRot(id)) end
local function SETROT(id, r) if MINE(id) then E.rx, E.ry, E.rz = r.x, r.y, r.z elseif id >= 0 then game.setRot(id, r.x, r.y, r.z) end end
local function SETSCALE(id, s) if MINE(id) then E.sx, E.sy, E.sz = s.x, s.y, s.z elseif id >= 0 then game.setScale(id, s.x, s.y, s.z) end end
local function PLAYER() return V(game.cameraPos()) end
local function LOOK(id, t, speed)
    local p, r = POS(id), ROT(id)
    if math.abs(t.x - p.x) + math.abs(t.z - p.z) < 1e-4 then return end
    local want = math.deg(math.atan(t.x - p.x, t.z - p.z))
    if speed > 0 then
        local d = (want - r.y + 180) % 360 - 180
        local m = speed * DT
        want = r.y + math.max(-m, math.min(m, d))
    end
    SETROT(id, V(r.x, want, r.z))
end
local function DESTROY(id, after)
    if id < 0 then return end
    if after > 0 then game.destroy(id, after) else game.destroy(id) end
end
local function SOUND(name, volume, here)
    if here then
        local x, y, z = game.getPos(E.id)
        if x then game.sound(name, volume, 1, x, y, z) return end
    end
    game.sound(name, volume, 1)
end
local function CLICKED()
    if not game.mousePressed(0) then return false end
    local ox, oy, oz, dx, dy, dz = game.mouseRay()
    if not ox then return false end
    local hit = game.raycast(ox, oy, oz, dx, dy, dz)
    while hit and hit >= 0 do
        if hit == E.id then return true end
        hit = game.getParent(hit)
    end
    return false
end

-- Time: a Delay waits here. Reached again while it waits, it keeps the first wait.
local function AFTER(key, seconds, fn)
    if TIMERS[key] then return end
    TIMERS[key] = {T + seconds, fn}
end
local function TICK()
    if next(TIMERS) == nil then return end
    local due = {}
    for k, tm in pairs(TIMERS) do if T >= tm[1] then due[#due + 1] = k end end
    table.sort(due, function(a, b) return TIMERS[a][1] < TIMERS[b][1] or (TIMERS[a][1] == TIMERS[b][1] and a < b) end)
    for _, k in ipairs(due) do
        local tm = TIMERS[k]
        TIMERS[k] = nil
        tm[2]()
    end
end

-- Messages between behaviours: one mailbox per object in `shared`, emptied on
-- the frame after a message is sent.
local function MAILBOX()
    shared._behaviourMail = shared._behaviourMail or {}
    local box = shared._behaviourMail[E.id]
    if not box then box = {} shared._behaviourMail[E.id] = box end
    return box
end
local function SEND(to, msg)
    shared._behaviourMail = shared._behaviourMail or {}
    local all = shared._behaviourMail
    if to == -1 then
        for _, box in pairs(all) do box[#box + 1] = {msg, E.id} end
    elseif to >= 0 then
        all[to] = all[to] or {}
        local box = all[to]
        box[#box + 1] = {msg, E.id}
    end
end
)LUA";

std::string varLiteral(const Variable& v) {
    switch (v.kind) {
        case Kind::Bool:   return v.def.b ? "true" : "false";
        case Kind::Text:   return quote(v.def.text);
        case Kind::Vector: return "V(" + num(v.def.vec.x) + ", " + num(v.def.vec.y) + ", " + num(v.def.vec.z) + ")";
        default:           return num(v.def.num);
    }
}

} // namespace

Compiled compile(const Graph& g, const std::string& label) {
    Gen gen(g);
    std::string fns;
    // Node order decides the order events run in on one frame: the order they
    // were added, which is also the order the graph lists them in.
    for (const Node& n : g.nodes) {
        const NodeDef* d = findDef(n.type);
        if (!d) {
            gen.warn("A node of a kind this editor does not know (\"" + n.type + "\") is skipped.");
            continue;
        }
        if (d->cat == Category::Event) {
            bool wired = false;
            for (const Link& l : g.links)
                if (l.fromNode == n.id) wired = true;
            if (!wired) {
                gen.warn(d->title + " does nothing yet: wire one of its outputs to something.");
                continue;
            }
            d->event(gen, n);
            continue;
        }
        if (!d->exec) continue;
        bool reached = false;
        for (int pin = 0; pin < static_cast<int>(d->ins.size()); ++pin) {
            if (d->ins[static_cast<std::size_t>(pin)].kind != Kind::Exec) continue;
            bool linked = false;
            for (const Link& l : g.links)
                if (l.toNode == n.id && l.toPin == pin) linked = true;
            if (!linked) continue;
            reached = true;
            fns += "F.n" + std::to_string(n.id) + "_" + std::to_string(pin) + " = function() -- " + d->title + "\n" +
                   "    STEP(" + std::to_string(n.id) + ")\n" + indent(d->exec(gen, n, pin), 1) + "end\n";
        }
        if (!reached) gen.warn(d->title + " never runs: nothing leads to it (wire a white output into it).");
    }

    std::string s;
    s += "-- Behaviour of " + label + ", compiled from its graph at Play. Edit the graph, not this:\n";
    s += "-- it is written afresh every time the game starts.\n";
    s += "local LABEL = " + quote(label) + "\n";
    s += kPrelude;
    s += "\n-- The behaviour's variables (the editor reads them while it runs).\n_vars = {\n";
    for (const Variable& v : g.vars) s += "    [" + quote(v.name) + "] = " + varLiteral(v) + ",\n";
    s += "}\nlocal VARS = _vars\n\n-- The nodes. Each runs when a white wire reaches it.\n";
    s += fns;
    // Only a behaviour that listens has a mailbox: one that does not would
    // collect every message sent to everyone, for ever.
    const bool listens = !gen.mailCode.empty();
    s += "\nfunction start(e, dt, t)\n    E, DT, T, STEPS = e, dt, t, 0\n" + std::string(listens ? "    MAILBOX()\n" : "") +
         indent(gen.startCode, 1) +
         "end\n\n";
    s += "function update(e, dt, t)\n    E, DT, T, STEPS = e, dt, t, 0\n    _now = t\n    TICK()\n";
    if (listens) {
        s += "    local box = MAILBOX()\n    if #box > 0 then\n        shared._behaviourMail[E.id] = {}\n"
             "        for _, m in ipairs(box) do\n" +
             indent(gen.mailCode, 3) + "        end\n    end\n";
    } else {
        s += "    if shared._behaviourMail then shared._behaviourMail[E.id] = nil end -- nobody listens here\n";
    }
    s += indent(gen.updateCode, 1) + "end\n";
    return {std::move(s), std::move(gen.warnings)};
}

// --- Persistence ------------------------------------------------------------------

namespace {

const char* varKindName(Kind k) {
    switch (k) {
        case Kind::Bool:   return "bool";
        case Kind::Text:   return "text";
        case Kind::Vector: return "vector";
        default:           return "number";
    }
}
Kind varKindOf(const std::string& s) {
    if (s == "bool") return Kind::Bool;
    if (s == "text") return Kind::Text;
    if (s == "vector") return Kind::Vector;
    return Kind::Number;
}

// Only what differs from nothing: a value is mostly zero in all but one field.
nlohmann::json saveValue(const Value& v) {
    nlohmann::json j = nlohmann::json::object();
    if (v.num != 0.0f) j["n"] = v.num;
    if (v.b) j["b"] = true;
    if (v.vec != glm::vec3(0.0f)) j["v"] = {v.vec.x, v.vec.y, v.vec.z};
    if (!v.text.empty()) j["t"] = v.text;
    if (v.choice != 0) j["c"] = v.choice;
    return j;
}
Value loadValue(const nlohmann::json& j) {
    Value v;
    if (!j.is_object()) return v;
    v.num    = j.value("n", 0.0f);
    v.b      = j.value("b", false);
    v.text   = j.value("t", std::string());
    v.choice = j.value("c", 0);
    if (j.contains("v") && j["v"].is_array() && j["v"].size() == 3)
        v.vec = glm::vec3(j["v"][0].get<float>(), j["v"][1].get<float>(), j["v"][2].get<float>());
    return v;
}

} // namespace

void save(nlohmann::json& j, const Graph& g) {
    nlohmann::json nodes = nlohmann::json::array();
    for (const Node& n : g.nodes) {
        nlohmann::json nj;
        nj["id"]   = n.id;
        nj["type"] = n.type;
        nj["pos"]  = {n.pos.x, n.pos.y};
        nlohmann::json vals = nlohmann::json::array();
        for (const Value& v : n.values) vals.push_back(saveValue(v));
        nj["values"] = std::move(vals);
        nodes.push_back(std::move(nj));
    }
    nlohmann::json links = nlohmann::json::array();
    for (const Link& l : g.links) links.push_back({l.fromNode, l.fromPin, l.toNode, l.toPin});
    nlohmann::json vars = nlohmann::json::array();
    for (const Variable& v : g.vars)
        vars.push_back({{"name", v.name}, {"kind", varKindName(v.kind)}, {"value", saveValue(v.def)}});
    j["nodes"]  = std::move(nodes);
    j["links"]  = std::move(links);
    j["vars"]   = std::move(vars);
    j["nextId"] = g.nextId;
}

void load(const nlohmann::json& j, Graph& g) {
    g = Graph{};
    int maxId = 0;
    if (j.contains("nodes") && j["nodes"].is_array())
        for (const auto& nj : j["nodes"]) {
            Node n;
            n.id   = nj.value("id", 0);
            n.type = nj.value("type", std::string());
            if (nj.contains("pos") && nj["pos"].is_array() && nj["pos"].size() == 2)
                n.pos = glm::vec2(nj["pos"][0].get<float>(), nj["pos"][1].get<float>());
            if (nj.contains("values") && nj["values"].is_array())
                for (const auto& vj : nj["values"]) n.values.push_back(loadValue(vj));
            // A node type that has gained inputs since the graph was saved gets
            // them at their defaults; one that lost some keeps the extras unread.
            if (const NodeDef* d = findDef(n.type))
                for (std::size_t i = n.values.size(); i < d->ins.size(); ++i) n.values.push_back(d->ins[i].def);
            maxId = std::max(maxId, n.id);
            g.nodes.push_back(std::move(n));
        }
    if (j.contains("links") && j["links"].is_array())
        for (const auto& lj : j["links"])
            if (lj.is_array() && lj.size() == 4)
                g.links.push_back({lj[0].get<int>(), lj[1].get<int>(), lj[2].get<int>(), lj[3].get<int>()});
    if (j.contains("vars") && j["vars"].is_array())
        for (const auto& vj : j["vars"]) {
            Variable v;
            v.name = vj.value("name", std::string());
            v.kind = varKindOf(vj.value("kind", std::string("number")));
            if (vj.contains("value")) v.def = loadValue(vj["value"]);
            g.vars.push_back(std::move(v));
        }
    g.nextId = std::max(j.value("nextId", 1), maxId + 1);
}

// --- Templates ----------------------------------------------------------------------

namespace {

// A small builder for the templates: nodes by type at grid positions, wires by
// (node, pin) pairs. Pins are numbered as the defs list them.
struct Build {
    Graph g;
    int add(const char* type, float col, float row) {
        g.nodes.push_back(makeNode(g, type, glm::vec2(col * 264.0f, row * 120.0f)));
        return g.nodes.back().id;
    }
    Value& val(int node, int pin) { return findNode(g, node)->values[static_cast<std::size_t>(pin)]; }
    void wire(int a, int ap, int b, int bp) { connect(g, {a, ap, b, bp}); }
    void var(const char* name, Kind k, Value v = {}) { g.vars.push_back({name, k, std::move(v)}); }
};

std::vector<Template> buildTemplates() {
    std::vector<Template> t;
    t.push_back({"Turn round", "Turns the object round its up axis, all the time.", [] {
                     Build b;
                     const int up = b.add("on_update", 0, 0);
                     const int rot = b.add("rotate_by", 1, 0);
                     b.val(rot, 2).vec = glm::vec3(0.0f, 45.0f, 0.0f);
                     b.wire(up, 0, rot, 0);
                     return b.g;
                 }});
    t.push_back({"Bob up and down", "Floats up and down round the place it started.", [] {
                     Build b;
                     b.var("home", Kind::Vector);
                     const int st  = b.add("on_start", 0, 0);
                     const int pos = b.add("position_of", 0, 1);
                     const int set = b.add("set_variable", 1, 0);
                     b.val(set, 1).text = "home";
                     b.wire(st, 0, set, 0);
                     b.wire(pos, 0, set, 2);
                     const int up   = b.add("on_update", 0, 3);
                     const int home = b.add("get_variable", 0, 4);
                     b.val(home, 0).text = "home";
                     const int wave = b.add("wave", 0, 5);
                     b.val(wave, 0).num = 0.5f;
                     b.val(wave, 1).num = 0.25f;
                     const int mk = b.add("make_vector", 1, 5);
                     b.wire(wave, 0, mk, 1);
                     const int add = b.add("vector_math", 2, 4);
                     b.wire(home, 0, add, 0);
                     b.wire(mk, 0, add, 2);
                     const int sp = b.add("set_position", 3, 3);
                     b.wire(up, 0, sp, 0);
                     b.wire(add, 0, sp, 2);
                     return b.g;
                 }});
    t.push_back({"Follow the player", "Turns to the player and walks up to them when they come within 12 m.", [] {
                     Build b;
                     const int up   = b.add("on_update", 0, 0);
                     const int pp   = b.add("player_pos", 0, 1);
                     const int self = b.add("position_of", 0, 2);
                     const int dist = b.add("distance", 1, 2);
                     b.wire(pp, 0, dist, 0);
                     b.wire(self, 0, dist, 1);
                     const int cmp = b.add("compare", 2, 2);
                     b.val(cmp, 1).choice = 1;   // <
                     b.val(cmp, 2).num    = 12.0f;
                     b.wire(dist, 0, cmp, 0);
                     const int br = b.add("branch", 3, 0);
                     b.wire(up, 0, br, 0);
                     b.wire(cmp, 0, br, 1);
                     const int look = b.add("look_at", 4, 0);
                     b.val(look, 3).num = 180.0f;
                     b.wire(br, 0, look, 0);
                     b.wire(pp, 0, look, 2);
                     const int mv = b.add("move_toward", 5, 0);
                     b.val(mv, 3).num = 2.5f;
                     b.wire(look, 0, mv, 0);
                     b.wire(pp, 0, mv, 2);
                     return b.g;
                 }});
    t.push_back({"Patrol", "Walks to a point 6 m ahead and back again, for ever, pausing at each end.", [] {
                     Build b;
                     b.var("a", Kind::Vector);
                     b.var("b", Kind::Vector);
                     b.var("target", Kind::Vector);
                     Value moving;
                     moving.b = true;
                     b.var("moving", Kind::Bool, moving);
                     // At the start: a = here, b = 6 m along +Z, go to b.
                     const int st   = b.add("on_start", 0, 0);
                     const int here = b.add("position_of", 0, 1);
                     const int seq  = b.add("sequence", 1, 0);
                     b.wire(st, 0, seq, 0);
                     const int setA = b.add("set_variable", 2, 0);
                     b.val(setA, 1).text = "a";
                     b.wire(seq, 0, setA, 0);
                     b.wire(here, 0, setA, 2);
                     const int off  = b.add("vector_math", 1, 2);
                     b.val(off, 2).vec = glm::vec3(0.0f, 0.0f, 6.0f);
                     b.wire(here, 0, off, 0);
                     const int setB = b.add("set_variable", 2, 1);
                     b.val(setB, 1).text = "b";
                     b.wire(seq, 1, setB, 0);
                     b.wire(off, 0, setB, 2);
                     const int setT = b.add("set_variable", 2, 2);
                     b.val(setT, 1).text = "target";
                     b.wire(seq, 2, setT, 0);
                     b.wire(off, 0, setT, 2);
                     // Every frame: walk while moving; at the end, wait, turn round.
                     const int up   = b.add("on_update", 0, 4);
                     const int mov  = b.add("get_variable", 0, 5);
                     b.val(mov, 0).text = "moving";
                     const int br   = b.add("branch", 1, 4);
                     b.wire(up, 0, br, 0);
                     b.wire(mov, 0, br, 1);
                     const int tgt  = b.add("get_variable", 1, 6);
                     b.val(tgt, 0).text = "target";
                     const int mv   = b.add("move_toward", 2, 4);
                     b.wire(br, 0, mv, 0);
                     b.wire(tgt, 0, mv, 2);
                     const int stop = b.add("set_variable", 3, 4);
                     b.val(stop, 1).text = "moving";
                     b.val(stop, 2).b    = false;
                     b.wire(mv, 1, stop, 0);
                     const int wait = b.add("delay", 4, 4);
                     b.wire(stop, 0, wait, 0);
                     const int ff   = b.add("flip_flop", 5, 4);
                     b.wire(wait, 0, ff, 0);
                     const int ga   = b.add("get_variable", 5, 6);
                     b.val(ga, 0).text = "a";
                     const int gb   = b.add("get_variable", 5, 7);
                     b.val(gb, 0).text = "b";
                     const int toA  = b.add("set_variable", 6, 4);
                     b.val(toA, 1).text = "target";
                     b.wire(ff, 0, toA, 0);
                     b.wire(ga, 0, toA, 2);
                     const int toB  = b.add("set_variable", 6, 5);
                     b.val(toB, 1).text = "target";
                     b.wire(ff, 1, toB, 0);
                     b.wire(gb, 0, toB, 2);
                     const int goA  = b.add("set_variable", 7, 4);
                     b.val(goA, 1).text = "moving";
                     b.val(goA, 2).b    = true;
                     b.wire(toA, 0, goA, 0);
                     const int goB  = b.add("set_variable", 7, 5);
                     b.val(goB, 1).text = "moving";
                     b.val(goB, 2).b    = true;
                     b.wire(toB, 0, goB, 0);
                     // ...and the walker faces where it goes.
                     const int look = b.add("look_at", 3, 6);
                     b.val(look, 3).num = 360.0f;
                     b.wire(mv, 0, look, 0);
                     b.wire(tgt, 0, look, 2);
                     return b.g;
                 }});
    t.push_back({"Click to collect", "A click on the object scores a point, plays a sound and takes it away.", [] {
                     Build b;
                     const int click = b.add("on_click", 0, 0);
                     const int score = b.add("add_score", 1, 0);
                     b.wire(click, 0, score, 0);
                     const int snd = b.add("play_sound", 2, 0);
                     b.wire(score, 0, snd, 0);
                     const int gone = b.add("destroy", 3, 0);
                     b.wire(snd, 0, gone, 0);
                     return b.g;
                 }});
    t.push_back({"Blink when near", "While the player is within 4 m, the object flashes red and white.", [] {
                     Build b;
                     const int every = b.add("every", 0, 0);
                     b.val(every, 0).num = 0.4f;
                     const int near = b.add("player_near", 0, 2);
                     b.val(near, 0).num = 4.0f;
                     const int gate = b.add("gate", 1, 0);
                     b.val(gate, 3).b = false;
                     b.wire(every, 0, gate, 0);
                     b.wire(near, 0, gate, 1);
                     b.wire(near, 1, gate, 2);
                     const int ff  = b.add("flip_flop", 2, 0);
                     b.wire(gate, 0, ff, 0);
                     const int red = b.add("set_color", 3, 0);
                     b.wire(ff, 0, red, 0);
                     const int white = b.add("set_color", 3, 1);
                     b.val(white, 2).vec = glm::vec3(1.0f);
                     b.wire(ff, 1, white, 0);
                     return b.g;
                 }});
    return t;
}

} // namespace

const std::vector<Template>& templates() {
    static const std::vector<Template> t = buildTemplates();
    return t;
}

} // namespace behaviour

// --- The component ----------------------------------------------------------------------

const std::vector<Property>& BehaviourComponent::properties() {
    // Nothing for the generic inspector: the graph is edited in its own window.
    static const std::vector<Property> props;
    return props;
}
void BehaviourComponent::save(nlohmann::json& j) const { behaviour::save(j, graph); }
void BehaviourComponent::load(const nlohmann::json& j) { behaviour::load(j, graph); }

namespace {
struct RegisterBehaviour {
    RegisterBehaviour() {
        components::registerType({"behaviour", "Behaviour",
                                  [] { return std::unique_ptr<ComponentBase>(std::make_unique<BehaviourComponent>()); }});
    }
} g_registerBehaviour;
} // namespace
