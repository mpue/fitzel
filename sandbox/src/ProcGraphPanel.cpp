#include "ProcGraphPanel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include "Command.hpp"
#include "Component.hpp"
#include "EditorContext.hpp"
#include "Modifiers.hpp"      // ModifierStackComponent: smooth shading
#include "ProcPresets.hpp"
#include "UiStyle.hpp"
#include "ViewportFrame.hpp"

namespace procui {

namespace {

// --- A number without a drag --------------------------------------------------
// ui::stepper, plus one thing a graph needs that a modelling panel does not: a
// ring's radius goes from 20 to 200, and nobody should click a stepper ninety
// times. So the value in the middle is a button that turns into a text field --
// one click, type, Enter. Typing is the one way to a far-away value that needs
// no steady hand at all.
ImGuiID g_typing    = 0;
char    g_typeBuf[64] = {};
bool    g_typeFocus = false;

// The harness's probe (Panel::probe) for this frame, and the call that feeds
// it the item just drawn.
const std::function<void(const std::string&, ImVec2, ImVec2)>* g_probe = nullptr;
void tell(const std::string& key) {
    if (g_probe && *g_probe) (*g_probe)(key, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

bool numberField(const char* id, float& v, float step, float lo, float hi,
                 const char* fmt, float width, const std::string& key) {
    const float em = ImGui::GetFontSize();
    const bool  bounded = hi > lo;
    auto clampV = [&](float x) { return bounded ? std::clamp(x, lo, hi) : x; };
    bool changed = false;
    ImGui::PushID(id);
    const ImGuiID me = ImGui::GetID("##value");
    const ImVec2 bs(std::max(em * 1.6f, 26.0f), ImGui::GetFrameHeight() + em * 0.35f);
    const float  mid = std::max(width - 2.0f * bs.x - 6.0f, em * 2.5f);
    if (ImGui::Button("-", bs)) { v = clampV(v - step); changed = true; }
    tell(key + "-");
    ImGui::SetItemTooltip("%g less", step);
    ImGui::SameLine(0.0f, 3.0f);
    if (g_typing == me) {
        ImGui::SetNextItemWidth(mid);
        if (g_typeFocus) { ImGui::SetKeyboardFocusHere(); g_typeFocus = false; }
        const ImGuiStyle& st = ImGui::GetStyle();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(st.FramePadding.x, std::max(0.0f, (bs.y - em) * 0.5f)));
        const bool enter = ImGui::InputText("##value", g_typeBuf, sizeof g_typeBuf,
                                            ImGuiInputTextFlags_EnterReturnsTrue |
                                            ImGuiInputTextFlags_CharsScientific |
                                            ImGuiInputTextFlags_AutoSelectAll);
        tell(key + "=");
        ImGui::PopStyleVar();
        if (enter || ImGui::IsItemDeactivated()) {
            char* end = nullptr;
            const float t = std::strtof(g_typeBuf, &end);
            if (end != g_typeBuf && std::isfinite(t)) { v = clampV(t); changed = true; }
            g_typing = 0;
        }
    } else {
        char buf[64];
        std::snprintf(buf, sizeof buf, fmt, v);
        const ImVec4 frame = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
        ImGui::PushStyleColor(ImGuiCol_Button, frame);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, frame);
        if (ImGui::Button(buf, ImVec2(mid, bs.y))) {
            g_typing = me;
            std::snprintf(g_typeBuf, sizeof g_typeBuf, "%g", v);
            g_typeFocus = true;
        }
        tell(key + "=");
        ImGui::PopStyleColor(3);
        ImGui::SetItemTooltip("Click to type a value");
    }
    ImGui::SameLine(0.0f, 3.0f);
    if (ImGui::Button("+", bs)) { v = clampV(v + step); changed = true; }
    tell(key + "+");
    ImGui::SetItemTooltip("%g more", step);
    ImGui::PopID();
    return changed;
}

// The node's colour, by what kind of step it is: shapes, combining, copies,
// detail, look. Muted, so the selection and the output flag stay the loudest
// things on the canvas.
ImVec4 categoryTint(const std::string& cat) {
    if (cat == "Shapes")  return ImVec4(0.20f, 0.45f, 0.45f, 1.0f);
    if (cat == "Curves")  return ImVec4(0.22f, 0.40f, 0.58f, 1.0f);
    if (cat == "Points")  return ImVec4(0.50f, 0.48f, 0.20f, 1.0f);
    if (cat == "Combine") return ImVec4(0.32f, 0.36f, 0.45f, 1.0f);
    if (cat == "Copies")  return ImVec4(0.42f, 0.30f, 0.52f, 1.0f);
    if (cat == "Detail")  return ImVec4(0.55f, 0.38f, 0.20f, 1.0f);
    if (cat == "Look")    return ImVec4(0.55f, 0.26f, 0.38f, 1.0f);
    return ImVec4(0.35f, 0.35f, 0.35f, 1.0f);
}
ImU32 col(const ImVec4& c, float a = 1.0f) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * a)); }

// --- Layout -------------------------------------------------------------------
// The canvas places every node itself; nobody drags one. Rows by distance from
// the end of the graph -- the output at the bottom, everything a step needs
// above it, as far up as its longest way down -- so each branch ends just above
// what it feeds. Within a row a node sits over the input it feeds (the
// barycentre of its consumers' input dots), pushed aside only as far as the
// row's neighbours need.
struct Layout {
    std::unordered_map<int, ImVec2> pos;   // top-left, in canvas units
    std::unordered_map<int, float>  width; // a node with many inputs is wider
    ImVec2 size{0.0f, 0.0f};
    float w(int id) const {
        auto it = width.find(id);
        return it == width.end() ? 0.0f : it->second;
    }
};

// Where on node `n` (top-left `p`, width `w`) input `slot` of `slots` sits.
float portX(const ImVec2& p, float w, int slot, int slots) {
    return p.x + w * static_cast<float>(slot + 1) / static_cast<float>(slots + 1);
}
int shownSlots(const proc::Node& n) {
    // A merge shows one dot more than it has wires: the free one to add to.
    return n.variadic() ? static_cast<int>(n.inputs.size()) + 1 : n.inputSlots();
}

