#include "StreetSignPanel.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include <imgui.h>

#include "Command.hpp"
#include "Document.hpp"
#include "Selection.hpp"
#include "UiStyle.hpp"

namespace signui {

namespace {

using streetsign::Face;
using streetsign::Style;

ImU32 colourOf(const glm::vec3& c) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, 1.0f));
}

// Paint a laid-out sign into `dl`, fitted into the box at `o` of `size`
// (centred, never wider or taller than the box). Returns the scale used.
void drawFace(ImDrawList* dl, const Face& f, const Style& st, ImVec2 o, ImVec2 size) {
    if (f.width <= 0.0f || f.height <= 0.0f) return;
    const float sc = std::min(size.x / f.width, size.y / f.height);
    const ImVec2 c(o.x + 0.5f * size.x, o.y + 0.5f * size.y);
    std::vector<ImVec2> pts;
    auto put = [&](const std::vector<glm::vec2>& poly, ImU32 col) {
        pts.clear();
        // y flips on screen, so the counter-clockwise polygons arrive clockwise
        // -- the winding ImGui's filler expects.
        for (const glm::vec2& v : poly) pts.emplace_back(c.x + v.x * sc, c.y - v.y * sc);
        dl->AddConvexPolyFilled(pts.data(), static_cast<int>(pts.size()), col);
    };
    // The letters are many touching pieces; anti-aliased edges would draw a
    // hairline of plate colour along every seam between them.
    const ImDrawListFlags keep = dl->Flags;
    dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
    put(f.outline, colourOf(st.plate));
    const ImU32 ink = colourOf(st.ink);
    for (const streetsign::Poly& p : f.ink) put(p.pts, ink);
    dl->Flags = keep;
}

// One preset as a big click target: the preset's own name, lettered in it.
bool presetTile(int i, bool active, float w, float h) {
    const streetsign::Preset& pr = streetsign::presets()[static_cast<std::size_t>(i)];
    ImGui::PushID(i);
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##preset", ImVec2(w, h));
    const bool hot = ImGui::IsItemHovered();
    ImGui::SetItemTooltip("%s", pr.name);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, ImVec2(o.x + w, o.y + h), ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);
    if (active || hot)
        dl->AddRect(o, ImVec2(o.x + w, o.y + h),
                    ImGui::GetColorU32(active ? ImGuiCol_CheckMark : ImGuiCol_ButtonHovered), 4.0f,
                    0, active ? 3.0f : 1.5f);
    const float pad = 6.0f;
    drawFace(dl, streetsign::layout(pr.name, pr.style, 0.085f), pr.style,
             ImVec2(o.x + pad, o.y + pad), ImVec2(w - 2 * pad, h - 2 * pad));
    ImGui::PopID();
    return clicked;
}

// A measurement: stepped with - and +, or typed -- never dragged.
bool num(const char* label, float& v, float step, float lo, float hi, const char* fmt) {
    ImGui::PushID(label);
    const bool ch = ui::stepper("##v", v, step, lo, hi, fmt, ui::stepperWidth(fmt) + 90.0f);
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
    ImGui::PopID();
    return ch;
}

} // namespace

StreetSignTool::StreetSignTool(Deps d) : m_d(std::move(d)) {
    std::snprintf(m_bufA, sizeof m_bufA, "%s", m_cfg.textA.c_str());
}

int StreetSignTool::selectedSignId() const {
    if (!m_d.sel.valid()) return -1;
    const Entity* e = &m_d.document.entities()[static_cast<std::size_t>(m_d.sel.index())];
    for (int depth = 0; e && depth < 64; ++depth) {
        if (e->components.get<StreetSignComponent>()) return e->id;
        if (e->parent < 0) break;
        e = m_d.document.find(e->parent);
    }
    return -1;
}

void StreetSignTool::place() {
    const glm::vec3 at = m_d.spawnPoint(12.0f);
    const streetsign::Palette pal = streetsign::ensurePalette(m_d.document.materials(), m_cfg.style);
    std::vector<Entity> es = streetsign::entities(m_cfg, pal, m_d.entityCounter, at);
    if (es.empty()) return;
    es.front().name = "Sign " + m_cfg.textA;
    m_liveId = es.front().id;
    m_d.history.push(std::make_unique<AddEntitiesCmd>(std::move(es), "Street sign"), m_d.document);
    m_d.sel.select(m_liveId);
    m_d.status = "Placed a street sign.";
}

