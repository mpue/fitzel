#include "BehaviourPanel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include <imgui.h>

#include "EditorContext.hpp"
#include "SceneTypes.hpp"
#include "UiStyle.hpp"

namespace behaviourui {

namespace {

using behaviour::Category;
using behaviour::Graph;
using behaviour::Kind;
using behaviour::Link;
using behaviour::Node;
using behaviour::NodeDef;
using behaviour::PinDef;
using behaviour::Value;

// Node geometry in graph units. A graph unit is a sixteenth of the font size at
// zoom 1, so a node holds its text at whatever type size the editor runs at --
// a canvas measured in pixels opens with its labels running out of the boxes.
constexpr float kNodeW  = 200.0f;
constexpr float kHeadH  = 26.0f;
constexpr float kRowH   = 22.0f;
constexpr float kPadH   = 8.0f;
constexpr float kGrid   = 12.0f;
constexpr float kPinR   = 5.0f;

// The copy "Copy" keeps, for "Paste" on another object.
Graph g_clipboard;
bool  g_haveClipboard = false;

ImU32 col(float r, float g, float b, float a = 1.0f) { return ImGui::GetColorU32(ImVec4(r, g, b, a)); }

ImVec4 kindColour(Kind k) {
    switch (k) {
        case Kind::Exec:   return ImVec4(0.95f, 0.95f, 0.95f, 1.0f);
        case Kind::Number: return ImVec4(0.55f, 0.88f, 0.45f, 1.0f);
        case Kind::Bool:   return ImVec4(0.95f, 0.42f, 0.40f, 1.0f);
        case Kind::Vector: return ImVec4(0.98f, 0.80f, 0.30f, 1.0f);
        case Kind::Text:   return ImVec4(0.95f, 0.52f, 0.85f, 1.0f);
        case Kind::Entity: return ImVec4(0.40f, 0.72f, 1.00f, 1.0f);
        default:           return ImVec4(0.70f, 0.70f, 0.72f, 1.0f);
    }
}

ImVec4 headColour(Category c) {
    switch (c) {
        case Category::Event:  return ImVec4(0.55f, 0.20f, 0.18f, 1.0f);
        case Category::Flow:   return ImVec4(0.34f, 0.34f, 0.40f, 1.0f);
        case Category::Action: return ImVec4(0.18f, 0.32f, 0.52f, 1.0f);
        case Category::Data:   return ImVec4(0.18f, 0.40f, 0.28f, 1.0f);
    }
    return ImVec4(0.3f, 0.3f, 0.3f, 1.0f);
}

float nodeHeight(const NodeDef* d) {
    const std::size_t rows = d ? std::max<std::size_t>(std::max(d->ins.size(), d->outs.size()), 1) : 1;
    return kHeadH + static_cast<float>(rows) * kRowH + kPadH;
}

// What an unwired input shows on the node: its typed value, short.
std::string valueText(const Graph& g, const Node& n, int pin) {
    const NodeDef* d = behaviour::findDef(n.type);
    if (!d || pin >= static_cast<int>(d->ins.size())) return {};
    const PinDef& pd = d->ins[static_cast<std::size_t>(pin)];
    const Value&  v  = pin < static_cast<int>(n.values.size()) ? n.values[static_cast<std::size_t>(pin)] : pd.def;
    Kind k = pd.kind;
    if (k == Kind::Any) k = behaviour::pinKind(g, n, true, pin);
    char b[96];
    switch (k) {
        case Kind::Number: std::snprintf(b, sizeof(b), "%.3g", v.num); return b;
        case Kind::Bool:   return v.b ? "yes" : "no";
        case Kind::Vector: std::snprintf(b, sizeof(b), "%.3g, %.3g, %.3g", v.vec.x, v.vec.y, v.vec.z); return b;
        case Kind::Text: {
            std::string t = v.text.size() > 18 ? v.text.substr(0, 17) + "..." : v.text;
            return "\"" + t + "\"";
        }
        case Kind::Entity: return v.text.empty() ? "self" : v.text;
        case Kind::Key: {
            const auto& ks = behaviour::keys();
            return ks[static_cast<std::size_t>(std::clamp(v.choice, 0, static_cast<int>(ks.size()) - 1))].label;
        }
        case Kind::Choice:
            return pd.choices.empty() ? std::string()
                                      : pd.choices[static_cast<std::size_t>(
                                            std::clamp(v.choice, 0, static_cast<int>(pd.choices.size()) - 1))];
        case Kind::Var: return v.text.empty() ? "(none)" : v.text;
        default: return {};
    }
}

bool inputWired(const Graph& g, int node, int pin) {
    for (const Link& l : g.links)
        if (l.toNode == node && l.toPin == pin) return true;
    return false;
}

std::string pinLabel(const Graph& g, int node, bool input, int pin) {
    const Node*    n = behaviour::findNode(g, node);
    const NodeDef* d = n ? behaviour::findDef(n->type) : nullptr;
    if (!d) return "?";
    const auto& pins = input ? d->ins : d->outs;
    std::string s    = d->title;
    if (pin >= 0 && pin < static_cast<int>(pins.size()) && !pins[static_cast<std::size_t>(pin)].name.empty())
        s += ": " + pins[static_cast<std::size_t>(pin)].name;
    return s;
}

// Draw a pin: a triangle for "what happens next", a dot for a value -- filled
// when wired, a ring when not.
void drawPin(ImDrawList* dl, ImVec2 c, float r, Kind k, bool wired, bool hot) {
    const ImU32 cc = ImGui::GetColorU32(kindColour(k));
    if (k == Kind::Exec) {
        const ImVec2 a(c.x - r, c.y - r * 1.1f), b(c.x - r, c.y + r * 1.1f), t(c.x + r * 1.1f, c.y);
        if (wired) dl->AddTriangleFilled(a, b, t, cc);
        else dl->AddTriangle(a, b, t, cc, 1.5f);
    } else {
        if (wired) dl->AddCircleFilled(c, r, cc);
        else dl->AddCircle(c, r, cc, 0, 1.5f);
    }
    if (hot) dl->AddCircle(c, r * 1.9f, col(1.0f, 1.0f, 1.0f, 0.8f), 0, 1.5f);
}

void wire(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c, float thick, float scale) {
    const float dx = std::max(std::fabs(b.x - a.x) * 0.5f, 40.0f * scale);
    dl->AddBezierCubic(a, ImVec2(a.x + dx, a.y), ImVec2(b.x - dx, b.y), b, c, thick);
}

// A value editor for one kind, in the node's settings and for a variable's
// start value. True when it changed.
bool editValue(const char* id, Kind k, Value& v, const PinDef* pd, const std::vector<Entity>* scene,
               const Graph* g, float width) {
    bool ch = false;
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(width);
    switch (k) {
        case Kind::Number:
            ch = ImGui::InputFloat("##v", &v.num, 0.1f, 1.0f, "%.3g");
            break;
        case Kind::Bool:
            ch = ImGui::Checkbox("##v", &v.b);
            break;
        case Kind::Vector:
            ch = ImGui::InputFloat3("##v", &v.vec.x, "%.3g");
            break;
        case Kind::Text: {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s", v.text.c_str());
            if (ImGui::InputText("##v", buf, sizeof(buf))) {
                v.text = buf;
                ch = true;
            }
            break;
        }
        case Kind::Entity: {
            // The scene's objects by name -- a click, not a name to remember --
            // and a field to type one that does not exist yet (a spawned one).
            const char* shown = v.text.empty() ? "Self (this object)" : v.text.c_str();
            if (ImGui::BeginCombo("##pick", shown, ImGuiComboFlags_HeightLarge)) {
                if (ImGui::Selectable("Self (this object)", v.text.empty())) {
                    v.text.clear();
                    ch = true;
                }
                if (scene) {
                    std::vector<std::string> names;
                    for (const Entity& e : *scene)
                        if (!e.name.empty() && std::find(names.begin(), names.end(), e.name) == names.end())
                            names.push_back(e.name);
                    for (const std::string& nm : names)
                        if (ImGui::Selectable(nm.c_str(), v.text == nm)) {
                            v.text = nm;
                            ch = true;
                        }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s", v.text.c_str());
            ImGui::SetNextItemWidth(width * 0.6f);
            if (ImGui::InputTextWithHint("##name", "or type a name", buf, sizeof(buf))) {
                v.text = buf;
                ch = true;
            }
            break;
        }
        case Kind::Key: {
            const auto& ks = behaviour::keys();
            const int   ci = std::clamp(v.choice, 0, static_cast<int>(ks.size()) - 1);
            if (ImGui::BeginCombo("##v", ks[static_cast<std::size_t>(ci)].label, ImGuiComboFlags_HeightLarge)) {
                for (int i = 0; i < static_cast<int>(ks.size()); ++i)
                    if (ImGui::Selectable(ks[static_cast<std::size_t>(i)].label, i == ci)) {
                        v.choice = i;
                        ch = true;
                    }
                ImGui::EndCombo();
            }
            break;
        }
        case Kind::Choice: {
            if (pd && !pd->choices.empty()) {
                const int ci = std::clamp(v.choice, 0, static_cast<int>(pd->choices.size()) - 1);
                if (ImGui::BeginCombo("##v", pd->choices[static_cast<std::size_t>(ci)].c_str())) {
                    for (int i = 0; i < static_cast<int>(pd->choices.size()); ++i)
                        if (ImGui::Selectable(pd->choices[static_cast<std::size_t>(i)].c_str(), i == ci)) {
                            v.choice = i;
                            ch = true;
                        }
                    ImGui::EndCombo();
                }
            }
            break;
        }
        case Kind::Var: {
            if (ImGui::BeginCombo("##v", v.text.empty() ? "(choose)" : v.text.c_str())) {
                if (g)
                    for (const behaviour::Variable& var : g->vars)
                        if (ImGui::Selectable(var.name.c_str(), v.text == var.name)) {
                            v.text = var.name;
                            ch = true;
                        }
                ImGui::EndCombo();
            }
            if (g && g->vars.empty()) {
                ImGui::SameLine();
                ui::hint("add one on the left first");
            }
            break;
        }
        default:
            ImGui::TextDisabled("(wire a value in)");
            break;
    }
    ImGui::PopID();
    return ch;
}

} // namespace

Panel::Panel(Deps d) : m_d(std::move(d)) {}

Entity* Panel::target() {
    Selection& sel = m_d.ed.sel;
    if (sel.valid()) return &m_d.ed.entities[static_cast<std::size_t>(sel.index())];
    // Nothing selected: stay on the object shown last. Play starts with the
    // selection cleared, and a window that went blank the moment the game began
    // would hide exactly the run it is there to show.
    for (Entity& e : m_d.ed.entities)
        if (e.id == m_of) return &e;
    return nullptr;
}

void Panel::pick(const std::string& type) {
    Entity* e = target();
    if (!e) return;
    m_of = e->id;
    if (auto* bc = e->components.get<BehaviourComponent>())
        for (const Node& n : bc->graph.nodes)
            if (n.type == type) {
                m_sel = n.id;
                return;
            }
}

void Panel::commit() {
    if (!m_pending) return;
    m_pending = false;
    Entity* e = nullptr;
    for (Entity& x : m_d.ed.entities)
        if (x.id == m_of) e = &x;
    if (!e) return;
    auto* bc = e->components.get<BehaviourComponent>();
    if (!bc) return;
    Entity before = *e;
    if (auto* old = before.components.get<BehaviourComponent>()) old->graph = m_before;
    m_d.ed.commitEdit({std::move(before)}, {e->id}, "Behaviour");
}

int Panel::addNode(Graph& g, const std::string& type, glm::vec2 at) {
    Node n = behaviour::makeNode(g, type, glm::vec2(std::round(at.x / kGrid) * kGrid, std::round(at.y / kGrid) * kGrid));
    const int id = n.id;
    g.nodes.push_back(std::move(n));
    m_sel = id;
    changed();
    return id;
}

bool Panel::tryConnect(Graph& g, PinRef a, PinRef b) {
    if (!a.valid() || !b.valid()) return false;
    if (a.input == b.input) {
        m_message   = a.input ? "Two inputs: a wire goes from an output (right side) to an input (left side)."
                              : "Two outputs: a wire goes from an output (right side) to an input (left side).";
        m_messageAt = static_cast<float>(ImGui::GetTime());
        return false;
    }
    const PinRef out = a.input ? b : a;
    const PinRef in  = a.input ? a : b;
    std::string why;
    if (!behaviour::connect(g, Link{out.node, out.pin, in.node, in.pin}, &why)) {
        m_message   = why;
        m_messageAt = static_cast<float>(ImGui::GetTime());
        return false;
    }
    changed();
    return true;
}

void Panel::startTiles(Entity& e) {
    ui::title("%s has no behaviour yet", e.name.c_str());
    ui::hint("A behaviour is what an object does while the game runs, drawn as a graph: an event "
             "(the game starts, the player comes near, a key is pressed) wired to the steps it sets off.");
    if (m_d.playing()) {
        ui::hint("Stop the game to give it one.");
        return;
    }
    auto give = [&](Graph g) {
        auto before = m_d.ed.snapshot({e.id});
        auto bc     = std::make_unique<BehaviourComponent>();
        bc->graph   = std::move(g);
        e.components.items.push_back(std::move(bc));
        m_d.ed.commitEdit(std::move(before), {e.id}, "Add Behaviour");
        m_fit = true;
    };
    ImGui::Spacing();
    if (ImGui::Button("Empty behaviour")) {
        Graph g;
        g.nodes.push_back(behaviour::makeNode(g, "on_start", glm::vec2(0.0f, 0.0f)));
        g.nodes.push_back(behaviour::makeNode(g, "on_update", glm::vec2(0.0f, 96.0f)));
        give(std::move(g));
        return;
    }
    ImGui::SameLine();
    ui::hint("On Start and On Update, ready to wire.");
    ImGui::Spacing();
    ui::sectionText("Or start from");
    for (const behaviour::Template& t : behaviour::templates()) {
        ImGui::PushID(t.name);
        if (ImGui::Button(t.name, ImVec2(ImGui::GetFontSize() * 11.0f, 0.0f))) {
            Graph g = t.make();
            behaviour::autoLayout(g);
            give(std::move(g));
            ImGui::PopID();
            return;
        }
        ImGui::SameLine();
        ui::hint("%s", t.help);
        ImGui::PopID();
    }
}

void Panel::draw(bool& show) {
    m_keys = false;
    if (!show) {
        commit();
        return;
    }
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(em * 64.0f, em * 40.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Behaviour", &show)) {
        ImGui::End();
        commit();
        return;
    }
    m_keys = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) ||
             ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    Entity* e = target();
    if (!e || e->id != m_of) {
        // Another object (or none): bank what was being changed on the last one.
        commit();
        m_of        = e ? e->id : -1;
        m_sel       = -1;
        m_link      = {};
        m_linkClick = false;
        m_pulled    = {};
        m_fit       = true;
    }
    if (!e) {
        ui::hint("Select an object to see what it does -- or to give it a behaviour.");
        ImGui::End();
        return;
    }
    auto* bc = e->components.get<BehaviourComponent>();
    if (!bc) {
        startTiles(*e);
        ImGui::End();
        return;
    }
    Graph& g = bc->graph;
    if (!m_pending) m_before = g;

    const bool playing = m_d.playing();
    ScriptSystem::Live live;
    const bool haveLive = playing && m_d.live && m_d.live(e->id, live);
    if (playing) {
        m_link      = {};
        m_linkClick = false;
    }

    toolbar(*e, *bc);
    const float setH = em * 10.5f;
    ImGui::BeginChild("##side", ImVec2(em * 16.0f, -setH), ImGuiChildFlags_Borders);
    sidebar(g);
    if (haveLive && !live.vars.empty()) {
        ui::sectionText("Now");
        for (const auto& [name, value] : live.vars) ImGui::Text("%s = %s", name.c_str(), value.c_str());
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##canvas", ImVec2(0.0f, -setH), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    canvas(g, haveLive ? &live : nullptr);
    ImGui::EndChild();
    ImGui::BeginChild("##settings", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    settings(g, haveLive ? &live : nullptr);
    if (playing && live.failed) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
        ImGui::TextWrapped("This behaviour stopped: %s", m_d.lastError ? m_d.lastError().c_str() : "an error");
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::End();

    if (m_showLua) luaWindow(*e, g);

    if (m_changed) {
        m_pending = true;
        m_changed = false;
    }
    if (m_pending && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsAnyItemActive()) commit();
}

void Panel::toolbar(Entity& e, BehaviourComponent& bc) {
    Graph&     g       = bc.graph;
    const bool playing = m_d.playing();
    ui::title("%s", e.name.c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(playing);
    if (ImGui::Button("Add node")) {
        m_openMenu = true;
        m_menuLink = {};
        m_menuAt   = m_viewCentre;
    }
    ImGui::SetItemTooltip("Or right-click on the canvas, or click a node in the list on the left");
    ImGui::SameLine();
    if (ImGui::Button("Lay out")) {
        behaviour::autoLayout(g);
        m_fit = true;
        changed();
    }
    ImGui::SetItemTooltip("Line every node up by what leads to what -- the way out of a knot,\n"
                          "and the way to use this editor without dragging nodes at all");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Fit")) m_fit = true;
    ImGui::SetItemTooltip("Show the whole graph");
    ImGui::SameLine();
    ImGui::BeginDisabled(playing);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
    if (ImGui::BeginCombo("##tpl", "Start from...")) {
        for (const behaviour::Template& t : behaviour::templates()) {
            if (ImGui::Selectable(t.name)) {
                g = t.make();
                behaviour::autoLayout(g);
                m_sel = -1;
                m_fit = true;
                changed();
            }
            ImGui::SetItemTooltip("%s\n(replaces this graph -- Ctrl+Z brings it back)", t.help);
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Copy")) {
        g_clipboard     = g;
        g_haveClipboard = true;
    }
    ImGui::SetItemTooltip("Keep this graph to give another object the same behaviour");
    ImGui::SameLine();
    ImGui::BeginDisabled(playing || !g_haveClipboard);
    if (ImGui::Button("Paste")) {
        g     = g_clipboard;
        m_sel = -1;
        m_fit = true;
        changed();
    }
    ImGui::SetItemTooltip("Replace this graph with the one copied");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(m_showLua ? "Hide Lua" : "Show Lua")) m_showLua = !m_showLua;
    ImGui::SetItemTooltip("The Lua this graph runs as -- also the way on to a hand-written script");

    ImGui::SameLine();
    if (playing) {
        ImGui::TextColored(ImVec4(0.5f, 0.95f, 0.55f, 1.0f), "Running -- lit nodes just ran");
    } else {
        const behaviour::Compiled c = behaviour::compile(g, e.name);
        if (c.warnings.empty()) {
            ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.55f, 1.0f), "Ready");
        } else {
            ImGui::TextColored(ImVec4(0.98f, 0.78f, 0.35f, 1.0f), "%d note%s", static_cast<int>(c.warnings.size()),
                               c.warnings.size() == 1 ? "" : "s");
            if (ImGui::BeginItemTooltip()) {
                for (const std::string& w : c.warnings) ImGui::BulletText("%s", w.c_str());
                ImGui::EndTooltip();
            }
        }
    }
    if (m_messageAt >= 0.0f && ImGui::GetTime() - m_messageAt < 4.0) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "%s", m_message.c_str());
    }
}

void Panel::sidebar(Graph& g) {
    const bool  playing = m_d.playing();
    const float w       = ImGui::GetContentRegionAvail().x;
    ImGui::BeginDisabled(playing);
    ui::sectionText("Variables");
    ui::hint("What the behaviour remembers from frame to frame.");
    for (int i = 0; i < static_cast<int>(g.vars.size()); ++i) {
        behaviour::Variable& v = g.vars[static_cast<std::size_t>(i)];
        ImGui::PushID(i);
        char nb[64];
        std::snprintf(nb, sizeof(nb), "%s", v.name.c_str());
        ImGui::SetNextItemWidth(w * 0.42f);
        if (ImGui::InputText("##vn", nb, sizeof(nb))) {
            // A rename travels to every node that names the variable, or they
            // would quietly read and write one that is not there.
            const std::string was = v.name;
            v.name = nb;
            for (Node& n : g.nodes)
                if (const NodeDef* d = behaviour::findDef(n.type))
                    for (std::size_t p = 0; p < d->ins.size() && p < n.values.size(); ++p)
                        if (d->ins[p].kind == Kind::Var && n.values[p].text == was) n.values[p].text = v.name;
            changed();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(w - w * 0.42f - ImGui::GetFontSize() * 2.6f);
        static const Kind kinds[] = {Kind::Number, Kind::Bool, Kind::Text, Kind::Vector};
        int ki = 0;
        for (int k = 0; k < 4; ++k)
            if (kinds[k] == v.kind) ki = k;
        if (ImGui::Combo("##vk", &ki, "Number\0Yes/no\0Text\0Vector\0")) {
            v.kind = kinds[ki];
            changed();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            g.vars.erase(g.vars.begin() + i);
            changed();
            ImGui::PopID();
            break;
        }
        ImGui::TextDisabled("starts at");
        ImGui::SameLine();
        if (editValue("vd", v.kind, v.def, nullptr, nullptr, nullptr, ImGui::GetContentRegionAvail().x)) changed();
        ImGui::PopID();
    }
    if (ImGui::Button("Add variable")) {
        behaviour::Variable v;
        int n = static_cast<int>(g.vars.size()) + 1;
        do v.name = "var" + std::to_string(n++);
        while (behaviour::findVar(g, v.name) >= 0);
        g.vars.push_back(std::move(v));
        changed();
    }

    ui::sectionText("Nodes");
    ui::hint("Click one to put it on the canvas.");
    ui::searchBox("##palette", m_paletteFilter, sizeof(m_paletteFilter));
    for (Category c : {Category::Event, Category::Flow, Category::Action, Category::Data}) {
        bool any = false;
        for (const NodeDef& d : behaviour::catalogue())
            if (d.cat == c && (ui::icontains(d.title.c_str(), m_paletteFilter) ||
                               ui::icontains(d.help.c_str(), m_paletteFilter)))
                any = true;
        if (!any) continue;
        const ImVec4 hc = headColour(c);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(std::min(1.0f, hc.x * 1.7f), std::min(1.0f, hc.y * 1.7f),
                                                    std::min(1.0f, hc.z * 1.7f), 1.0f));
        const bool open = ImGui::TreeNodeEx(behaviour::categoryName(c),
                                            ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
        ImGui::PopStyleColor();
        if (!open) continue;
        for (const NodeDef& d : behaviour::catalogue()) {
            if (d.cat != c || !(ui::icontains(d.title.c_str(), m_paletteFilter) ||
                                ui::icontains(d.help.c_str(), m_paletteFilter)))
                continue;
            if (ImGui::Selectable(d.title.c_str())) {
                // Dropped where the canvas is looking, staggered so a few clicks
                // in a row do not stack every node on one spot.
                const float k = static_cast<float>(g.nodes.size() % 5);
                addNode(g, d.type, m_viewCentre + glm::vec2(k * 24.0f - 100.0f, k * 24.0f - 40.0f));
            }
            ImGui::SetItemTooltip("%s", d.help.c_str());
        }
        ImGui::TreePop();
    }
    ImGui::EndDisabled();
}

void Panel::canvas(Graph& g, const ScriptSystem::Live* live) {
    const bool    playing = m_d.playing();
    const ImVec2  origin  = ImGui::GetCursorScreenPos();
    const ImVec2  size    = ImGui::GetContentRegionAvail();
    ImDrawList*   dl      = ImGui::GetWindowDrawList();
    ImGuiIO&      io      = ImGui::GetIO();
    const float   em      = ImGui::GetFontSize();
    if (size.x < 8.0f || size.y < 8.0f) return;

    // Show the whole graph: on a new object, after Lay out, on "Fit".
    if (m_fit) {
        m_fit = false;
        if (!g.nodes.empty()) {
            glm::vec2 lo(1e9f), hi(-1e9f);
            for (const Node& n : g.nodes) {
                lo = glm::min(lo, n.pos);
                hi = glm::max(hi, n.pos + glm::vec2(kNodeW, nodeHeight(behaviour::findDef(n.type))));
            }
            // Never so small that the nodes lose their words: a graph wider
            // than the window is shown from its left, where the events are, and
            // the rest is a pan away.
            const glm::vec2 ext  = hi - lo + glm::vec2(80.0f);
            const float     unit = em / 16.0f;
            const float     fit  = std::min(size.x / (ext.x * unit), size.y / (ext.y * unit));
            m_zoom = std::clamp(fit, 0.55f, 1.25f);
            const float s = unit * m_zoom;
            m_pan = glm::vec2(size.x, size.y) / (2.0f * s) - (lo + hi) * 0.5f;
            if (fit < m_zoom) m_pan.x = 40.0f - lo.x;
            if (ext.y * s > size.y) m_pan.y = 40.0f - lo.y;
        } else {
            m_zoom = 1.0f;
            m_pan  = glm::vec2(40.0f);
        }
    }
    const float S = em / 16.0f * m_zoom;   // pixels per graph unit
    auto toScreen = [&](glm::vec2 p) { return ImVec2(origin.x + (p.x + m_pan.x) * S, origin.y + (p.y + m_pan.y) * S); };
    auto toGraph  = [&](ImVec2 p) { return glm::vec2((p.x - origin.x) / S - m_pan.x, (p.y - origin.y) / S - m_pan.y); };
    m_viewCentre = toGraph(ImVec2(origin.x + size.x * 0.5f, origin.y + size.y * 0.5f));

    // --- Background -------------------------------------------------------------
    dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), col(0.085f, 0.09f, 0.11f));
    const float gridPx = kGrid * 4.0f * S;
    if (gridPx > 8.0f) {
        for (float x = std::fmod(m_pan.x * S, gridPx); x < size.x; x += gridPx)
            dl->AddLine(ImVec2(origin.x + x, origin.y), ImVec2(origin.x + x, origin.y + size.y),
                        col(1.0f, 1.0f, 1.0f, 0.035f));
        for (float y = std::fmod(m_pan.y * S, gridPx); y < size.y; y += gridPx)
            dl->AddLine(ImVec2(origin.x, origin.y + y), ImVec2(origin.x + size.x, origin.y + y),
                        col(1.0f, 1.0f, 1.0f, 0.035f));
    }
    if (g.nodes.empty()) {
        const char* t = "Right-click here (or \"Add node\") to add the first node.";
        const ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(origin.x + (size.x - ts.x) * 0.5f, origin.y + size.y * 0.45f), col(1, 1, 1, 0.45f), t);
    }

    // The background yields to the nodes and pins on top of it (they are
    // submitted after it), and owns panning, zooming and the empty-canvas click.
    ImGui::SetCursorScreenPos(origin);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##bg", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle |
                               ImGuiButtonFlags_MouseButtonRight);
    const bool bgHovered = ImGui::IsItemHovered();
    if (ImGui::IsItemActivated()) {
        m_bgDragged = false;
        if (m_linkClick && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            // The second click of a click-made wire, on nothing: let it go.
            m_link      = {};
            m_linkClick = false;
        }
    }
    if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Left) ||
                                  ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
                                  ImGui::IsMouseDragging(ImGuiMouseButton_Right))) {
        m_pan += glm::vec2(io.MouseDelta.x, io.MouseDelta.y) / S;
        m_bgDragged = true;
    }
    if (ImGui::IsItemDeactivated() && !m_bgDragged) {
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) && !playing) {
            m_openMenu = true;
            m_menuLink = {};
            m_menuAt   = toGraph(io.MousePos);
        } else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            m_sel = -1;
        }
    }
    if (bgHovered && io.MouseWheel != 0.0f) {
        const glm::vec2 at  = toGraph(io.MousePos);
        m_zoom              = std::clamp(m_zoom * (1.0f + io.MouseWheel * 0.12f), 0.3f, 2.2f);
        const float     s2  = em / 16.0f * m_zoom;
        m_pan = glm::vec2(io.MousePos.x - origin.x, io.MousePos.y - origin.y) / s2 - at;
    }

    // --- Where the pins are this frame (drawing, dropping a wire) --------------------
    struct Spot {
        PinRef ref;
        ImVec2 at;
        Kind   kind;
    };
    std::vector<Spot> spots;
    std::map<int, float> age;   // node id -> seconds since it last ran
    if (live)
        for (const auto& [id, at] : live->pulses) age[id] = live->now - at;
    auto lit = [&](int id) {
        const auto it = age.find(id);
        return it != age.end() && it->second >= -0.01f && it->second < 0.45f;
    };
    auto pinAt = [&](const Node& n, bool input, int pin) {
        const float y = n.pos.y + kHeadH + kRowH * (static_cast<float>(pin) + 0.5f);
        return toScreen(glm::vec2(input ? n.pos.x : n.pos.x + kNodeW, y));
    };
    for (const Node& n : g.nodes) {
        const NodeDef* d = behaviour::findDef(n.type);
        if (!d) continue;
        for (int i = 0; i < static_cast<int>(d->ins.size()); ++i)
            if (!behaviour::isSetting(d->ins[static_cast<std::size_t>(i)].kind))
                spots.push_back({{n.id, i, true}, pinAt(n, true, i), behaviour::pinKind(g, n, true, i)});
        for (int i = 0; i < static_cast<int>(d->outs.size()); ++i)
            spots.push_back({{n.id, i, false}, pinAt(n, false, i), behaviour::pinKind(g, n, false, i)});
    }
    auto spotUnder = [&](ImVec2 m) -> const Spot* {
        const Spot* best = nullptr;
        float bestD = 14.0f * std::max(1.0f, m_zoom);
        for (const Spot& s : spots) {
            const float d = std::hypot(s.at.x - m.x, s.at.y - m.y);
            if (d < bestD) {
                bestD = d;
                best  = &s;
            }
        }
        return best;
    };
    auto spotOf = [&](const PinRef& r) -> const Spot* {
        for (const Spot& s : spots)
            if (s.ref.node == r.node && s.ref.pin == r.pin && s.ref.input == r.input) return &s;
        return nullptr;
    };

    // --- Wires ------------------------------------------------------------------
    dl->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    for (const Link& l : g.links) {
        const Node* a = behaviour::findNode(g, l.fromNode);
        const Node* b = behaviour::findNode(g, l.toNode);
        if (!a || !b) continue;
        const Kind  k   = behaviour::pinKind(g, *a, false, l.fromPin);
        const bool  hot = k == Kind::Exec && lit(a->id) && lit(b->id);
        const bool  sel = l.fromNode == m_sel || l.toNode == m_sel;
        ImVec4      c   = kindColour(k);
        c.w = sel || hot ? 1.0f : 0.75f;
        if (hot) c = ImVec4(0.55f, 1.0f, 0.55f, 1.0f);
        wire(dl, pinAt(*a, false, l.fromPin), pinAt(*b, true, l.toPin), ImGui::GetColorU32(c),
             (k == Kind::Exec ? 3.0f : 2.0f) * std::max(0.6f, m_zoom) + (hot ? 1.5f : 0.0f), S);
    }

    // --- Nodes ------------------------------------------------------------------
    const float fontPx   = std::clamp(em * m_zoom, 6.0f, 48.0f);
    const bool  showText = m_zoom > 0.4f;
    ImFont*     font     = ImGui::GetFont();
    int         remove   = -1;
    for (Node& n : g.nodes) {
        const NodeDef* d = behaviour::findDef(n.type);
        const float    h = nodeHeight(d);
        const ImVec2   a = toScreen(n.pos);
        const ImVec2   b(a.x + kNodeW * S, a.y + h * S);
        const float    r = 6.0f * S;
        const bool     isSel = n.id == m_sel;
        const bool     isLit = lit(n.id);

        ImGui::PushID(n.id);
        ImGui::SetCursorScreenPos(a);
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##node", ImVec2(kNodeW * S, h * S));
        if (ImGui::IsItemActivated()) {
            m_sel = n.id;
            if (m_linkClick) {
                m_link      = {};
                m_linkClick = false;
            }
        }
        if (!playing && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            n.pos += glm::vec2(io.MouseDelta.x, io.MouseDelta.y) / S;
            changed();
        }
        // Snapped on release, not under the hand that is moving it.
        if (ImGui::IsItemDeactivated()) {
            const glm::vec2 snapped(std::round(n.pos.x / kGrid) * kGrid, std::round(n.pos.y / kGrid) * kGrid);
            if (snapped != n.pos) {
                n.pos = snapped;
                changed();
            }
        }
        if (d && ImGui::IsItemHovered() && !ImGui::IsItemActive() && ImGui::GetIO().KeyCtrl)
            ImGui::SetTooltip("%s", d->help.c_str());
        if (!playing && ImGui::BeginPopupContextItem("##nodemenu")) {
            m_sel = n.id;
            if (ImGui::MenuItem("Duplicate")) {
                Node copy = n;
                copy.id   = g.nextId++;
                copy.pos += glm::vec2(24.0f, 24.0f);
                m_sel     = copy.id;
                g.nodes.push_back(std::move(copy));
                changed();
                ImGui::EndPopup();
                ImGui::PopID();
                break;   // `n` is dangling after the push
            }
            if (ImGui::MenuItem("Take its wires out")) {
                g.links.erase(std::remove_if(g.links.begin(), g.links.end(),
                                             [&](const Link& l) { return l.fromNode == n.id || l.toNode == n.id; }),
                              g.links.end());
                changed();
            }
            if (ImGui::MenuItem("Delete", "Del")) remove = n.id;
            ImGui::EndPopup();
        }

        // Body and header.
        const ImVec4 hc   = d ? headColour(d->cat) : ImVec4(0.5f, 0.1f, 0.1f, 1.0f);
        dl->AddRectFilled(a, b, col(0.13f, 0.14f, 0.17f, 0.97f), r);
        dl->AddRectFilled(a, ImVec2(b.x, a.y + kHeadH * S), ImGui::GetColorU32(hc), r, ImDrawFlags_RoundCornersTop);
        const ImU32 edge = isLit ? col(0.55f, 1.0f, 0.55f) : isSel ? col(1.0f, 1.0f, 1.0f, 0.95f)
                                                                   : col(1.0f, 1.0f, 1.0f, 0.16f);
        dl->AddRect(a, b, edge, r, 0, isLit || isSel ? 2.5f : 1.2f);
        if (isLit) dl->AddRect(ImVec2(a.x - 3, a.y - 3), ImVec2(b.x + 3, b.y + 3), col(0.55f, 1.0f, 0.55f, 0.35f), r + 3, 0, 4.0f);

        if (showText) {
            dl->PushClipRect(a, b, true);
            const float pad = 8.0f * S;
            dl->AddText(font, fontPx, ImVec2(a.x + pad, a.y + (kHeadH * S - fontPx) * 0.5f), col(1, 1, 1),
                        d ? d->title.c_str() : ("? " + n.type).c_str());
            if (d) {
                for (int i = 0; i < static_cast<int>(d->ins.size()); ++i) {
                    const PinDef& pd = d->ins[static_cast<std::size_t>(i)];
                    const float   y  = a.y + (kHeadH + kRowH * i) * S + (kRowH * S - fontPx) * 0.5f;
                    std::string   t  = pd.name;
                    const bool    wired = inputWired(g, n.id, i);
                    if (pd.kind != Kind::Exec && !wired) {
                        const std::string v = valueText(g, n, i);
                        t += t.empty() ? v : "  " + v;
                    }
                    const float x = a.x + (behaviour::isSetting(pd.kind) ? pad : pad + 6.0f * S);
                    dl->AddText(font, fontPx, ImVec2(x, y),
                                behaviour::isSetting(pd.kind) ? col(0.80f, 0.84f, 0.92f) : col(0.92f, 0.93f, 0.96f),
                                t.c_str());
                }
                for (int i = 0; i < static_cast<int>(d->outs.size()); ++i) {
                    const std::string& t  = d->outs[static_cast<std::size_t>(i)].name;
                    const ImVec2       ts = font->CalcTextSizeA(fontPx, 1e9f, 0.0f, t.c_str());
                    const float        y  = a.y + (kHeadH + kRowH * i) * S + (kRowH * S - fontPx) * 0.5f;
                    dl->AddText(font, fontPx, ImVec2(b.x - pad - 6.0f * S - ts.x, y), col(0.92f, 0.93f, 0.96f),
                                t.c_str());
                }
            }
            dl->PopClipRect();
        }

        // Pins: drawn, and each an item of its own (after the node, so it wins).
        if (d) {
            auto pinButton = [&](bool input, int i, Kind k) {
                const ImVec2 c = pinAt(n, input, i);
                bool wired = false;
                for (const Link& l : g.links)
                    if (input ? (l.toNode == n.id && l.toPin == i) : (l.fromNode == n.id && l.fromPin == i))
                        wired = true;
                const bool isOrigin = m_link.valid() && m_link.node == n.id && m_link.pin == i && m_link.input == input;
                ImGui::PushID(input ? 1000 + i : 2000 + i);
                const float hs = std::max(8.0f, 9.0f * S);
                ImGui::SetCursorScreenPos(ImVec2(c.x - hs, c.y - hs));
                ImGui::InvisibleButton("##pin", ImVec2(hs * 2.0f, hs * 2.0f),
                                       ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
                const bool hov = ImGui::IsItemHovered();
                drawPin(dl, c, kPinR * S, k, wired, hov || isOrigin);
                if (hov && !playing) {
                    ImGui::SetTooltip("%s%s\n%s", behaviour::kindName(k), input ? " in" : " out",
                                      wired ? "Right-click: take the wire out" : "Drag to a pin -- or click, then click the other");
                }
                if (!playing && ImGui::IsItemActivated()) {
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                        behaviour::disconnect(g, n.id, input, i);
                        changed();
                    } else if (m_linkClick && m_link.valid()) {
                        if (!isOrigin) tryConnect(g, m_link, PinRef{n.id, i, input});
                        m_link      = {};
                        m_linkClick = false;
                    } else {
                        m_link   = PinRef{n.id, i, input};
                        m_pulled = {};
                        m_sel    = n.id;
                    }
                }
                ImGui::PopID();
            };
            for (int i = 0; i < static_cast<int>(d->ins.size()); ++i)
                if (!behaviour::isSetting(d->ins[static_cast<std::size_t>(i)].kind))
                    pinButton(true, i, behaviour::pinKind(g, n, true, i));
            for (int i = 0; i < static_cast<int>(d->outs.size()); ++i)
                pinButton(false, i, behaviour::pinKind(g, n, false, i));
        }
        ImGui::PopID();
    }

    // --- A wire being drawn ---------------------------------------------------------
    if (m_link.valid() && !playing) {
        const Spot* from = spotOf(m_link);
        if (!from) {
            m_link      = {};
            m_linkClick = false;
        } else if (!m_linkClick) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                // Pulling a wired input: the wire comes off it and follows the
                // hand from where it came, to go somewhere else or nowhere.
                if (m_link.input && !m_pulled.valid() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 6.0f)) {
                    for (const Link& l : g.links)
                        if (l.toNode == m_link.node && l.toPin == m_link.pin) {
                            m_pulled = m_link;
                            m_link   = PinRef{l.fromNode, l.fromPin, false};
                            behaviour::disconnect(g, m_pulled.node, true, m_pulled.pin);
                            changed();
                            break;
                        }
                }
            } else {
                // Let go: on a pin, a wire; on the pin it started from without
                // having moved, the first click of two; on nothing, the node
                // menu with what fits -- or, for a wire pulled off, nothing.
                const ImVec2 drag  = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
                const bool   moved = std::fabs(drag.x) + std::fabs(drag.y) > 6.0f;
                const Spot*  over  = spotUnder(io.MousePos);
                if (over && !(over->ref.node == m_link.node && over->ref.pin == m_link.pin &&
                              over->ref.input == m_link.input)) {
                    tryConnect(g, m_link, over->ref);
                    m_link = {};
                } else if (!moved && !m_pulled.valid()) {
                    m_linkClick = true;
                } else if (m_pulled.valid()) {
                    m_link = {};
                } else {
                    m_openMenu = true;
                    m_menuLink = m_link;
                    m_menuAt   = toGraph(io.MousePos);
                    m_link     = {};
                }
                m_pulled = {};
            }
        }
        if (m_link.valid()) {
            if (const Spot* f = spotOf(m_link)) {
                const ImU32 c = ImGui::GetColorU32(kindColour(f->kind));
                if (m_link.input) wire(dl, io.MousePos, f->at, c, 2.5f, S);
                else wire(dl, f->at, io.MousePos, c, 2.5f, S);
                // Where it may go: every pin that fits is ringed.
                for (const Spot& s : spots) {
                    if (s.ref.input == m_link.input || s.ref.node == m_link.node) continue;
                    const bool fits = m_link.input ? behaviour::canConnect(s.kind, f->kind)
                                                   : behaviour::canConnect(f->kind, s.kind);
                    if (fits) dl->AddCircle(s.at, kPinR * S * 1.9f, col(1.0f, 1.0f, 1.0f, 0.35f), 0, 1.2f);
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                m_link      = {};
                m_linkClick = false;
            }
        }
    }
    dl->PopClipRect();

    // --- Keys ---------------------------------------------------------------------
    if (!playing && m_keys && !io.WantTextInput && m_sel >= 0 &&
        (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)))
        remove = m_sel;
    if (remove >= 0) {
        behaviour::removeNode(g, remove);
        if (m_sel == remove) m_sel = -1;
        changed();
    }

    addMenu(g);
}

