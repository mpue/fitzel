// behaviourcheck -- does a behaviour graph do what its picture says?
//
// A graph is compiled to Lua and run by the real ScriptSystem, so the two
// things that can go wrong are the compiler (Lua that does not load, a wire
// read as the wrong value, a node that runs twice or never) and the meaning of
// the nodes (Move Toward overshooting, a Delay that fires again while it waits,
// a message that never arrives). Neither throws in the editor: a wrong graph
// simply makes an object stand still. So every template and a set of graphs
// built here are played out against a small world in code -- objects with
// positions, a player, a mouse, a key -- and what they did is measured.
//
// Also: the wiring rules (kinds, one wire per data input and exec output, no
// value made of itself), save and load giving the same graph back, a runaway
// loop stopping only its own behaviour, and the live readout the editor shows.
//
//   build/linux/bin/behaviourcheck
// Exits non-zero if any check fails.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Behaviour.hpp"
#include "Component.hpp"
#include "SceneTypes.hpp"
#include "ScriptHost.hpp"
#include "ScriptSystem.hpp"

namespace {

int g_fails = 0;

void check(bool ok, const std::string& what, const std::string& detail = {}) {
    if (ok) std::printf("  ok   %s%s%s\n", what.c_str(), detail.empty() ? "" : " -- ", detail.c_str());
    else {
        std::printf("[FAIL] %s%s%s\n", what.c_str(), detail.empty() ? "" : " -- ", detail.c_str());
        ++g_fails;
    }
}

std::string f3(glm::vec3 v) {
    char b[96];
    std::snprintf(b, sizeof(b), "(%.2f, %.2f, %.2f)", v.x, v.y, v.z);
    return b;
}

using namespace behaviour;

// A world of a few root objects (local == world), a player at the camera, a
// mouse and a keyboard, run at 60 frames a second.
struct World {
    std::vector<Entity> ents;
    ScriptHost          host;
    ScriptSystem        scripts;
    std::vector<std::string> logs;
    std::vector<int>    destroyed;
    std::vector<std::pair<int, glm::vec3>> colours;
    int   pressKey   = -1;   // keyPressed this frame
    int   holdKey    = -1;
    bool  click      = false;
    int   clickHit   = -1;   // what the mouse ray hits
    float t          = 0.0f;
    int   nextId     = 100;

    Entity* find(int id) {
        for (Entity& e : ents)
            if (e.id == id) return &e;
        return nullptr;
    }

    World() {
        host.getPos = [this](int id, glm::vec3& p) {
            Entity* e = find(id);
            if (!e) return false;
            p = e->localCenter;
            return true;
        };
        host.setPos = [this](int id, glm::vec3 p) {
            if (Entity* e = find(id)) e->localCenter = p;
        };
        host.getRot = [this](int id, glm::vec3& r) {
            Entity* e = find(id);
            if (!e) return false;
            r = e->localRotation;
            return true;
        };
        host.setRot = [this](int id, glm::vec3 r) {
            if (Entity* e = find(id)) e->localRotation = r;
        };
        host.findEntity = [this](const std::string& n) {
            for (const Entity& e : ents)
                if (e.name == n) return e.id;
            return -1;
        };
        host.entityInfo = [this](int id, ScriptEntityInfo& info) {
            Entity* e = find(id);
            if (!e) return false;
            info.id = id;
            info.parent = e->parent;
            info.name = e->name;
            return true;
        };
        host.log = [this](const std::string& s) { logs.push_back(s); };
        host.keyPressed = [this](int k) { return k == pressKey; };
        host.keyDown = [this](int k) { return k == holdKey || k == pressKey; };
        host.mousePressed = [this](int b) { return b == 0 && click; };
        host.mouseRay = [](glm::vec3& o, glm::vec3& d) {
            o = glm::vec3(0.0f, 5.0f, -10.0f);
            d = glm::vec3(0.0f, 0.0f, 1.0f);
            return true;
        };
        host.raycast = [this](glm::vec3, glm::vec3, float, glm::vec3& hit, float& dist) {
            hit = glm::vec3(0.0f);
            dist = 1.0f;
            return clickHit;
        };
        host.destroy = [this](int id) { destroyed.push_back(id); };
        host.destroyAfter = [this](int id, float) { destroyed.push_back(id); };
        host.setEntityColor = [this](int id, glm::vec3 c) {
            colours.emplace_back(id, c);
            return true;
        };
        host.playSound = [](const std::string&) {};
        host.playSoundEx = [](const std::string&, float, float, const glm::vec3*, float, float,
                              const std::string&) {};
        host.camPos = glm::vec3(0.0f, 1.7f, 30.0f);
        scripts.setHost(&host);
    }