void StreetSignTool::rebuild() {
    m_pending = false;
    const int idx = m_d.document.indexOf(m_liveId);
    if (idx < 0) { m_liveId = -1; return; }
    const Entity old = m_d.document.entities()[static_cast<std::size_t>(idx)];
    // The subtree: the root and everything below it.
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
    const streetsign::Palette pal = streetsign::ensurePalette(m_d.document.materials(), m_cfg.style);
    std::vector<Entity> es = streetsign::entities(m_cfg, pal, m_d.entityCounter, old.localCenter);
    if (es.empty()) return;
    // Keep what the user did to the placed sign: where it hangs and how it turns.
    // A name the panel gave ("Sign <name>") follows the text; one the user typed stays.
    const auto* was = old.components.get<StreetSignComponent>();
    const bool autoName = was && old.name == "Sign " + was->params.textA;
    es.front().name          = autoName ? "Sign " + m_cfg.textA : old.name;
    es.front().parent        = old.parent;
    es.front().localRotation = old.localRotation;
    es.front().rotation      = old.rotation;
    es.front().center        = old.center;
    m_liveId = es.front().id;
    m_d.history.push(std::make_unique<ReplaceEntitiesCmd>(m_d.document, ids, std::move(es),
                                                          "Street sign"),
                     m_d.document);
    m_d.sel.select(m_liveId);
}

void StreetSignTool::editSelected() {
    const int id = selectedSignId();
    const Entity* e = id >= 0 ? m_d.document.find(id) : nullptr;
    if (!e) return;
    m_cfg    = e->components.get<StreetSignComponent>()->params;
    m_liveId = id;
    m_second = !m_cfg.textB.empty();
    std::snprintf(m_bufA, sizeof m_bufA, "%s", m_cfg.textA.c_str());
    std::snprintf(m_bufB, sizeof m_bufB, "%s", m_cfg.textB.c_str());
    m_d.sel.select(id);
    m_d.status = "Editing " + e->name + ".";
}

