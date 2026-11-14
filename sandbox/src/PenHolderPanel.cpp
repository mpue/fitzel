#include "PenHolderPanel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include <imgui.h>

#include "Command.hpp"
#include "Document.hpp"
#include "SceneGraph.hpp"
#include "Selection.hpp"
#include "UiStyle.hpp"

namespace penui {

using penholder::Layout;
using penholder::Params;

namespace {

ImU32 colourOf(const glm::vec3& c, float a = 1.0f, float k = 1.0f) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r * k, c.g * k, c.b * k, a));
}

// A count: stepped with - and +, or typed.
bool count(const char* label, int& v, int lo, int hi) {
    ImGui::PushID(label);
    float f = static_cast<float>(v);
    const bool ch = ui::stepper("##v", f, 1.0f, static_cast<float>(lo), static_cast<float>(hi), "%.0f",
                                ui::stepperWidth("%.0f") + 90.0f);
    ImGui::SameLine();
    ImGui::TextUnformatted(label, std::strstr(label, "##"));
    ImGui::PopID();
    if (!ch) return false;
    const int n = std::clamp(static_cast<int>(std::lround(f)), lo, hi);
    if (n == v) return false;
    v = n;
    return true;
}

// A measurement: stepped, or typed -- never dragged.
bool mm(const char* label, float& v, float step, float lo, float hi, const char* fmt = "%.1f mm") {
    ImGui::PushID(label);
    const bool ch = ui::stepper("##v", v, step, lo, hi, fmt, ui::stepperWidth(fmt) + 90.0f);
    ImGui::SameLine();
    ImGui::TextUnformatted(label, std::strstr(label, "##"));
    ImGui::PopID();
    return ch;
}

// ImGui's fills want an outline clockwise on the screen (y down).
void clockwise(std::vector<ImVec2>& pts) {
    float a = 0.0f;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const ImVec2 u = pts[i], v = pts[(i + 1) % pts.size()];
        a += u.x * v.y - v.x * u.y;
    }
    if (a < 0.0f) std::reverse(pts.begin(), pts.end());
}

} // namespace

PenHolderTool::PenHolderTool(Deps d) : m_d(std::move(d)) {}

int PenHolderTool::selectedId() const {
    if (!m_d.sel.valid()) return -1;
    const Entity* e = &m_d.document.entities()[static_cast<std::size_t>(m_d.sel.index())];
    for (int depth = 0; e && depth < 64; ++depth) {
        if (e->components.get<PenHolderComponent>()) return e->id;
        if (e->parent < 0) break;
        e = m_d.document.find(e->parent);
    }
    return -1;
}

void PenHolderTool::placeAt(const glm::vec3& at, int parentId) {
    const fitzel::AssetId mat = penholder::ensureMaterial(m_d.document.materials(), m_cfg);
    std::vector<Entity> es = penholder::entities(m_cfg, mat, m_d.entityCounter, at);
    if (es.empty()) {
        m_d.status = "That pen holder could not be built.";
        return;
    }
    if (const Entity* parent = parentId >= 0 ? m_d.document.find(parentId) : nullptr) {
        Entity& root = es.front();
        root.parent = parent->id;
        const glm::mat4 pw = scenegraph::worldOf(*parent);
        scenegraph::setWorld(root, root.center, root.rotation, &pw);
    }
    const std::string name = es.front().name;
    m_liveId = es.front().id;
    m_raise  = true;   // docked behind another tab, it would open unseen
    m_d.history.push(std::make_unique<AddEntitiesCmd>(std::move(es), "Pen holder"), m_d.document);
    m_d.sel.select(m_liveId);
    m_d.status = "Placed " + name + ".";
}

void PenHolderTool::place() { placeAt(m_d.spawnPoint(0.6f)); }

void PenHolderTool::updateWith(const Params& p) {
    m_cfg = p;
    rebuild();
}