    Entity& add(const std::string& name, glm::vec3 at, Graph g = {}) {
        Entity e;
        e.id          = nextId++;
        e.name        = name;
        e.localCenter = e.center = at;
        e.half        = glm::vec3(0.5f);
        if (!g.nodes.empty()) {
            auto bc   = std::make_unique<BehaviourComponent>();
            bc->graph = std::move(g);
            e.components.items.push_back(std::move(bc));
        }
        ents.push_back(std::move(e));
        return ents.back();
    }

    void frame(float dt = 1.0f / 60.0f) {
        host.camDir = glm::vec3(0.0f, 0.0f, -1.0f);
        scripts.beginFrame();
        for (Entity& e : ents)
            if (auto* bc = e.components.get<BehaviourComponent>()) {
                const Graph& g = bc->graph;
                const std::string label = e.name;
                scripts.updateSource(e, "Behaviour", [&] { return compile(g, label).lua; }, dt, t);
            }
        t += dt;
        pressKey = -1;
        click    = false;
    }
    void run(float seconds) {
        for (float end = t + seconds - 1e-4f; t < end;) frame();
    }
};

Graph templateNamed(const char* name) {
    for (const Template& t : templates())
        if (std::string(t.name) == name) return t.make();
    std::printf("[FAIL] no template named %s\n", name);
    ++g_fails;
    return {};
}

int add(Graph& g, const char* type, float x = 0.0f, float y = 0.0f) {
    g.nodes.push_back(makeNode(g, type, glm::vec2(x, y)));
    return g.nodes.back().id;
}
Value& val(Graph& g, int node, int pin) { return findNode(g, node)->values[static_cast<std::size_t>(pin)]; }
bool wire(Graph& g, int a, int ap, int b, int bp, std::string* why = nullptr) { return connect(g, {a, ap, b, bp}, why); }

} // namespace