// `w` is a node's least width; `dotGap` the room each input dot needs, so a
// merge of ten strands is wide enough that no two of its dots share a pixel of
// their click squares.
Layout layoutGraph(const proc::Graph& g, float w, float h, float gapX, float gapY, float pad,
                   float dotGap) {
    Layout L;
    for (const auto& n : g.nodes)
        L.width[n->id] = std::max(w, static_cast<float>(shownSlots(*n) + 1) * dotGap);
    // Who reads each node, and at which slot.
    std::unordered_map<int, std::vector<std::pair<int, int>>> readers;
    for (const auto& n : g.nodes)
        for (std::size_t s = 0; s < n->inputs.size(); ++s)
            if (n->inputs[s] >= 0 && g.find(n->inputs[s]))
                readers[n->inputs[s]].push_back({n->id, static_cast<int>(s)});

    // Rank = longest way down to a node nobody reads (0). Memoised; a loop
    // (only possible from a hand-edited file) is cut where it is met.
    std::unordered_map<int, int> rank;
    std::unordered_map<int, int> state;   // 1 visiting, 2 done
    std::function<int(int)> rankOf = [&](int id) -> int {
        if (state[id] == 2) return rank[id];
        if (state[id] == 1) return 0;
        state[id] = 1;
        int r = 0;
        for (const auto& [reader, slot] : readers[id]) r = std::max(r, rankOf(reader) + 1);
        rank[id] = r;
        state[id] = 2;
        return r;
    };
    int maxRank = 0;
    for (const auto& n : g.nodes) maxRank = std::max(maxRank, rankOf(n->id));

    std::vector<std::vector<int>> rows(static_cast<std::size_t>(maxRank + 1));
    for (const auto& n : g.nodes) rows[static_cast<std::size_t>(rank[n->id])].push_back(n->id);

    // Bottom row first: everything above is placed over what it feeds.
    float maxX = 0.0f;
    for (int r = 0; r <= maxRank; ++r) {
        std::vector<int>& row = rows[static_cast<std::size_t>(r)];
        const float y = pad + static_cast<float>(maxRank - r) * (h + gapY);
        std::vector<std::pair<float, int>> want;
        for (std::size_t i = 0; i < row.size(); ++i) {
            float sum = 0.0f;
            int   cnt = 0;
            for (const auto& [reader, slot] : readers[row[i]]) {
                auto it = L.pos.find(reader);
                const proc::Node* rn = g.find(reader);
                if (it == L.pos.end() || !rn) continue;
                sum += portX(it->second, L.w(reader), slot, shownSlots(*rn));
                ++cnt;
            }
            const float x = cnt > 0 ? sum / static_cast<float>(cnt) - 0.5f * L.w(row[i])
                                    : pad + static_cast<float>(i) * (w + gapX);
            want.push_back({x, row[i]});
        }
        std::stable_sort(want.begin(), want.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        float next = -1e9f;
        for (auto& [x, id] : want) {
            x = std::max(x, next);
            next = x + L.w(id) + gapX;
            L.pos[id] = ImVec2(x, y);
        }
        rows[static_cast<std::size_t>(r)].clear();
        for (const auto& [x, id] : want) rows[static_cast<std::size_t>(r)].push_back(id);
    }
    // Then back down: a node that takes inputs sits under the middle of them
    // (a merge under its ten strands, not at their left end), the row's order
    // kept and its neighbours pushed aside only as far as they must.
    for (int r = maxRank - 1; r >= 0; --r) {
        std::vector<int>& row = rows[static_cast<std::size_t>(r)];
        float next = -1e9f;
        for (int id : row) {
            const proc::Node* n = g.find(id);
            float x = L.pos[id].x;
            float sum = 0.0f;
            int   cnt = 0;
            for (int in : n->inputs) {
                auto it = L.pos.find(in);
                if (in < 0 || it == L.pos.end()) continue;
                sum += it->second.x + 0.5f * L.w(in);
                ++cnt;
            }
            if (cnt > 0) x = sum / static_cast<float>(cnt) - 0.5f * L.w(id);
            x = std::max(x, next);
            next = x + L.w(id) + gapX;
            L.pos[id].x = x;
        }
    }
    // Shift everything right if a row was pushed past the left edge.
    float minX = 1e9f;
    for (const auto& [id, p] : L.pos) minX = std::min(minX, p.x);
    const float dx = L.pos.empty() ? 0.0f : pad - minX;
    for (auto& [id, p] : L.pos) {
        p.x += dx;
        maxX = std::max(maxX, p.x + L.w(id));
    }
    L.size = ImVec2(maxX + pad, pad * 2.0f + static_cast<float>(maxRank + 1) * (h + gapY));
    return L;
}

// The canvas's measures, in text heights: everything scales with the zoom.
constexpr float kNodeW  = 10.0f;
constexpr float kNodeH  = 3.1f;
constexpr float kGapX   = 1.4f;
constexpr float kGapY   = 2.6f;
constexpr float kPad    = 1.5f;
constexpr float kDotGap = 2.2f;   // room each input dot needs on a node's top
constexpr float kGrid   = 1.0f;   // what a node let go of snaps to

float nodeWidth(const proc::Node& n) {
    return std::max(kNodeW, static_cast<float>(shownSlots(n) + 1) * kDotGap);
}

// The first spot from `want` on -- along the row, then the rows below -- where
// a node `w` wide stands clear of every node placed by hand. Where a new node
// goes once the graph is laid out by hand: below the picked one, beside it if
// that is taken, never on top of anything.
glm::vec2 freeSpot(const proc::Graph& g, glm::vec2 want, float w, int self) {
    for (int row = 0; row < 60; ++row)
        for (int k = 0; k < 60; ++k) {
            const glm::vec2 p = want + glm::vec2(static_cast<float>(k) * (kNodeW + kGapX),
                                                 static_cast<float>(row) * (kNodeH + kGapY));
            bool clear = true;
            for (const auto& n : g.nodes) {
                if (!n->placed || n->id == self) continue;
                const float nw = nodeWidth(*n);
                if (p.x < n->pos.x + nw + kGapX * 0.5f && n->pos.x < p.x + w + kGapX * 0.5f &&
                    p.y < n->pos.y + kNodeH + kGapY * 0.5f && n->pos.y < p.y + kNodeH + kGapY * 0.5f) {
                    clear = false;
                    break;
                }
            }
            if (clear) return p;
        }
    return want;
}

// Every node keeps the place the canvas gave it, from now on by hand: what the
// first move of any node does, so that moving one does not make the others
// jump to wherever a fresh layout would put them.
void pinAll(proc::Graph& g, const Layout& L, float em) {

    for (auto& n : g.nodes) {
        auto it = L.pos.find(n->id);
        if (it == L.pos.end() || n->placed) continue;
        n->pos    = glm::vec2(it->second.x / em, it->second.y / em);
        n->placed = true;
    }
}

float snapTo(float v, float step) { return std::round(v / step) * step; }

// Has the left button, held now or let go this frame, travelled past the drag
// threshold since it went down? GetMouseDragDelta says nothing until it has.
bool draggedLeft() {
    const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
    return d.x != 0.0f || d.y != 0.0f;
}

// --- A curve's points as handles ----------------------------------------------
// Any node whose settings hold a point list (a "points" text, as the Curve's
// does) gets handles in the viewport; the handles know no kind.
const std::string* pointsOf(const proc::Node& n) {
    for (const Property& pr : n.props())
        if (pr.kind == PropKind::Text && pr.key == "points")
            return static_cast<const std::string*>(pr.field(const_cast<proc::Node*>(&n)));
    return nullptr;
}
std::string* pointsOf(proc::Node& n) {
    return const_cast<std::string*>(pointsOf(static_cast<const proc::Node&>(n)));
}
bool closedOf(const proc::Node& n) {
    for (const Property& pr : n.props())
        if (pr.kind == PropKind::Bool && pr.key == "closed")
            return *static_cast<const bool*>(pr.field(const_cast<proc::Node*>(&n)));
    return false;
}

// Graph space -> world: back past the pivot the mesh was centred by, then the
// object's own transform.
glm::mat4 graphToWorld(const Entity& e, const MeshComponent& mc, const ProcGraphComponent& pg) {
    return meshModelOf(e, mc) * glm::translate(glm::mat4(1.0f), -pg.pivot);
}

// The axis a curve's handles do not drag along. A curve lying flat in one of
// the graph's planes stays in it -- a path on the ground, a profile standing
// up to be revolved -- and one that bends in all three is dragged across the
// ground. Ctrl at the grab, and Page Up/Down, move along this axis instead.
int squareAxis(const std::vector<glm::vec3>& pts) {
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (const glm::vec3& p : pts) {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    const glm::vec3 ext = hi - lo;
    if (ext.y <= 1e-3f) return 1;
    if (ext.z <= 1e-3f) return 2;
    if (ext.x <= 1e-3f) return 0;
    return 1;
}

// Is a key that nudges a picked point held? The burst it starts is one undo
// step, closed once every one of them is up.
bool nudgeHeld() {
    return ImGui::IsKeyDown(ImGuiKey_UpArrow) || ImGui::IsKeyDown(ImGuiKey_DownArrow) ||
           ImGui::IsKeyDown(ImGuiKey_LeftArrow) || ImGui::IsKeyDown(ImGuiKey_RightArrow) ||
           ImGui::IsKeyDown(ImGuiKey_PageUp) || ImGui::IsKeyDown(ImGuiKey_PageDown);
}

// How far from a handle (pixels) the pointer still grabs it: well past the dot,
// for a hand that cannot land on five pixels -- the spline tool's reach. A "+"
// reaches a little less, so a point beside one wins.
constexpr float kPointReach = 14.0f;
constexpr float kPlusReach  = 11.0f;
// A dragged point lands on a tenth of a metre: what the list shows, and raster
// enough to swallow a tremor. A nudge moves by whole steps and needs none.
constexpr float kPointSnap  = 0.1f;

} // namespace

// =============================================================================
// The object
// =============================================================================

namespace {

// One step of a graph's object for the undo history: the object itself, and
// the prefabs its graph had placed under it, before and after. A cook that
// changed the list takes the old placed objects out of the scene and puts new
// ones in, so undoing it has to swap them back -- the object's own snapshot
// alone would leave the new ones standing beside the old graph.
class ProcCookCmd : public Command {
public:
    ProcCookCmd(Entity rootBefore, Entity rootAfter, std::vector<Entity> madeBefore,
                std::vector<Entity> madeAfter)
        : m_rootBefore(std::move(rootBefore)), m_rootAfter(std::move(rootAfter)),
          m_madeBefore(std::move(madeBefore)), m_madeAfter(std::move(madeAfter)) {}
    void redo(Document& d) override { swapIn(d, m_madeBefore, m_madeAfter); assign(d, m_rootAfter); }
    void undo(Document& d) override { swapIn(d, m_madeAfter, m_madeBefore); assign(d, m_rootBefore); }
    const char* name() const override { return "Procedural"; }
    bool trivial() const {
        if (!sameEntity(m_rootBefore, m_rootAfter) || m_madeBefore.size() != m_madeAfter.size())
            return false;
        for (std::size_t i = 0; i < m_madeBefore.size(); ++i)
            if (!sameEntity(m_madeBefore[i], m_madeAfter[i])) return false;
        return true;
    }

private:
    static void assign(Document& d, const Entity& v) {
        if (Entity* e = d.find(v.id)) *e = v;
    }
    static void swapIn(Document& d, const std::vector<Entity>& out, const std::vector<Entity>& in) {
        auto& es = d.entities();
        es.erase(std::remove_if(es.begin(), es.end(), [&](const Entity& e) {
                     for (const Entity& o : out) if (o.id == e.id) return true;
                     return false;
                 }), es.end());
        for (const Entity& e : in) es.push_back(e);
    }
    Entity m_rootBefore, m_rootAfter;
    std::vector<Entity> m_madeBefore, m_madeAfter;
};

// Degrees in the scene's order (Rz * Ry * Rx) of a rotation matrix -- what
// an entity's rotation has to say for it to stand the way the graph put it.
glm::vec3 eulerOf(const glm::mat3& r) {
    const float y = std::asin(std::clamp(-r[0][2], -1.0f, 1.0f));
    const float x = std::atan2(r[1][2], r[2][2]);
    const float z = std::atan2(r[0][1], r[0][0]);
    return glm::degrees(glm::vec3(x, y, z));
}
glm::mat3 eulerMat(const glm::vec3& deg) {
    glm::mat4 m(1.0f);
    m = glm::rotate(m, glm::radians(deg.z), glm::vec3(0.0f, 0.0f, 1.0f));
    m = glm::rotate(m, glm::radians(deg.y), glm::vec3(0.0f, 1.0f, 0.0f));
    m = glm::rotate(m, glm::radians(deg.x), glm::vec3(1.0f, 0.0f, 0.0f));
    return glm::mat3(m);
}

} // namespace

Panel::Panel(Deps d) : m_d(std::move(d)) {}

std::vector<Entity> Panel::madeUnder(int owner) const {
    const std::vector<Entity>& es = m_d.ed.entities;
    std::vector<int> ids;
    for (const Entity& e : es)
        if (e.parent == owner && e.components.get<ProcMadeComponent>()) ids.push_back(e.id);
    // Everything below those, whatever depth (a prefab is a tree).
    for (bool grew = true; grew;) {
        grew = false;
        for (const Entity& e : es)
            if (e.parent >= 0 && std::find(ids.begin(), ids.end(), e.parent) != ids.end() &&
                std::find(ids.begin(), ids.end(), e.id) == ids.end()) {
                ids.push_back(e.id);
                grew = true;
            }
    }
    std::vector<Entity> out;
    for (const Entity& e : es)
        if (std::find(ids.begin(), ids.end(), e.id) != ids.end()) out.push_back(e);
    return out;
}

void Panel::placePrefabs(int owner) {
    EditorContext& ed = m_d.ed;
    // Which list this is, in one string: a placed object carries it, and a
    // cook that asks for the same list again finds it already there.
    std::string key;
    char buf[64];
    for (const proc::Instance& in : m_placeList) {
        key += in.prefab;
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 3; ++r) {
                std::snprintf(buf, sizeof buf, ",%.4g", in.xf[c][r]);
                key += buf;
            }
        key += ';';
    }
    std::snprintf(buf, sizeof buf, "|%.4g,%.4g,%.4g", m_placePivot.x, m_placePivot.y, m_placePivot.z);
    key += buf;
    std::snprintf(buf, sizeof buf, "%016llx",
                  static_cast<unsigned long long>(std::hash<std::string>{}(key)));
    const std::string sig = buf;

    std::vector<int> roots;
    bool same = true;
    for (const Entity& e : ed.entities)
        if (e.parent == owner)
            if (const auto* pm = e.components.get<ProcMadeComponent>()) {
                roots.push_back(e.id);
                same = same && pm->sig == sig;
            }
    if (roots.size() != m_placeList.size()) same = false;
    if (same) return;

    // Out with the old...
    const int active = ed.sel.activeId();
    std::vector<int> gone;
    for (const Entity& e : madeUnder(owner)) gone.push_back(e.id);
    ed.entities.erase(std::remove_if(ed.entities.begin(), ed.entities.end(), [&](const Entity& e) {
                          return std::find(gone.begin(), gone.end(), e.id) != gone.end();
                      }), ed.entities.end());
    // ...in with the new: each a fresh copy of its prefab, hung under the
    // object where the graph put it -- in the object's own space, which is the
    // graph's moved by the pivot its mesh was centred by.
    std::vector<std::string> missing;
    for (const proc::Instance& in : m_placeList) {
        std::vector<Entity> ents = spawnPrefab ? spawnPrefab(in.prefab, ed.entityCounter)
                                               : std::vector<Entity>{};
        if (ents.empty()) {
            if (std::find(missing.begin(), missing.end(), in.prefab) == missing.end())
                missing.push_back(in.prefab);
            continue;
        }
        glm::mat3 m(in.xf);
        glm::vec3 len(glm::length(m[0]), glm::length(m[1]), glm::length(m[2]));
        for (int k = 0; k < 3; ++k) m[k] /= std::max(len[k], 1e-6f);
        // A mirrored copy cannot be an object; it stands the right way round.
        if (glm::determinant(m) < 0.0f) m[0] = -m[0];
        const float s = std::cbrt(std::max(len.x * len.y * len.z, 1e-9f));
        Entity& root = ents.front();
        root.parent        = owner;
        root.localCenter   = glm::vec3(in.xf[3]) - m_placePivot;
        root.localRotation = eulerOf(m * eulerMat(root.localRotation));
        root.center        = root.localCenter;
        root.rotation      = root.localRotation;
        if (std::fabs(s - 1.0f) > 1e-4f)
            for (std::size_t i = 0; i < ents.size(); ++i) {
                ents[i].half *= s;
                if (i > 0) ents[i].localCenter *= s;
            }
        auto tag = std::make_unique<ProcMadeComponent>();
        tag->sig = sig;
        root.components.items.push_back(std::move(tag));
        for (Entity& e : ents) ed.entities.push_back(std::move(e));
    }
    if (!missing.empty()) {
        m_note = "No prefab called";
        for (const std::string& n : missing) m_note += " \"" + n + "\"";
        m_note += " in this project.";
    }
    if (active >= 0 && ed.document.find(active)) ed.sel.select(active);
}

void Panel::removePicked(Entity& e) {
    std::vector<int> ids = m_sel;
    if (ids.empty() && m_node >= 0) ids.push_back(m_node);
    if (ids.empty()) return;
    // One node taken out of a chain hands its place to its first input: the
    // pick goes there, so pressing the key again walks up the chain.
    int through = -1;
    if (ids.size() == 1)
        if (const proc::Node* n = e.components.get<ProcGraphComponent>()->graph.find(ids[0]))
            through = n->inputs.empty() ? -1 : n->inputs.front();
    change(e, [&](proc::Graph& g) {
        for (int id : ids) g.remove(id);
    });
    pickNode(through);
}

Entity* Panel::target() {
    EditorContext& ed = m_d.ed;
    if (!ed.sel.valid()) return nullptr;
    Entity& e = ed.entities[static_cast<std::size_t>(ed.sel.index())];
    return e.components.get<ProcGraphComponent>() ? &e : nullptr;
}

void Panel::change(Entity& e, const std::function<void(proc::Graph&)>& fn, bool cook) {
    if (m_editId != e.id) {
        commit();
        m_editId     = e.id;
        m_before     = e;
        m_madeBefore = madeUnder(e.id);
    }
    ProcGraphComponent* pg = e.components.get<ProcGraphComponent>();
    if (!pg) return;
    fn(pg->graph);
    if (cook) cookInto(e);
}

void Panel::commit() {
    if (m_editId < 0) return;
    EditorContext& ed = m_d.ed;
    // The prefabs the edit asked for go in first, so the step banks them.
    if (m_placeFor == m_editId) {
        placePrefabs(m_placeFor);
        m_placeFor = -1;
    }
    if (const Entity* e = ed.document.find(m_editId)) {
        auto cmd = std::make_unique<ProcCookCmd>(m_before, *e, m_madeBefore, madeUnder(m_editId));
        if (!cmd->trivial()) ed.history.pushApplied(std::move(cmd));
    }
    m_editId = -1;
    m_madeBefore.clear();
}

void Panel::cookInto(Entity& e) {
    ProcGraphComponent* pg = e.components.get<ProcGraphComponent>();
    if (!pg) return;
    MeshComponent* mc = e.components.get<MeshComponent>();
    if (!mc) {
        e.components.items.push_back(std::make_unique<MeshComponent>());
        mc = e.components.get<MeshComponent>();
        mc->mesh = EditMesh::box(e.half);
    }
    proc::CookInfo info;
    const proc::Geo geo = proc::cookGeo(pg->graph, pg->graph.output, &info);
    EditMesh m = proc::facesOf(geo);
    // Something to click, even when the output makes nothing (yet).
    if (m.faces.empty()) { m = EditMesh::box(glm::vec3(0.25f)); m.paint.clear(); }
    // The scale the object applies to its mesh (1 unless someone scaled it),
    // kept across the cook; the new geometry goes into the old mesh's space,
    // and the re-centre then moves the object by however far the middle of the
    // shape moved -- so what stayed the same in the graph stays put on screen.
    const glm::vec3 scale = editmesh::fitScale(mc->mesh, e.half);
    for (glm::vec3& v : m.verts) v -= pg->pivot;
    glm::vec3 mn, mx;
    m.bounds(mn, mx);
    const glm::vec3 shift = 0.5f * (mn + mx);
    mc->mesh = std::move(m);
    normalizeMeshEntity(m_d.ed.entities, e, *mc, scale);
    if (glm::dot(shift, shift) >= 1e-12f) pg->pivot += shift;
    // The prefabs it places go into the scene once the frame's drawing is
    // done -- that changes the entity list, which the panel is drawing from.
    m_placeFor   = e.id;
    m_placeList  = geo.instances;
    m_placePivot = pg->pivot;
    m_info     = std::move(info);
    m_infoFor  = e.id;
    m_infoHash = proc::hashOf(pg->graph);
}