void PenHolderTool::rebuild() {
    m_pending = false;
    const int idx = m_d.document.indexOf(m_liveId);
    if (idx < 0) { m_liveId = -1; return; }
    const Entity old = m_d.document.entities()[static_cast<std::size_t>(idx)];
    std::vector<int> ids{old.id};
    for (bool grew = true; grew;) {
        grew = false;
        for (const Entity& e : m_d.document.entities())
            if (std::find(ids.begin(), ids.end(), e.id) == ids.end() &&
                std::find(ids.begin(), ids.end(), e.parent) != ids.end()) {
                ids.push_back(e.id);
                grew = true;
            }
    }
    // Where the placed one's bottom stands, along its own up: the new one stands
    // there too, however much taller or lower it is.
    const glm::vec3 upL(scenegraph::compose(glm::vec3(0.0f), old.localRotation, glm::vec3(1.0f)) *
                        glm::vec4(0.0f, 1.0f, 0.0f, 0.0f));
    const glm::vec3 upW(scenegraph::worldOf(old) * glm::vec4(0.0f, 1.0f, 0.0f, 0.0f));
    const fitzel::AssetId mat = penholder::ensureMaterial(m_d.document.materials(), m_cfg);
    std::vector<Entity> es = penholder::entities(m_cfg, mat, m_d.entityCounter, glm::vec3(0.0f));
    if (es.empty()) return;
    const float lift = es.front().half.y - old.half.y;
    // A name the panel gave follows the settings; one typed stays.
    const auto* was = old.components.get<PenHolderComponent>();
    if (!(was && old.name == penholder::autoName(was->params))) es.front().name = old.name;
    es.front().parent        = old.parent;
    es.front().localRotation = old.localRotation;
    es.front().rotation      = old.rotation;
    es.front().localCenter   = old.localCenter + upL * lift;
    es.front().center        = old.center + upW * lift;
    m_liveId = es.front().id;
    m_d.history.push(std::make_unique<ReplaceEntitiesCmd>(m_d.document, ids, std::move(es), "Pen holder"),
                     m_d.document);
    m_d.sel.select(m_liveId);
}

void PenHolderTool::editSelected() {
    const int id = selectedId();
    const Entity* e = id >= 0 ? m_d.document.find(id) : nullptr;
    if (!e) return;
    m_cfg    = e->components.get<PenHolderComponent>()->params;
    m_liveId = id;
    m_d.sel.select(id);
    m_d.status = "Editing " + e->name + ".";
}

void PenHolderTool::refresh() {
    const Params p = penholder::sane(m_cfg);
    if (m_haveModel && p == m_shown) return;
    m_shown = p;
    m_haveModel = true;
    m_model = penholder::build(p);
}