void StreetSignTool::panel(bool& show) {
    ImGui::SetNextWindowSize(ImVec2(460, 720), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Street signs", &show)) { ImGui::End(); return; }
    if (m_liveId >= 0 && m_d.document.indexOf(m_liveId) < 0) m_liveId = -1;
    bool changed = false;
    const float avail = ImGui::GetContentRegionAvail().x;

    // --- The preview ------------------------------------------------------------
    {
        const Face a = streetsign::layout(m_cfg.textA, m_cfg.style, m_cfg.letterHeight);
        const bool two = m_cfg.mount == streetsign::Mount::Post && m_second;
        const Face b = two ? streetsign::layout(m_cfg.textB, m_cfg.style, m_cfg.letterHeight) : Face{};
        const float wMax = std::max(a.width, b.width);
        const float sc = std::min((avail - 16.0f) / std::max(wMax, 0.01f), 110.0f / std::max(a.height, 0.01f));
        const float hA = a.height * sc, hB = two ? b.height * sc : 0.0f;
        const float gap = two ? 8.0f : 0.0f;
        const ImVec2 o = ImGui::GetCursorScreenPos();
        const ImVec2 size(avail, hA + hB + gap + 16.0f);
        ImGui::Dummy(size);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(o, ImVec2(o.x + size.x, o.y + size.y), IM_COL32(112, 128, 140, 255), 4.0f);
        drawFace(dl, a, m_cfg.style, ImVec2(o.x + 8, o.y + 8), ImVec2(avail - 16, hA));
        if (two)
            drawFace(dl, b, m_cfg.style, ImVec2(o.x + 8, o.y + 8 + hA + gap), ImVec2(avail - 16, hB));
        ImGui::TextDisabled("%.2f m x %.2f m, capitals %.0f mm", a.width, a.height,
                            m_cfg.letterHeight * 1000.0f);
    }

    // --- The name ---------------------------------------------------------------
    ui::sectionText("Name");
    auto nameRow = [&](const char* id, char* buf, std::size_t cap, std::string& text, int which) {
        ImGui::PushID(id);
        ImGui::SetNextItemWidth(avail - 110.0f);
        if (ImGui::InputText("##name", buf, cap)) { text = buf; changed = true; }
        ImGui::SameLine();
        if (ImGui::Button("Frankfurt...", ImVec2(100.0f, 0.0f))) {
            m_pickFor = which;
            ImGui::OpenPopup("##pick");
        }
        ImGui::SetItemTooltip("Pick one of Frankfurt am Main's %zu street names.",
                              streetsign::frankfurtNames().size());
        if (ImGui::BeginPopup("##pick")) {
            ui::searchBox("##search", m_search, sizeof m_search, "Search streets...");
            std::vector<int> hits;
            const auto& names = streetsign::frankfurtNames();
            for (int i = 0; i < static_cast<int>(names.size()); ++i)
                if (m_search[0] == '\0' || ui::icontains(names[static_cast<std::size_t>(i)].c_str(), m_search))
                    hits.push_back(i);
            ImGui::BeginChild("##list", ImVec2(360.0f, 320.0f), ImGuiChildFlags_Borders);
            ImGuiListClipper clip;
            clip.Begin(static_cast<int>(hits.size()), ImGui::GetTextLineHeightWithSpacing() + 6.0f);
            while (clip.Step())
                for (int k = clip.DisplayStart; k < clip.DisplayEnd; ++k) {
                    const std::string& n = names[static_cast<std::size_t>(hits[static_cast<std::size_t>(k)])];
                    if (ImGui::Selectable(n.c_str(), false, 0, ImVec2(0.0f, ImGui::GetTextLineHeight() + 6.0f))) {
                        std::snprintf(buf, cap, "%s", n.c_str());
                        text = buf;
                        changed = true;
                        ImGui::CloseCurrentPopup();
                    }
                }
            ImGui::EndChild();
            ImGui::EndPopup();
        }
        ImGui::PopID();
    };
    nameRow("a", m_bufA, sizeof m_bufA, m_cfg.textA, 0);
    if (m_cfg.mount == streetsign::Mount::Post) {
        if (ImGui::Checkbox("Second blade (a crossing)", &m_second)) {
            m_cfg.textB = m_second ? std::string(m_bufB) : std::string();
            changed = true;
        }
        if (m_second) {
            nameRow("b", m_bufB, sizeof m_bufB, m_cfg.textB, 1);
            if (m_cfg.textB.empty()) ImGui::TextDisabled("An empty second name means no second blade.");
        }
    }

    // --- The style --------------------------------------------------------------
    ui::sectionText("Style");
    {
        const int n = static_cast<int>(streetsign::presets().size());
        const float sp = ImGui::GetStyle().ItemSpacing.x;
        const float w = (avail - sp) * 0.5f, h = 52.0f;
        for (int i = 0; i < n; ++i) {
            if (i % 2) ImGui::SameLine();
            if (presetTile(i, i == m_cfg.preset, w, h)) {
                m_cfg.preset = i;
                m_cfg.style  = streetsign::presetStyle(i);
                changed = true;
            }
        }
    }
    if (ui::header("Customise the style")) {
        Style& st = m_cfg.style;
        changed |= ImGui::ColorEdit3("Plate", &st.plate.x, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        changed |= ImGui::ColorEdit3("Lettering", &st.ink.x, ImGuiColorEditFlags_NoInputs);
        int fr = static_cast<int>(st.frame);
        const char* frames[] = {streetsign::frameName(streetsign::Frame::None),
                                streetsign::frameName(streetsign::Frame::Line),
                                streetsign::frameName(streetsign::Frame::Rounded),
                                streetsign::frameName(streetsign::Frame::Notched)};
        for (int i = 0; i < static_cast<int>(streetsign::Frame::Count); ++i) {
            if (i) ImGui::SameLine();
            if (ImGui::RadioButton(frames[i], &fr, i)) {
                st.frame = static_cast<streetsign::Frame>(fr);
                changed = true;
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("frame");
        changed |= ImGui::Checkbox("Dots either side", &st.dots);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Capitals", &st.capitals);
        changed |= num("Letter width", st.condense, 0.05f, 0.6f, 1.4f, "%.2f");
        changed |= num("Plate corners", st.corner, 0.05f, 0.0f, 1.0f, "%.2f");
    }

    // --- Size and mount ----------------------------------------------------------
    if (ui::header("Size and mount", ImGuiTreeNodeFlags_DefaultOpen)) {
        int mount = static_cast<int>(m_cfg.mount);
        for (int i = 0; i < static_cast<int>(streetsign::Mount::Count); ++i) {
            if (i) ImGui::SameLine();
            if (ImGui::RadioButton(streetsign::mountName(static_cast<streetsign::Mount>(i)), &mount, i)) {
                m_cfg.mount = static_cast<streetsign::Mount>(mount);
                changed = true;
            }
        }
        float mm = m_cfg.letterHeight * 1000.0f;
        if (num("Capital height", mm, 5.0f, 20.0f, 400.0f, "%.0f mm")) {
            m_cfg.letterHeight = mm / 1000.0f;
            changed = true;
        }
        if (m_cfg.mount == streetsign::Mount::Post) {
            changed |= num("Post height", m_cfg.postHeight, 0.1f, 0.5f, 6.0f, "%.1f m");
            if (m_second) changed |= num("Second blade turn", m_cfg.angleB, 15.0f, -180.0f, 180.0f, "%.0f deg");
        }
    }

    // --- Place / rebuild ---------------------------------------------------------
    ImGui::Separator();
    const bool live = m_liveId >= 0;
    if (changed) m_pending = true;
    if (ImGui::Button("Place new sign", ImVec2(avail * 0.5f - 4.0f, 34.0f))) place();
    ImGui::SetItemTooltip("At the 3D cursor if it is shown, else in front of the camera.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!live);
    if (ImGui::Button("Update placed sign", ImVec2(-1.0f, 34.0f))) rebuild();
    ImGui::EndDisabled();
    ImGui::Checkbox("Update the placed sign on every change", &m_auto);
    const int selSign = selectedSignId();
    ImGui::BeginDisabled(selSign < 0 || selSign == m_liveId);
    if (ImGui::Button("Edit selected sign", ImVec2(-1.0f, 30.0f))) editSelected();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Load the selected sign's name and style into this panel.");
    if (live) {
        const Entity* e = m_d.document.find(m_liveId);
        ImGui::TextDisabled("Editing: %s", e ? e->name.c_str() : "?");
    }
    // Rebuild once the edit settles (typing a name is one undo step, not one per key).
    if (m_pending && m_auto && live && !ImGui::IsAnyItemActive()) rebuild();
    if (!live) m_pending = false;
    ImGui::End();
}

} // namespace signui
