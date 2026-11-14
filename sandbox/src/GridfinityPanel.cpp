#include "GridfinityPanel.hpp"

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

namespace gridui {

using gridfinity::Kind;
using gridfinity::Params;

namespace {

ImU32 colourOf(const glm::vec3& c, float a = 1.0f, float k = 1.0f) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r * k, c.g * k, c.b * k, a));
}

// A count: stepped with - and +, or typed.
bool count(const char* label, int& v, int lo, int hi, const char* fmt = "%.0f") {
    ImGui::PushID(label);
    float f = static_cast<float>(v);
    const bool ch = ui::stepper("##v", f, 1.0f, static_cast<float>(lo), static_cast<float>(hi), fmt,
                                ui::stepperWidth(fmt) + 90.0f);
    ImGui::SameLine();
    ImGui::TextUnformatted(label, std::strstr(label, "##"));
    ImGui::PopID();
    if (!ch) return false;
    const int n = std::clamp(static_cast<int>(std::lround(f)), lo, hi);
    if (n == v) return false;
    v = n;
    return true;
}

// A measurement in millimetres: stepped, or typed -- never dragged.
bool mm(const char* label, float& v, float step, float lo, float hi, const char* fmt = "%.2f mm") {
    ImGui::PushID(label);
    const bool ch = ui::stepper("##v", v, step, lo, hi, fmt, ui::stepperWidth(fmt) + 90.0f);
    ImGui::SameLine();
    ImGui::TextUnformatted(label, std::strstr(label, "##"));
    ImGui::PopID();
    return ch;
}

// ImGui's fills want an outline clockwise on the screen (y down); a cut comes
// out either way round.
void clockwise(std::vector<ImVec2>& pts) {
    float a = 0.0f;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const ImVec2 u = pts[i], v = pts[(i + 1) % pts.size()];
        a += u.x * v.y - v.x * u.y;
    }
    if (a < 0.0f) std::reverse(pts.begin(), pts.end());
}

} // namespace

GridfinityTool::GridfinityTool(Deps d) : m_d(std::move(d)) {}

int GridfinityTool::selectedId() const {
    if (!m_d.sel.valid()) return -1;
    const Entity* e = &m_d.document.entities()[static_cast<std::size_t>(m_d.sel.index())];
    for (int depth = 0; e && depth < 64; ++depth) {
        if (e->components.get<GridfinityComponent>()) return e->id;
        if (e->parent < 0) break;
        e = m_d.document.find(e->parent);
    }
    return -1;
}