void Panel::addMenu(Graph& g) {
    if (m_openMenu) {
        m_openMenu = false;
        m_menuFilter[0] = '\0';
        ImGui::OpenPopup("##addnode");
    }
    if (!ImGui::BeginPopup("##addnode")) return;
    // With a wire dragged out into the open: only the nodes that can take it,
    // and the new one arrives wired.
    Kind linkKind = Kind::Any;
    bool linkIn   = false;
    if (m_menuLink.valid())
        if (const Node* n = behaviour::findNode(g, m_menuLink.node)) {
            linkKind = behaviour::pinKind(g, *n, m_menuLink.input, m_menuLink.pin);
            linkIn   = m_menuLink.input;
        }
    auto fitPin = [&](const NodeDef& d) -> int {
        if (!m_menuLink.valid()) return -2;
        // The new node's side is the other one: a wire out of an output goes
        // into one of its inputs.
        const auto& pins = linkIn ? d.outs : d.ins;
        for (int i = 0; i < static_cast<int>(pins.size()); ++i) {
            const Kind k = pins[static_cast<std::size_t>(i)].kind;
            if (behaviour::isSetting(k)) continue;
            if (linkIn ? behaviour::canConnect(k, linkKind) : behaviour::canConnect(linkKind, k)) return i;
        }
        return -1;
    };
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    ImGui::InputTextWithHint("##find", "Find a node...", m_menuFilter, sizeof(m_menuFilter));
    if (m_menuLink.valid()) ui::hint("Nodes that take a %s wire", behaviour::kindName(linkKind));
    const NodeDef* chosen = nullptr;
    const NodeDef* first  = nullptr;
    for (Category c : {Category::Event, Category::Flow, Category::Action, Category::Data}) {
        bool header = false;
        for (const NodeDef& d : behaviour::catalogue()) {
            if (d.cat != c || fitPin(d) == -1) continue;
            if (!ui::icontains(d.title.c_str(), m_menuFilter) && !ui::icontains(d.help.c_str(), m_menuFilter)) continue;
            if (!header) {
                ui::sectionText(behaviour::categoryName(c));
                header = true;
            }
            if (!first) first = &d;
            if (ImGui::Selectable(d.title.c_str())) chosen = &d;
            ImGui::SetItemTooltip("%s", d.help.c_str());
        }
    }
    // Enter takes the first match -- typing "dela", Enter is a Delay.
    if (!chosen && first && m_menuFilter[0] && ImGui::IsKeyPressed(ImGuiKey_Enter, false)) chosen = first;
    if (chosen) {
        const int pin = fitPin(*chosen);
        glm::vec2 at  = m_menuAt;
        // A node made from a wire out of an output stands to its right, one made
        // for an input to its left -- its pin where the hand let go.
        if (pin >= 0) at += linkIn ? glm::vec2(-kNodeW, -kHeadH - kRowH * (pin + 0.5f))
                                   : glm::vec2(0.0f, -kHeadH - kRowH * (pin + 0.5f));
        const int id = addNode(g, chosen->type, at);
        if (pin >= 0) tryConnect(g, m_menuLink, PinRef{id, pin, !linkIn});
        m_menuLink = {};
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void Panel::settings(Graph& g, const ScriptSystem::Live*) {
    const bool playing = m_d.playing();
    Node*      n       = m_sel >= 0 ? behaviour::findNode(g, m_sel) : nullptr;
    const NodeDef* d   = n ? behaviour::findDef(n->type) : nullptr;
    if (!n || !d) {
        if (playing) {
            ui::hint("Lit nodes are the ones that just ran. Stop the game to change the graph.");
            return;
        }
        ui::hint("Click a node to set it up. Wire an event's white output to what it should do; "
                 "wire values in from below (Values) or type them into the node.");
        ui::hint("Drag from pin to pin -- or click one pin, then the other. A wire let go on empty "
                 "canvas offers the nodes that fit. Right-click a pin to take its wire out.");
        return;
    }
    if (n->values.size() < d->ins.size())
        for (std::size_t i = n->values.size(); i < d->ins.size(); ++i) n->values.push_back(d->ins[i].def);

    ImGui::BeginDisabled(playing);
    ui::title("%s", d->title.c_str());
    ImGui::SameLine();
    ui::hint("%s", d->help.c_str());
    const float em     = ImGui::GetFontSize();
    const float labelW = em * 7.0f;
    const float valueW = em * 12.0f;

    // Inputs: the value typed in, or where the wire comes from -- and a list to
    // wire one in from, for the hand that would rather not aim at a pin.
    for (int i = 0; i < static_cast<int>(d->ins.size()); ++i) {
        const PinDef& pd = d->ins[static_cast<std::size_t>(i)];
        const Kind    k  = behaviour::pinKind(g, *n, true, i);
        if (k == Kind::Exec) continue;
        ImGui::PushID(i);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kindColour(k), "%s", pd.name.empty() ? "value" : pd.name.c_str());
        ImGui::SameLine(labelW);
        const Link* in = nullptr;
        for (const Link& l : g.links)
            if (l.toNode == n->id && l.toPin == i) in = &l;
        if (in) {
            ImGui::Text("<- %s", pinLabel(g, in->fromNode, false, in->fromPin).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("take out")) {
                behaviour::disconnect(g, n->id, true, i);
                changed();
            }
        } else {
            Value& v = n->values[static_cast<std::size_t>(i)];
            if (editValue("val", k, v, &pd, &m_d.ed.entities, &g, valueW)) changed();
        }
        if (!behaviour::isSetting(k)) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(em * 9.0f);
            if (ImGui::BeginCombo("##from", "wire from...", ImGuiComboFlags_HeightLarge)) {
                for (const Node& o : g.nodes) {
                    if (o.id == n->id) continue;
                    const NodeDef* od = behaviour::findDef(o.type);
                    if (!od) continue;
                    for (int p = 0; p < static_cast<int>(od->outs.size()); ++p) {
                        if (!behaviour::canConnect(behaviour::pinKind(g, o, false, p), k)) continue;
                        ImGui::PushID(o.id * 64 + p);
                        if (ImGui::Selectable(pinLabel(g, o.id, false, p).c_str()))
                            tryConnect(g, PinRef{o.id, p, false}, PinRef{n->id, i, true});
                        ImGui::PopID();
                    }
                }
                ImGui::EndCombo();
            }
        }
        ImGui::PopID();
    }
    // Exec outputs: what each leads to, chosen from a list as well.
    for (int i = 0; i < static_cast<int>(d->outs.size()); ++i) {
        if (d->outs[static_cast<std::size_t>(i)].kind != Kind::Exec) continue;
        ImGui::PushID(500 + i);
        ImGui::AlignTextToFramePadding();
        const std::string& nm = d->outs[static_cast<std::size_t>(i)].name;
        ImGui::TextUnformatted(nm.empty() ? "then" : nm.c_str());
        ImGui::SameLine(labelW);
        std::string to = "(nothing)";
        for (const Link& l : g.links)
            if (l.fromNode == n->id && l.fromPin == i) to = pinLabel(g, l.toNode, true, l.toPin);
        ImGui::SetNextItemWidth(valueW);
        if (ImGui::BeginCombo("##to", ("-> " + to).c_str(), ImGuiComboFlags_HeightLarge)) {
            if (ImGui::Selectable("(nothing)")) {
                behaviour::disconnect(g, n->id, false, i);
                changed();
            }
            for (const Node& o : g.nodes) {
                if (o.id == n->id) continue;
                const NodeDef* od = behaviour::findDef(o.type);
                if (!od) continue;
                for (int p = 0; p < static_cast<int>(od->ins.size()); ++p) {
                    if (od->ins[static_cast<std::size_t>(p)].kind != Kind::Exec) continue;
                    ImGui::PushID(o.id * 64 + p);
                    if (ImGui::Selectable(pinLabel(g, o.id, true, p).c_str()))
                        tryConnect(g, PinRef{n->id, i, false}, PinRef{o.id, p, true});
                    ImGui::PopID();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }
    if (ImGui::Button("Duplicate")) {
        Node copy = *n;
        copy.id   = g.nextId++;
        copy.pos += glm::vec2(24.0f, 24.0f);
        m_sel     = copy.id;
        g.nodes.push_back(std::move(copy));
        changed();
    } else {
        ImGui::SameLine();
        if (ImGui::Button("Delete node")) {
            behaviour::removeNode(g, n->id);
            m_sel = -1;
            changed();
        }
    }
    ImGui::EndDisabled();
}

void Panel::luaWindow(const Entity& e, const Graph& g) {
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(em * 44.0f, em * 36.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Behaviour Lua###BehaviourLua", &m_showLua)) {
        ImGui::End();
        return;
    }
    const behaviour::Compiled c = behaviour::compile(g, e.name);
    ui::hint("What %s's graph runs as -- written afresh at every Play. To go on by hand, copy it into "
             "a script of its own (Assets > Scripts) and take the Behaviour off.", e.name.c_str());
    if (ImGui::Button("Copy to clipboard")) ImGui::SetClipboardText(c.lua.c_str());
    for (const std::string& w : c.warnings) ImGui::TextColored(ImVec4(0.98f, 0.78f, 0.35f, 1.0f), "%s", w.c_str());
    static std::string text;
    text = c.lua;
    ImGui::InputTextMultiline("##lua", text.data(), text.size() + 1, ImVec2(-1.0f, -1.0f), ImGuiInputTextFlags_ReadOnly);
    ImGui::End();
}

} // namespace behaviourui
