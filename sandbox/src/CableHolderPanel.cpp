#include "CableHolderPanel.hpp"

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

namespace cableui {

using cableholder::HookMount;
using cableholder::Mount;
using cableholder::Params;
using cableholder::Style;

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

const char* hint(Style s) {
    switch (s) {
    case Style::Comb:     return "A bar with seats the cables snap into: on the desk, on a pad, screwed or on magnets.";
    case Style::DeskClip: return "The bar on a clamp over the desk's edge: charging cables stay up when unplugged.";
    case Style::Tray:     return "A channel screwed under the desk for power strips and cable runs.";
    case Style::Hook:     return "For coiled cables and headphones, on a wall or under the desk.";
    case Style::Winder:   return "Wind an earphone or charging cable round it; its ends press into the slits.";
    default:              return "";
    }
}

} // namespace

CableHolderTool::CableHolderTool(Deps d) : m_d(std::move(d)) { syncText(); }

void CableHolderTool::syncText() {
    std::snprintf(m_diameters, sizeof m_diameters, "%s", m_cfg.diameters.c_str());
    std::snprintf(m_labels, sizeof m_labels, "%s", m_cfg.labels.c_str());
    std::snprintf(m_winderLabel, sizeof m_winderLabel, "%s", m_cfg.winderLabel.c_str());
}

int CableHolderTool::selectedId() const {
    if (!m_d.sel.valid()) return -1;
    const Entity* e = &m_d.document.entities()[static_cast<std::size_t>(m_d.sel.index())];
    for (int depth = 0; e && depth < 64; ++depth) {
        if (e->components.get<CableHolderComponent>()) return e->id;
        if (e->parent < 0) break;
        e = m_d.document.find(e->parent);
    }
    return -1;
}

