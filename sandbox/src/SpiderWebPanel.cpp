#include "SpiderWebPanel.hpp"

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

namespace webui {

using spiderweb::Params;
using spiderweb::Shape;

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

// Another shape: a corner holds fewer radii than a whole web, so a count that
// suits the one is carried over to one that suits the other.
void reshape(Params& c, Shape s) {
    const bool wasCorner = c.shape == Shape::Corner;
    c.shape = s;
    if (s == Shape::Corner && !wasCorner && c.spokes > 10) c.spokes = 7;
    if (s != Shape::Corner && wasCorner && c.spokes < 12) c.spokes = 20;
}

} // namespace

SpiderWebTool::SpiderWebTool(Deps d) : m_d(std::move(d)) {}

void SpiderWebTool::setShape(Shape s) { reshape(m_cfg, s); }

int SpiderWebTool::selectedId() const {
    if (!m_d.sel.valid()) return -1;
    const Entity* e = &m_d.document.entities()[static_cast<std::size_t>(m_d.sel.index())];
    for (int depth = 0; e && depth < 64; ++depth) {
        if (e->components.get<SpiderWebComponent>()) return e->id;
        if (e->parent < 0) break;
        e = m_d.document.find(e->parent);
    }
    return -1;
}

void SpiderWebTool::placeAt(const glm::vec3& at, int parentId) {
    const fitzel::AssetId mat = spiderweb::ensureMaterial(m_d.document.materials(), m_cfg);
    std::vector<Entity> es = spiderweb::entities(m_cfg, mat, m_d.entityCounter, at);
    if (es.empty()) {
        m_d.status = "That spider web could not be built.";
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
    m_d.history.push(std::make_unique<AddEntitiesCmd>(std::move(es), "Spider web"), m_d.document);
    m_d.sel.select(m_liveId);
    m_d.status = "Placed " + name + ".";
}

void SpiderWebTool::place() { placeAt(m_d.spawnPoint(0.6f)); }

void SpiderWebTool::updateWith(const Params& p) {
    m_cfg = p;
    rebuild();
}

void SpiderWebTool::rebuild() {
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
    const fitzel::AssetId mat = spiderweb::ensureMaterial(m_d.document.materials(), m_cfg);
    std::vector<Entity> es = spiderweb::entities(m_cfg, mat, m_d.entityCounter, glm::vec3(0.0f));
    if (es.empty()) return;
    const float lift = es.front().half.y - old.half.y;
    // A name the panel gave follows the settings; one typed stays.
    const auto* was = old.components.get<SpiderWebComponent>();
    if (!(was && old.name == spiderweb::autoName(was->params))) es.front().name = old.name;
    es.front().parent        = old.parent;
    es.front().localRotation = old.localRotation;
    es.front().rotation      = old.rotation;
    es.front().localCenter   = old.localCenter + upL * lift;
    es.front().center        = old.center + upW * lift;
    m_liveId = es.front().id;
    m_d.history.push(std::make_unique<ReplaceEntitiesCmd>(m_d.document, ids, std::move(es), "Spider web"),
                     m_d.document);
    m_d.sel.select(m_liveId);
}

void SpiderWebTool::editSelected() {
    const int id = selectedId();
    const Entity* e = id >= 0 ? m_d.document.find(id) : nullptr;
    if (!e) return;
    m_cfg    = e->components.get<SpiderWebComponent>()->params;
    m_liveId = id;
    m_d.sel.select(id);
    m_d.status = "Editing " + e->name + ".";
}

void SpiderWebTool::refresh() {
    const Params p = spiderweb::sane(m_cfg);
    if (m_haveModel && p == m_shown) return;
    m_shown = p;
    m_haveModel = true;
    m_model = spiderweb::build(p);
}

void SpiderWebTool::panel(bool& show) {
    ImGui::SetNextWindowSize(ImVec2(440, 860), ImGuiCond_FirstUseEver);
    if (m_raise) {
        ImGui::SetNextWindowFocus();
        m_raise = false;
    }
    if (!ImGui::Begin("Spider web", &show)) { ImGui::End(); return; }
    if (m_liveId >= 0 && m_d.document.indexOf(m_liveId) < 0) m_liveId = -1;
    bool changed = false;
    Params& c = m_cfg;
    const float avail = ImGui::GetContentRegionAvail().x;

    // --- The shape ---------------------------------------------------------------------
    {
        ImGui::PushID("shape");
        const int n = static_cast<int>(Shape::Count);
        const float w = (avail - ImGui::GetStyle().ItemSpacing.x * (n - 1)) / n;
        for (int i = 0; i < n; ++i) {
            if (i) ImGui::SameLine();
            const bool on = static_cast<int>(c.shape) == i;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(spiderweb::shapeName(static_cast<Shape>(i)), ImVec2(w, 28.0f)) && !on) {
                reshape(c, static_cast<Shape>(i));
                changed = true;
            }
            if (on) ImGui::PopStyleColor();
        }
        ImGui::PopID();
        ui::hint(c.shape == Shape::Round   ? "An orb web in a hoop, a loop at the top to hang it by."
                 : c.shape == Shape::Frame ? "An orb web on a frame of straight threads, anchored from its corners."
                                           : "A quarter web between two walls, for the corner of a window or a room.");
    }

    // --- The drawing -------------------------------------------------------------------
    refresh();
    const spiderweb::Model& m = m_model;
    {
        const float h = 300.0f;
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(avail, h));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(o, ImVec2(o.x + avail, o.y + h), IM_COL32(38, 42, 50, 255), 4.0f);
        if (m.size.x > 0.0f && m.size.y > 0.0f) {
            const float pad = 12.0f;
            const float s = std::min((avail - 2.0f * pad) / m.size.x, (h - 2.0f * pad) / m.size.y);
            const ImVec2 mid(o.x + 0.5f * avail, o.y + 0.5f * h);
            const glm::vec3 col = c.color.r + c.color.g + c.color.b < 0.6f ? glm::vec3(0.55f, 0.6f, 0.68f) : c.color;
            const ImU32 ink = colourOf(col);
            std::vector<ImVec2> pts;
            for (const spiderweb::Thread& t : m.threadsDrawn) {
                pts.clear();
                for (const glm::vec2& q : t.pts) pts.push_back(ImVec2(mid.x + q.x * s, mid.y - q.y * s));
                dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), ink, ImDrawFlags_None,
                                std::max(1.0f, t.width * s));
            }
            for (const spiderweb::Disc& d : m.discs)
                dl->AddCircleFilled(ImVec2(mid.x + d.centre.x * s, mid.y - d.centre.y * s), d.radius * s, ink);
        }
        if (!m.ok) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "Not built cleanly: %s", m.error.c_str());
        ui::hint("%d radii, %d turns, %d holes; %.1f x %.1f mm, %.2g mm thick", m.spokes, m.turns, m.holes, m.size.x,
                 m.size.y, m.size.z);
        ImGui::Text("%.1f g of PLA, %.2f m of filament", m.grams, m.metres);
        if (std::max(m.size.x, m.size.y) > 256.0f)
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Larger than a 256 mm bed.");
        if (!m.printHint.empty()) ui::hint("%s", m.printHint.c_str());
    }

    // --- Settings ------------------------------------------------------------------------
    const Params used = spiderweb::sane(c);
    if (ui::header("Web", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= mm("Size", c.size, 5.0f, 40.0f, 250.0f, "%.0f mm");
        ImGui::SetItemTooltip(c.shape == Shape::Round   ? "Across the hoop, outside to outside (the loop on top comes on top)."
                              : c.shape == Shape::Frame ? "Across the frame's corners; the anchor threads reach further."
                                                        : "Along each of the two walls from the corner.");
        changed |= count("Radii", c.spokes, c.shape == Shape::Corner ? 2 : 5, c.shape == Shape::Corner ? 12 : 48);
        ImGui::SetItemTooltip("The threads running out from the middle (in a corner: between the walls).\n"
                              "A garden spider spins 20 to 30.");
        if (used.spokes < c.spokes)
            ui::hint("%d used: more would crowd the hub (or the corner) into a blob.", used.spokes);
        changed |= mm("Spiral spacing", c.spacing, 0.5f, 2.0f, 30.0f);
        ImGui::SetItemTooltip(c.shape == Shape::Corner ? "From one thread across to the next."
                                                       : "From one turn of the capture spiral to the next.");
        if (used.spacing > c.spacing + 0.01f)
            ui::hint("%.1f mm used: closer, the droop and the unevenness would close the holes.", used.spacing);
        if (c.shape != Shape::Corner) {
            changed |= count("Hub turns", c.hubTurns, 0, 6);
            ImGui::SetItemTooltip("The tight turns round the hub's disc, as strong as the radii.");
            changed |= mm("Free zone", c.freeZone, 1.0f, 0.0f, 40.0f);
            ImGui::SetItemTooltip("The open ring between the hub and the capture spiral.");
        }
        changed |= mm("Droop", c.sag, 0.01f, 0.0f, 0.3f, "%.2f");
        ImGui::SetItemTooltip("How far a thread across sags towards the middle between two radii,\n"
                              "as a share of its length: the scallops a web is drawn with.");
        if (used.sag < c.sag - 0.001f) ui::hint("%.2f used: more would close the holes between two turns.", used.sag);
        changed |= mm("Uneven", c.irregular, 0.05f, 0.0f, 1.0f, "%.2f");
        ImGui::SetItemTooltip("0: drawn with a ruler. 1: radii, turns and frame as uneven as spun.");
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
            ImGui::SetItemTooltip("Which uneven web, which threads torn: the same seed, the same web.");
        }
        changed |= mm("Torn", c.torn, 0.05f, 0.0f, 0.5f, "%.2f");
        ImGui::SetItemTooltip("The share of the threads across that are missing: an old web for Halloween.");
    }
    if (ui::header("Threads", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= mm(c.shape == Shape::Corner ? "Across" : "Spiral", c.spiralWidth, 0.1f, 0.8f, 4.0f);
        ImGui::SetItemTooltip("How wide the capture threads are, seen from above.");
        changed |= mm("Radii##w", c.spokeWidth, 0.1f, 0.8f, 5.0f);
        ImGui::SetItemTooltip("The radii and the hub's turns.");
        changed |= mm(c.shape == Shape::Round ? "Hoop" : c.shape == Shape::Frame ? "Frame" : "Walls", c.frameWidth,
                      0.2f, 0.8f, 8.0f);
        ImGui::SetItemTooltip("What holds the web: stiffer the wider.");
        changed |= mm("Thickness", c.thickness, 0.2f, 0.4f, 5.0f);
        ImGui::SetItemTooltip("The plate's height: every thread as high as this. Whole layers print best\n"
                              "(1.2 mm: six of 0.2 mm). Thin enough and it bends to a window.");
        ui::hint("Every thread at least 0.8 mm: two lines of a 0.4 mm nozzle.");
    }
    if (c.shape != Shape::Corner && ui::header(c.shape == Shape::Round ? "Hanging" : "Frame", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (c.shape == Shape::Round) {
            changed |= ImGui::Checkbox("A loop at the top", &c.eyelet);
            ImGui::SetItemTooltip("To hang it by a thread from a hook or a window's latch.");
        } else {
            changed |= count("Corners", c.corners, 3, 8);
            ImGui::SetItemTooltip("The frame's straight threads between them, each corner held by an anchor thread.");
            if (used.corners < c.corners) ui::hint("%d used: no more corners than radii.", used.corners);
            changed |= mm("Anchors", c.anchor, 1.0f, 3.0f, 40.0f, "%.0f mm");
            ImGui::SetItemTooltip("How far the anchor threads run out from the frame's corners.");
            for (int i = 0; i < static_cast<int>(spiderweb::Ends::Count); ++i) {
                if (i) ImGui::SameLine();
                int v = static_cast<int>(c.ends);
                if (ImGui::RadioButton(spiderweb::endsName(static_cast<spiderweb::Ends>(i)), &v, i)) {
                    c.ends = static_cast<spiderweb::Ends>(v);
                    changed = true;
                }
            }
            ImGui::SetItemTooltip("Pads: round discs to tape or pin up. Loops: to hang it by threads or\n"
                                  "over nails. Free: the anchor threads just end.");
        }
    }
    if (ui::header("Colour")) {
        changed |= ImGui::ColorEdit3("Web", &c.color.x, ImGuiColorEditFlags_NoInputs);
        ui::hint("White or glow-in-the-dark on a dark window, black on a light wall.");
    }

    // --- Place / rebuild ---------------------------------------------------------------------
    ImGui::Separator();
    const Entity* liveE = m_liveId >= 0 ? m_d.document.find(m_liveId) : nullptr;
    const bool live = liveE && liveE->components.get<SpiderWebComponent>();
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
    ImGui::SetItemTooltip("Load the selected spider web's settings into this panel.");
    if (live) ImGui::TextDisabled("Editing: %s", liveE->name.c_str());
    // Rebuild once the edit settles (a value typed is one undo step, not one per key).
    if (m_pending && m_auto && live && !ImGui::IsAnyItemActive()) rebuild();
    if (!live) m_pending = false;
    ImGui::End();
}

} // namespace webui
