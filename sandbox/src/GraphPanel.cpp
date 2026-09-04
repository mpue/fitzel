#include "GraphPanel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>

#include "Component.hpp"
#include "SceneTypes.hpp"
#include "Selection.hpp"
#include "UiStyle.hpp"

namespace graphui {

namespace {

using animgraph::Condition;
using animgraph::Graph;
using animgraph::Param;
using animgraph::State;
using animgraph::Transition;

// What is selected and where the canvas is looking. Editor state, not scene
// data: nobody else acts on it and it does not belong in the .fitzel. Every
// index is re-validated each frame, so deleting a state leaves the selection
// pointing at nothing rather than at whatever moved into its slot.
int       g_selState = -1;
int       g_selTrans = -1;
glm::vec2 g_pan{0.0f};
float     g_zoom = 1.0f;
// A link being drawn by hand: the state it started from, or -1.
int       g_linkFrom = -1;
// Did the press that is currently down land on an arrow? See the canvas: the
// background owns the click, so it is the background that would otherwise clear
// a selection the arrow had just made.
bool      g_pressHitArrow = false;

// Node geometry, in graph units. The canvas scales them by the zoom, so these
// are the sizes at 1:1 and nothing below is a raw pixel count.
constexpr float kNodeW  = 168.0f;
constexpr float kNodeH  = 56.0f;
constexpr float kGrid   = 24.0f;   // what a dragged node snaps to
constexpr float kAnyY   = -120.0f; // where the Any State node sits

ImU32 col(float r, float g, float b, float a = 1.0f) {
    return ImGui::GetColorU32(ImVec4(r, g, b, a));
}

float snapTo(float v, float step) { return std::round(v / step) * step; }

// The centre of a state's node in graph space; kAnyState has a node of its own
// above the graph so its arrows have somewhere to come from.
glm::vec2 nodeCentre(const Graph& g, int state) {
    if (state == Transition::kAnyState) {
        // Over the middle of everything, so its arrows fan out rather than
        // crossing the whole graph from a corner.
        float minX = 0.0f, maxX = 0.0f, minY = 0.0f;
        bool first = true;
        for (const State& s : g.states) {
            if (first) { minX = maxX = s.pos.x; minY = s.pos.y; first = false; }
            minX = std::min(minX, s.pos.x);
            maxX = std::max(maxX, s.pos.x);
            minY = std::min(minY, s.pos.y);
        }
        return glm::vec2((minX + maxX) * 0.5f + kNodeW * 0.5f, minY + kAnyY);
    }
    if (state < 0 || state >= static_cast<int>(g.states.size())) return glm::vec2(0.0f);
    return g.states[static_cast<std::size_t>(state)].pos +
           glm::vec2(kNodeW * 0.5f, kNodeH * 0.5f);
}

// Where a line from a to b leaves a's box -- so arrows touch the node's edge
// instead of vanishing under it.
ImVec2 edgePoint(ImVec2 from, ImVec2 to, float halfW, float halfH) {
    const float dx = to.x - from.x, dy = to.y - from.y;
    if (std::fabs(dx) < 1e-4f && std::fabs(dy) < 1e-4f) return from;
    const float sx = std::fabs(dx) > 1e-4f ? halfW / std::fabs(dx) : 1e9f;
    const float sy = std::fabs(dy) > 1e-4f ? halfH / std::fabs(dy) : 1e9f;
    const float t  = std::min(sx, sy);
    return ImVec2(from.x + dx * t, from.y + dy * t);
}

// How far `p` is from the segment a-b. The arrow's hit test used to measure to
// its MIDPOINT only, which made a line twenty pixels wide and a couple of
// hundred long into a small disc in the middle of it -- the one shape this
// editor must not ask anybody to hit.
float distToSegment(ImVec2 p, ImVec2 a, ImVec2 b) {
    const float vx = b.x - a.x, vy = b.y - a.y;
    const float wx = p.x - a.x, wy = p.y - a.y;
    const float len2 = vx * vx + vy * vy;
    const float t = len2 > 1e-6f
                  ? std::clamp((wx * vx + wy * vy) / len2, 0.0f, 1.0f) : 0.0f;
    const float dx = wx - vx * t, dy = wy - vy * t;
    return std::sqrt(dx * dx + dy * dy);
}

void arrow(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c, float thick, float head) {
    dl->AddLine(a, b, c, thick);
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float l  = std::sqrt(dx * dx + dy * dy);
    if (l < 1e-3f) return;
    const ImVec2 d(dx / l, dy / l), n(-d.y, d.x);
    const ImVec2 tip = b;
    dl->AddTriangleFilled(tip,
                          ImVec2(tip.x - d.x * head + n.x * head * 0.55f,
                                 tip.y - d.y * head + n.y * head * 0.55f),
                          ImVec2(tip.x - d.x * head - n.x * head * 0.55f,
                                 tip.y - d.y * head - n.y * head * 0.55f), c);
}

// A short line saying what a transition waits for, drawn beside its arrow: an
// arrow you cannot read the condition of is only half the picture.
std::string transitionLabel(const Graph& g, const Transition& t) {
    std::string s;
    if (t.hasExitTime) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "after %.0f%%", t.exitTime * 100.0f);
        s = buf;
    }
    for (const Condition& c : t.conditions) {
        if (!s.empty()) s += " + ";
        s += c.param;
        switch (c.op) {
            case Condition::Op::Fired:   break;
            case Condition::Op::IsTrue:  s += " on";  break;
            case Condition::Op::IsFalse: s += " off"; break;
            default: {
                char buf[32];
                std::snprintf(buf, sizeof(buf), " %s %.2g",
                              animgraph::opName(c.op), c.value);
                s += buf;
                break;
            }
        }
    }
    return s;
}