void CableHolderTool::placeAt(const glm::vec3& at, int parentId) {
    const cableholder::Palette pal = cableholder::ensurePalette(m_d.document.materials(), m_cfg);
    std::vector<Entity> es = cableholder::entities(m_cfg, pal, m_d.entityCounter, at);
    if (es.empty()) {
        m_d.status = "That cable holder could not be built.";
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
    m_d.history.push(std::make_unique<AddEntitiesCmd>(std::move(es), "Cable holder"), m_d.document);
    m_d.sel.select(m_liveId);
    m_d.status = "Placed " + name + ".";
}

void CableHolderTool::place() { placeAt(m_d.spawnPoint(0.6f)); }

void CableHolderTool::setStyle(Style s) {
    if (m_cfg.style != s) m_liveId = -1;   // a comb does not turn into a hook
    m_cfg.style = s;
}

void CableHolderTool::setParams(const Params& p) {
    m_cfg = p;
    syncText();
}

void CableHolderTool::updateWith(const Params& p) {
    setParams(p);
    rebuild();
}

void CableHolderTool::rebuild() {
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
    const cableholder::Palette pal = cableholder::ensurePalette(m_d.document.materials(), m_cfg);
    std::vector<Entity> es = cableholder::entities(m_cfg, pal, m_d.entityCounter, glm::vec3(0.0f));
    if (es.empty()) return;
    const float lift = es.front().half.y - old.half.y;
    // A name the panel gave follows the settings; one typed stays.
    const auto* was = old.components.get<CableHolderComponent>();
    if (!(was && old.name == cableholder::autoName(was->params))) es.front().name = old.name;
    es.front().parent        = old.parent;
    es.front().localRotation = old.localRotation;
    es.front().rotation      = old.rotation;
    es.front().localCenter   = old.localCenter + upL * lift;
    es.front().center        = old.center + upW * lift;
    m_liveId = es.front().id;
    m_d.history.push(std::make_unique<ReplaceEntitiesCmd>(m_d.document, ids, std::move(es), "Cable holder"),
                     m_d.document);
    m_d.sel.select(m_liveId);
}

void CableHolderTool::editSelected() {
    const int id = selectedId();
    const Entity* e = id >= 0 ? m_d.document.find(id) : nullptr;
    if (!e) return;
    m_cfg    = e->components.get<CableHolderComponent>()->params;
    m_liveId = id;
    syncText();
    m_d.sel.select(id);
    m_d.status = "Editing " + e->name + ".";
}

void CableHolderTool::refresh() {
    const Params p = cableholder::sane(m_cfg);
    if (m_haveModel && p == m_shown) return;
    m_shown = p;
    m_haveModel = true;
    m_model = cableholder::build(p);
}

void CableHolderTool::seatControls(bool& changed) {
    Params& c = m_cfg;
    if (ui::header("Seats", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= count("Seats", c.slots, 1, 24);
        ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Cables (mm)").x - 16.0f);
        if (ImGui::InputText("Cables (mm)", m_diameters, sizeof m_diameters)) {
            c.diameters = m_diameters;
            changed = true;
        }
        ImGui::SetItemTooltip("Each cable's diameter, left to right: \"5\" for all, \"3.5, 5, 8\" each its own\n"
                              "(the last one is used for the rest). Measure the cable, not the plug.");
        changed |= mm("Clearance", c.clearance, 0.1f, -0.5f, 2.0f, "%.1f mm");
        ImGui::SetItemTooltip("Added to every diameter: 0.3 holds a cable loosely, 0 or less grips it.");
        float pct = c.neck * 100.0f;
        if (mm("Opening", pct, 5.0f, 30.0f, 100.0f, "%.0f %%")) {
            c.neck = pct / 100.0f;
            changed = true;
        }
        ImGui::SetItemTooltip("How wide the neck is, as a share of the seat: smaller snaps the cable in\n"
                              "harder; 100 % is an open U to drop it in.");
        changed |= mm("Between seats", c.tooth, 0.5f, 1.0f, 30.0f);
        changed |= mm("Ends", c.margin, 0.5f, 1.5f, 50.0f);
        changed |= mm("Depth", c.depth, 1.0f, 6.0f, 100.0f, "%.0f mm");
        ImGui::SetItemTooltip("Front to back, along the cables.");
        changed |= mm("Base", c.base, 0.5f, 1.0f, 30.0f);
        ImGui::SetItemTooltip("Plastic under the seats (raised where labels, magnets or a pad need it).");
        if (const float used = cableholder::sane(c).base; used > c.base + 0.01f)
            ui::hint("%.1f mm used: the %s need it.", used,
                     !cableholder::labelsOf(c).empty() ? "labels" : c.mount == Mount::Magnets ? "magnets" : "pad");
        changed |= mm("Rounding", c.rounding, 0.1f, 0.0f, 5.0f);
        ImGui::SetItemTooltip("The edges round every seat and along the top, front and back --\n"
                              "kind to cables, nice to touch. Kept to what the teeth leave room for.");
    }
    if (ui::header("Labels")) {
        ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Cables (mm)").x - 16.0f);
        if (ImGui::InputText("Labels##list", m_labels, sizeof m_labels)) {   // (the section is "Labels")
            c.labels = m_labels;
            changed = true;
        }
        ImGui::SetItemTooltip("One per seat, left to right, comma separated: \"USB-C, Phone, Lamp\".\n"
                              "Raised 0.6 mm under the seats on the front, a part of its own to print\n"
                              "in a second colour.");
        changed |= mm("Letter height", c.labelHeight, 0.25f, 1.5f, 15.0f);
        ImGui::SetItemTooltip("A capital's height; smaller where a label would not fit under its seat.");
    }
}

void CableHolderTool::styleControls(bool& changed) {
    Params& c = m_cfg;
    switch (c.style) {
    case Style::Comb: {
        seatControls(changed);
        if (ui::header("Mount", ImGuiTreeNodeFlags_DefaultOpen)) {
            int m = static_cast<int>(c.mount);
            for (int i = 0; i < static_cast<int>(Mount::Count); ++i) {
                if (i) ImGui::SameLine();
                if (ImGui::RadioButton(cableholder::mountName(static_cast<Mount>(i)), &m, i)) {
                    c.mount = static_cast<Mount>(m);
                    changed = true;
                }
            }
            switch (c.mount) {
            case Mount::Adhesive:
                ui::hint("A 0.6 mm recess underneath takes a double-sided pad, flush.");
                break;
            case Mount::Screws:
                changed |= mm("Screw", c.screwShank, 0.5f, 2.0f, 8.0f);
                changed |= mm("Head", c.screwHead, 0.5f, c.screwShank + 1.0f, 20.0f);
                ui::hint("A flange at each end with a countersunk hole.");
                break;
            case Mount::Magnets:
                changed |= mm("Magnet", c.magnetDiameter, 0.5f, 2.0f, 20.0f, "%.1f mm across");
                changed |= mm("Magnet height", c.magnetHeight, 0.5f, 0.5f, 10.0f);
                ui::hint("Pockets under the outer seats, for a steel desk or a strip.");
                break;
            default:
                break;
            }
        }
        break;
    }
    case Style::DeskClip:
        seatControls(changed);
        if (ui::header("Clamp", ImGuiTreeNodeFlags_DefaultOpen)) {
            changed |= mm("Desk", c.deskThickness, 1.0f, 5.0f, 80.0f, "%.0f mm thick");
            changed |= mm("Grip", c.grip, 0.1f, 0.0f, 3.0f);
            ImGui::SetItemTooltip("The lip under the desk leans up by this much at its tip, so it springs\n"
                                  "against the desk and holds.");
            changed |= mm("Reach", c.reach, 1.0f, 5.0f, 60.0f, "%.0f mm");
            ImGui::SetItemTooltip("How far the lip reaches under the desk.");
            changed |= mm("Clamp", c.clipWall, 0.5f, 2.0f, 10.0f);
            ImGui::SetItemTooltip("The thickness of the clamp's plate, wall and lip.");
        }
        break;
    case Style::Tray:
        if (ui::header("Channel", ImGuiTreeNodeFlags_DefaultOpen)) {
            changed |= mm("Length", c.trayLength, 10.0f, 40.0f, 600.0f, "%.0f mm");
            changed |= mm("Inside width", c.trayWidth, 1.0f, 12.0f, 150.0f, "%.0f mm");
            changed |= mm("Inside height", c.trayHeight, 1.0f, 12.0f, 150.0f, "%.0f mm");
            changed |= mm("Wall", c.trayWall, 0.1f, 1.2f, 6.0f);
            changed |= ImGui::Checkbox("Open front", &c.trayOpenFront);
            ImGui::SetItemTooltip("The front wall half as high, so cables go in from the front; it hangs\n"
                                  "from the back flange alone. Off: a closed channel, flanges both sides.");
            changed |= ImGui::Checkbox("Outlets", &c.trayOutlets);
            ImGui::SetItemTooltip("Round holes in the floor for the cables to drop through.");
            if (c.trayOutlets) {
                ImGui::Indent();
                changed |= mm("Outlet", c.outletDiameter, 1.0f, 5.0f, c.trayWidth - 10.0f, "%.0f mm across");
                changed |= mm("Spacing", c.outletSpacing, 5.0f, c.outletDiameter + 5.0f, 600.0f, "%.0f mm");
                ImGui::Unindent();
            }
            changed |= mm("Screw", c.screwShank, 0.5f, 2.0f, 8.0f);
            changed |= mm("Head", c.screwHead, 0.5f, c.screwShank + 1.0f, 20.0f);
        }
        break;
    case Style::Hook:
        if (ui::header("Hook", ImGuiTreeNodeFlags_DefaultOpen)) {
            int m = static_cast<int>(c.hookMount);
            if (ImGui::RadioButton("On a wall", &m, 0)) changed = true;
            ImGui::SameLine();
            if (ImGui::RadioButton("Under the desk", &m, 1)) changed = true;
            c.hookMount = static_cast<HookMount>(m);
            changed |= mm("Width", c.hookWidth, 1.0f, c.screwHead + 3.0f, 100.0f, "%.0f mm");
            changed |= mm("Thickness", c.hookThickness, 0.5f, 2.5f, 15.0f);
            changed |= mm("Plate", c.hookPlate, 1.0f, c.screwHead + 8.0f, 200.0f, "%.0f mm");
            ImGui::SetItemTooltip("The part against the wall or the desk, with the screw holes.");
            changed |= mm("Arm", c.hookReach, 1.0f, 5.0f, 200.0f, "%.0f mm");
            changed |= mm("Bend", c.hookRadius, 1.0f, 2.0f, 80.0f, "%.0f mm inside");
            ImGui::SetItemTooltip("The bend the cable hangs in: at least the coil's thickness.");
            changed |= mm("Tip", c.hookTip, 1.0f, 0.0f, 100.0f, "%.0f mm");
            changed |= mm("Screw", c.screwShank, 0.5f, 2.0f, 8.0f);
            changed |= mm("Head", c.screwHead, 0.5f, c.screwShank + 1.0f, 20.0f);
        }
        break;
    case Style::Winder:
        if (ui::header("Winder", ImGuiTreeNodeFlags_DefaultOpen)) {
            changed |= mm("Length", c.winderLength, 1.0f, 35.0f, 200.0f, "%.0f mm");
            changed |= mm("Width", c.winderWidth, 1.0f, 16.0f, 100.0f, "%.0f mm");
            changed |= mm("Thickness", c.winderThickness, 0.25f, 1.5f, 10.0f);
            changed |= mm("Notch", c.notchWidth, 0.5f, 3.0f, c.winderWidth - 8.0f, "%.1f mm wide");
            changed |= mm("Notch depth", c.notchDepth, 0.5f, 3.0f, 0.5f * c.winderLength - 15.0f);
            changed |= mm("Cable", c.slit, 0.1f, 0.8f, 8.0f, "%.1f mm across");
            ImGui::SetItemTooltip("The cable's diameter: the slits at the ends hold it snugly.");
            changed |= mm("Rounding", c.rounding, 0.1f, 0.0f, 5.0f);
            ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Cables (mm)").x - 16.0f);
            if (ImGui::InputText("Label", m_winderLabel, sizeof m_winderLabel)) {
                c.winderLabel = m_winderLabel;
                changed = true;
            }
            ImGui::SetItemTooltip("Raised on top in the second colour: what the cable is.");
            changed |= mm("Letter height", c.labelHeight, 0.25f, 1.5f, 15.0f);
        }
        break;
    default:
        break;
    }
}

void CableHolderTool::panel(bool& show) {
    ImGui::SetNextWindowSize(ImVec2(440, 820), ImGuiCond_FirstUseEver);
    if (m_raise) {
        ImGui::SetNextWindowFocus();
        m_raise = false;
    }
    if (!ImGui::Begin("Cable holders", &show)) { ImGui::End(); return; }
    if (m_liveId >= 0 && m_d.document.indexOf(m_liveId) < 0) m_liveId = -1;
    bool changed = false;
    const float avail = ImGui::GetContentRegionAvail().x;

    // --- The kind --------------------------------------------------------------------
    {
        static const char* names[] = {"Comb", "Clip", "Tray", "Hook", "Winder"};
        // Their own scope: the sections below are called the same ("Hook", "Winder").
        ImGui::PushID("kind");
        const float sp = ImGui::GetStyle().ItemSpacing.x;
        const float w = (avail - 4.0f * sp) / 5.0f;
        for (int i = 0; i < static_cast<int>(Style::Count); ++i) {
            if (i) ImGui::SameLine();
            const bool on = static_cast<int>(m_cfg.style) == i;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(names[i], ImVec2(w, 28.0f)) && !on) {
                setStyle(static_cast<Style>(i));
                changed = true;
            }
            if (on) ImGui::PopStyleColor();
            ImGui::SetItemTooltip("%s", hint(static_cast<Style>(i)));
        }
        ImGui::PopID();
        ui::hint("%s", hint(m_cfg.style));
    }

    // --- The drawing -------------------------------------------------------------------
    refresh();
    const Params p = m_shown;
    const cableholder::Model& m = m_model;
    {
        const float h = 200.0f;
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(avail, h));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 bg = IM_COL32(38, 42, 50, 255);
        dl->AddRectFilled(o, ImVec2(o.x + avail, o.y + h), bg, 4.0f);
        // Every view at one scale, side by side, so they read together.
        const float gap = 18.0f, top = 20.0f;
        std::vector<std::pair<glm::vec2, glm::vec2>> boxes;
        float sumW = 0.0f, maxH = 0.0f;
        for (const cableholder::View& v : m.views) {
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
            std::vector<ImVec2> pts;
            for (std::size_t k = 0; k < m.views.size(); ++k) {
                const cableholder::View& v = m.views[k];
                const auto [lo, hi] = boxes[k];
                const float y0 = o.y + top + 0.5f * (h - top - 12.0f - (hi.y - lo.y) * s) + (hi.y - lo.y) * s;
                auto P = [&](glm::vec2 q) { return ImVec2(x + (q.x - lo.x) * s, y0 - (q.y - lo.y) * s); };
                for (std::size_t l = 0; l < v.loops.size(); ++l) {
                    pts.clear();
                    for (const glm::vec2& q : v.loops[l]) pts.push_back(P(q));
                    if (pts.size() < 3) continue;
                    clockwise(pts);
                    const bool solid = static_cast<int>(l) < v.solids;
                    const glm::vec3 col = p.color.r + p.color.g + p.color.b < 0.6f ? glm::vec3(0.55f, 0.6f, 0.68f) : p.color;
                    dl->AddConcavePolyFilled(pts.data(), static_cast<int>(pts.size()),
                                             solid ? colourOf(col, 1.0f, 0.75f) : bg);
                    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), colourOf(col), ImDrawFlags_Closed, 1.0f);
                }
                dl->AddText(ImVec2(x, o.y + 4.0f), IM_COL32(200, 205, 215, 200), v.name.c_str());
                x += (hi.x - lo.x) * s + gap;
            }
        }
        if (!m.ok) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "Not built cleanly: %s", m.error.c_str());
        ui::hint("%.1f x %.1f x %.1f mm, %.1f cm3 of plastic", m.size.x, m.size.y, m.size.z, m.volume / 1000.0);
        if (!m.labels.faces.empty()) ui::hint("Labels: a second part, for a second colour (or a filament change).");
        if (m.missingGlyphs > 0)
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%d label character(s) the font does not have.",
                               m.missingGlyphs);
        if (std::max(m.size.x, m.size.y) > 256.0f)
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Larger than a 256 mm bed.");
        if (!m.printHint.empty()) ui::hint("%s", m.printHint.c_str());
    }

    // --- Settings ------------------------------------------------------------------------
    styleControls(changed);
    if (ui::header("Colours")) {
        changed |= ImGui::ColorEdit3("Holder", &m_cfg.color.x, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        changed |= ImGui::ColorEdit3("Labels##colour", &m_cfg.labelColor.x, ImGuiColorEditFlags_NoInputs);
        ui::hint("The 3MF export hands each colour its own filament.");
    }

    // --- Place / rebuild ---------------------------------------------------------------------
    ImGui::Separator();
    const Entity* liveE = m_liveId >= 0 ? m_d.document.find(m_liveId) : nullptr;
    const auto* liveC = liveE ? liveE->components.get<CableHolderComponent>() : nullptr;
    const bool live = liveC && liveC->params.style == m_cfg.style;
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
    ImGui::SetItemTooltip("Load the selected holder's settings into this panel.");
    if (live) ImGui::TextDisabled("Editing: %s", liveE->name.c_str());
    // Rebuild once the edit settles (typing a list is one undo step, not one per key).
    if (m_pending && m_auto && live && !ImGui::IsAnyItemActive()) rebuild();
    if (!live) m_pending = false;
    ImGui::End();
}

} // namespace cableui