int Panel::create(int preset) {
    EditorContext& ed = m_d.ed;
    commit();
    auto pg = std::make_unique<ProcGraphComponent>();
    pg->graph = procpreset::build(preset, ed.materials);
    EditMesh m = proc::cook(pg->graph, pg->graph.output);
    if (m.faces.empty()) { m = EditMesh::box(glm::vec3(0.25f)); m.paint.clear(); }
    pg->pivot = editmesh::recenter(m);
    glm::vec3 mn, mx;
    m.bounds(mn, mx);
    const glm::vec3 half = glm::max(0.5f * (mx - mn), glm::vec3(1e-3f));

    Entity e;
    e.type = EntityType::Box;
    const auto& presets = procpreset::list();
    e.name = preset >= 0 && preset < static_cast<int>(presets.size()) ? presets[static_cast<std::size_t>(preset)].name
                                                                       : "Procedural";
    e.id   = ed.entityCounter++;
    e.half = half;
    // Far enough ahead to see the whole of it, standing on the ground there.
    const float reach = std::max({half.x, half.y, half.z});
    const glm::vec3 at = m_d.spawnPoint ? m_d.spawnPoint(std::max(20.0f, 2.5f * reach))
                                        : glm::vec3(0.0f);
    e.center = e.localCenter = at + glm::vec3(0.0f, half.y, 0.0f);
    auto mc = std::make_unique<MeshComponent>();
    mc->mesh = std::move(m);
    mc->touch();
    e.components.items.push_back(std::move(mc));
    e.components.items.push_back(std::move(pg));
    // Round things read as round: tubes and rings shaded smooth, creases kept.
    auto ms = std::make_unique<ModifierStackComponent>();
    ms->smooth      = true;
    ms->smoothAngle = 40.0f;
    e.components.items.push_back(std::move(ms));

    const int id = e.id;
    ed.history.push(std::make_unique<AddEntityCmd>(std::move(e)), ed.document);
    ed.sel.select(id);
    m_node = -1;
    m_nodeOf = -1;
    ed.status = std::string("Made a procedural object: ") +
                (preset >= 0 && preset < static_cast<int>(presets.size())
                     ? presets[static_cast<std::size_t>(preset)].name : "Procedural") + ".";
    return id;
}

bool Panel::handles(const ViewportFrame& view) {
    g_probe   = &probe;
    m_ptHover = m_plusHover = -1;
    m_ptKeys  = false;
    Entity* e = m_open ? target() : nullptr;
    ProcGraphComponent* pg = e ? e->components.get<ProcGraphComponent>() : nullptr;
    const MeshComponent* mc = e ? e->components.get<MeshComponent>() : nullptr;
    // Only once the window has shown this object: until then the picked node
    // is another graph's.
    proc::Node* node = (pg && mc && m_nodeOf == e->id) ? pg->graph.find(m_node) : nullptr;
    const std::string* src = node ? pointsOf(*node) : nullptr;
    if (!src) {
        m_pt     = -1;
        m_ptDrag = false;
        return false;
    }
    if (m_ptFor != e->id || m_ptNode != node->id) {
        // Another curve: nothing of it picked yet.
        m_ptFor  = e->id;
        m_ptNode = node->id;
        m_pt     = -1;
        m_ptDrag = false;
    }
    std::vector<glm::vec3> pts = proc::parsePoints(*src);
    auto P = [&](int i) -> glm::vec3& { return pts[static_cast<std::size_t>(i)]; };
    const int  n      = static_cast<int>(pts.size());
    const bool closed = closedOf(*node) && n >= 3;
    const int  stretches = n < 2 ? 0 : (closed ? n : n - 1);
    if (m_pt >= n) m_pt = -1;
    const int nodeId = node->id;
    const glm::mat4 model   = graphToWorld(*e, *mc, *pg);
    const glm::mat4 toGraph = glm::inverse(model);
    auto screen = [&](const glm::vec3& p, ImVec2& out) {
        return view.project(glm::vec3(model * glm::vec4(p, 1.0f)), out);
    };
    auto midOf = [&](int i) { return 0.5f * (P(i) + P((i + 1) % n)); };
    // The points written back: one change, cooked. Its undo step stays open
    // while a drag or a burst of nudges goes on (see draw()).
    auto write = [&]() {
        const std::string text = proc::formatPoints(pts);
        change(*e, [&](proc::Graph& g) {
            if (proc::Node* nn = g.find(nodeId))
                if (std::string* s = pointsOf(*nn)) *s = text;
        });
    };
    // Where the pointer's ray meets the plane the grabbed point moves in.
    auto rayOnPlane = [&](glm::vec3& hit) {
        glm::vec3 ro, rd;
        view.mouseRay(ro, rd);
        const glm::vec3 o(toGraph * glm::vec4(ro, 1.0f));
        const glm::vec3 d(toGraph * glm::vec4(rd, 0.0f));
        const int   k   = m_ptAxis;
        const float len = glm::length(d);
        // A plane seen edge-on gives no sensible hit: the point holds still.
        if (len < 1e-9f || std::abs(d[k]) < 0.03f * len) return false;
        const float t = (m_ptStart[k] - o[k]) / d[k];
        if (t <= 0.0f) return false;
        hit = o + d * t;
        return true;
    };
    const ImGuiIO& io = ImGui::GetIO();

    // --- What the pointer is over: a point first, else a stretch's "+" ---------
    if (view.hovered && !m_ptDrag) {
        float best = kPointReach;
        for (int i = 0; i < n; ++i) {
            ImVec2 s;
            if (!screen(P(i), s)) continue;
            const float d = std::hypot(s.x - view.mousePos.x, s.y - view.mousePos.y);
            if (d < best) { best = d; m_ptHover = i; }
        }
        if (m_ptHover < 0) {
            best = kPlusReach;
            for (int i = 0; i < stretches; ++i) {
                ImVec2 s;
                if (!screen(midOf(i), s)) continue;
                const float d = std::hypot(s.x - view.mousePos.x, s.y - view.mousePos.y);
                if (d < best) { best = d; m_plusHover = i; }
            }
        }
    }
    // The harness clicks handles by name, like the window's own controls.
    if (g_probe && *g_probe) {
        for (int i = 0; i < n + stretches; ++i) {
            ImVec2 s;
            const bool pt = i < n;
            if (!screen(pt ? P(i) : midOf(i - n), s)) continue;
            (*g_probe)((pt ? "pt:" : "plus:") + std::to_string(pt ? i : i - n),
                       ImVec2(s.x - 3.0f, s.y - 3.0f), ImVec2(s.x + 3.0f, s.y + 3.0f));
        }
    }

    // --- A press: grab a point, or put a new one in on a stretch ---------------
    auto grab = [&](int i) {
        m_pt       = i;
        m_ptScroll = true;
        m_ptDrag   = true;
        m_ptMoved  = false;
        m_ptLift   = io.KeyCtrl;
        m_ptAxis   = squareAxis(pts);
        m_ptStart  = P(i);
        m_ptPress  = view.mousePos;
        m_ptGrab   = glm::vec3(0.0f);
        glm::vec3 hit;
        if (!m_ptLift && rayOnPlane(hit)) m_ptGrab = m_ptStart - hit;
    };
    if (view.hovered && !m_ptDrag && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (m_ptHover >= 0) {
            grab(m_ptHover);
        } else if (m_plusHover >= 0) {
            // Halfway along the stretch, picked and held: a drag from the "+"
            // carries the new point straight on.
            const int at = m_plusHover + 1;
            const glm::vec3 mid = midOf(m_plusHover);
            pts.insert(pts.begin() + at, mid);
            write();
            grab(at);
            m_plusHover = -1;
        } else {
            m_pt = -1;   // a click anywhere else lets the point go
        }
    }

    // --- The drag ----------------------------------------------------------------
    // Absolute from the press, like every drag here: where the pointer is now
    // says where the point is -- the gesture, not the journey -- so a hand
    // that overshoots and comes back leaves the point where it stopped.
    if (m_ptDrag) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || m_pt < 0 ||
            m_pt >= static_cast<int>(pts.size())) {
            m_ptDrag = false;
        } else {
            const ImVec2 mp = view.mousePos;
            // A press that has not travelled is a click: it picks, and changes nothing.
            if (std::hypot(mp.x - m_ptPress.x, mp.y - m_ptPress.y) >= io.MouseDragThreshold)
                m_ptMoved = true;
            if (m_ptMoved) {
                const int k  = m_ptAxis;
                glm::vec3 to = m_ptStart;
                bool      ok = false;
                if (m_ptLift) {
                    // Square to the plane: as far along the axis as the pointer
                    // went along the axis's picture on screen.
                    glm::vec3 axis(0.0f);
                    axis[k] = 1.0f;
                    ImVec2 a, b;
                    if (screen(m_ptStart, a) && screen(m_ptStart + axis, b)) {
                        const glm::vec2 v(b.x - a.x, b.y - a.y);
                        const float vv = glm::dot(v, v);
                        if (vv > 1.0f) {   // an axis pointing at the eye moves nothing
                            const glm::vec2 dm(mp.x - m_ptPress.x, mp.y - m_ptPress.y);
                            to[k] = snapTo(m_ptStart[k] + glm::dot(dm, v) / vv, kPointSnap);
                            ok = true;
                        }
                    }
                } else {
                    glm::vec3 hit;
                    if (rayOnPlane(hit)) {
                        to = hit + m_ptGrab;
                        for (int c = 0; c < 3; ++c) to[c] = c == k ? m_ptStart[k] : snapTo(to[c], kPointSnap);
                        ok = true;
                    }
                }
                if (ok && to != P(m_pt)) {
                    P(m_pt) = to;
                    write();
                }
            }
        }
    }

    // --- Keys, for the picked point while the pointer is on the scene ----------
    if (m_pt >= 0 && m_pt < static_cast<int>(pts.size()) && view.hovered && !m_ptDrag &&
        !io.WantTextInput) {
        // The Delete key is the point's: never the object's, nor the window's
        // (picked from the list, the window may still have the focus).
        m_ptKeys      = true;
        m_ptKeysFrame = ImGui::GetFrameCount();
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
            if (pts.size() > 2) {   // a curve keeps two at least
                pts.erase(pts.begin() + m_pt);
                write();
                // The one before is picked next: a second press -- meant or
                // not -- takes another point, never the whole object.
                m_pt = std::max(0, m_pt - 1);
            }
        } else {
            // Camera relative, because "left" means what you see. Of the two
            // axes in the plane, the one nearer the camera's right is
            // Left/Right; the other is Up/Down -- up on screen, or away from
            // the camera for a plane seen flat on. Page Up/Down go square to it.
            const float step = io.KeyShift ? 2.5f : 0.5f;
            const int   k = squareAxis(pts);
            const int   a = (k + 1) % 3, b = (k + 2) % 3;
            const glm::mat3 m3(model);
            auto worldDir = [&](int ax) {
                glm::vec3 u(0.0f);
                u[ax] = 1.0f;
                const glm::vec3 w = m3 * u;
                return glm::length(w) > 1e-9f ? glm::normalize(w) : u;
            };
            const glm::vec3 fwd = glm::length(view.cameraFront) > 1e-6f ? glm::normalize(view.cameraFront)
                                                                       : glm::vec3(0.0f, 0.0f, -1.0f);
            glm::vec3 right = glm::cross(fwd, glm::vec3(0.0f, 1.0f, 0.0f));
            right = glm::length(right) > 1e-4f ? glm::normalize(right) : glm::vec3(1.0f, 0.0f, 0.0f);
            const glm::vec3 up = glm::cross(right, fwd);
            const glm::vec3 wa = worldDir(a), wb = worldDir(b);
            const bool  aRight = std::abs(glm::dot(wa, right)) >= std::abs(glm::dot(wb, right));
            const int   rAx = aRight ? a : b, uAx = aRight ? b : a;
            const glm::vec3 wr = aRight ? wa : wb, wu = aRight ? wb : wa;
            const float rSign = glm::dot(wr, right) >= 0.0f ? 1.0f : -1.0f;
            const float onUp  = glm::dot(wu, up);
            const float uSign = (std::abs(onUp) > 0.1f ? onUp : glm::dot(wu, fwd)) >= 0.0f ? 1.0f : -1.0f;
            glm::vec3 d(0.0f);
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) d[rAx] += rSign * step;
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow,  true)) d[rAx] -= rSign * step;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow,    true)) d[uAx] += uSign * step;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow,  true)) d[uAx] -= uSign * step;
            if (ImGui::IsKeyPressed(ImGuiKey_PageUp,     true)) d[k] += step;
            if (ImGui::IsKeyPressed(ImGuiKey_PageDown,   true)) d[k] -= step;
            if (d != glm::vec3(0.0f)) {
                P(m_pt) += d;
                m_ptNudge = true;
                write();
            }
        }
    }
    if (!nudgeHeld()) m_ptNudge = false;
    return m_ptDrag || m_ptHover >= 0 || m_plusHover >= 0;
}

