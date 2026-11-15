#include "WinterStencilPanel.hpp"

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

namespace stencilui {

using winterstencil::Frame;
using winterstencil::Params;

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

} // namespace

WinterStencilTool::WinterStencilTool(Deps d) : m_d(std::move(d)) {}

int WinterStencilTool::selectedId() const {
    if (!m_d.sel.valid()) return -1;
    const Entity* e = &m_d.document.entities()[static_cast<std::size_t>(m_d.sel.index())];
    for (int depth = 0; e && depth < 64; ++depth) {
        if (e->components.get<WinterStencilComponent>()) return e->id;
        if (e->parent < 0) break;
        e = m_d.document.find(e->parent);
    }
    return -1;
}

void WinterStencilTool::placeAt(const glm::vec3& at, int parentId) {
    const fitzel::AssetId mat = winterstencil::ensureMaterial(m_d.document.materials(), m_cfg);
    std::vector<Entity> es = winterstencil::entities(m_cfg, mat, m_d.entityCounter, at);
    if (es.empty()) {
        m_d.status = "That winter stencil could not be built.";
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
    m_d.history.push(std::make_unique<AddEntitiesCmd>(std::move(es), "Winter stencil"), m_d.document);
    m_d.sel.select(m_liveId);
    m_d.status = "Placed " + name + ".";
}

void WinterStencilTool::place() { placeAt(m_d.spawnPoint(0.6f)); }

void WinterStencilTool::updateWith(const Params& p) {
    m_cfg = p;
    rebuild();
}

void WinterStencilTool::rebuild() {
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
    // Where the placed one's bottom lies, along its own up: the new one lies
    // there too, however much thicker or thinner it is.
    const glm::vec3 upL(scenegraph::compose(glm::vec3(0.0f), old.localRotation, glm::vec3(1.0f)) *
                        glm::vec4(0.0f, 1.0f, 0.0f, 0.0f));
    const glm::vec3 upW(scenegraph::worldOf(old) * glm::vec4(0.0f, 1.0f, 0.0f, 0.0f));
    const fitzel::AssetId mat = winterstencil::ensureMaterial(m_d.document.materials(), m_cfg);
    std::vector<Entity> es = winterstencil::entities(m_cfg, mat, m_d.entityCounter, glm::vec3(0.0f));
    if (es.empty()) return;
    const float lift = es.front().half.y - old.half.y;
    // A name the panel gave follows the settings; one typed stays.
    const auto* was = old.components.get<WinterStencilComponent>();
    if (!(was && old.name == winterstencil::autoName(was->params))) es.front().name = old.name;
    es.front().parent        = old.parent;
    es.front().localRotation = old.localRotation;
    es.front().rotation      = old.rotation;
    es.front().localCenter   = old.localCenter + upL * lift;
    es.front().center        = old.center + upW * lift;
    m_liveId = es.front().id;
    m_d.history.push(std::make_unique<ReplaceEntitiesCmd>(m_d.document, ids, std::move(es), "Winter stencil"),
                     m_d.document);
    m_d.sel.select(m_liveId);
}

void WinterStencilTool::editSelected() {
    const int id = selectedId();
    const Entity* e = id >= 0 ? m_d.document.find(id) : nullptr;
    if (!e) return;
    m_cfg    = e->components.get<WinterStencilComponent>()->params;
    m_liveId = id;
    m_d.sel.select(id);
    m_d.status = "Editing " + e->name + ".";
}

void WinterStencilTool::refresh() {
    const Params p = winterstencil::sane(m_cfg);
    if (m_haveModel && p == m_shown) return;
    m_shown = p;
    m_haveModel = true;
    m_model = winterstencil::build(p);
}

void WinterStencilTool::panel(bool& show) {
    ImGui::SetNextWindowSize(ImVec2(440, 860), ImGuiCond_FirstUseEver);
    if (m_raise) {
        ImGui::SetNextWindowFocus();
        m_raise = false;
    }
    if (!ImGui::Begin("Winter stencil", &show)) { ImGui::End(); return; }
    if (m_liveId >= 0 && m_d.document.indexOf(m_liveId) < 0) m_liveId = -1;
    bool changed = false;
    Params& c = m_cfg;
    const float avail = ImGui::GetContentRegionAvail().x;

    // --- The frame ---------------------------------------------------------------------
    {
        ImGui::PushID("frame");
        const int n = static_cast<int>(Frame::Count);
        const float w = (avail - ImGui::GetStyle().ItemSpacing.x * (n - 1)) / n;
        for (int i = 0; i < n; ++i) {
            if (i) ImGui::SameLine();
            const bool on = static_cast<int>(c.frame) == i;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(winterstencil::frameName(static_cast<Frame>(i)), ImVec2(w, 28.0f)) && !on) {
                c.frame = static_cast<Frame>(i);
                changed = true;
            }
            if (on) ImGui::PopStyleColor();
        }
        ImGui::PopID();
        ui::hint("A winter scene as a stencil for a window: the light behind it draws the picture.");
    }

    // --- The drawing: the plate dark against the light ----------------------------------
    refresh();
    const winterstencil::Model& m = m_model;
    {
        const float h = 330.0f;
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(avail, h));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilledMultiColor(o, ImVec2(o.x + avail, o.y + h), IM_COL32(250, 205, 120, 255), IM_COL32(250, 205, 120, 255),
                                    IM_COL32(220, 145, 70, 255), IM_COL32(220, 145, 70, 255));
        if (m.size.x > 0.0f && m.size.y > 0.0f) {
            const float pad = 10.0f;
            const float s = std::min((avail - 2.0f * pad) / m.size.x, (h - 2.0f * pad) / m.size.y);
            const ImVec2 mid(o.x + 0.5f * avail, o.y + 0.5f * h);
            const glm::vec3 col = c.color.r + c.color.g + c.color.b > 2.4f ? glm::vec3(0.11f, 0.12f, 0.15f) : c.color;
            const ImU32 ink = colourOf(col);
            // Triangles side by side: without the fill's soft edge, which would
            // draw every seam between them.
            const ImDrawListFlags was = dl->Flags;
            dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
            for (std::size_t k = 0; k + 2 < m.tris.size(); k += 3) {
                auto P = [&](const glm::vec2& q) { return ImVec2(mid.x + q.x * s, mid.y - q.y * s); };
                dl->AddTriangleFilled(P(m.tris[k]), P(m.tris[k + 1]), P(m.tris[k + 2]), ink);
            }
            dl->Flags = was;
        }
        if (!m.ok) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "Not built cleanly: %s", m.error.c_str());
        if (m.dropped > 0)
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%d piece(s) held on to nothing and were left out.", m.dropped);
        ui::hint("%.1f x %.1f mm, %.2g mm thick; %d openings", m.size.x, m.size.y, m.size.z, m.holes);
        ImGui::Text("%.1f g of PLA, %.2f m of filament", m.grams, m.metres);
        if (std::max(m.size.x, m.size.y) > 256.0f)
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Larger than a 256 mm bed.");
        if (!m.printHint.empty()) ui::hint("%s", m.printHint.c_str());
    }

    // --- Settings ------------------------------------------------------------------------
    const Params used = winterstencil::sane(c);
    if (ui::header("Frame", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= mm(c.frame == Frame::Round ? "Across" : "Width", c.width, 5.0f, 60.0f, 250.0f, "%.0f mm");
        if (c.frame != Frame::Round) {
            changed |= mm("Height", c.height, 5.0f, 50.0f, 250.0f, "%.0f mm");
            if (used.height > c.height + 0.01f) ui::hint("%.0f mm used: an arch needs its round on a body.", used.height);
        }
        changed |= mm("Border", c.border, 0.5f, 2.0f, 12.0f);
        ImGui::SetItemTooltip("The frame's band round the picture.");
        changed |= mm("Thickness", c.thickness, 0.2f, 0.6f, 5.0f);
        ImGui::SetItemTooltip("The plate's height: whole layers print best (1.6 mm: eight of 0.2 mm).");
        changed |= ImGui::Checkbox("A loop at the top", &c.eyelet);
        ImGui::SetItemTooltip("To hang it by a thread from a suction hook or the window's latch.");
    }
    if (ui::header("On the snow", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= mm("Hills", c.hills, 0.1f, 0.0f, 1.0f, "%.1f");
        ImGui::SetItemTooltip("0: the snow lies flat. 1: rolling hills.");
        changed |= count("Firs", c.trees, 0, 16);
        if (m.trees < used.trees) ui::hint("%d found room.", m.trees);
        changed |= ImGui::Checkbox("Cabin", &c.house);
        if (c.house) {
            ImGui::SameLine();
            changed |= ImGui::Checkbox("Smoke", &c.smoke);
        }
        changed |= ImGui::Checkbox("Snowman", &c.snowman);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Fence", &c.fence);
    }
    if (ui::header("In the sky", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= ImGui::Checkbox("Moon", &c.moon);
        changed |= count("Stars", c.stars, 0, 16);
        changed |= count("Snowflakes", c.flakes, 0, 12);
        if (m.stars < used.stars || m.flakes < used.flakes)
            ui::hint("%d stars and %d snowflakes found room: each hangs on its own thread, none above another.", m.stars,
                     m.flakes);
        else
            ui::hint("Each hangs from the frame on a thread: a stencil is one piece.");
    }
    if (ui::header("Look", ImGuiTreeNodeFlags_DefaultOpen)) {
        {
            ImGui::PushID("seed");
            float f = static_cast<float>(c.seed);
            if (ui::stepper("##v", f, 1.0f, 0.0f, 99999.0f, "%.0f", ui::stepperWidth("%.0f") + 90.0f)) {
                c.seed = static_cast<int>(std::lround(f));
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Another")) {
                c.seed = (c.seed * 1103515245 + 12345) & 0x7fff;
                changed = true;
            }
            ImGui::SameLine();
            ImGui::TextUnformatted("Seed");
            ImGui::PopID();
            ImGui::SetItemTooltip("How the hills run and where everything stands: the same seed, the same picture.");
        }
        changed |= mm("Lines", c.line, 0.1f, 0.8f, 3.0f);
        ImGui::SetItemTooltip("The threads, the window bars, the twigs and the fence's rails: the thinnest it draws.\n"
                              "Two lines of your nozzle at the least (0.8 mm for 0.4).");
        changed |= ImGui::ColorEdit3("Colour", &c.color.x, ImGuiColorEditFlags_NoInputs);
    }

    // --- Place / rebuild ---------------------------------------------------------------------
    ImGui::Separator();
    const Entity* liveE = m_liveId >= 0 ? m_d.document.find(m_liveId) : nullptr;
    const bool live = liveE && liveE->components.get<WinterStencilComponent>();
    if (changed) m_pending = true;
    if (ImGui::Button("Place new", ImVec2(avail * 0.5f - 4.0f, 34.0f))) place();
    ImGui::SetItemTooltip("At the 3D cursor if it is shown, else in front of the camera.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!live);
    if (ImGui::Button("Update placed", ImVec2(-1.0f, 34.0f))) rebuild();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("The placed one rebuilt with these settings, lying where it lies.");
    ImGui::Checkbox("Update the placed one on every change", &m_auto);
    const int selId = selectedId();
    ImGui::BeginDisabled(selId < 0 || selId == m_liveId);
    if (ImGui::Button("Edit selected", ImVec2(-1.0f, 30.0f))) editSelected();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Load the selected winter stencil's settings into this panel.");
    if (live) ImGui::TextDisabled("Editing: %s", liveE->name.c_str());
    // Rebuild once the edit settles (a value typed is one undo step, not one per key).
    if (m_pending && m_auto && live && !ImGui::IsAnyItemActive()) rebuild();
    if (!live) m_pending = false;
    ImGui::End();
}

} // namespace stencilui