void GridfinityTool::placeAt(const glm::vec3& at, int parentId) {
    const fitzel::AssetId mat = gridfinity::ensureMaterial(m_d.document.materials(), m_cfg);
    std::vector<Entity> es = gridfinity::entities(m_cfg, mat, m_d.entityCounter, at);
    if (es.empty()) {
        m_d.status = "That Gridfinity part could not be built.";
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
    m_d.history.push(std::make_unique<AddEntitiesCmd>(std::move(es), "Gridfinity"), m_d.document);
    m_d.sel.select(m_liveId);
    m_d.status = "Placed " + name + ".";
}

void GridfinityTool::place() { placeAt(m_d.spawnPoint(0.6f)); }

void GridfinityTool::setKind(Kind k) {
    if (m_cfg.kind != k) m_liveId = -1;   // a bin does not turn into a plate
    m_cfg.kind = k;
}

void GridfinityTool::setParams(const Params& p) { m_cfg = p; }

void GridfinityTool::updateWith(const Params& p) {
    m_cfg = p;
    rebuild();
}

void GridfinityTool::rebuild() {
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
    const fitzel::AssetId mat = gridfinity::ensureMaterial(m_d.document.materials(), m_cfg);
    std::vector<Entity> es = gridfinity::entities(m_cfg, mat, m_d.entityCounter, glm::vec3(0.0f));
    if (es.empty()) return;
    const float lift = es.front().half.y - old.half.y;
    // Keep what the user did to the placed one: where it hangs, how it turns.
    // A name the panel gave follows the sizes; one typed stays.
    const auto* was = old.components.get<GridfinityComponent>();
    if (!(was && old.name == gridfinity::autoName(was->params))) es.front().name = old.name;
    es.front().parent        = old.parent;
    es.front().localRotation = old.localRotation;
    es.front().rotation      = old.rotation;
    es.front().localCenter   = old.localCenter + upL * lift;
    es.front().center        = old.center + upW * lift;
    m_liveId = es.front().id;
    m_d.history.push(std::make_unique<ReplaceEntitiesCmd>(m_d.document, ids, std::move(es), "Gridfinity"),
                     m_d.document);
    m_d.sel.select(m_liveId);
}

void GridfinityTool::editSelected() {
    const int id = selectedId();
    const Entity* e = id >= 0 ? m_d.document.find(id) : nullptr;
    if (!e) return;
    m_cfg    = e->components.get<GridfinityComponent>()->params;
    m_liveId = id;
    m_d.sel.select(id);
    m_d.status = "Editing " + e->name + ".";
}

void GridfinityTool::refresh() {
    const Params p = gridfinity::sane(m_cfg);
    if (m_haveModel && p == m_shown) return;
    m_shown = p;
    m_haveModel = true;
    m_model = gridfinity::build(p);
    // Cut front to back through the first compartment (a bin) or through the
    // first column's magnets (a plate), a hair aside from any corner.
    if (p.kind == Kind::Bin) {
        const float A = 21.0f * static_cast<float>(p.unitsX) - 0.25f;
        const float pw = (2.0f * (A - p.wall) - static_cast<float>(p.compX - 1) * p.divider) / static_cast<float>(p.compX);
        m_cutX = -A + p.wall + 0.5f * pw + 0.013f;
    } else {
        m_cutX = -21.0f * static_cast<float>(p.plateX) + 21.0f - 13.0f + 0.013f;
    }
    m_section = gridfinity::section(m_model.mesh, m_cutX);
}

void GridfinityTool::binControls(bool& changed) {
    Params& c = m_cfg;
    if (ui::header("Size", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= count("Across", c.unitsX, 1, 16, "%.0f units");
        ImGui::SetItemTooltip("Grid units of 42 mm: the bin is 42 x units - 0.5 mm wide.");
        changed |= count("Front to back", c.unitsY, 1, 16, "%.0f units");
        changed |= count("Height", c.unitsZ, 1, 40, "%.0f x 7 mm");
        ImGui::SetItemTooltip("Height units of 7 mm, the base included, the stacking lip not.\n"
                              "1 is a solid tile: the floor of the compartments is at 7 mm.");
    }
    if (ui::header("Compartments", ImGuiTreeNodeFlags_DefaultOpen)) {
        const int mx = gridfinity::maxCompartments(c.unitsX, c.wall, c.divider);
        const int my = gridfinity::maxCompartments(c.unitsY, c.wall, c.divider);
        changed |= count("Across##comp", c.compX, 1, mx);
        ImGui::SetItemTooltip("At most %d: narrower ones would be all rounded corner.", mx);
        changed |= count("Front to back##comp", c.compY, 1, my);
        ImGui::SetItemTooltip("At most %d.", my);
        changed |= ImGui::Checkbox("Label shelf", &c.label);
        ImGui::SetItemTooltip("A shelf along each compartment's back wall, flush with its top, to stick\n"
                              "a label on. Its underside slopes at 36 deg: it prints without support.");
        if (c.label) {
            ImGui::Indent();
            changed |= mm("Shelf depth", c.labelDepth, 0.5f, 3.0f, 40.0f, "%.2f mm");
            ImGui::SetItemTooltip("From the back wall in. The standard's is 15.85 mm.");
            ImGui::Unindent();
        }
        changed |= ImGui::Checkbox("Scoop", &c.scoop);
        ImGui::SetItemTooltip("Each compartment's front wall rounded into its floor, so small parts\n"
                              "slide out on a fingertip.");
        if (c.scoop) {
            ImGui::Indent();
            changed |= mm("Scoop radius", c.scoopRadius, 1.0f, 2.0f, 100.0f, "%.0f mm");
            ImGui::Unindent();
        }
    }
    if (ui::header("Base and lip", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= ImGui::Checkbox("Stacking lip", &c.lip);
        ImGui::SetItemTooltip("A rim that takes the feet of a bin set on top -- 3.55 mm above the walls.\n"
                              "Off: a flat top, and the bin is lower by that much.");
        changed |= ImGui::Checkbox("Magnet holes", &c.magnets);
        ImGui::SetItemTooltip("6.5 mm x 2.4 mm under each corner of each foot, for 6 x 2 mm magnets\n"
                              "(glued, or pressed in during a pause at 2.4 mm).");
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Screw holes", &c.screws);
        ImGui::SetItemTooltip("3 mm holes 6 mm deep (M3), above the magnets when both are on.");
    }
    if (ui::header("Walls and floor")) {
        changed |= mm("Wall", c.wall, 0.05f, 0.4f, 2.4f);
        ImGui::SetItemTooltip("The standard's 0.95 mm prints as two lines of a 0.4 mm nozzle.");
        changed |= mm("Divider", c.divider, 0.1f, 0.4f, 5.0f);
        changed |= mm("Floor", c.floor, 0.25f, gridfinity::minFloor(c), 300.0f);
        ImGui::SetItemTooltip("The compartments' floor above the bottom of the feet. The standard's\n"
                              "is 7 mm, one height unit; lower gives room, down to %.2f mm here.",
                              gridfinity::minFloor(c));
        if (ImGui::SmallButton("The standard's")) {
            c.wall = 0.95f;
            c.divider = 1.2f;
            c.floor = 7.0f;
            changed = true;
        }
    }
}

void GridfinityTool::plateControls(bool& changed) {
    Params& c = m_cfg;
    if (ui::header("Size", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= ImGui::Checkbox("Fill a drawer", &c.fitDrawer);
        ImGui::SetItemTooltip("Type the drawer's inside: as many cells as fit, a solid margin round them.");
        if (c.fitDrawer) {
            ImGui::Indent();
            changed |= mm("Drawer width", c.drawerW, 1.0f, 42.0f, 2000.0f, "%.1f mm");
            changed |= mm("Drawer depth", c.drawerD, 1.0f, 42.0f, 2000.0f, "%.1f mm");
            const Params s = gridfinity::sane(c);
            ui::hint("%d x %d cells, margins %.2f mm a side and %.2f mm front and back.", s.plateX, s.plateY,
                     0.5 * (std::max<double>(s.drawerW, 42.0 * s.plateX) - 42.0 * s.plateX),
                     0.5 * (std::max<double>(s.drawerD, 42.0 * s.plateY) - 42.0 * s.plateY));
            ImGui::Unindent();
        } else {
            changed |= count("Across", c.plateX, 1, 40, "%.0f cells");
            changed |= count("Front to back", c.plateY, 1, 40, "%.0f cells");
        }
    }
    if (ui::header("Floor", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= ImGui::Checkbox("Solid floor", &c.plateFloor);
        ImGui::SetItemTooltip("Off: an open frame, 5 mm high, the bins standing on the drawer's bottom.");
        if (c.plateFloor) {
            ImGui::Indent();
            changed |= mm("Thickness", c.floorThickness, 0.2f, 1.0f, 30.0f, "%.1f mm");
            changed |= ImGui::Checkbox("Magnet holes##plate", &c.plateMagnets);
            ImGui::SetItemTooltip("Under each corner of each cell, where a bin's magnets come down.");
            ImGui::SameLine();
            changed |= ImGui::Checkbox("Screw holes##plate", &c.plateScrews);
            ImGui::SetItemTooltip("A countersunk 3.5 mm hole in each cell, to screw the plate down.");
            ImGui::Unindent();
        }
    }
}

void GridfinityTool::panel(bool& show) {
    ImGui::SetNextWindowSize(ImVec2(440, 820), ImGuiCond_FirstUseEver);
    if (m_raise) {
        ImGui::SetNextWindowFocus();
        m_raise = false;
    }
    if (!ImGui::Begin("Gridfinity", &show)) { ImGui::End(); return; }
    if (m_liveId >= 0 && m_d.document.indexOf(m_liveId) < 0) m_liveId = -1;
    bool changed = false;
    const float avail = ImGui::GetContentRegionAvail().x;

    // --- Bin or baseplate ------------------------------------------------------------
    {
        int k = m_cfg.kind == Kind::Bin ? 0 : 1;
        ImGui::PushID("kind");
        const float w = 0.5f * (avail - ImGui::GetStyle().ItemSpacing.x);
        for (int i = 0; i < 2; ++i) {
            if (i) ImGui::SameLine();
            const bool on = k == i;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(i == 0 ? "Bin" : "Baseplate", ImVec2(w, 28.0f)) && !on) {
                setKind(i == 0 ? Kind::Bin : Kind::Baseplate);
                changed = true;
            }
            if (on) ImGui::PopStyleColor();
        }
        ImGui::PopID();
    }

    // --- The preview: from above, and cut front to back ---------------------------------
    // Side by side where the panel is wide and the part is a bin, one above the
    // other for a flat plate or a narrow panel.
    refresh();
    const Params p = m_shown;
    const gridfinity::Model& m = m_model;
    {
        const bool wide = avail >= 380.0f && p.kind == Kind::Bin;
        const float planW = wide ? 0.5f * avail : avail;
        const float planH = wide ? 210.0f : std::clamp(avail * m.size.y / std::max(m.size.x, 1.0f) + 20.0f, 90.0f, 200.0f);
        const float cutW = wide ? avail - planW : avail;
        const float cutH = wide ? planH : std::clamp(avail * m.size.z / std::max(m.size.y, 1.0f) + 30.0f, 56.0f, 200.0f);
        const float h = wide ? planH : planH + cutH;
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(avail, h));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(o, ImVec2(o.x + avail, o.y + h), IM_COL32(38, 42, 50, 255), 4.0f);
        const glm::vec3 col = p.kind == Kind::Bin ? p.binColor : p.plateColor;
        // From above: x right, the back up.
        if (!m.plan.empty()) {
            const float s = std::min((planW - 20.0f) / m.size.x, (planH - 20.0f) / m.size.y);
            const ImVec2 c(o.x + 0.5f * planW, o.y + 0.5f * planH);
            auto P = [&](glm::vec2 v) { return ImVec2(c.x + v.x * s, c.y - v.y * s); };
            std::vector<ImVec2> pts;
            for (std::size_t l = 0; l < m.plan.size(); ++l) {
                pts.clear();
                for (glm::vec2 v : m.plan[l]) pts.push_back(P(v));
                if (pts.size() < 3) continue;
                clockwise(pts);
                if (l == 0) {
                    dl->AddConvexPolyFilled(pts.data(), static_cast<int>(pts.size()), colourOf(col, 1.0f, 0.75f));
                    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), colourOf(col), ImDrawFlags_Closed, 1.5f);
                } else {
                    // A bin: each compartment's top, then its floor; a plate: the cells.
                    const bool floorRing = p.kind == Kind::Bin && l % 2 == 0;
                    if (!floorRing) dl->AddConvexPolyFilled(pts.data(), static_cast<int>(pts.size()), IM_COL32(38, 42, 50, 255));
                    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()),
                                    floorRing ? colourOf(col, 0.55f) : colourOf(col), ImDrawFlags_Closed, 1.0f);
                }
            }
            for (const std::vector<glm::vec2>& hole : m.holes) {
                pts.clear();
                for (glm::vec2 v : hole) pts.push_back(P(v));
                dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), colourOf(col, 0.8f), ImDrawFlags_Closed, 1.0f);
            }
            // Where the cut runs.
            const ImVec2 a = P({m_cutX, -0.5f * m.size.y - 3.0f}), b = P({m_cutX, 0.5f * m.size.y + 3.0f});
            dl->AddLine(a, b, IM_COL32(255, 255, 255, 90), 1.0f);
        }
        // The cut: the front to the left, the bottom down.
        if (!m_section.empty()) {
            const ImVec2 co = wide ? ImVec2(o.x + planW, o.y) : ImVec2(o.x, o.y + planH);
            const float s = std::min((cutW - 24.0f) / m.size.y, (cutH - 30.0f) / std::max(m.size.z, 1.0f));
            const ImVec2 c(co.x + 0.5f * cutW, co.y + cutH - 12.0f);
            auto P = [&](glm::vec2 v) { return ImVec2(c.x + v.x * s, c.y - v.y * s); };
            std::vector<ImVec2> pts;
            for (const std::vector<glm::vec2>& loop : m_section) {
                pts.clear();
                for (glm::vec2 v : loop) pts.push_back(P(v));
                clockwise(pts);
                dl->AddConcavePolyFilled(pts.data(), static_cast<int>(pts.size()), colourOf(col, 1.0f, 0.75f));
                dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), colourOf(col), ImDrawFlags_Closed, 1.0f);
            }
            dl->AddText(ImVec2(co.x + 6.0f, co.y + 4.0f), IM_COL32(200, 205, 215, 200), "cut, front to the left");
        }
        if (!m.ok) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "Not built cleanly: %s", m.error.c_str());
        ui::hint("%.1f x %.1f x %.2f mm, %.1f cm3 of plastic", m.size.x, m.size.y, m.size.z, m.volume / 1000.0);
        if (p.kind == Kind::Bin) {
            if (m.compartments > 0)
                ui::hint("%d compartment%s of %.0f ml", m.compartments, m.compartments == 1 ? "" : "s",
                         m.capacity / 1000.0);
            else
                ui::hint("Solid: no room above the %.0f mm floor at this height.", p.floor);
        } else {
            ui::hint("%d x %d cells", p.plateX, p.plateY);
        }
        if (std::max(m.size.x, m.size.y) > 256.0f)
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Larger than a 256 mm bed: print it in pieces.");
    }

    // --- Settings -------------------------------------------------------------------------
    if (m_cfg.kind == Kind::Bin) binControls(changed);
    else plateControls(changed);
    if (ui::header("Colour")) {
        glm::vec3& col = m_cfg.kind == Kind::Bin ? m_cfg.binColor : m_cfg.plateColor;
        changed |= ImGui::ColorEdit3("##colour", &col.x, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        ui::hint("The 3MF export hands it to the slicer as its filament.");
    }

    // --- Place / rebuild -------------------------------------------------------------------
    ImGui::Separator();
    const Entity* liveE = m_liveId >= 0 ? m_d.document.find(m_liveId) : nullptr;
    const auto* liveG = liveE ? liveE->components.get<GridfinityComponent>() : nullptr;
    const bool live = liveG && liveG->params.kind == m_cfg.kind;
    if (changed) m_pending = true;
    const char* what = m_cfg.kind == Kind::Bin ? "bin" : "baseplate";
    if (ImGui::Button("Place new", ImVec2(avail * 0.5f - 4.0f, 34.0f))) place();
    ImGui::SetItemTooltip("A new %s at the 3D cursor if it is shown, else in front of the camera.", what);
    ImGui::SameLine();
    ImGui::BeginDisabled(!live);
    if (ImGui::Button("Update placed", ImVec2(-1.0f, 34.0f))) rebuild();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("The placed %s rebuilt with these settings, standing where it stands.", what);
    ImGui::Checkbox("Update the placed one on every change", &m_auto);
    const int selId = selectedId();
    ImGui::BeginDisabled(selId < 0 || selId == m_liveId);
    if (ImGui::Button("Edit selected", ImVec2(-1.0f, 30.0f))) editSelected();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Load the selected bin's or baseplate's settings into this panel.");
    if (live) ImGui::TextDisabled("Editing: %s", liveE->name.c_str());
    // Rebuild once the edit settles (a typed number is one undo step, not one per key).
    if (m_pending && m_auto && live && !ImGui::IsAnyItemActive()) rebuild();
    if (!live) m_pending = false;
    ImGui::End();
}

} // namespace gridui