void Panel::viewport(const ViewportFrame& view) {
    if (!m_open) return;
    Entity* e = target();
    if (!e) return;
    const ProcGraphComponent* pg = e->components.get<ProcGraphComponent>();
    const MeshComponent*      mc = e->components.get<MeshComponent>();
    if (!pg || !mc) return;
    const int node = pg->graph.find(m_node) ? m_node : pg->graph.output;
    const std::size_t h = proc::hashOf(pg->graph);
    if (m_overlayFor != e->id || m_overlayNode != node || m_overlayHash != h) {
        m_overlay     = proc::cookGeo(pg->graph, node);
        m_overlayFor  = e->id;
        m_overlayNode = node;
        m_overlayHash = h;
    }
    const glm::mat4 model = graphToWorld(*e, *mc, *pg);
    auto screen = [&](const glm::vec3& p, ImVec2& out) {
        return view.project(glm::vec3(model * glm::vec4(p, 1.0f)), out);
    };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 line = IM_COL32(90, 200, 255, 235);
    const ImU32 lit  = IM_COL32(255, 170, 40, 255);
    const ImU32 dim  = IM_COL32(205, 205, 215, 140);
    // A Curve picked: its own points become handles (handles()), and the dots
    // of what it made would only crowd them.
    const proc::Node*  hn   = m_nodeOf == e->id ? pg->graph.find(m_node) : nullptr;
    const std::string* hsrc = hn ? pointsOf(*hn) : nullptr;
    const std::vector<glm::vec3> hpts = hsrc ? proc::parsePoints(*hsrc) : std::vector<glm::vec3>{};
    const int  hcount    = static_cast<int>(hpts.size());
    const bool hclosed = hn && closedOf(*hn) && hcount >= 3;
    const int  hstretches = hcount < 2 ? 0 : (hclosed ? hcount : hcount - 1);
    auto H = [&](int i) { return hpts[static_cast<std::size_t>(i % hcount)]; };
    // The line through the points as given, faint, under the curve: a smoothed
    // curve bends near it, not through the middle of each stretch.
    for (int i = 0; i < hstretches; ++i) {
        ImVec2 a, b;
        if (screen(H(i), a) && screen(H(i + 1), b)) dl->AddLine(a, b, IM_COL32(205, 205, 215, 110), 1.0f);
    }
    const proc::Geo& g = m_overlay;
    for (const proc::Curve& c : g.curves) {
        const int n = static_cast<int>(c.pts.size());
        const int segs = c.loose ? 0 : c.closed ? n : n - 1;
        for (int i = 0; i < segs; ++i) {
            ImVec2 a, b;
            if (screen(c.pts[static_cast<std::size_t>(i)], a) &&
                screen(c.pts[static_cast<std::size_t>((i + 1) % n)], b))
                dl->AddLine(a, b, line, 2.0f);
        }
        if (hsrc) continue;
        for (int i = 0; i < n; ++i) {
            ImVec2 s;
            if (!screen(c.pts[static_cast<std::size_t>(i)], s)) continue;
            const bool picked = g.hasSel && proc::Geo::curvePicked(c, i, true);
            dl->AddCircleFilled(s, picked ? 5.5f : 3.0f, picked ? lit : line);
        }
    }
    // The handles: a "+" halfway along each stretch, the points on top -- the
    // picked one lit, with where it is -- and while a point is lifted square
    // to its plane, the line it travels on.
    if (hsrc) {
        if (m_ptDrag && m_ptMoved && m_ptLift) {
            glm::vec3 axis(0.0f);
            axis[m_ptAxis] = 1.0f;
            ImVec2 a, b;
            if (screen(m_ptStart - axis * 50.0f, a) && screen(m_ptStart + axis * 50.0f, b))
                dl->AddLine(a, b, IM_COL32(255, 210, 60, 120), 1.5f);
        }
        for (int i = 0; i < hstretches; ++i) {
            ImVec2 s;
            if (!screen(0.5f * (H(i) + H(i + 1)), s)) continue;
            const bool  hover = i == m_plusHover;
            const float r     = hover ? 6.5f : 4.5f;
            const ImU32 c     = hover ? IM_COL32(255, 255, 255, 255) : IM_COL32(215, 225, 235, 200);
            dl->AddCircleFilled(s, r, IM_COL32(20, 24, 30, 170));
            dl->AddCircle(s, r, c, 0, 1.5f);
            dl->AddLine(ImVec2(s.x - r * 0.55f, s.y), ImVec2(s.x + r * 0.55f, s.y), c, 1.5f);
            dl->AddLine(ImVec2(s.x, s.y - r * 0.55f), ImVec2(s.x, s.y + r * 0.55f), c, 1.5f);
        }
        for (int i = 0; i < hcount; ++i) {
            ImVec2 s;
            if (!screen(H(i), s)) continue;
            const bool  picked = i == m_pt;
            const bool  hover  = i == m_ptHover;
            const float rad    = picked ? 7.0f : (hover ? 6.5f : 5.0f);
            const ImU32 c      = picked ? IM_COL32(255, 210, 60, 255)
                               : hover  ? IM_COL32(210, 235, 255, 255)
                                        : IM_COL32(90, 180, 255, 235);
            dl->AddCircleFilled(s, rad, c);
            dl->AddCircle(s, rad, IM_COL32(0, 0, 0, 190), 0, 1.5f);
            if (picked) {
                const glm::vec3 p = H(i);
                char label[64];
                std::snprintf(label, sizeof label, "%.1f  %.1f  %.1f", p.x, p.y, p.z);
                const ImVec2 at(s.x + rad + 3.0f, s.y + 2.0f);
                dl->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f), IM_COL32(0, 0, 0, 200), label);
                dl->AddText(at, IM_COL32(255, 225, 140, 245), label);
            }
        }
    }
    // Where prefabs go: a small cross each, so a Prefab node's placements can
    // be seen before (or without) the objects themselves.
    for (const proc::Instance& in : g.instances) {
        ImVec2 c, x, y, z;
        const glm::vec3 o(in.xf[3]);
        if (!screen(o, c)) continue;
        if (screen(o + glm::normalize(glm::vec3(in.xf[0])) * 1.5f, x)) dl->AddLine(c, x, IM_COL32(240, 90, 80, 230), 2.0f);
        if (screen(o + glm::normalize(glm::vec3(in.xf[1])) * 1.5f, y)) dl->AddLine(c, y, IM_COL32(110, 220, 110, 230), 2.0f);
        if (screen(o + glm::normalize(glm::vec3(in.xf[2])) * 1.5f, z)) dl->AddLine(c, z, IM_COL32(90, 150, 255, 230), 2.0f);
        dl->AddCircleFilled(c, 4.0f, lit);
    }
    // The faces' corners only once someone selects among them: a hull of ten
    // thousand dots says nothing until some of them are lit.
    if (g.hasSel) {
        const bool many = g.mesh.verts.size() > 20000;
        for (int i = 0; i < static_cast<int>(g.mesh.verts.size()); ++i) {
            const bool picked = g.meshPicked(i);
            if (!picked && many) continue;
            ImVec2 s;
            if (!screen(g.mesh.verts[static_cast<std::size_t>(i)], s)) continue;
            dl->AddCircleFilled(s, picked ? 4.5f : 2.0f, picked ? lit : dim);
        }
    }
}

void Panel::refreshInfo(const Entity& e, const ProcGraphComponent& pg) {
    const std::size_t h = proc::hashOf(pg.graph);
    if (m_infoFor == e.id && m_infoHash == h) return;
    // Undo, another object, or the scene just loaded: the mesh is already
    // what it should be, only the per-node numbers are missing.
    m_info = proc::CookInfo{};
    proc::cook(pg.graph, pg.graph.output, &m_info);
    m_infoFor  = e.id;
    m_infoHash = h;
}
// =============================================================================
// The window
// =============================================================================

void Panel::draw(bool& show) {
    g_probe = &probe;
    m_open = false;
    m_keys = false;
    // A drag or a burst of nudges of the curve handles ends with its button
    // and its keys, whether or not handles() ran this frame.
    if (m_ptDrag && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_ptDrag = false;
    if (m_ptNudge && !nudgeHeld()) m_ptNudge = false;
    if (!show) { commit(); return; }
    ImGui::SetNextWindowSize(ImVec2(1000.0f, 640.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Procedural", &show)) {
        ImGui::End();
        commit();
        return;
    }
    m_open = true;
    // The keyboard is this window's while the pointer is over it (or over a
    // menu it opened) or it has the focus: then main leaves the Delete key be.
    m_keys = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) ||
             ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    Entity* e = target();
    ProcGraphComponent* pg = e ? e->components.get<ProcGraphComponent>() : nullptr;
    if (!e || !pg) {
        m_wireFrom = m_wireTo = -1;
        newObjectTiles();
    } else {
        if (m_nodeOf != e->id) {
            // Another object: start at what it shows.
            m_nodeOf = e->id;
            m_node   = pg->graph.output;
            m_scrollTo = true;
            m_wireFrom = m_wireTo = -1;
            m_wireDragFrom = m_wireDragTo = -1;
            m_dragNode = m_moveArmed = -1;
            m_boxing = false;
            m_sel.clear();
            if (m_node >= 0) m_sel.push_back(m_node);
            m_note.clear();
        }
        if (!pg->graph.find(m_node)) m_node = -1;
        // The pick, honest after an undo or a delete: only nodes that exist,
        // and the one whose settings show always among them.
        m_sel.erase(std::remove_if(m_sel.begin(), m_sel.end(),
                                   [&](int id) { return !pg->graph.find(id); }), m_sel.end());
        if (m_node >= 0 && std::find(m_sel.begin(), m_sel.end(), m_node) == m_sel.end())
            m_sel.push_back(m_node);
        refreshInfo(*e, *pg);
        toolbar(*e, *pg);

        const float avail = ImGui::GetContentRegionAvail().x;
        const float right = std::clamp(avail * 0.4f, ImGui::GetFontSize() * 22.0f,
                                       ImGui::GetFontSize() * 30.0f);
        ImGui::BeginChild("##procCanvas", ImVec2(std::max(avail - right - 8.0f, 100.0f), 0.0f),
                          ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        // The canvas and the settings may both change the graph; each looks
        // the object up again rather than trusting a pointer across a cook.
        canvas(*e, *pg);
        ImGui::EndChild();
        tell("canvasview");
        ImGui::SameLine();
        ImGui::BeginChild("##procSettings", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
        if (Entity* e2 = target())
            if (ProcGraphComponent* pg2 = e2->components.get<ProcGraphComponent>())
                settings(*e2, *pg2);
        ImGui::EndChild();
    }
    if (m_placeFor >= 0) {
        placePrefabs(m_placeFor);
        m_placeFor = -1;
    }
    if (m_pendingCreate >= 0) {
        create(m_pendingCreate);
        m_pendingCreate = -1;
    }
    // One interaction, one undo step: banked as soon as nothing is held down
    // (a stepper's click is over, a typed value was entered, a curve point's
    // drag or burst of nudges in the scene has ended).
    if (!ImGui::IsAnyItemActive() && !m_ptDrag && !m_ptNudge) commit();
    ImGui::End();
}

void Panel::newObjectTiles() {
    ui::title("No procedural object selected");
    ui::hint("Select one in the scene, or start a new one from a preset. A procedural\n"
             "object is a graph of nodes -- shapes, copies, panels, materials -- cooked\n"
             "into an ordinary mesh every time a number changes.");
    ImGui::Spacing();
    const float em = ImGui::GetFontSize();
    const ImVec2 tile(em * 12.0f, em * 4.0f);
    const auto& presets = procpreset::list();
    for (std::size_t i = 0; i < presets.size(); ++i) {
        if (i > 0) ImGui::SameLine();
        const bool make = ImGui::Button(presets[i].name, tile);
        tell("tile:" + std::to_string(i));
        ImGui::SetItemTooltip("%s", presets[i].tip);
        if (make) m_pendingCreate = static_cast<int>(i);
    }
}

void Panel::toolbar(Entity& e, ProcGraphComponent& pg) {
    const float em = ImGui::GetFontSize();
    const ImVec2 big(0.0f, ImGui::GetFrameHeight() + em * 0.35f);
    if (ImGui::Button("Add node", ImVec2(em * 7.0f, big.y))) ImGui::OpenPopup("##procAdd");
    tell("add");
    ImGui::SetItemTooltip("A new node, not wired to anything yet: wire it by dragging\n"
                          "from a dot to another, or by clicking the two.");
    if (ImGui::BeginPopup("##procAdd")) {
        std::string cat;
        for (const proc::TypeInfo& t : proc::registry()) {
            if (t.category != cat) {
                cat = t.category;
                ui::sectionText(cat.c_str());
            }
            const bool pick = ImGui::Selectable(t.displayName.c_str(), false, 0, ImVec2(em * 12.0f, em * 1.5f));
            tell("add:" + t.typeId);
            if (!t.tip.empty()) ImGui::SetItemTooltip("%s", t.tip.c_str());
            if (pick) addNode(e, t.typeId);
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("New...", ImVec2(em * 5.0f, big.y))) ImGui::OpenPopup("##procNew");
    ImGui::SetItemTooltip("Another procedural object, from a preset.");
    if (ImGui::BeginPopup("##procNew")) {
        const auto& presets = procpreset::list();
        for (std::size_t i = 0; i < presets.size(); ++i) {
            const bool pick = ImGui::Selectable(presets[i].name, false, 0, ImVec2(em * 12.0f, em * 1.5f));
            tell("new:" + std::to_string(i));
            ImGui::SetItemTooltip("%s", presets[i].tip);
            if (pick) m_pendingCreate = static_cast<int>(i);
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    const bool rebuild = ImGui::Button("Rebuild", ImVec2(em * 5.0f, big.y));
    tell("rebuild");
    if (rebuild) change(e, [](proc::Graph&) {});
    ImGui::SetItemTooltip("Cook the graph again -- after a material was changed or\n"
                          "removed, or when the mesh was edited by hand.");
    // The canvas's zoom: three plain buttons, never a wheel or a pinch.
    ImGui::SameLine(0.0f, em * 1.2f);
    if (ImGui::Button("-##zoom", ImVec2(em * 2.2f, big.y))) m_zoom = std::max(0.3f, m_zoom / 1.25f);
    tell("zoom-");
    ImGui::SetItemTooltip("Smaller nodes: more of the graph at once");
    ImGui::SameLine(0.0f, 3.0f);
    if (ImGui::Button("Fit", ImVec2(em * 3.2f, big.y))) m_fit = true;
    tell("fit");
    ImGui::SetItemTooltip("The whole graph in view");
    ImGui::SameLine(0.0f, 3.0f);
    if (ImGui::Button("+##zoom", ImVec2(em * 2.2f, big.y))) m_zoom = std::min(2.0f, m_zoom * 1.25f);
    tell("zoom+");
    ImGui::SetItemTooltip("Bigger nodes");
    ImGui::SameLine(0.0f, em * 1.2f);
    ImGui::BeginDisabled(!pg.graph.placedByHand());
    const bool arrange = ImGui::Button("Arrange", ImVec2(em * 5.5f, big.y));
    ImGui::EndDisabled();
    tell("arrange");
    ImGui::SetItemTooltip("Lay the graph out again by itself. Nodes you moved go back\n"
                          "into the layout (Undo brings your positions back).");
    if (arrange) {
        change(e, [](proc::Graph& g) { for (auto& n : g.nodes) n->placed = false; }, false);
        m_scrollTo = true;
    }
    ImGui::SameLine();
    const MeshComponent* mc = e.components.get<MeshComponent>();
    const proc::Node* out = pg.graph.find(pg.graph.output);
    ImGui::AlignTextToFramePadding();
    ui::hint("%s  |  %zu nodes  |  output: %s  |  %zu faces  |  %.1f ms",
             e.name.c_str(), pg.graph.nodes.size(), out ? out->name.c_str() : "(none)",
             mc ? mc->mesh.faces.size() : std::size_t(0), m_info.ms);
    // The line under the toolbar says what a click will do -- in a space of
    // fixed height, so the canvas below never jumps when the message changes
    // length: a target that moves the moment it is about to be clicked is the
    // last thing this editor may do to a shaking hand.
    const float statusY = ImGui::GetCursorPosY();
    const float statusH = ImGui::GetTextLineHeightWithSpacing() * 3.0f;
    if (!m_note.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.35f, 1.0f), "%s", m_note.c_str());
    } else if (const proc::Node* mv = pg.graph.find(m_moveArmed)) {
        ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.0f, 1.0f),
                           "Moving %s: click an empty spot on the canvas. A node cancels.",
                           mv->name.c_str());
    } else if (m_wireFrom >= 0 || m_wireTo >= 0) {
        ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.0f, 1.0f), "%s",
            m_wireFrom >= 0
                ? "Wiring: now click the node (or input dot) this should feed. Background cancels."
                : "Wiring: now click the node that should feed this input. Background cancels.");
    } else {
        ui::hint("Click picks a node (Shift: more), a drag on empty space boxes several; the\n"
                 "Delete key removes them. Shift+A or a right-click adds at the pointer. Drag\n"
                 "from a dot to wire (or click the dot, then the other end).");
    }
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), statusY + statusH));
}