// Lay the states out in a row of rows. Not clever, and that is the point: it is
// the button you press when the graph has become a knot, or when you would
// rather not drag nodes at all.
void autoLayout(Graph& g) {
    const int perRow = std::max(1, static_cast<int>(std::ceil(
        std::sqrt(static_cast<float>(std::max<std::size_t>(g.states.size(), 1))))));
    for (int i = 0; i < static_cast<int>(g.states.size()); ++i)
        g.states[static_cast<std::size_t>(i)].pos =
            glm::vec2(static_cast<float>(i % perRow) * (kNodeW + 70.0f),
                      static_cast<float>(i / perRow) * (kNodeH + 90.0f));
}

// The machine the selected object is running, if it is running this graph.
animgraph::Instance* liveInstance(const PanelState& s, const Graph& g) {
    if (!s.sel.valid()) return nullptr;
    Entity& e = s.entities[s.sel.index()];
    auto* ag = e.components.get<AnimGraphComponent>();
    if (!ag || ag->graph != g.name) return nullptr;
    return &ag->runtime;
}

} // namespace

void drawPanel(const PanelState& s) {
    if (!s.show) return;
    // In font terms, like every other size in this editor: at a comfortable type
    // size a graph canvas measured in pixels opens too small to hold its own
    // toolbar, and the first thing the author meets is a cut-off button.
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(em * 52.0f, em * 38.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Animation graph", &s.show)) { ImGui::End(); return; }

    // --- Which graph --------------------------------------------------------
    if (s.graphs.empty()) {
        ui::hint("This scene has no animation graphs yet.");
        ui::hint("A graph is a set of states -- each one a clip from the Timeline -- "
                 "and the arrows between them.");
        if (ImGui::Button("New graph")) {
            Graph g;
            g.name = "Graph 1";
            s.graphs.push_back(std::move(g));
            s.editGraph = 0;
            s.markDirty();
        }
        ImGui::End();
        return;
    }
    s.editGraph = std::clamp(s.editGraph, 0, static_cast<int>(s.graphs.size()) - 1);
    Graph& g = s.graphs[s.editGraph];

    ImGui::SetNextItemWidth(em * 10.0f);
    if (ImGui::BeginCombo("##graph", g.name.c_str())) {
        for (int i = 0; i < static_cast<int>(s.graphs.size()); ++i)
            if (ImGui::Selectable(s.graphs[i].name.c_str(), i == s.editGraph)) {
                s.editGraph = i;
                g_selState = g_selTrans = -1;
            }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    {   // Renaming travels to the components that chose this graph by name, the
        // way a clip rename does -- otherwise they quietly stop finding it.
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s", g.name.c_str());
        ImGui::SetNextItemWidth(em * 10.0f);
        if (ImGui::InputText("Name##graphname", buf, sizeof(buf))) {
            const std::string was = g.name;
            g.name = buf;
            for (Entity& e : s.entities)
                if (auto* ag = e.components.get<AnimGraphComponent>())
                    if (ag->graph == was) ag->graph = g.name;
            s.markDirty();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("New")) {
        Graph n;
        n.name = "Graph " + std::to_string(s.graphs.size() + 1);
        s.graphs.push_back(std::move(n));
        s.editGraph = static_cast<int>(s.graphs.size()) - 1;
        g_selState = g_selTrans = -1;
        s.markDirty();
        ImGui::End();
        return;                     // `g` is dangling after the push
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete graph")) {
        s.graphs.erase(s.graphs.begin() + s.editGraph);
        s.editGraph = std::max(0, s.editGraph - 1);
        g_selState = g_selTrans = -1;
        s.markDirty();
        ImGui::End();
        return;
    }
    if (ImGui::Button("Add state")) {
        State st;
        st.name = "State " + std::to_string(g.states.size() + 1);
        // Dropped where the canvas is looking, not at the origin: a new node off
        // screen is one you have to go and find. Staggered by how many there
        // already are, because dropping every one on the same spot buries them
        // in a pile that has to be dragged apart before it can be read.
        const int n = static_cast<int>(g.states.size());
        st.pos  = glm::vec2(snapTo(-g_pan.x + 60.0f + (n % 3) * (kNodeW + 60.0f), kGrid),
                            snapTo(-g_pan.y + 60.0f + (n / 3) * (kNodeH + 70.0f), kGrid));
        g.states.push_back(std::move(st));
        g_selState = static_cast<int>(g.states.size()) - 1;
        g_selTrans = -1;
        s.markDirty();
    }
    ImGui::SameLine();
    if (ImGui::Button("Lay out")) { autoLayout(g); s.markDirty(); }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Arrange every state in a grid -- the way out of a knot,\n"
                          "and the way to use this editor without dragging at all");
    ImGui::SameLine();
    if (ImGui::Button("Centre")) { g_pan = glm::vec2(40.0f, 160.0f); g_zoom = 1.0f; }

    ImGui::Separator();

    animgraph::Instance* live = liveInstance(s, g);

    // --- Parameters ---------------------------------------------------------
    const float paramW = em * 15.0f;
    ImGui::BeginChild("##params", ImVec2(paramW, -em * 9.0f), true);
    ui::sectionText("Parameters");
    ui::hint("What the game sets to move the machine along.");
    for (int i = 0; i < static_cast<int>(g.params.size()); ++i) {
        Param& p = g.params[static_cast<std::size_t>(i)];
        ImGui::PushID(i);
        char nb[48];
        std::snprintf(nb, sizeof(nb), "%s", p.name.c_str());
        ImGui::SetNextItemWidth(paramW * 0.5f);
        if (ImGui::InputText("##pname", nb, sizeof(nb))) {
            // A rename has to travel to every condition that reads it, or the
            // arrows silently stop firing -- and a transition whose parameter is
            // missing blocks, so the graph would just quietly stop working.
            const std::string was = p.name;
            p.name = nb;
            for (Transition& t : g.transitions)
                for (Condition& c : t.conditions)
                    if (c.param == was) c.param = p.name;
            s.markDirty();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(paramW * 0.32f);
        int type = static_cast<int>(p.type);
        if (ImGui::Combo("##ptype", &type, "Trigger\0Bool\0Number\0")) {
            p.type = static_cast<Param::Type>(type);
            s.markDirty();
        }

        // While the game runs, the same row drives the live machine. This is the
        // half that makes a graph debuggable: press the trigger and watch which
        // node lights up, instead of reading the arrows and hoping.
        if (live) {
            const float v = i < static_cast<int>(live->values.size())
                          ? live->values[static_cast<std::size_t>(i)] : 0.0f;
            ImGui::SameLine();
            if (p.type == Param::Type::Trigger) {
                if (ImGui::SmallButton("Fire")) animgraph::fire(g, *live, p.name);
            } else if (p.type == Param::Type::Bool) {
                bool b = v >= 0.5f;
                if (ImGui::Checkbox("##pv", &b)) animgraph::setBool(g, *live, p.name, b);
            } else {
                float f = v;
                ImGui::SetNextItemWidth(paramW * 0.28f);
                if (ImGui::DragFloat("##pv", &f, 0.05f))
                    animgraph::setNumber(g, *live, p.name, f);
            }
        } else if (p.type != Param::Type::Trigger) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(paramW * 0.28f);
            if (ImGui::DragFloat("##pdef", &p.def, 0.05f)) s.markDirty();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Starting value");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            g.params.erase(g.params.begin() + i);
            s.markDirty();
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (ImGui::Button("Add parameter")) {
        Param p;
        p.name = "param" + std::to_string(g.params.size() + 1);
        g.params.push_back(std::move(p));
        s.markDirty();
    }
    if (live)
        ui::hint("Running: %s",
                 live->state >= 0 && live->state < static_cast<int>(g.states.size())
                     ? g.states[static_cast<std::size_t>(live->state)].name.c_str()
                     : "(not started)");
    else
        ui::hint("Select an object using this graph to drive it here.");
    ImGui::EndChild();

    // --- The canvas ---------------------------------------------------------
    ImGui::SameLine();
    ImGui::BeginChild("##canvas", ImVec2(0.0f, -em * 9.0f), true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size   = ImGui::GetContentRegionAvail();
    ImDrawList*  dl     = ImGui::GetWindowDrawList();

    auto toScreen = [&](glm::vec2 p) {
        return ImVec2(origin.x + (p.x + g_pan.x) * g_zoom,
                      origin.y + (p.y + g_pan.y) * g_zoom);
    };

    // The background: a grid to snap against, and the whole area as one target
    // that pans and deselects.
    dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y),
                      col(0.09f, 0.10f, 0.12f));
    const float gridPx = kGrid * g_zoom;
    if (gridPx > 6.0f) {
        for (float x = std::fmod(g_pan.x * g_zoom, gridPx); x < size.x; x += gridPx)
            dl->AddLine(ImVec2(origin.x + x, origin.y),
                        ImVec2(origin.x + x, origin.y + size.y),
                        col(1.0f, 1.0f, 1.0f, 0.04f));
        for (float y = std::fmod(g_pan.y * g_zoom, gridPx); y < size.y; y += gridPx)
            dl->AddLine(ImVec2(origin.x, origin.y + y),
                        ImVec2(origin.x + size.x, origin.y + y),
                        col(1.0f, 1.0f, 1.0f, 0.04f));
    }

    // THE BACKGROUND MUST YIELD TO THE NODES ON TOP OF IT. It is submitted first
    // (it is behind everything), and without this it would take the press before
    // any node exists to want it: Dear ImGui decides who owns a click while the
    // item is being submitted, so the first item under the pointer wins unless it
    // says otherwise. Clicking a node did nothing at all -- the canvas had
    // already swallowed it.
    ImGui::SetCursorScreenPos(origin);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##bg", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    const bool bgHovered = ImGui::IsItemHovered();
    // A fresh press on the canvas: nothing claimed yet. The arrows below get
    // their say later in this same frame.
    if (ImGui::IsItemActivated()) g_pressHitArrow = false;
    if (ImGui::IsItemActive() &&
        (ImGui::IsMouseDragging(ImGuiMouseButton_Left) ||
         ImGui::IsMouseDragging(ImGuiMouseButton_Middle))) {
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        g_pan += glm::vec2(d.x, d.y) / g_zoom;
    }
    // A click on EMPTY canvas clears the selection. Two things must not:
    //
    // A drag was a pan -- letting go would otherwise deselect whatever you were
    // about to edit. And a click that landed on an ARROW belongs to that arrow:
    // arrows are drawn, not Dear ImGui items, so the background owns their
    // clicks and used to select on the press and clear on the release, one frame
    // later. Selecting an arrow simply did not stick -- except when the hand
    // wobbled past the drag threshold, which made it look intermittent and made
    // the steady click the one that failed.
    if (ImGui::IsItemDeactivated() && !g_pressHitArrow) {
        const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
        if (std::fabs(d.x) + std::fabs(d.y) < 4.0f) { g_selState = -1; g_selTrans = -1; }
    }
    if (bgHovered && ImGui::GetIO().MouseWheel != 0.0f) {
        // Zoom about the pointer, so the thing you are looking at stays put.
        const float old = g_zoom;
        g_zoom = std::clamp(g_zoom * (1.0f + ImGui::GetIO().MouseWheel * 0.12f), 0.35f, 2.5f);
        const ImVec2 m = ImGui::GetIO().MousePos;
        const glm::vec2 mv((m.x - origin.x), (m.y - origin.y));
        g_pan += mv / g_zoom - mv / old;
    }

    // Arrows first, so nodes sit on top of them -- but their LABELS last, for
    // the same reason in reverse: between two nodes standing close together the
    // arrow is short and its caption ran under the node it points at, which
    // hides the one thing the arrow needed to say.
    struct Caption { ImVec2 at; std::string text; };
    std::vector<Caption> captions;

    for (int ti = 0; ti < static_cast<int>(g.transitions.size()); ++ti) {
        const Transition& t = g.transitions[static_cast<std::size_t>(ti)];
        if (t.to < 0 || t.to >= static_cast<int>(g.states.size())) continue;
        if (t.from != Transition::kAnyState &&
            (t.from < 0 || t.from >= static_cast<int>(g.states.size()))) continue;

        const ImVec2 a = toScreen(nodeCentre(g, t.from));
        const ImVec2 b = toScreen(nodeCentre(g, t.to));
        const float hw = kNodeW * 0.5f * g_zoom, hh = kNodeH * 0.5f * g_zoom;
        ImVec2 p0 = edgePoint(a, b, hw, hh);
        ImVec2 p1 = edgePoint(b, a, hw, hh);

        // TWO STATES THAT LEAD TO EACH OTHER ARE THE COMMON CASE -- idle to
        // moving and back is the first graph anybody draws -- and both arrows
        // ran down the same line, one on top of the other. It read as a single
        // line with a head at each end, only one of the two captions was
        // visible, and a click always landed on the same one because they shared
        // a midpoint: the second arrow could not be selected at all. So a pair
        // bows apart, each to its own side, and everything after this -- the
        // caption, the hit test -- follows the bowed line.
        bool paired = false;
        for (const Transition& o : g.transitions)
            if (o.from == t.to && o.to == t.from) { paired = true; break; }
        if (paired) {
            const float dx = p1.x - p0.x, dy = p1.y - p0.y;
            const float l  = std::sqrt(dx * dx + dy * dy);
            if (l > 1e-3f) {
                // Perpendicular to THIS arrow's own direction, and nothing else.
                //
                // The first attempt also flipped a sign for the "other" arrow of
                // the pair, which cancelled out exactly: the two run in opposite
                // directions, so their perpendiculars already point opposite
                // ways, and negating one of them put both back on the same side.
                // They stayed on top of each other and the fix looked like no
                // fix at all. The direction flip IS the separation.
                const float off = 14.0f * g_zoom;
                const ImVec2 n(-dy / l * off, dx / l * off);
                p0 = ImVec2(p0.x + n.x, p0.y + n.y);
                p1 = ImVec2(p1.x + n.x, p1.y + n.y);
            }
        }

        const bool sel = (g_selTrans == ti);
        // An arrow whose conditions hold RIGHT NOW is lit, which turns the graph
        // into a live readout while the game runs.
        const bool hot = live && animgraph::ready(g, *live, t, 1.0f) &&
                         (t.from == Transition::kAnyState || t.from == live->state);
        const ImU32 c = sel ? col(1.0f, 1.0f, 1.0f)
                      : hot ? col(0.45f, 0.95f, 0.55f)
                            : col(0.62f, 0.68f, 0.80f, 0.85f);
        arrow(dl, p0, p1, c, sel ? 3.0f : 2.0f, 11.0f * g_zoom);

        // Clicking an arrow: anywhere along its length, not just the middle.
        const ImVec2 mid((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
        if (bgHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (distToSegment(ImGui::GetIO().MousePos, p0, p1) < 10.0f * g_zoom) {
                g_selTrans = ti;
                g_selState = -1;
                g_pressHitArrow = true;   // this press is the arrow's, not the canvas's
            }
        }
        std::string lbl = transitionLabel(g, t);
        if (!lbl.empty() && g_zoom > 0.6f)
            captions.push_back({ImVec2(mid.x, mid.y - ImGui::GetFontSize() - 4.0f),
                                std::move(lbl)});
    }

    // The Any State node, drawn only when something comes from it.
    bool haveAny = false;
    for (const Transition& t : g.transitions)
        if (t.from == Transition::kAnyState) { haveAny = true; break; }
    if (haveAny) {
        const ImVec2 c = toScreen(nodeCentre(g, Transition::kAnyState));
        const ImVec2 a(c.x - kNodeW * 0.5f * g_zoom, c.y - kNodeH * 0.5f * g_zoom);
        const ImVec2 b(c.x + kNodeW * 0.5f * g_zoom, c.y + kNodeH * 0.5f * g_zoom);
        dl->AddRectFilled(a, b, col(0.20f, 0.18f, 0.28f), 8.0f * g_zoom);
        dl->AddRect(a, b, col(0.55f, 0.50f, 0.75f), 8.0f * g_zoom, 0, 1.5f);
        dl->AddText(ImVec2(a.x + 10.0f * g_zoom, a.y + 8.0f * g_zoom),
                    col(0.85f, 0.82f, 1.0f), "Any State");
    }

    // Nodes.
    for (int i = 0; i < static_cast<int>(g.states.size()); ++i) {
        State& st = g.states[static_cast<std::size_t>(i)];
        const ImVec2 a = toScreen(st.pos);
        const ImVec2 b(a.x + kNodeW * g_zoom, a.y + kNodeH * g_zoom);

        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(1000 + i);
        ImGui::InvisibleButton("##node", ImVec2(kNodeW * g_zoom, kNodeH * g_zoom));
        const bool held = ImGui::IsItemActive();
        if (ImGui::IsItemActivated()) { g_selState = i; g_selTrans = -1; }
        if (held && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            const ImVec2 d = ImGui::GetIO().MouseDelta;
            st.pos += glm::vec2(d.x, d.y) / g_zoom;
            s.markDirty();
        }
        // SNAPPED ON RELEASE, not while dragging: snapping under the cursor
        // fights the hand that is moving it, and the point of the grid is that
        // wherever you let go, the node lines up with its neighbours.
        if (ImGui::IsItemDeactivated()) {
            st.pos = glm::vec2(snapTo(st.pos.x, kGrid), snapTo(st.pos.y, kGrid));
        }

        const bool isEntry = (i == g.entry);
        const bool isLive  = live && live->state == i;
        const ImU32 fill = isLive ? col(0.16f, 0.32f, 0.20f)
                         : (g_selState == i) ? col(0.20f, 0.24f, 0.32f)
                                             : col(0.15f, 0.17f, 0.21f);
        const ImU32 edge = isLive ? col(0.45f, 0.95f, 0.55f)
                         : (g_selState == i) ? col(1.0f, 1.0f, 1.0f, 0.9f)
                         : isEntry ? col(0.95f, 0.76f, 0.32f)
                                   : col(1.0f, 1.0f, 1.0f, 0.25f);
        dl->AddRectFilled(a, b, fill, 8.0f * g_zoom);
        dl->AddRect(a, b, edge, 8.0f * g_zoom, 0, isEntry || isLive ? 2.5f : 1.5f);
        if (g_zoom > 0.5f) {
            // WHERE THE MACHINE STARTS, IN WORDS. It used to be an amber border
            // and nothing else, which is not a label -- an author whose entry
            // state was the wrong one saw their animation run the moment the
            // game started and had no reason to connect the two.
            if (isEntry) {
                const ImVec2 tp(a.x, a.y - ImGui::GetFontSize() - 6.0f * g_zoom);
                const ImVec2 sz = ImGui::CalcTextSize("starts here");
                dl->AddRectFilled(tp, ImVec2(tp.x + sz.x + 10.0f, tp.y + sz.y + 3.0f),
                                  col(0.95f, 0.76f, 0.32f), 4.0f);
                dl->AddText(ImVec2(tp.x + 5.0f, tp.y + 1.0f), col(0.10f, 0.09f, 0.06f),
                            "starts here");
            }
            dl->AddText(ImVec2(a.x + 10.0f * g_zoom, a.y + 7.0f * g_zoom),
                        col(1.0f, 1.0f, 1.0f), st.name.c_str());
            dl->AddText(ImVec2(a.x + 10.0f * g_zoom, a.y + 28.0f * g_zoom),
                        col(0.75f, 0.80f, 0.90f, 0.9f),
                        st.clip.empty() ? "(no clip)" : st.clip.c_str());
        }

        // The out port: press it and drag onto another node to draw an arrow.
        // The same arrow can be made from the two combos below, which is the
        // version that needs no aim at all.
        const ImVec2 port(b.x, (a.y + b.y) * 0.5f);
        dl->AddCircleFilled(port, 6.0f * g_zoom, col(0.62f, 0.68f, 0.80f));
        ImGui::SetCursorScreenPos(ImVec2(port.x - 9.0f * g_zoom, port.y - 9.0f * g_zoom));
        ImGui::InvisibleButton("##port", ImVec2(18.0f * g_zoom, 18.0f * g_zoom));
        if (ImGui::IsItemActivated()) g_linkFrom = i;
        ImGui::PopID();

        // Dropping a pending link on this node makes the transition.
        if (g_linkFrom >= 0 && g_linkFrom != i &&
            ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            const ImVec2 m = ImGui::GetIO().MousePos;
            if (m.x >= a.x && m.x <= b.x && m.y >= a.y && m.y <= b.y) {
                Transition t;
                t.from = g_linkFrom;
                t.to   = i;
                g.transitions.push_back(t);
                g_selTrans = static_cast<int>(g.transitions.size()) - 1;
                g_selState = -1;
                s.markDirty();
            }
        }
    }
    // The captions, over everything, each on a plate of its own so it stays
    // readable wherever it lands.
    for (const Caption& c : captions) {
        const ImVec2 sz = ImGui::CalcTextSize(c.text.c_str());
        const ImVec2 a(c.at.x - sz.x * 0.5f - 5.0f, c.at.y - 2.0f);
        const ImVec2 b(a.x + sz.x + 10.0f, a.y + sz.y + 4.0f);
        dl->AddRectFilled(a, b, col(0.10f, 0.11f, 0.14f, 0.92f), 4.0f);
        dl->AddRect(a, b, col(1.0f, 1.0f, 1.0f, 0.12f), 4.0f);
        dl->AddText(ImVec2(a.x + 5.0f, a.y + 2.0f), col(0.88f, 0.91f, 0.97f),
                    c.text.c_str());
    }

    if (g_linkFrom >= 0) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const ImVec2 from = toScreen(nodeCentre(g, g_linkFrom));
            arrow(dl, from, ImGui::GetIO().MousePos, col(1.0f, 1.0f, 1.0f, 0.7f),
                  2.0f, 10.0f * g_zoom);
        } else {
            g_linkFrom = -1;
        }
    }
    ImGui::EndChild();

    // --- What is selected ---------------------------------------------------
    g_selState = g_selState < static_cast<int>(g.states.size()) ? g_selState : -1;
    g_selTrans = g_selTrans < static_cast<int>(g.transitions.size()) ? g_selTrans : -1;

    ImGui::BeginChild("##sel", ImVec2(0.0f, 0.0f), true);
    if (g_selState >= 0) {
        State& st = g.states[static_cast<std::size_t>(g_selState)];
        ui::sectionText("State");
        char nb[64];
        std::snprintf(nb, sizeof(nb), "%s", st.name.c_str());
        ImGui::SetNextItemWidth(em * 10.0f);
        if (ImGui::InputText("Name##st", nb, sizeof(nb))) { st.name = nb; s.markDirty(); }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(em * 10.0f);
        if (ImGui::BeginCombo("Clip", st.clip.empty() ? "(none)" : st.clip.c_str())) {
            if (ImGui::Selectable("(none)", st.clip.empty())) { st.clip.clear(); s.markDirty(); }
            for (const anim::Clip& c : s.clips)
                if (ImGui::Selectable(c.name.c_str(), st.clip == c.name))
                    { st.clip = c.name; s.markDirty(); }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Loop##st", &st.loop)) s.markDirty();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(em * 4.0f);
        if (ImGui::DragFloat("Speed##st", &st.speed, 0.02f, 0.05f, 8.0f, "%.2fx"))
            s.markDirty();

        ImGui::BeginDisabled(g.entry == g_selState);
        if (ImGui::Button("Make entry state")) { g.entry = g_selState; s.markDirty(); }
        ImGui::EndDisabled();
        ImGui::SameLine();
        // Drawing an arrow without touching the canvas.
        ImGui::SetNextItemWidth(em * 9.0f);
        if (ImGui::BeginCombo("Arrow to", "choose...")) {
            for (int i = 0; i < static_cast<int>(g.states.size()); ++i) {
                if (i == g_selState) continue;
                if (ImGui::Selectable(g.states[static_cast<std::size_t>(i)].name.c_str())) {
                    Transition t;
                    t.from = g_selState;
                    t.to   = i;
                    g.transitions.push_back(t);
                    g_selTrans = static_cast<int>(g.transitions.size()) - 1;
                    g_selState = -1;
                    s.markDirty();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete state")) {
            // Its arrows go with it, and every index above it shifts down --
            // leaving them would point transitions at whatever moved into the
            // slot, which is a graph that quietly does something else.
            g.transitions.erase(
                std::remove_if(g.transitions.begin(), g.transitions.end(),
                               [&](const Transition& t) {
                                   return t.from == g_selState || t.to == g_selState;
                               }),
                g.transitions.end());
            for (Transition& t : g.transitions) {
                if (t.from > g_selState) --t.from;
                if (t.to   > g_selState) --t.to;
            }
            g.states.erase(g.states.begin() + g_selState);
            if (g.entry > g_selState) --g.entry;
            g.entry = g.states.empty() ? 0
                    : std::clamp(g.entry, 0, static_cast<int>(g.states.size()) - 1);
            g_selState = -1;
            s.markDirty();
        }
    } else if (g_selTrans >= 0) {
        Transition& t = g.transitions[static_cast<std::size_t>(g_selTrans)];
        ui::sectionText("Transition");

        auto stateName = [&](int i) -> const char* {
            if (i == Transition::kAnyState) return "Any State";
            if (i < 0 || i >= static_cast<int>(g.states.size())) return "(gone)";
            return g.states[static_cast<std::size_t>(i)].name.c_str();
        };
        ImGui::SetNextItemWidth(em * 9.0f);
        if (ImGui::BeginCombo("From", stateName(t.from))) {
            if (ImGui::Selectable("Any State", t.from == Transition::kAnyState))
                { t.from = Transition::kAnyState; s.markDirty(); }
            for (int i = 0; i < static_cast<int>(g.states.size()); ++i)
                if (ImGui::Selectable(g.states[static_cast<std::size_t>(i)].name.c_str(),
                                      t.from == i))
                    { t.from = i; s.markDirty(); }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(em * 9.0f);
        if (ImGui::BeginCombo("To", stateName(t.to))) {
            for (int i = 0; i < static_cast<int>(g.states.size()); ++i)
                if (ImGui::Selectable(g.states[static_cast<std::size_t>(i)].name.c_str(),
                                      t.to == i))
                    { t.to = i; s.markDirty(); }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Wait for the clip", &t.hasExitTime)) s.markDirty();
        if (t.hasExitTime) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(em * 6.0f);
            if (ImGui::SliderFloat("##exit", &t.exitTime, 0.0f, 1.0f, "%.0f%% through"))
                s.markDirty();
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete arrow")) {
            g.transitions.erase(g.transitions.begin() + g_selTrans);
            g_selTrans = -1;
            s.markDirty();
        } else {
            ui::hint("All of these must hold at once. For \"either\", draw a second arrow.");
            for (int ci = 0; ci < static_cast<int>(t.conditions.size()); ++ci) {
                Condition& c = t.conditions[static_cast<std::size_t>(ci)];
                ImGui::PushID(ci);
                ImGui::SetNextItemWidth(em * 8.0f);
                if (ImGui::BeginCombo("##cp", c.param.empty() ? "(parameter)" : c.param.c_str())) {
                    for (const Param& p : g.params)
                        if (ImGui::Selectable(p.name.c_str(), c.param == p.name)) {
                            c.param = p.name;
                            // Match the operator to the KIND of parameter: a
                            // trigger has nothing to compare and a number does
                            // not fire, and an operator left over from the last
                            // choice is a condition that can never hold.
                            const int pi = animgraph::findParam(g, p.name);
                            if (pi >= 0) {
                                switch (g.params[static_cast<std::size_t>(pi)].type) {
                                    case Param::Type::Trigger: c.op = Condition::Op::Fired; break;
                                    case Param::Type::Bool:    c.op = Condition::Op::IsTrue; break;
                                    case Param::Type::Number:  c.op = Condition::Op::Greater; break;
                                }
                            }
                            s.markDirty();
                        }
                    ImGui::EndCombo();
                }
                const int pi = animgraph::findParam(g, c.param);
                const Param::Type pt = pi >= 0 ? g.params[static_cast<std::size_t>(pi)].type
                                               : Param::Type::Trigger;
                if (pt != Param::Type::Trigger) {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(em * 6.0f);
                    int op = static_cast<int>(c.op);
                    const char* items = pt == Param::Type::Bool
                                      ? "is true\0is false\0"
                                      : "is true\0is false\0>\0<\0==\0";
                    if (ImGui::Combo("##cop", &op, items))
                        { c.op = static_cast<Condition::Op>(op); s.markDirty(); }
                    if (pt == Param::Type::Number) {
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(em * 5.0f);
                        if (ImGui::DragFloat("##cv", &c.value, 0.05f)) s.markDirty();
                    }
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) {
                    t.conditions.erase(t.conditions.begin() + ci);
                    s.markDirty();
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            ImGui::BeginDisabled(g.params.empty());
            if (ImGui::Button("Add condition")) {
                Condition c;
                c.param = g.params.front().name;
                c.op    = g.params.front().type == Param::Type::Number
                            ? Condition::Op::Greater
                            : (g.params.front().type == Param::Type::Bool
                                   ? Condition::Op::IsTrue : Condition::Op::Fired);
                t.conditions.push_back(std::move(c));
                s.markDirty();
            }
            ImGui::EndDisabled();
            if (g.params.empty())
                ui::hint("Add a parameter on the left first -- an arrow with nothing "
                         "to wait for is never taken.");
        }
    } else {
        ui::hint("Click a state or an arrow to edit it. Drag from a node's right-hand "
                 "dot to another node to connect them, or use \"Arrow to\" on the state.");
        ui::hint("Objects run a graph through an Animation Graph component; a script "
                 "moves it along with game.animTrigger(id, \"name\").");
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace graphui