int main() {
    std::printf("behaviourcheck: behaviour graphs compiled to Lua and played out\n");

    // --- The catalogue -----------------------------------------------------------
    {
        std::printf("-- the catalogue\n");
        int bad = 0;
        std::vector<std::string> seen;
        for (const NodeDef& d : catalogue()) {
            const bool hasOne = (d.cat == Category::Event && d.event) ||
                                ((d.cat == Category::Flow || d.cat == Category::Action) && d.exec) ||
                                (d.cat == Category::Data && d.expr);
            if (!hasOne || std::find(seen.begin(), seen.end(), d.type) != seen.end()) {
                std::printf("       bad def: %s\n", d.type.c_str());
                ++bad;
            }
            seen.push_back(d.type);
        }
        check(bad == 0, "every node type is unique and has its code", std::to_string(catalogue().size()) + " types");

        // Every node on its own, every input unwired, wired from an event where
        // it has an exec input -- the Lua must at least load and run a frame.
        World w;
        Graph g;
        g.vars.push_back({"v", Kind::Number, {}});
        const int up = add(g, "on_update");
        int prev = up, prevPin = 0, count = 0;
        for (const NodeDef& d : catalogue()) {
            if (d.type == "load_scene" || d.type == "teleport_player" || d.type == "destroy" ||
                d.type == "set_visible")
                continue;   // these reach out of the world; fine on their own, not in a chain
            const int id = add(g, d.type.c_str());
            if (d.type == "set_variable" || d.type == "add_variable" || d.type == "get_variable")
                val(g, id, d.type == "get_variable" ? 0 : 1).text = "v";
            if (!d.ins.empty() && d.ins[0].kind == Kind::Exec && d.cat != Category::Event) {
                if (wire(g, prev, prevPin, id, 0)) ++count;
                prev = id;
                prevPin = 0;
            }
        }
        const Compiled c = compile(g, "Everything");
        w.add("Everything", glm::vec3(0.0f), g);
        w.run(0.2f);
        check(w.scripts.lastError().empty(), "a chain of every action and flow node loads and runs",
              std::to_string(count) + " chained" + (w.scripts.lastError().empty() ? "" : ": " + w.scripts.lastError()));
        if (!w.scripts.lastError().empty()) {
            // The text, with line numbers, to find what the compiler wrote wrong.
            int line = 1;
            std::size_t at = 0;
            while (at < c.lua.size()) {
                const std::size_t nl = c.lua.find('\n', at);
                std::printf("%4d  %s\n", line++, c.lua.substr(at, nl - at).c_str());
                if (nl == std::string::npos) break;
                at = nl + 1;
            }
        }
    }

    // --- Templates ---------------------------------------------------------------
    {
        std::printf("-- the templates\n");
        for (const Template& t : templates()) {
            World w;
            const Graph g = t.make();
            const Compiled c = compile(g, t.name);
            w.add("Thing", glm::vec3(0.0f), g);
            w.run(1.0f);
            check(w.scripts.lastError().empty() && c.warnings.empty(), std::string("\"") + t.name + "\" runs",
                  w.scripts.lastError().empty() ? (c.warnings.empty() ? "" : c.warnings.front()) : w.scripts.lastError());
        }
    }
    {
        World w;
        Entity& e = w.add("Spinner", glm::vec3(0.0f), templateNamed("Turn round"));
        const int id = e.id;
        w.run(2.0f);
        const float ry = w.find(id)->localRotation.y;
        check(std::fabs(ry - 90.0f) < 1.5f, "Turn round turns 45 degrees a second", "after 2 s: " + std::to_string(ry));
    }
    {
        World w;
        const int id = w.add("Float", glm::vec3(1.0f, 2.0f, 3.0f), templateNamed("Bob up and down")).id;
        float lo = 1e9f, hi = -1e9f;
        for (int i = 0; i < 240; ++i) {
            w.frame();
            const glm::vec3 p = w.find(id)->localCenter;
            lo = std::min(lo, p.y);
            hi = std::max(hi, p.y);
            if (std::fabs(p.x - 1.0f) > 1e-4f || std::fabs(p.z - 3.0f) > 1e-4f) {
                check(false, "Bob up and down stays where it was across", f3(p));
                break;
            }
        }
        check(std::fabs(lo - 1.75f) < 0.02f && std::fabs(hi - 2.25f) < 0.02f,
              "Bob up and down floats 0.25 m either side of where it started",
              std::to_string(lo) + " .. " + std::to_string(hi));
    }
    {
        World w;
        const int id = w.add("Guard", glm::vec3(0.0f), templateNamed("Follow the player")).id;
        w.host.camPos = glm::vec3(0.0f, 1.7f, 30.0f);   // too far: stays
        w.run(1.0f);
        check(glm::length(w.find(id)->localCenter) < 1e-4f, "Follow the player ignores a player 30 m away");
        w.host.camPos = glm::vec3(8.0f, 1.7f, 0.0f);    // near, to its right (+X)
        w.run(1.0f);
        const Entity& g = *w.find(id);
        check(std::fabs(g.localCenter.x - 2.5f) < 0.1f && std::fabs(g.localCenter.y) < 1e-4f,
              "...walks toward one 8 m away at 2.5 m/s, keeping its height", f3(g.localCenter));
        check(std::fabs(g.localRotation.y - 90.0f) < 1.0f, "...and faces them (+X is yaw 90)",
              std::to_string(g.localRotation.y));
        w.run(4.0f);
        check(std::fabs(w.find(id)->localCenter.x - 8.0f) < 0.05f, "...and stops on arriving, not past",
              f3(w.find(id)->localCenter));
    }
    {
        World w;
        const int id = w.add("Walker", glm::vec3(0.0f), templateNamed("Patrol")).id;
        w.run(2.5f);   // 6 m at 2 m/s = 3 s
        const float z1 = w.find(id)->localCenter.z;
        check(z1 > 4.5f && z1 < 5.5f, "Patrol walks out along +Z", std::to_string(z1));
        w.run(1.0f);   // arrives at 3 s, waits a second
        const float z2 = w.find(id)->localCenter.z;
        check(std::fabs(z2 - 6.0f) < 0.05f, "...stops at the far end and waits", std::to_string(z2));
        w.run(2.5f);
        const float z3 = w.find(id)->localCenter.z;
        check(z3 < 4.0f && z3 > 0.5f, "...then walks back", std::to_string(z3));
        check(std::fabs(std::fabs(w.find(id)->localRotation.y) - 180.0f) < 2.0f, "...facing back the way it came",
              std::to_string(w.find(id)->localRotation.y));
        w.run(4.0f);
        check(w.find(id)->localCenter.z > 0.5f, "...and out again after the near end",
              std::to_string(w.find(id)->localCenter.z));
    }
    {
        World w;
        const int id = w.add("Coin", glm::vec3(0.0f), templateNamed("Click to collect")).id;
        w.run(0.2f);
        w.click    = true;
        w.clickHit = -1;
        w.frame();
        check(w.host.score == 0 && w.destroyed.empty(), "Click to collect ignores a click on nothing");
        w.click    = true;
        w.clickHit = id;
        w.frame();
        check(w.host.score == 1 && w.destroyed.size() == 1 && w.destroyed[0] == id,
              "...and a click on it scores and takes it away", "score " + std::to_string(w.host.score));
    }
    {
        World w;
        const int id = w.add("Lamp", glm::vec3(0.0f), templateNamed("Blink when near")).id;
        w.host.camPos = glm::vec3(0.0f, 1.7f, 20.0f);
        w.run(1.0f);
        check(w.colours.empty(), "Blink when near stays as it is while the player is far");
        w.host.camPos = glm::vec3(0.0f, 1.7f, 2.0f);
        w.run(2.0f);
        check(w.colours.size() >= 4 && w.colours.front().first == id, "...and flashes while they are near",
              std::to_string(w.colours.size()) + " changes in 2 s");
        const std::size_t n = w.colours.size();
        w.host.camPos = glm::vec3(0.0f, 1.7f, 20.0f);
        w.run(1.0f);
        check(w.colours.size() <= n + 1, "...and stops when they leave");
    }

    // --- Nodes worth measuring one by one ---------------------------------------
    {
        std::printf("-- nodes\n");
        // Messages: A waits half a second and sends "ping" to B; B scores.
        World w;
        Graph a;
        {
            const int st = add(a, "on_start");
            const int dl = add(a, "delay");
            val(a, dl, 1).num = 0.5f;
            const int sm = add(a, "send_message");
            val(a, sm, 1).text = "ping";
            val(a, sm, 3).text = "B";
            wire(a, st, 0, dl, 0);
            wire(a, dl, 0, sm, 0);
        }
        Graph b;
        {
            const int om = add(b, "on_message");
            val(b, om, 0).text = "ping";
            const int sc = add(b, "add_score");
            val(b, sc, 1).num = 10.0f;
            wire(b, om, 0, sc, 0);
            const int lg = add(b, "log");
            wire(b, sc, 0, lg, 0);
            const int nm = add(b, "position_of");   // From -> its position, as text
            wire(b, om, 1, nm, 0);
            wire(b, nm, 0, lg, 1);
        }
        w.add("A", glm::vec3(7.0f, 0.0f, 0.0f), a);
        w.add("B", glm::vec3(0.0f), b);
        w.run(0.4f);
        check(w.host.score == 0, "a Delay waits", "score " + std::to_string(w.host.score));
        w.run(0.3f);
        check(w.host.score == 10, "...and the message arrives", "score " + std::to_string(w.host.score));
        check(!w.logs.empty() && w.logs.back() == "B: 7.00, 0.00, 0.00", "On Message says who sent it",
              w.logs.empty() ? "(no log)" : w.logs.back());
        w.run(1.0f);
        check(w.host.score == 10, "...once");
    }
    {
        // Broadcast to everyone who listens.
        World w;
        Graph s;
        const int st = add(s, "on_start");
        const int dl = add(s, "delay");
        val(s, dl, 1).num = 0.1f;
        const int sm = add(s, "send_message");
        val(s, sm, 1).text = "go";
        val(s, sm, 2).choice = 1;
        wire(s, st, 0, dl, 0);
        wire(s, dl, 0, sm, 0);
        Graph r;
        const int om = add(r, "on_message");
        val(r, om, 0).text = "go";
        const int sc = add(r, "add_score");
        wire(r, om, 0, sc, 0);
        w.add("Sender", glm::vec3(0.0f), s);
        for (int i = 0; i < 3; ++i) w.add("Listener " + std::to_string(i), glm::vec3(0.0f), r);
        w.run(0.5f);
        check(w.host.score == 3, "a message to everyone reaches every behaviour that listens",
              "score " + std::to_string(w.host.score));
    }
    {
        // Cooldown + Add To Variable: a counter that may rise four times a second.
        World w;
        Graph g;
        g.vars.push_back({"n", Kind::Number, {}});
        const int up = add(g, "on_update");
        const int cd = add(g, "cooldown");
        val(g, cd, 1).num = 0.25f;
        const int av = add(g, "add_variable");
        val(g, av, 1).text = "n";
        wire(g, up, 0, cd, 0);
        wire(g, cd, 0, av, 0);
        const int id = w.add("Counter", glm::vec3(0.0f), g).id;
        w.run(1.0f);
        ScriptSystem::Live live;
        const bool on = w.scripts.live(id, "Behaviour", live);
        std::string n = "?";
        for (const auto& [k, v] : live.vars)
            if (k == "n") n = v;
        check(on && n == "4", "Cooldown lets through four times in a second; the editor reads the variable", n);
        bool pulsed = false;
        for (const auto& [node, at] : live.pulses)
            if (node == av && live.now - at < 0.3f) pulsed = true;
        check(pulsed, "...and sees which nodes ran, and when");
    }
    {
        // Repeat + Do Once + Gate + Branch on a Compare, all in one frame.
        World w;
        Graph g;
        g.vars.push_back({"sum", Kind::Number, {}});
        g.vars.push_back({"once", Kind::Number, {}});
        const int up = add(g, "on_update");
        const int seq = add(g, "sequence");
        wire(g, up, 0, seq, 0);
        const int rp = add(g, "repeat");
        val(g, rp, 1).num = 5.0f;
        wire(g, seq, 0, rp, 0);
        const int av = add(g, "add_variable");
        val(g, av, 1).text = "sum";
        wire(g, rp, 0, av, 0);
        wire(g, rp, 2, av, 2);   // add the index: 1+2+3+4+5
        const int dn = add(g, "do_once");
        wire(g, seq, 1, dn, 0);
        const int ao = add(g, "add_variable");
        val(g, ao, 1).text = "once";
        wire(g, dn, 0, ao, 0);
        const int id = w.add("Loops", glm::vec3(0.0f), g).id;
        w.run(0.5f);   // 30 frames
        ScriptSystem::Live live;
        w.scripts.live(id, "Behaviour", live);
        std::string sum, once;
        for (const auto& [k, v] : live.vars) {
            if (k == "sum") sum = v;
            if (k == "once") once = v;
        }
        check(sum == "450", "Repeat runs five times a frame with its index (15 x 30 frames)", sum);
        check(once == "1", "Do Once lets through the first time only", once);
    }
    {
        // On Key: pressed fires once, held every frame.
        World w;
        Graph g;
        const int k1 = add(g, "on_key");
        val(g, k1, 0).choice = 4;   // E
        const int s1 = add(g, "add_score");
        wire(g, k1, 0, s1, 0);
        const int k2 = add(g, "on_key");
        val(g, k2, 0).choice = 36;  // Space
        val(g, k2, 1).choice = 2;   // held
        const int s2 = add(g, "add_score");
        val(g, s2, 1).num = 100.0f;
        wire(g, k2, 0, s2, 0);
        w.add("Keys", glm::vec3(0.0f), g);
        w.run(0.1f);
        w.pressKey = 69;   // GLFW_KEY_E
        w.frame();
        w.run(0.1f);
        check(w.host.score == 1, "On Key (pressed) fires on the frame the key goes down", std::to_string(w.host.score));
        w.holdKey = 32;    // Space
        for (int i = 0; i < 3; ++i) w.frame();
        w.holdKey = -1;
        w.frame();
        check(w.host.score == 301, "On Key (held) fires on every frame it is held", std::to_string(w.host.score));
    }
    {
        // Move By along its own heading: turned 90 degrees, "forward" is +X.
        World w;
        Graph g;
        const int up = add(g, "on_update");
        const int mb = add(g, "move_by");
        val(g, mb, 2).vec = glm::vec3(0.0f, 0.0f, 2.0f);
        val(g, mb, 4).choice = 1;   // own
        wire(g, up, 0, mb, 0);
        Entity& e = w.add("Car", glm::vec3(0.0f), g);
        e.localRotation.y = 90.0f;
        const int id = e.id;
        w.run(1.0f);
        const glm::vec3 p = w.find(id)->localCenter;
        check(std::fabs(p.x - 2.0f) < 0.05f && std::fabs(p.z) < 0.05f, "Move By along its own heading drives forward",
              f3(p));
    }
    {
        // Another object, moved by name; a vector variable read back.
        World w;
        Graph g;
        const int st = add(g, "on_start");
        const int sp = add(g, "set_position");
        val(g, sp, 1).text = "Box";
        val(g, sp, 2).vec = glm::vec3(1.0f, 2.0f, 3.0f);
        wire(g, st, 0, sp, 0);
        w.add("Mover", glm::vec3(0.0f), g);
        const int box = w.add("Box", glm::vec3(0.0f)).id;
        w.run(0.1f);
        check(glm::length(w.find(box)->localCenter - glm::vec3(1.0f, 2.0f, 3.0f)) < 1e-4f,
              "Set Position moves another object found by name", f3(w.find(box)->localCenter));
    }

    // --- Errors stay where they are ----------------------------------------------
    {
        std::printf("-- errors\n");
        World w;
        Graph bad;
        const int up = add(bad, "on_update");
        const int a = add(bad, "sequence");
        const int b = add(bad, "sequence");
        wire(bad, up, 0, a, 0);
        wire(bad, a, 0, b, 0);
        wire(bad, b, 0, a, 0);   // round and round with no Delay
        w.add("Runaway", glm::vec3(0.0f), bad);
        const int ok = w.add("Spinner", glm::vec3(0.0f), templateNamed("Turn round")).id;
        w.run(1.0f);
        check(w.scripts.lastError().find("loop with no Delay") != std::string::npos,
              "a loop with no Delay stops with a reason", w.scripts.lastError());
        check(w.find(ok)->localRotation.y > 40.0f, "...and only that behaviour stops");
    }

    // --- Wiring rules ------------------------------------------------------------
    {
        std::printf("-- wiring\n");
        Graph g;
        const int st = add(g, "on_start");
        const int lg = add(g, "log");
        const int l2 = add(g, "log");
        const int n1 = add(g, "math");
        const int n2 = add(g, "math");
        std::string why;
        check(!wire(g, st, 0, n1, 0, &why), "exec cannot feed a number", why);
        check(wire(g, st, 0, lg, 0) && wire(g, st, 0, l2, 0) && g.links.size() == 1 && g.links[0].toNode == l2,
              "an exec output leads to one place: a second wire replaces the first");
        check(wire(g, n1, 0, n2, 0) && !wire(g, n2, 0, n1, 0, &why), "a value cannot be made of itself", why);
        check(wire(g, n1, 0, lg, 1), "a number can be shown as text");
        const int vec = add(g, "make_vector");
        check(!wire(g, vec, 0, n2, 2, &why), "a vector does not go into a number", why);
        wire(g, n2, 0, lg, 1);
        int into = 0;
        for (const Link& l : g.links)
            if (l.toNode == lg && l.toPin == 1) ++into;
        check(into == 1, "a data input takes one wire");
        removeNode(g, n1);
        bool dangling = false;
        for (const Link& l : g.links)
            if (l.fromNode == n1 || l.toNode == n1) dangling = true;
        check(!dangling, "removing a node takes its wires with it");
    }

    // --- Save and load ------------------------------------------------------------
    {
        std::printf("-- persistence\n");
        int same = 0;
        for (const Template& t : templates()) {
            const Graph g = t.make();
            nlohmann::json a, b;
            save(a, g);
            Graph back;
            load(a, back);
            save(b, back);
            if (a == b && compile(g, "x").lua == compile(back, "x").lua) ++same;
            else std::printf("       %s differs after a round trip\n", t.name);
        }
        check(same == static_cast<int>(templates().size()), "every template saves and loads back the same");

        // Through the component, as a scene file stores it.
        BehaviourComponent bc;
        bc.graph = templateNamed("Patrol");
        nlohmann::json j;
        bc.save(j);
        auto made = components::create("behaviour");
        check(made != nullptr, "the Behaviour component is registered");
        if (made) {
            made->load(j);
            auto* back = dynamic_cast<BehaviourComponent*>(made.get());
            check(back && back->graph.nodes.size() == bc.graph.nodes.size() &&
                      back->graph.links.size() == bc.graph.links.size() && back->graph.vars.size() == 4,
                  "...and carries its graph through a save");
        }
    }

    // --- Layout -------------------------------------------------------------------
    {
        Graph g = templateNamed("Follow the player");
        autoLayout(g);
        const Node* up = nullptr;
        const Node* mv = nullptr;
        const Node* pp = nullptr;
        for (const Node& n : g.nodes) {
            if (n.type == "on_update") up = &n;
            if (n.type == "move_toward") mv = &n;
            if (n.type == "player_pos") pp = &n;
        }
        check(up && mv && pp && up->pos.x < mv->pos.x && pp->pos.x < mv->pos.x,
              "Lay out puts the event left of what it sets off, a value left of what reads it");
    }

    std::printf(g_fails ? "behaviourcheck: %d FAILED\n" : "behaviourcheck: all passed\n", g_fails);
    return g_fails ? 1 : 0;
}