void Panel::addNode(Entity& e, const std::string& kind) {
    const int picked = m_node;
    int made = -1;
    change(e, [&](proc::Graph& g) {
        std::unique_ptr<proc::Node> n = proc::make(kind);
        if (!n) return;
        const bool byHand = g.placedByHand();
        proc::Node& added = g.add(std::move(n));
        made = added.id;
        // Not wired to anything: that is the author's. Only the first node of
        // an empty graph is shown by itself -- there is nothing to wire it to.
        if (!g.find(g.output)) g.output = made;
        if (byHand) {
            // A graph laid out by hand keeps its layout: the newcomer goes
            // below the picked node, or beside it, wherever there is room.
            const proc::Node* at = g.find(picked);
            const glm::vec2 want = (at && at->placed) ? at->pos + glm::vec2(0.0f, kNodeH + kGapY)
                                                      : glm::vec2(kPad);
            added.pos    = freeSpot(g, want, nodeWidth(added), made);
            added.placed = true;
        }
    });
    if (made >= 0) pickNode(made);
}
// =============================================================================
// The canvas
// =============================================================================

void Panel::canvas(Entity& e, ProcGraphComponent& pg) {
    // The zoom is the text size: everything below is measured in it, so the
    // nodes, the gaps and the dots all scale with the writing on them.
    ImGui::PushFont(nullptr, ImGui::GetFontSize() * m_zoom);
    struct PopAtEnd { ~PopAtEnd() { ImGui::PopFont(); } } popAtEnd;
    ImGuiIO& io = ImGui::GetIO();
    const float em  = ImGui::GetFontSize();
    const float W   = em * kNodeW;
    const float H   = em * kNodeH;
    const float pad = em * kPad;
    const float dotR = std::max(em * 0.32f, 5.0f);
    // Dots are clicked by a big square -- as big as the gap between two dots
    // on a merge allows, so no two squares share a pixel.
    const float hitR = std::min(std::max(em * 0.75f, 12.0f), em * 1.0f);

    const proc::Graph& g = pg.graph;
    // The layout the canvas would make by itself; nodes placed by hand stand
    // where they were put instead.
    Layout L = layoutGraph(g, W, H, em * kGapX, em * kGapY, pad, em * kDotGap);
    if (g.placedByHand()) {
        float maxX = 0.0f, maxY = 0.0f;
        for (const auto& n : g.nodes) {
            if (n->placed) L.pos[n->id] = ImVec2(n->pos.x * em, n->pos.y * em);
            const ImVec2 p = L.pos[n->id];
            maxX = std::max(maxX, p.x + L.w(n->id));
            maxY = std::max(maxY, p.y + H);
        }
        L.size = ImVec2(maxX + pad, maxY + pad + em * kGapY);
    }
    const ImVec2 view = ImGui::GetContentRegionAvail();
    if (m_fit) {
        // Applied next frame, at the new size: the layout scales with the zoom.
        m_fit = false;
        const float k = std::min(view.x / std::max(L.size.x, 1.0f), view.y / std::max(L.size.y, 1.0f));
        m_zoom = std::clamp(m_zoom * k * 0.97f, 0.3f, 2.0f);
        ImGui::SetScrollX(0.0f);
        ImGui::SetScrollY(0.0f);
    } else if (m_scrollTo) {
        m_scrollTo = false;
        if (auto it = L.pos.find(m_node); it != L.pos.end()) {
            ImGui::SetScrollX(std::max(0.0f, it->second.x + 0.5f * L.w(m_node) - 0.5f * view.x));
            ImGui::SetScrollY(std::max(0.0f, it->second.y + 0.5f * H - 0.5f * view.y));
        }
    }
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto at = [&](const ImVec2& p) { return ImVec2(origin.x + p.x, origin.y + p.y); };
    auto inDot = [&](int id, int s) {
        const proc::Node* n = g.find(id);
        const ImVec2 p = at(L.pos[id]);
        return ImVec2(portX(p, L.w(id), s, n ? shownSlots(*n) : 1), p.y);
    };
    auto outDot = [&](int id) {
        const ImVec2 p = at(L.pos[id]);
        return ImVec2(p.x + 0.5f * L.w(id), p.y + H);
    };
    auto picked = [&](int id) { return std::find(m_sel.begin(), m_sel.end(), id) != m_sel.end(); };
    // Where the pointer is on the canvas, in text heights: where a node made
    // from the Add menu goes.
    auto canvasAt = [&](ImVec2 screen) {
        return glm::vec2((screen.x - origin.x) / em, (screen.y - origin.y) / em);
    };
    const bool canvasHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    // ...unless a curve point picked in the scene has them (handles()).
    const bool keys = m_keys && !io.WantTextInput && !pointKeys();

    // The background first: it gives the canvas its extent (the scroll bars)
    // and takes the clicks nothing else takes -- a click puts down a wire in
    // the making (or a node being moved) and drops the pick, a drag draws a
    // box round nodes. Submitted before the nodes and open to overlap, so
    // every node and dot drawn over it keeps its own clicks.
    const ImVec2 room(std::max(L.size.x, view.x), std::max(L.size.y, view.y));
    ImGui::SetNextItemAllowOverlap();
    bool clickBackground = ImGui::InvisibleButton("##background", room);
    tell("background");
    if (ImGui::IsItemActivated()) {
        m_boxing  = true;
        m_boxFrom = canvasAt(io.MousePos);
    }
    if (clickBackground && draggedLeft()) clickBackground = false;
    const bool rightOnBackground = ImGui::IsItemClicked(ImGuiMouseButton_Right);
    // The middle button pans, wherever on the canvas it is held.
    if (canvasHovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        ImGui::SetScrollX(ImGui::GetScrollX() - io.MouseDelta.x);
        ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
    }

    // What the hand did, acted on after the drawing (the graph must not change
    // under the loop that draws it).
    int clickNode = -1, clickOut = -1, clickIn = -1, clickInSlot = 0;
    bool moveNodes = false, snapNodes = false;
    glm::vec2 moveBy(0.0f);
    bool openAdd = false, openNodeMenu = false;

    // Wires first, under the nodes.
    const ImU32 wire    = ImGui::GetColorU32(ImGuiCol_Text, 0.45f);
    const ImU32 wireSel = ImGui::GetColorU32(ImGuiCol_PlotLinesHovered);
    // A wire's bend: straight down out of the output, straight down into the input.
    auto bend = [&](ImVec2 a, ImVec2 b) { return std::max(std::fabs(b.y - a.y) * 0.5f, em * 1.5f); };
    auto bezier = [&](ImVec2 a, ImVec2 b, ImU32 c, float thick) {
        const float dy = bend(a, b);
        dl->AddBezierCubic(a, ImVec2(a.x, a.y + dy), ImVec2(b.x, b.y - dy), b, c, thick);
    };

    // A node let go over a wire goes into it: the wire's source feeds the node,
    // the node feeds what the wire fed -- Houdini's and Blender's drop onto a
    // link. Only a node free to take it: one input with nothing wired into it,
    // feeding nothing yet.
    auto insertable = [&](const proc::Node& n) {
        if (n.variadic() || n.inputSlots() != 1) return false;
        if (!n.inputs.empty() && n.inputs.front() >= 0) return false;
        for (const auto& o : g.nodes)
            for (int src : o->inputs)
                if (src == n.id) return false;
        return true;
    };
    // The wire running through a node's box (screen top-left `p`, `w` wide,
    // the node height tall), the one passing nearest its middle -- the box, not
    // the pointer, because the box is what the hand puts over the wire, and a
    // whole box is a generous target. Wires of node `self` don't count.
    struct WireHit { int consumer = -1, slot = 0, source = -1; };
    auto wireThrough = [&](ImVec2 p, float w, int self) {
        WireHit best;
        float bestD = 1e30f;
        const ImVec2 mid(p.x + 0.5f * w, p.y + 0.5f * H);
        const float pad = em * 0.4f;
        for (const auto& n : g.nodes)
            for (std::size_t s = 0; s < n->inputs.size(); ++s) {
                const int src = n->inputs[s];
                if (src < 0 || !g.find(src) || src == self || n->id == self) continue;
                const ImVec2 a = outDot(src), b = inDot(n->id, static_cast<int>(s));
                const float dy = bend(a, b);
                const ImVec2 c1(a.x, a.y + dy), c2(b.x, b.y - dy);
                for (int k = 0; k <= 32; ++k) {
                    const float t = static_cast<float>(k) / 32.0f, u = 1.0f - t;
                    const float w0 = u * u * u, w1 = 3.0f * u * u * t, w2 = 3.0f * u * t * t, w3 = t * t * t;
                    const ImVec2 q(w0 * a.x + w1 * c1.x + w2 * c2.x + w3 * b.x,
                                   w0 * a.y + w1 * c1.y + w2 * c2.y + w3 * b.y);
                    if (q.x < p.x - pad || q.x > p.x + w + pad || q.y < p.y - pad || q.y > p.y + H + pad) continue;
                    const float d = std::hypot(q.x - mid.x, q.y - mid.y);
                    if (d < bestD) {
                        bestD = d;
                        best  = {n->id, static_cast<int>(s), src};
                    }
                }
            }
        return best;
    };
    auto insertInto = [](proc::Graph& gg, int id, const WireHit& w) {
        gg.connect(id, 0, w.source);
        gg.connect(w.consumer, w.slot, id);
    };
    // One node being dragged that could go in: the wire it would go into,
    // lit while it is over it, so the drop is no surprise.
    WireHit dropWire;
    const int dragOne = (m_dragNode >= 0 && m_dragMoved && m_dragFrom.size() == 1) ? m_dragNode : -1;
    if (const proc::Node* dn = g.find(dragOne); dn && insertable(*dn) && L.pos.count(dragOne))
        dropWire = wireThrough(at(L.pos[dragOne]), L.w(dragOne), dragOne);

    for (const auto& n : g.nodes)
        for (std::size_t s = 0; s < n->inputs.size(); ++s) {
            const int src = n->inputs[s];
            if (src < 0 || !g.find(src)) continue;
            const bool hot  = picked(n->id) || picked(src);
            const bool into = n->id == dropWire.consumer && static_cast<int>(s) == dropWire.slot;
            bezier(outDot(src), inDot(n->id, static_cast<int>(s)), into ? col(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark)) : hot ? wireSel : wire,
                   into ? 4.0f : hot ? 2.6f : 1.8f);
        }

    const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
    const ImVec4 outBlue(0.30f, 0.62f, 1.0f, 1.0f);
    const bool wiring = m_wireFrom >= 0 || m_wireDragFrom >= 0;
    for (const auto& n : g.nodes) {
        const ImVec2 p0 = at(L.pos[n->id]);
        const float  nw = L.w(n->id);
        const ImVec2 p1(p0.x + nw, p0.y + H);
        const proc::TypeInfo* ti = proc::typeInfo(n->typeId());
        const ImVec4 tint = categoryTint(ti ? ti->category : "");
        const bool isOut = n->id == g.output;
        auto err = m_info.errors.find(n->id);
        const bool bad = err != m_info.errors.end();

        // The body is one big button: a press picks the node (Shift: adds it or
        // takes it away), a drag moves every picked node with it.
        ImGui::PushID(n->id);
        ImGui::SetCursorScreenPos(p0);
        ImGui::SetNextItemAllowOverlap();
        const bool pressed = ImGui::InvisibleButton("##node", ImVec2(nw, H));
        tell("node:" + n->name);
        if (ImGui::IsItemActivated()) {
            if (io.KeyShift) {
                if (picked(n->id)) m_sel.erase(std::find(m_sel.begin(), m_sel.end(), n->id));
                else               m_sel.push_back(n->id);
            } else if (!picked(n->id)) {
                m_sel.assign(1, n->id);
            }
            m_node = picked(n->id) ? n->id : (m_sel.empty() ? -1 : m_sel.back());
            m_dragNode  = n->id;
            m_dragMoved = false;
            m_dragFrom.clear();
            for (int id : m_sel)
                if (L.pos.count(id))
                    m_dragFrom.push_back({id, glm::vec2(L.pos[id].x / em, L.pos[id].y / em)});
        }
        // Absolute, not accumulated: every picked node sits where the pointer
        // is now relative to where the press began, so a hand that wobbles and
        // comes back leaves them where it let go, not where the wobble summed to.
        if (ImGui::IsItemActive() && m_dragNode == n->id && picked(n->id) &&
            ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
            moveNodes = true;
            moveBy    = glm::vec2(d.x / em, d.y / em);
        }
        const bool wasDrag = m_dragNode == n->id && m_dragMoved;
        if (ImGui::IsItemDeactivated() && m_dragNode == n->id) {
            if (wasDrag) snapNodes = true;
            m_dragNode = -1;
        }
        if (pressed && !wasDrag) clickNode = n->id;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            if (!picked(n->id)) m_sel.assign(1, n->id);
            m_node       = n->id;
            m_menuNode   = n->id;
            openNodeMenu = true;
        }
        const bool hov = ImGui::IsItemHovered();
        if (hov && m_dragNode < 0 && !m_boxing) {
            ImGui::BeginTooltip();
            ImGui::Text("%s  (%s)", n->name.c_str(), n->displayName());
            if (auto f = m_info.faces.find(n->id); f != m_info.faces.end())
                ui::hint("%zu faces", f->second);
            if (bad) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", err->second.c_str());
            if (n->bypass) ui::hint("Bypassed: passes its input through.");
            ui::hint("Drag to move it; right-click for its menu.");
            ImGui::EndTooltip();
        }

        const float rnd = em * 0.35f;
        dl->AddRectFilled(p0, p1, col(tint, n->bypass ? 0.35f : (hov ? 0.95f : 0.8f)), rnd);
        if (isOut) {
            // The output flag: a blue band down the right edge, as in Houdini.
            dl->AddRectFilled(ImVec2(p1.x - em * 0.9f, p0.y), p1, col(outBlue), rnd,
                              ImDrawFlags_RoundCornersRight);
        }
        const bool isPick   = picked(n->id);
        const bool isActive = n->id == m_node;
        const bool moving   = m_moveArmed == n->id;
        // Picked: the accent rim; the one whose settings show: a heavier one.
        const ImU32 rim = bad ? ImGui::GetColorU32(ImVec4(1.0f, 0.35f, 0.3f, 1.0f))
                        : (isPick || moving) ? col(accent) : ImGui::GetColorU32(ImGuiCol_Border);
        dl->AddRect(p0, p1, rim, rnd, 0, isActive || bad || moving ? 3.5f : isPick ? 2.0f : 1.0f);        ImGui::PushClipRect(p0, ImVec2(p1.x - (isOut ? em * 1.0f : em * 0.3f), p1.y), true);
        if (ImFont* bold = ui::boldFont()) ImGui::PushFont(bold, 0.0f);
        dl->AddText(ImVec2(p0.x + em * 0.55f, p0.y + em * 0.35f), ImGui::GetColorU32(ImGuiCol_Text),
                    n->name.c_str());
        if (ui::boldFont()) ImGui::PopFont();
        // What it made, in the fewest words: faces, else curves, else points --
        // and how many points are selected, where a selection exists.
        char sub[128];
        {
            auto count = [&](const std::unordered_map<int, std::size_t>& m) {
                auto it = m.find(n->id);
                return it == m.end() ? std::size_t(0) : it->second;
            };
            const std::size_t fc = count(m_info.faces), cc = count(m_info.curves),
                              pc = count(m_info.corners), ic = count(m_info.prefabs);
            const auto sl = m_info.selected.find(n->id);
            char made[48] = "";
            if (fc > 0)      std::snprintf(made, sizeof made, "  %zu f", fc);
            else if (cc > 0) std::snprintf(made, sizeof made, "  %zu crv", cc);
            else if (ic > 0) std::snprintf(made, sizeof made, "  %zu pf", ic);
            else if (pc > 0) std::snprintf(made, sizeof made, "  %zu pt", pc);
            char sel[32] = "";
            if (sl != m_info.selected.end()) std::snprintf(sel, sizeof sel, "  %zu sel", sl->second);
            std::snprintf(sub, sizeof sub, "%s%s%s", n->displayName(), made, sel);
        }
        dl->AddText(ImVec2(p0.x + em * 0.55f, p0.y + em * 1.55f),
                    ImGui::GetColorU32(ImGuiCol_Text, 0.65f), sub);
        ImGui::PopClipRect();
        if (isOut)
            dl->AddText(ImVec2(p1.x - em * 0.78f, p0.y + H * 0.5f - em * 0.5f),
                        IM_COL32(255, 255, 255, 230), "O");
        if (bad)
            dl->AddText(ImVec2(p1.x - em * (isOut ? 1.7f : 0.9f), p0.y + em * 0.3f),
                        IM_COL32(255, 120, 100, 255), "!");

        // Dots: inputs along the top, the output at the bottom. Click one and
        // then its partner, or drag from one to the other.
        const int slots = shownSlots(*n);
        for (int s = 0; s < slots; ++s) {
            const ImVec2 c = inDot(n->id, s);
            ImGui::SetCursorScreenPos(ImVec2(c.x - hitR, c.y - hitR));
            ImGui::PushID(1000 + s);
            if (ImGui::InvisibleButton("##in", ImVec2(hitR * 2.0f, hitR * 2.0f))) {
                clickIn = n->id;
                clickInSlot = s;
            }
            tell("in:" + n->name + ":" + std::to_string(s));
            if (ImGui::IsItemActivated()) { m_wireDragTo = n->id; m_wireDragToSlot = s; }
            const bool hovDot = ImGui::IsItemHovered();
            if (hovDot && m_wireDragFrom < 0 && m_wireDragTo < 0) {
                const bool extra = n->variadic() && s == slots - 1;
                ImGui::SetTooltip("%s", extra ? "Wire something more in"
                                              : n->inputName(s));
            }
            ImGui::PopID();
            const bool wired = s < static_cast<int>(n->inputs.size()) && n->inputs[static_cast<std::size_t>(s)] >= 0;
            const bool armed = wiring || (m_wireTo == n->id && m_wireToSlot == s);
            dl->AddCircleFilled(c, hovDot || armed ? dotR * 1.4f : dotR,
                                wired ? ImGui::GetColorU32(ImGuiCol_Text)
                                      : ImGui::GetColorU32(ImGuiCol_FrameBg));
            dl->AddCircle(c, hovDot || armed ? dotR * 1.4f : dotR,
                          ImGui::GetColorU32(ImGuiCol_Text), 0, 1.5f);
        }
        {
            const ImVec2 c = outDot(n->id);
            ImGui::SetCursorScreenPos(ImVec2(c.x - hitR, c.y - hitR));
            if (ImGui::InvisibleButton("##out", ImVec2(hitR * 2.0f, hitR * 2.0f))) clickOut = n->id;
            tell("out:" + n->name);
            if (ImGui::IsItemActivated()) m_wireDragFrom = n->id;
            const bool hovDot = ImGui::IsItemHovered();
            if (hovDot && m_wireDragFrom < 0 && m_wireDragTo < 0)
                ImGui::SetTooltip("Output: drag it onto the node it should feed,\n"
                                  "or click it and then that node");
            const bool armed = m_wireFrom == n->id || m_wireDragFrom == n->id;
            dl->AddCircleFilled(c, hovDot || armed ? dotR * 1.4f : dotR,
                                armed ? col(accent) : ImGui::GetColorU32(ImGuiCol_Text));
        }
        ImGui::PopID();
    }

    // --- The box round nodes --------------------------------------------------
    // Drawn while the button is held; let go, every node it touches is picked
    // (with Shift, added to the pick).
    if (m_boxing) {
        const ImVec2 a = at(ImVec2(m_boxFrom.x * em, m_boxFrom.y * em));
        const ImVec2 b = io.MousePos;
        const ImVec2 lo(std::min(a.x, b.x), std::min(a.y, b.y)), hi(std::max(a.x, b.x), std::max(a.y, b.y));
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (draggedLeft()) {
                dl->AddRectFilled(lo, hi, col(accent, 0.12f));
                dl->AddRect(lo, hi, col(accent, 0.9f), 0.0f, 0, 1.5f);
            }
        } else {
            if (draggedLeft()) {
                if (!io.KeyShift) m_sel.clear();
                for (const auto& n : g.nodes) {
                    const ImVec2 p0 = at(L.pos[n->id]);
                    const ImVec2 p1(p0.x + L.w(n->id), p0.y + H);
                    if (p0.x <= hi.x && p1.x >= lo.x && p0.y <= hi.y && p1.y >= lo.y && !picked(n->id))
                        m_sel.push_back(n->id);
                }
                if (!picked(m_node)) m_node = m_sel.empty() ? -1 : m_sel.front();
            }
            m_boxing = false;
        }
    }    // --- Keys, while the keyboard is this window's --------------------------
    bool removeIt = false;
    if (keys) {
        const bool plain = !io.KeyCtrl && !io.KeyShift && !io.KeyAlt;
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || (plain && ImGui::IsKeyPressed(ImGuiKey_X, false)))
            removeIt = true;
        if (ImGui::IsKeyPressed(ImGuiKey_A, false)) {
            if (io.KeyShift && !io.KeyCtrl && !io.KeyAlt) {
                openAdd = true;
            } else if (io.KeyAlt) {
                m_sel.clear();
                m_node = -1;
            } else if (plain) {
                m_sel.clear();
                for (const auto& n : g.nodes) m_sel.push_back(n->id);
            }
        }
    }
    if (rightOnBackground) openAdd = true;

    // --- The Add menu at the pointer -------------------------------------------
    // Shift+A or a right-click on the empty canvas. One list with its sections
    // and a search field on top -- no submenus that open on hover, which a
    // hand that wanders closes again. Typing narrows the list; Enter takes the
    // first that is left.
    std::string addKind;
    if (openAdd) {
        // Where the pointer was; with it off the canvas, the middle of the view.
        const ImVec2 at0 = canvasHovered ? io.MousePos
                                         : ImVec2(origin.x + ImGui::GetScrollX() + 0.5f * view.x,
                                                  origin.y + ImGui::GetScrollY() + 0.5f * view.y);
        m_addAt = canvasAt(at0);
        m_addFilter[0] = '\0';
        m_addFocus = true;
        ImGui::OpenPopup("##procAddHere");
    }
    if (ImGui::BeginPopup("##procAddHere")) {
        ui::title("Add node");
        if (m_addFocus) { ImGui::SetKeyboardFocusHere(); m_addFocus = false; }
        ImGui::SetNextItemWidth(em * 14.0f);
        const bool enter = ImGui::InputTextWithHint("##addFilter", "Search...", m_addFilter, sizeof m_addFilter,
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
        tell("addhere:search");
        std::string cat, first;
        for (const proc::TypeInfo& t : proc::registry()) {
            if (!ui::icontains(t.displayName.c_str(), m_addFilter) &&
                !ui::icontains(t.typeId.c_str(), m_addFilter)) continue;
            if (first.empty()) first = t.typeId;
            if (t.category != cat) {
                cat = t.category;
                ui::sectionText(cat.c_str());
            }
            if (ImGui::Selectable(t.displayName.c_str(), false, 0, ImVec2(em * 14.0f, em * 1.5f)))
                addKind = t.typeId;
            tell("addhere:" + t.typeId);
            if (!t.tip.empty()) ImGui::SetItemTooltip("%s", t.tip.c_str());
        }
        if (enter && !first.empty()) addKind = first;
        // One Escape closes it, as in Blender -- not one for the search field
        // and a second for the menu.
        if (!addKind.empty() || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // --- A node's menu -----------------------------------------------------------
    bool menuShow = false, menuBypass = false;
    if (openNodeMenu) ImGui::OpenPopup("##procNodeMenu");
    if (ImGui::BeginPopup("##procNodeMenu")) {
        const proc::Node* mn = g.find(m_menuNode);
        if (!mn) {
            ImGui::CloseCurrentPopup();
        } else {
            ui::title("%s", mn->name.c_str());
            const ImVec2 row(em * 12.0f, em * 1.5f);
            if (ImGui::Selectable("Show as output", false, mn->id == g.output ? ImGuiSelectableFlags_Disabled : 0, row))
                menuShow = true;
            tell("menu:show");
            if (ImGui::Selectable(mn->bypass ? "Stop bypassing" : "Bypass", false, 0, row)) menuBypass = true;
            tell("menu:bypass");
            char label[48];
            std::snprintf(label, sizeof label, m_sel.size() > 1 ? "Delete %zu nodes" : "Delete", m_sel.size());
            if (ImGui::Selectable(label, false, 0, row)) removeIt = true;
            tell("menu:delete");
        }
        ImGui::EndPopup();
    }

    // --- Acting on what the hand did ------------------------------------------
    auto wireUp = [&](int consumer, int slot, int source) {
        bool ok = true;
        change(e, [&](proc::Graph& gg) {
            const proc::Node* c = gg.find(consumer);
            if (!c) { ok = false; return; }
            ok = gg.connect(consumer, slot, source);
        });
        m_note = ok ? "" : "That would make a loop: a node cannot feed what it is made from.";
        m_wireFrom = m_wireTo = -1;
    };
    // The input of a node an output was dropped on: its first empty one, a
    // merge's extra dot, or else its first.
    auto freeSlot = [&](int id) {
        const proc::Node* n = g.find(id);
        if (!n) return 0;
        if (n->variadic()) return static_cast<int>(n->inputs.size());
        for (int s = 0; s < n->inputSlots(); ++s)
            if (s >= static_cast<int>(n->inputs.size()) || n->inputs[static_cast<std::size_t>(s)] < 0) return s;
        return 0;
    };
    auto takesInput = [&](int id) {
        const proc::Node* n = g.find(id);
        return n && (n->variadic() || n->inputSlots() > 0);
    };

    // Picked nodes dragged: placed by hand from now on -- all of them, the
    // first time, so the rest stay where they stand.
    if (moveNodes) {
        change(e, [&](proc::Graph& gg) {
            pinAll(gg, L, em);
            for (const auto& [id, from] : m_dragFrom)
                if (proc::Node* n = gg.find(id)) {
                    n->pos    = glm::max(from + moveBy, glm::vec2(0.0f));
                    n->placed = true;
                }
        }, false);
        m_dragMoved = true;
    }
    // ...and let go: onto the grid, where they line up with their neighbours.
    // Snapped on release, not while moving, so the grid never fights the hand.
    if (snapNodes) {
        change(e, [&](proc::Graph& gg) {
            for (const auto& [id, from] : m_dragFrom)
                if (proc::Node* n = gg.find(id))
                    n->pos = glm::vec2(snapTo(n->pos.x, kGrid), snapTo(n->pos.y, kGrid));
        }, false);
        // ...and over a wire: into it (the same undo step as the move).
        if (dropWire.consumer >= 0) {
            const WireHit w = dropWire;
            change(e, [&](proc::Graph& gg) { insertInto(gg, dragOne, w); });
        }
    }

    // A wire dragged from a dot: drawn to the pointer while the button is
    // held, made where it is let go -- on an input dot, or on a node. An
    // input's wire pulled off and dropped on nothing comes out.
    if (m_wireDragFrom >= 0 || m_wireDragTo >= 0) {
        const ImVec2 mouse = io.MousePos;
        const bool dragged = draggedLeft();
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (dragged) {
                if (m_wireDragFrom >= 0 && g.find(m_wireDragFrom))
                    bezier(outDot(m_wireDragFrom), mouse, col(accent), 2.6f);
                else if (m_wireDragTo >= 0 && g.find(m_wireDragTo))
                    bezier(mouse, inDot(m_wireDragTo, m_wireDragToSlot), col(accent), 2.6f);
            }
        } else {
            if (dragged) {
                int dropNode = -1, dropIn = -1, dropSlot = 0;
                for (const auto& n : g.nodes) {
                    const ImVec2 p0 = at(L.pos[n->id]);
                    const int slots = shownSlots(*n);
                    for (int s = 0; s < slots; ++s) {
                        const ImVec2 c = inDot(n->id, s);
                        if (std::fabs(mouse.x - c.x) <= hitR && std::fabs(mouse.y - c.y) <= hitR) {
                            dropIn = n->id;
                            dropSlot = s;
                        }
                    }
                    if (mouse.x >= p0.x && mouse.x <= p0.x + L.w(n->id) &&
                        mouse.y >= p0.y - hitR && mouse.y <= p0.y + H + hitR)
                        dropNode = n->id;
                }
                if (m_wireDragFrom >= 0) {
                    if (dropIn >= 0 && dropIn != m_wireDragFrom)
                        wireUp(dropIn, dropSlot, m_wireDragFrom);
                    else if (dropNode >= 0 && dropNode != m_wireDragFrom && takesInput(dropNode))
                        wireUp(dropNode, freeSlot(dropNode), m_wireDragFrom);
                } else {
                    const proc::Node* c = g.find(m_wireDragTo);
                    const bool wired = c && m_wireDragToSlot < static_cast<int>(c->inputs.size()) &&
                                       c->inputs[static_cast<std::size_t>(m_wireDragToSlot)] >= 0;
                    if (dropNode >= 0 && dropNode != m_wireDragTo)
                        wireUp(m_wireDragTo, m_wireDragToSlot, dropNode);
                    else if (dropNode < 0 && wired) {
                        const int consumer = m_wireDragTo, slot = m_wireDragToSlot;
                        change(e, [&](proc::Graph& gg) { gg.connect(consumer, slot, -1); });
                        m_note.clear();
                    }
                }
                // A drag is not a click: whatever the release landed on must
                // not also arm a two-click wire.
                clickOut = clickIn = clickNode = -1;
            }
            m_wireDragFrom = m_wireDragTo = -1;
        }
    }

    if (clickBackground && clickNode < 0 && clickIn < 0 && clickOut < 0) {
        if (m_moveArmed >= 0 && g.find(m_moveArmed)) {
            // "Move" without a drag: the node goes where the click was.
            const int id = m_moveArmed;
            const glm::vec2 to = canvasAt(io.MousePos) - glm::vec2(0.5f * kNodeW, 0.5f * kNodeH);
            const glm::vec2 put(snapTo(std::max(to.x, 0.0f), kGrid), snapTo(std::max(to.y, 0.0f), kGrid));
            // Put down over a wire, it goes into it -- as a dragged one does.
            const proc::Node* mn = g.find(id);
            const WireHit w = (mn && insertable(*mn))
                                  ? wireThrough(at(ImVec2(put.x * em, put.y * em)), L.w(id), id) : WireHit{};
            change(e, [&](proc::Graph& gg) {
                pinAll(gg, L, em);
                if (proc::Node* n = gg.find(id)) {
                    n->pos    = put;
                    n->placed = true;
                }
                if (w.consumer >= 0) insertInto(gg, id, w);
            }, w.consumer >= 0);
        } else if (!io.KeyShift && m_wireFrom < 0 && m_wireTo < 0) {
            // A click on nothing drops the pick (a wire in the making is put
            // down first, by the same click).
            m_sel.clear();
            m_node = -1;
        }
        m_moveArmed = -1;
        m_wireFrom = m_wireTo = -1;
        m_note.clear();
    }

    if (clickOut >= 0) {
        m_note.clear();
        m_moveArmed = -1;
        if (m_wireTo >= 0) wireUp(m_wireTo, m_wireToSlot, clickOut);
        else m_wireFrom = (m_wireFrom == clickOut) ? -1 : clickOut;
    } else if (clickIn >= 0) {
        m_note.clear();
        m_moveArmed = -1;
        if (m_wireFrom >= 0) wireUp(clickIn, clickInSlot, m_wireFrom);
        else if (m_wireTo == clickIn && m_wireToSlot == clickInSlot) m_wireTo = -1;
        else {
            m_wireTo = clickIn;
            m_wireToSlot = clickInSlot;
            if (!picked(clickIn)) m_sel.assign(1, clickIn);
            m_node = clickIn;
        }
    } else if (clickNode >= 0) {
        m_moveArmed = -1;
        if (m_wireFrom >= 0 && takesInput(clickNode) && clickNode != m_wireFrom) {
            wireUp(clickNode, freeSlot(clickNode), m_wireFrom);
        } else if (m_wireTo >= 0 && clickNode != m_wireTo) {
            wireUp(m_wireTo, m_wireToSlot, clickNode);
        } else {
            m_wireFrom = m_wireTo = -1;
            m_note.clear();
        }
    }

    // The menus' choices, last: each changes the graph.
    if (menuShow && g.find(m_menuNode)) {
        const int id = m_menuNode;
        change(e, [&](proc::Graph& gg) { gg.output = id; });
    }
    if (menuBypass && g.find(m_menuNode)) {
        const int id = m_menuNode;
        change(e, [&](proc::Graph& gg) { if (proc::Node* n = gg.find(id)) n->bypass = !n->bypass; });
    }
    if (!addKind.empty()) {
        // At the pointer, which means placed by hand: the others are held
        // where they stand first, so none of them jumps.
        int made = -1;
        const glm::vec2 spot = m_addAt - glm::vec2(0.5f * kNodeW, 0.5f * kNodeH);
        const glm::vec2 put(snapTo(std::max(spot.x, 0.0f), kGrid), snapTo(std::max(spot.y, 0.0f), kGrid));
        // Added over a wire, a node that can goes into it.
        const WireHit w = wireThrough(at(ImVec2(put.x * em, put.y * em)), kNodeW * em, -1);
        change(e, [&](proc::Graph& gg) {
            std::unique_ptr<proc::Node> n = proc::make(addKind);
            if (!n) return;
            pinAll(gg, L, em);
            proc::Node& a = gg.add(std::move(n));
            made     = a.id;
            a.pos    = put;
            a.placed = true;
            if (!gg.find(gg.output)) gg.output = made;
            if (w.consumer >= 0 && !a.variadic() && a.inputSlots() == 1) insertInto(gg, made, w);
        });
        if (made >= 0) {
            m_node = made;
            m_sel.assign(1, made);
        }
    }
    if (removeIt) removePicked(e);
}
// =============================================================================
// The picked node's settings
// =============================================================================

bool Panel::drawProps(proc::Node& n) {
    EditorContext& ed = m_d.ed;
    const float em     = ImGui::GetFontSize();
    const float labelW = em * 8.5f;
    const float full   = ImGui::GetContentRegionAvail().x;
    bool changed = false;
    for (const Property& pr : n.props()) {
        if (pr.visible && !pr.visible(&n)) continue;
        void* f = pr.field(&n);
        ImGui::PushID(pr.key.c_str());
        // A curve's points: a list, a row of three steppers each, with buttons
        // to add one (carrying the line on, or at the 3D cursor) and take one off.
        if (pr.kind == PropKind::Text && pr.key == "points") {
            std::string& s = *static_cast<std::string*>(f);
            std::vector<glm::vec3> pts = proc::parsePoints(s);
            ImGui::TextUnformatted(pr.label.c_str());
            // Two lines a point: its number with the button that takes it off,
            // then X, Y and Z across the whole width -- three steppers side by
            // side next to a label and a button would not fit a narrow panel.
            const float xw   = em * 2.4f;
            const float each = (full - 2.0f * em * 0.4f) / 3.0f;
            int  removeAt = -1;
            bool edited   = false;
            for (int i = 0; i < static_cast<int>(pts.size()); ++i) {
                ImGui::PushID(i);
                ImGui::AlignTextToFramePadding();
                // The row's name picks the point, as a click on its handle in
                // the scene does: lit there, where the arrow keys move it.
                const bool picked = m_drawing && m_ptFor == m_drawing->id && m_ptNode == n.id && m_pt == i;
                char rowName[24];
                std::snprintf(rowName, sizeof rowName, "Point %d", i + 1);
                if (ImGui::Selectable(rowName, picked, 0,
                                      ImVec2(std::max(full - xw, em * 6.0f) - em * 0.6f, 0.0f))) {
                    m_pt     = picked ? -1 : i;
                    m_ptNode = n.id;
                    m_ptFor  = m_drawing ? m_drawing->id : -1;
                }
                tell("points.pick." + std::to_string(i));
                ImGui::SetItemTooltip("Pick this point: its handle lights up in the scene,\n"
                                      "where the arrow keys move it (the pointer over the scene).");
                if (picked && m_ptScroll) {
                    ImGui::SetScrollHereY(0.35f);
                    m_ptScroll = false;
                }
                ImGui::SameLine(std::max(full - xw, em * 6.0f));
                ImGui::BeginDisabled(pts.size() <= 2);
                if (ImGui::Button("X", ImVec2(xw, 0.0f))) removeAt = i;
                ImGui::EndDisabled();
                tell("points.remove." + std::to_string(i));
                ImGui::SetItemTooltip("Take this point off (a curve keeps two at least)");
                const char* ids[3] = {"x", "y", "z"};
                for (int k = 0; k < 3; ++k) {
                    if (k > 0) ImGui::SameLine(0.0f, em * 0.4f);
                    edited |= numberField(ids[k], pts[static_cast<std::size_t>(i)][k], 0.5f, 0.0f, 0.0f, "%.1f",
                                          each, "points." + std::to_string(i) + "." + ids[k]);
                    ImGui::SetItemTooltip("%s", k == 0 ? "X" : k == 1 ? "Y" : "Z");
                }
                ImGui::PopID();
            }
            if (removeAt >= 0) {
                pts.erase(pts.begin() + removeAt);
                edited = true;
            }
            const ImVec2 btn(0.0f, ImGui::GetFrameHeight() + em * 0.35f);
            if (ImGui::Button("Add point", btn)) {
                // Carrying the line on the way its last stretch went.
                const glm::vec3 last = pts.empty() ? glm::vec3(0.0f) : pts.back();
                const glm::vec3 step = pts.size() >= 2 ? last - pts[pts.size() - 2] : glm::vec3(5.0f, 0.0f, 0.0f);
                pts.push_back(last + step);
                edited = true;
            }
            tell("points.add");
            ImGui::SameLine();
            glm::vec3 cur(0.0f);
            const bool haveCursor = m_d.cursor && m_d.cursor(cur) && m_drawing &&
                                    m_drawing->components.get<MeshComponent>() &&
                                    m_drawing->components.get<ProcGraphComponent>();
            ImGui::BeginDisabled(!haveCursor);
            if (ImGui::Button("Add point at the 3D cursor", btn) && haveCursor) {
                // From the world into the graph's own space: through the
                // object's transform, then back past the pivot its mesh was
                // centred by.
                const MeshComponent* mc = m_drawing->components.get<MeshComponent>();
                const ProcGraphComponent* pgc = m_drawing->components.get<ProcGraphComponent>();
                const glm::mat4 inv = glm::inverse(meshModelOf(*m_drawing, *mc));
                pts.push_back(glm::vec3(inv * glm::vec4(cur, 1.0f)) + pgc->pivot);
                edited = true;
            }
            ImGui::EndDisabled();
            tell("points.cursor");
            ImGui::SetItemTooltip("Put the 3D cursor where the point should go (Shift +\n"
                                  "right-click on the ground), then press this.");
            ui::hint("In the scene: drag a point (Ctrl: square to the curve's plane);\n"
                     "the + halfway along a stretch adds one; the arrow keys move\n"
                     "the picked one.");
            if (edited) {
                s = proc::formatPoints(pts);
                changed = true;
            }
            ImGui::PopID();
            continue;
        }
        // A position or an offset: three numbers need the whole width, so their
        // name goes on a line of its own.
        if (pr.kind == PropKind::Vec3) {
            ImGui::TextUnformatted(pr.label.c_str());
            glm::vec3& v = *static_cast<glm::vec3*>(f);
            const float each = (full - 2.0f * em * 0.5f) / 3.0f;
            const char* fmt = pr.fmt.empty() ? "%.2f" : pr.fmt.c_str();
            for (int k = 0; k < 3; ++k) {
                if (k > 0) ImGui::SameLine(0.0f, em * 0.5f);
                const char* ids[3] = {"x", "y", "z"};
                changed |= numberField(ids[k], v[k], pr.speed > 0.0f ? pr.speed : 0.1f,
                                       pr.min, pr.max, fmt, each, pr.key + "." + ids[k]);
                ImGui::SetItemTooltip("%s", k == 0 ? "X" : k == 1 ? "Y" : "Z");
            }
            ImGui::PopID();
            continue;
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(pr.label.c_str());
        ImGui::SameLine(labelW);
        const float rest = std::max(full - labelW, em * 8.0f);
        switch (pr.kind) {
            case PropKind::Float: {
                float& v = *static_cast<float*>(f);
                changed |= numberField("v", v, pr.speed > 0.0f ? pr.speed : 0.1f, pr.min, pr.max,
                                       pr.fmt.empty() ? "%.2f" : pr.fmt.c_str(), rest, pr.key);
                break;
            }
            case PropKind::Int: {
                int& v = *static_cast<int*>(f);
                float tmp = static_cast<float>(v);
                if (numberField("v", tmp, pr.speed >= 1.0f ? pr.speed : 1.0f, pr.min, pr.max, "%.0f", rest,
                                pr.key)) {
                    v = static_cast<int>(std::lround(tmp));
                    changed = true;
                }
                break;
            }
            case PropKind::Bool:
                changed |= ImGui::Checkbox("##v", static_cast<bool*>(f));
                tell(pr.key);
                break;
            case PropKind::EnumInt: {
                int& v = *static_cast<int*>(f);
                const int count = static_cast<int>(pr.enumLabels.size());
                const char* cur = (v >= 0 && v < count) ? pr.enumLabels[static_cast<std::size_t>(v)].c_str() : "?";
                ImGui::SetNextItemWidth(rest);
                const bool open = ImGui::BeginCombo("##v", cur, ImGuiComboFlags_HeightLarge);
                tell(pr.key);
                if (open) {
                    for (int i = 0; i < count; ++i) {
                        const bool pick = ImGui::Selectable(pr.enumLabels[static_cast<std::size_t>(i)].c_str(),
                                                            i == v, 0, ImVec2(0.0f, em * 1.5f));
                        tell(pr.key + ":" + std::to_string(i));
                        if (pick) {
                            v = i;
                            changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                break;
            }
            case PropKind::Text: {
                std::string& s = *static_cast<std::string*>(f);
                if (pr.key == "material") {
                    // The scene's materials, by name; stored as the GUID.
                    const fitzel::AssetId id = fitzel::AssetId::fromString(s);
                    const MaterialDef* cur = nullptr;
                    for (const MaterialDef& m : ed.materials)
                        if (id.valid() && m.assetId == id) cur = &m;
                    const char* shown = !id.valid() ? "(the object's own)"
                                      : cur ? cur->name.c_str() : "(missing material)";
                    ImGui::SetNextItemWidth(rest);
                    const bool open = ImGui::BeginCombo("##v", shown, ImGuiComboFlags_HeightLarge);
                    tell(pr.key);
                    if (open) {
                        ui::searchBox("##matFilter", m_matFilter, sizeof m_matFilter, "Find a material...");
                        if (ImGui::Selectable("(the object's own)", !id.valid(), 0, ImVec2(0.0f, em * 1.5f))) {
                            s.clear();
                            changed = true;
                        }
                        for (const MaterialDef& m : ed.materials) {
                            if (!m.assetId.valid() || !ui::icontains(m.name.c_str(), m_matFilter)) continue;
                            ImGui::PushID(&m);
                            const ImVec4 sw(m.albedo.x, m.albedo.y, m.albedo.z, 1.0f);
                            ImGui::ColorButton("##sw", sw, ImGuiColorEditFlags_NoTooltip, ImVec2(em, em));
                            ImGui::SameLine();
                            const bool pick = ImGui::Selectable(m.name.c_str(), cur == &m);
                            tell(pr.key + ":" + m.name);
                            if (pick) {
                                s = m.assetId.toString();
                                changed = true;
                            }
                            ImGui::PopID();
                        }
                        ImGui::EndCombo();
                    }
                } else if (pr.key == "prefab") {
                    // The project's prefabs, by name -- the name is what is kept.
                    ImGui::SetNextItemWidth(rest);
                    const bool open = ImGui::BeginCombo("##v", s.empty() ? "(pick a prefab)" : s.c_str(),
                                                        ImGuiComboFlags_HeightLarge);
                    tell(pr.key);
                    if (open) {
                        const std::vector<std::string> names = prefabNames ? prefabNames()
                                                                           : std::vector<std::string>{};
                        if (names.empty()) ui::hint("No prefabs in this project yet:\nEdit > Save as Prefab.");
                        for (const std::string& nm : names) {
                            const bool pick = ImGui::Selectable(nm.c_str(), nm == s, 0, ImVec2(0.0f, em * 1.5f));
                            tell(pr.key + ":" + nm);
                            if (pick) {
                                s = nm;
                                changed = true;
                            }
                        }
                        ImGui::EndCombo();
                    }
                } else {
                    char buf[256];
                    std::snprintf(buf, sizeof buf, "%s", s.c_str());
                    ImGui::SetNextItemWidth(rest);
                    ImGui::InputText("##v", buf, sizeof buf);
                    if (ImGui::IsItemDeactivatedAfterEdit()) { s = buf; changed = true; }
                }
                break;
            }
            default:
                break;
        }
        ImGui::PopID();
    }
    return changed;
}

void Panel::settings(Entity& e, ProcGraphComponent& pg) {
    const float em = ImGui::GetFontSize();
    proc::Graph& g = pg.graph;
    proc::Node* n = g.find(m_node);
    if (!n) {
        ui::hint("Click a node on the left to see its settings.\n\n"
                 "Add node puts a new one in, not wired to anything:\n"
                 "drag from its dots, or click a dot and then the other end.");
        return;
    }
    const int id = n->id;
    const ImVec2 btn(0.0f, ImGui::GetFrameHeight() + em * 0.35f);

    // --- Name and kind --------------------------------------------------------
    if (m_nameFor != id) {
        std::snprintf(m_name, sizeof m_name, "%s", n->name.c_str());
        m_nameFor = id;
    }
    ui::title("%s", n->displayName());
    if (const proc::TypeInfo* ti = proc::typeInfo(n->typeId()); ti && !ti->tip.empty())
        ui::hint("%s", ti->tip.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##name", m_name, sizeof m_name);
    tell("name");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        const std::string want = m_name;
        bool taken = want.empty();
        for (const auto& o : g.nodes) taken = taken || (o->id != id && o->name == want);
        if (taken) {
            m_note = want.empty() ? "A node needs a name." : "Another node is called that already.";
            std::snprintf(m_name, sizeof m_name, "%s", n->name.c_str());
        } else {
            m_note.clear();
            change(e, [&](proc::Graph& gg) { if (proc::Node* x = gg.find(id)) x->name = want; });
        }
        return;   // the cook replaced what `n` pointed into
    }

    // --- Flags ------------------------------------------------------------------
    const bool isOut = g.output == id;
    ImGui::BeginDisabled(isOut);
    const bool showIt = ImGui::Button(isOut ? "Shown as output" : "Show as output", btn);
    tell("show");
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("What this node makes becomes the object -- the quickest way\n"
                          "to look at one step on its own. Set it back on the last node.");
    if (showIt) {
        change(e, [&](proc::Graph& gg) { gg.output = id; });
        return;
    }
    ImGui::SameLine();
    bool bypass = n->bypass;
    const bool flip = ImGui::Checkbox("Bypass", &bypass);
    tell("bypass");
    if (flip) {
        change(e, [&](proc::Graph& gg) { if (proc::Node* x = gg.find(id)) x->bypass = bypass; });
        return;
    }
    ImGui::SetItemTooltip("Skip this step: its input passes through untouched.");
    ImGui::SameLine();
    const bool drop = ImGui::Button(m_sel.size() > 1 ? "Delete all picked" : "Delete", btn);
    tell("delete");
    if (drop) {
        removePicked(e);
        return;
    }
    ImGui::SetItemTooltip("Take the picked nodes out (the Delete key does the same).\n"
                          "What one fed is fed by its first input instead, so a\n"
                          "chain stays closed. Undo brings them back.");
    ImGui::SameLine();
    const bool moveIt = ImGui::Button(m_moveArmed == id ? "Moving..." : "Move", btn);
    tell("move");
    ImGui::SetItemTooltip("Move the node without dragging: press this, then click\n"
                          "where it should go on the canvas.");
    if (moveIt) m_moveArmed = (m_moveArmed == id) ? -1 : id;
    const std::vector<int> used = g.upstream(g.output);
    if (std::find(used.begin(), used.end(), id) == used.end()) {
        const bool merge = ImGui::Button("Merge into output", btn);
        tell("merge");
        if (merge) {
            change(e, [&](proc::Graph& gg) { proc::mergeIntoOutput(gg, id); });
            return;
        }
        ImGui::SetItemTooltip("This node is not part of the object yet. Add what it makes\n"
                              "to the output.");
        ImGui::SameLine();
        ui::hint("Not part of the object yet.");
    }

    // --- Inputs -------------------------------------------------------------------
    const int slots = n->variadic() ? static_cast<int>(n->inputs.size()) : n->inputSlots();
    if (slots > 0 || n->variadic()) {
        ui::sectionText("Inputs");
        // The nodes that may feed `id` without making a loop.
        std::vector<const proc::Node*> feeders;
        for (const auto& o : g.nodes)
            if (!g.wouldCycle(id, o->id)) feeders.push_back(o.get());
        auto pick = [&](const char* label, int cur, int slot, bool removable) -> bool {
            const proc::Node* src = g.find(cur);
            ImGui::PushID(slot);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::SameLine(em * 8.5f);
            ImGui::SetNextItemWidth(removable ? -(em * 3.0f) : -1.0f);
            bool done = false;
            const bool open = ImGui::BeginCombo("##src", src ? src->name.c_str() : "(nothing)",
                                                ImGuiComboFlags_HeightLarge);
            tell("input:" + std::to_string(slot));
            if (open) {
                if (!removable && ImGui::Selectable("(nothing)", cur < 0, 0, ImVec2(0.0f, em * 1.5f))) {
                    change(e, [&](proc::Graph& gg) { gg.connect(id, slot, -1); });
                    done = true;
                }
                for (const proc::Node* o : feeders) {
                    if (done) break;
                    const bool chosen = ImGui::Selectable(o->name.c_str(), o->id == cur, 0,
                                                          ImVec2(0.0f, em * 1.5f));
                    tell("input:" + std::to_string(slot) + ":" + o->name);
                    if (chosen) {
                        const int sid = o->id;
                        change(e, [&](proc::Graph& gg) { gg.connect(id, slot, sid); });
                        done = true;
                    }
                }
                ImGui::EndCombo();
            }
            if (removable && !done) {
                ImGui::SameLine();
                const bool unwire = ImGui::Button("X", ImVec2(em * 2.4f, 0.0f));
                tell("unwire:" + std::to_string(slot));
                if (unwire) {
                    change(e, [&](proc::Graph& gg) { gg.connect(id, slot, -1); });
                    done = true;
                }
                ImGui::SetItemTooltip("Unwire this input");
            }
            ImGui::PopID();
            return done;
        };
        if (n->variadic()) {
            for (int s = 0; s < slots; ++s) {
                char label[32];
                std::snprintf(label, sizeof label, "Input %d", s + 1);
                if (pick(label, n->inputs[static_cast<std::size_t>(s)], s, true)) return;
            }
            if (pick("Add", -1, slots, false)) return;
        } else {
            for (int s = 0; s < slots; ++s) {
                const int cur = s < static_cast<int>(n->inputs.size()) ? n->inputs[static_cast<std::size_t>(s)] : -1;
                if (pick(n->inputName(s), cur, s, false)) return;
            }
        }
    }

    // --- Settings -----------------------------------------------------------------
    if (!n->props().empty()) {
        ui::sectionText("Settings");
        // Drawn against a copy: the undo step needs the object as it was BEFORE
        // the click, and a widget changes its value the moment it is clicked.
        std::unique_ptr<proc::Node> work = n->clone();
        m_drawing = &e;
        const bool edited = drawProps(*work);
        m_drawing = nullptr;
        if (edited) {
            change(e, [&](proc::Graph& gg) {
                for (auto& slot : gg.nodes)
                    if (slot->id == id) { slot = std::move(work); break; }
            });
            return;
        }
    }

    // --- What it made ---------------------------------------------------------------
    ImGui::Spacing();
    if (m_info.faces.find(id) == m_info.faces.end()) {
        proc::CookInfo more;
        proc::cook(g, id, &more);
        for (const auto& [k, v] : more.faces)   m_info.faces[k]   = v;
        for (const auto& [k, v] : more.corners) m_info.corners[k] = v;
        for (const auto& [k, v] : more.errors)  m_info.errors[k]  = v;
        for (const auto& [k, v] : more.curves)  m_info.curves[k]  = v;
        for (const auto& [k, v] : more.selected) m_info.selected[k] = v;
    }
    auto count = [&](const std::unordered_map<int, std::size_t>& m) {
        auto it = m.find(id);
        return it == m.end() ? std::size_t(0) : it->second;
    };
    if (m_info.faces.count(id)) {
        ui::hint("Makes %zu faces, %zu curves, %zu points.", count(m_info.faces),
                 count(m_info.curves), count(m_info.corners));
        if (count(m_info.prefabs) > 0)
            ui::hint("Places %zu prefabs (as objects under this one).", count(m_info.prefabs));
        if (m_info.selected.count(id))
            ui::hint("%zu of the points are selected (lit in the viewport).", count(m_info.selected));
    }
    if (auto err = m_info.errors.find(id); err != m_info.errors.end())
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", err->second.c_str());
    ImGui::Spacing();
    ui::hint("The object is cooked from this graph on every change; edits made\n"
             "to its mesh by hand are replaced by the next one.");
}

} // namespace procui