void PenHolderTool::panel(bool& show) {
    ImGui::SetNextWindowSize(ImVec2(440, 820), ImGuiCond_FirstUseEver);
    if (m_raise) {
        ImGui::SetNextWindowFocus();
        m_raise = false;
    }
    if (!ImGui::Begin("Pen holder", &show)) { ImGui::End(); return; }
    if (m_liveId >= 0 && m_d.document.indexOf(m_liveId) < 0) m_liveId = -1;
    bool changed = false;
    Params& c = m_cfg;
    const float avail = ImGui::GetContentRegionAvail().x;

    // --- The layout ---------------------------------------------------------------------
    {
        ImGui::PushID("layout");
        const float w = 0.5f * (avail - ImGui::GetStyle().ItemSpacing.x);
        for (int i = 0; i < static_cast<int>(Layout::Count); ++i) {
            if (i) ImGui::SameLine();
            const bool on = static_cast<int>(c.layout) == i;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(penholder::layoutName(static_cast<Layout>(i)), ImVec2(w, 28.0f)) && !on) {
                c.layout = static_cast<Layout>(i);
                changed = true;
            }
            if (on) ImGui::PopStyleColor();
        }
        ImGui::PopID();
        ui::hint("A honeycomb, one cell a pen: the least plastic round a set of pens, and room for nothing else.");
    }

    // --- The drawing -------------------------------------------------------------------
    refresh();
    const penholder::Model& m = m_model;
    {
        const float h = 210.0f;
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(avail, h));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 bg = IM_COL32(38, 42, 50, 255);
        dl->AddRectFilled(o, ImVec2(o.x + avail, o.y + h), bg, 4.0f);
        // Both views at one scale, side by side, so they read together.
        const float gap = 18.0f, top = 20.0f;
        std::vector<std::pair<glm::vec2, glm::vec2>> boxes;
        float sumW = 0.0f, maxH = 0.0f;
        for (const penholder::View& v : m.views) {
            glm::vec2 lo(1e9f), hi(-1e9f);
            for (const auto& loop : v.loops)
                for (const glm::vec2& q : loop) {
                    lo = glm::min(lo, q);
                    hi = glm::max(hi, q);
                }
            boxes.push_back({lo, hi});
            sumW += hi.x - lo.x;
            maxH = std::max(maxH, hi.y - lo.y);
        }
        if (!boxes.empty() && sumW > 0.0f && maxH > 0.0f) {
            const float n = static_cast<float>(boxes.size());
            const float s = std::min((avail - gap * (n + 1.0f)) / sumW, (h - top - 12.0f) / maxH);
            float x = o.x + 0.5f * (avail - (sumW * s + gap * (n - 1.0f)));
            const glm::vec3 col = c.color.r + c.color.g + c.color.b < 0.6f ? glm::vec3(0.55f, 0.6f, 0.68f) : c.color;
            std::vector<ImVec2> pts;
            for (std::size_t k = 0; k < m.views.size(); ++k) {
                const penholder::View& v = m.views[k];
                const auto [lo, hi] = boxes[k];
                const float y0 = o.y + top + 0.5f * (h - top - 12.0f - (hi.y - lo.y) * s) + (hi.y - lo.y) * s;
                auto P = [&](glm::vec2 q) { return ImVec2(x + (q.x - lo.x) * s, y0 - (q.y - lo.y) * s); };
                for (std::size_t l = 0; l < v.loops.size(); ++l) {
                    pts.clear();
                    for (const glm::vec2& q : v.loops[l]) pts.push_back(P(q));
                    if (pts.size() < 3) continue;
                    clockwise(pts);
                    const bool solid = static_cast<int>(l) < v.solids;
                    dl->AddConcavePolyFilled(pts.data(), static_cast<int>(pts.size()),
                                             solid ? colourOf(col, 1.0f, 0.75f) : bg);
                    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), colourOf(col), ImDrawFlags_Closed, 1.0f);
                }
                dl->AddText(ImVec2(x, o.y + 4.0f), IM_COL32(200, 205, 215, 200), v.name.c_str());
                x += (hi.x - lo.x) * s + gap;
            }
        }
        if (!m.ok) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "Not built cleanly: %s", m.error.c_str());
        ui::hint("%d pens, %.1f x %.1f x %.1f mm", m.cells, m.size.x, m.size.y, m.size.z);
        ImGui::Text("%.1f g of PLA, %.2f m of filament  (%.1f g a pen)", m.grams, m.metres,
                    m.cells > 0 ? m.grams / m.cells : 0.0);
        if (std::max(m.size.x, m.size.y) > 256.0f)
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Larger than a 256 mm bed.");
        if (!m.printHint.empty()) ui::hint("%s", m.printHint.c_str());
    }

    // --- Settings ------------------------------------------------------------------------
    if (ui::header("Cells", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (c.layout == Layout::Round) {
            changed |= count("Rings", c.rings, 0, 4);
            ImGui::SetItemTooltip("Rings of cells round the middle one: 0 is one pen, 1 seven, 2 nineteen.");
        } else {
            changed |= count("Columns", c.columns, 1, 12);
            changed |= count("Rows", c.rows, 1, 8);
            ImGui::SetItemTooltip("Every other row half a cell along, as honeycombs go.");
        }
        changed |= mm("Thickest pen", c.pen, 0.5f, 4.0f, penholder::kMaxCell - c.play);
        ImGui::SetItemTooltip("Across its barrel or its clip, whichever is wider. Up to %.1f mm with this play:\n"
                              "a cell stays at most %.0f mm across, so it takes a pen and nothing bigger.",
                              penholder::kMaxCell - c.play, penholder::kMaxCell);
        changed |= mm("Play", c.play, 0.1f, 0.3f, 3.0f);
        ImGui::SetItemTooltip("Added to the pen: the cell's width across its flats.");
        changed |= mm("Height", c.height, 5.0f, 20.0f, 200.0f, "%.0f mm");
        ImGui::SetItemTooltip("A third to a half of a pen's length holds it; every millimetre costs plastic.");
        if (const float used = penholder::sane(c).height; used > c.height + 0.01f)
            ui::hint("%.0f mm used: the funnels need that much under the cells.", used);
        changed |= mm("Slope", c.slope, 1.0f, 0.0f, 45.0f, "%.0f deg");
        ImGui::SetItemTooltip("The cells' mouths falling away by this angle: the outer pens stand out\n"
                              "further and come to hand. Height is then the highest point.");
        if (c.slope > 0.0f) {
            ImGui::Indent();
            for (int i = 0; i < static_cast<int>(penholder::SlopeTo::Count); ++i) {
                if (i) ImGui::SameLine();
                int v = static_cast<int>(c.slopeTo);
                if (ImGui::RadioButton(penholder::slopeName(static_cast<penholder::SlopeTo>(i)), &v, i)) {
                    c.slopeTo = static_cast<penholder::SlopeTo>(v);
                    changed = true;
                }
            }
            ImGui::SetItemTooltip("To the rim: highest in the middle, falling all round (a cone).\n"
                                  "To the front: highest at the back, for one on the desk's back edge.");
            if (const float used = penholder::sane(c).slope; used < c.slope - 0.01f)
                ui::hint("%.1f deg used: steeper, the outer cells would hold less than 10 mm of a pen.", used);
            ui::hint("The shallowest cell is %.0f mm deep, the top %.0f to %.0f mm high.", m.shallowest,
                     m.lowestTop, m.size.z);
            ImGui::Unindent();
        }
        changed |= mm("Wall", c.wall, 0.05f, 0.6f, 3.0f, "%.2f mm");
        ImGui::SetItemTooltip("Every wall, shared between two cells: two lines of your extrusion width\n"
                              "(0.9 mm for 0.45) is the least that prints solid -- slice with 2 walls.");
    }
    if (ui::header("Bottom", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= mm("Drain", c.drain, 0.25f, 1.5f, 6.0f);
        ImGui::SetItemTooltip("The hole each cell's funnel ends in: what is this small falls through\n"
                              "to the desk instead of gathering in the holder.");
        changed |= mm("Throat", c.tipRoom, 0.5f, 2.0f, 25.0f);
        ImGui::SetItemTooltip("How far above the desk the funnel ends: a pen's or a pencil's point\n"
                              "stands in the drain and must not reach the desk.");
        if (const float used = penholder::sane(c).tipRoom; used > c.tipRoom + 0.01f)
            ui::hint("%.1f mm used: a sharpened pencil's point would reach the desk through this drain.", used);
        changed |= mm("Foot", c.foot, 0.5f, 0.0f, 20.0f);
        ImGui::SetItemTooltip("A thin rim round the cells on the desk, so a tall one stands steady\n"
                              "(and sticks to the bed). 0: the walls stand on the desk alone.");
        if (c.foot > 0.0f) {
            ImGui::Indent();
            changed |= mm("Foot height", c.footHeight, 0.2f, 0.6f, 5.0f);
            ImGui::Unindent();
        }
    }
    if (ui::header("Only pens", ImGuiTreeNodeFlags_DefaultOpen)) {
        const Params s = m_shown;
        ui::hint("- One pen a cell: nothing wider than %.1f mm goes in at all (corner to corner), not even a "
                 "coin on edge -- the smallest euro coin is 16.25 mm. No bowl, no compartment.", m.widest);
        ui::hint("- No floor: each cell ends in a funnel and a %.1f mm drain; crumbs, shavings, staples fall "
                 "through to the desk. Turned over, anything that is not a pen falls out.", s.drain);
        ui::hint("- Nothing to put things on: the top is only the walls' edges, %.2f mm wide.", s.wall);
    }
    if (ui::header("Colour")) {
        changed |= ImGui::ColorEdit3("Holder", &c.color.x, ImGuiColorEditFlags_NoInputs);
    }

    // --- Place / rebuild ---------------------------------------------------------------------
    ImGui::Separator();
    const Entity* liveE = m_liveId >= 0 ? m_d.document.find(m_liveId) : nullptr;
    const bool live = liveE && liveE->components.get<PenHolderComponent>();
    if (changed) m_pending = true;
    if (ImGui::Button("Place new", ImVec2(avail * 0.5f - 4.0f, 34.0f))) place();
    ImGui::SetItemTooltip("At the 3D cursor if it is shown, else in front of the camera.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!live);
    if (ImGui::Button("Update placed", ImVec2(-1.0f, 34.0f))) rebuild();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("The placed one rebuilt with these settings, standing where it stands.");
    ImGui::Checkbox("Update the placed one on every change", &m_auto);
    const int selId = selectedId();
    ImGui::BeginDisabled(selId < 0 || selId == m_liveId);
    if (ImGui::Button("Edit selected", ImVec2(-1.0f, 30.0f))) editSelected();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Load the selected pen holder's settings into this panel.");
    if (live) ImGui::TextDisabled("Editing: %s", liveE->name.c_str());
    // Rebuild once the edit settles (a value typed is one undo step, not one per key).
    if (m_pending && m_auto && live && !ImGui::IsAnyItemActive()) rebuild();
    if (!live) m_pending = false;
    ImGui::End();
}

} // namespace penui
