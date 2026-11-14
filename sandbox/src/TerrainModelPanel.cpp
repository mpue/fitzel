#include "TerrainModelPanel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

#include <imgui.h>

#include "Command.hpp"
#include "Document.hpp"
#include "FolderDialog.hpp"
#include "SceneGraph.hpp"
#include "Selection.hpp"
#include "UiStyle.hpp"

namespace terrainui {

using terrainmodel::Params;
using terrainmodel::Source;
using terrainmodel::Style;

namespace {

ImU32 colourOf(const glm::vec3& c, float k = 1.0f) {
    const glm::vec3 v = glm::clamp(c * k, glm::vec3(0.0f), glm::vec3(1.0f));
    return ImGui::ColorConvertFloat4ToU32(ImVec4(v.r, v.g, v.b, 1.0f));
}

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

bool num(const char* label, float& v, float step, float lo, float hi, const char* fmt) {
    ImGui::PushID(label);
    const bool ch = ui::stepper("##v", v, step, lo, hi, fmt, ui::stepperWidth(fmt) + 90.0f);
    ImGui::SameLine();
    ImGui::TextUnformatted(label, std::strstr(label, "##"));
    ImGui::PopID();
    return ch;
}

} // namespace

TerrainModelTool::TerrainModelTool(Deps d) : m_d(std::move(d)) {}

terrainmodel::Heights TerrainModelTool::heights() const {
    if (!m_d.hasTerrain || !m_d.hasTerrain() || !m_d.ground) return nullptr;
    return m_d.ground;
}

int TerrainModelTool::selectedId() const {
    if (!m_d.sel.valid()) return -1;
    const Entity* e = &m_d.document.entities()[static_cast<std::size_t>(m_d.sel.index())];
    for (int depth = 0; e && depth < 64; ++depth) {
        if (e->components.get<TerrainModelComponent>()) return e->id;
        if (e->parent < 0) break;
        e = m_d.document.find(e->parent);
    }
    return -1;
}

void TerrainModelTool::aimAtView() {
    glm::vec3 c;
    if (!(m_d.cursor && m_d.cursor(c))) c = m_d.spawnPoint(400.0f);
    m_cfg.centreX = std::round(c.x);
    m_cfg.centreZ = std::round(c.z);
    if (m_d.waterLevel) {
        const float w = m_d.waterLevel();
        m_cfg.water = w > -500.0f;
        if (m_cfg.water) m_cfg.waterLevel = w;
    }
}

void TerrainModelTool::placeAt(const glm::vec3& at, int parentId) {
    refresh(true);
    std::vector<Entity> es =
        terrainmodel::entities(m_cfg, m_model, m_d.document.materials(), m_d.entityCounter, at);
    if (es.empty()) {
        m_d.status = "No terrain model: " + (m_model.error.empty() ? std::string("nothing to build") : m_model.error) + ".";
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
    m_raise = true;
    m_d.history.push(std::make_unique<AddEntitiesCmd>(std::move(es), "Terrain model"), m_d.document);
    m_d.sel.select(m_liveId);
    m_d.status = "Placed " + name + ".";
}

void TerrainModelTool::place() { placeAt(m_d.spawnPoint(0.6f)); }

void TerrainModelTool::setParams(const Params& p) {
    m_cfg = p;
    std::snprintf(m_image, sizeof m_image, "%s", m_cfg.image.c_str());
}

void TerrainModelTool::updateWith(const Params& p) {
    setParams(p);
    rebuild();
}

void TerrainModelTool::rebuild() {
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
    refresh(true);
    const glm::vec3 upL(scenegraph::compose(glm::vec3(0.0f), old.localRotation, glm::vec3(1.0f)) *
                        glm::vec4(0.0f, 1.0f, 0.0f, 0.0f));
    const glm::vec3 upW(scenegraph::worldOf(old) * glm::vec4(0.0f, 1.0f, 0.0f, 0.0f));
    std::vector<Entity> es =
        terrainmodel::entities(m_cfg, m_model, m_d.document.materials(), m_d.entityCounter, glm::vec3(0.0f));
    if (es.empty()) return;
    const float lift = es.front().half.y - old.half.y;
    const auto* was = old.components.get<TerrainModelComponent>();
    if (!(was && old.name == terrainmodel::autoName(was->params))) es.front().name = old.name;
    es.front().parent        = old.parent;
    es.front().localRotation = old.localRotation;
    es.front().rotation      = old.rotation;
    es.front().localCenter   = old.localCenter + upL * lift;
    es.front().center        = old.center + upW * lift;
    m_liveId = es.front().id;
    m_d.history.push(std::make_unique<ReplaceEntitiesCmd>(m_d.document, ids, std::move(es), "Terrain model"),
                     m_d.document);
    m_d.sel.select(m_liveId);
}

void TerrainModelTool::editSelected() {
    const int id = selectedId();
    const Entity* e = id >= 0 ? m_d.document.find(id) : nullptr;
    if (!e) return;
    setParams(e->components.get<TerrainModelComponent>()->params);
    m_liveId = id;
    m_d.sel.select(id);
    m_d.status = "Editing " + e->name + ".";
}

void TerrainModelTool::refresh(bool force) {
    const Params p = terrainmodel::sane(m_cfg);
    if (!force && m_haveModel && p == m_shown) return;
    m_shown = p;
    m_haveModel = true;
    m_model = terrainmodel::build(p, heights());
}

void TerrainModelTool::panel(bool& show) {
    ImGui::SetNextWindowSize(ImVec2(440, 820), ImGuiCond_FirstUseEver);
    if (m_raise) {
        ImGui::SetNextWindowFocus();
        m_raise = false;
    }
    if (!ImGui::Begin("Terrain model", &show)) { ImGui::End(); return; }
    if (m_liveId >= 0 && m_d.document.indexOf(m_liveId) < 0) m_liveId = -1;
    bool changed = false;
    Params& c = m_cfg;
    const float avail = ImGui::GetContentRegionAvail().x;

    // --- Where from ------------------------------------------------------------------
    {
        ImGui::PushID("source");
        const float w = 0.5f * (avail - ImGui::GetStyle().ItemSpacing.x);
        for (int i = 0; i < 2; ++i) {
            if (i) ImGui::SameLine();
            const bool on = static_cast<int>(c.source) == i;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(i == 0 ? "The scene's terrain" : "A heightmap image", ImVec2(w, 28.0f)) && !on) {
                c.source = static_cast<Source>(i);
                changed = true;
            }
            if (on) ImGui::PopStyleColor();
        }
        ImGui::PopID();
    }

    // --- The map ---------------------------------------------------------------------
    refresh();
    const terrainmodel::Model& m = m_model;
    const Params p = m_shown;
    {
        const float h = 220.0f;
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(avail, h));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(o, ImVec2(o.x + avail, o.y + h), IM_COL32(38, 42, 50, 255), 4.0f);
        if (m.nx > 1 && m.ny > 1) {
            // At most ~90 cells across; each the colour its height gets on the
            // print (terraces: its slab's), lit from the north-west.
            const int step = std::max(1, (std::max(m.nx, m.ny) + 89) / 90);
            const int cx = (m.nx - 1) / step + 1, cy = (m.ny - 1) / step + 1;
            const float cell = std::min((avail - 16.0f) / cx, (h - 16.0f) / cy);
            const ImVec2 at(o.x + 0.5f * (avail - cell * cx), o.y + 0.5f * (h - cell * cy));
            auto H = [&](int i, int j) {
                i = std::clamp(i, 0, m.nx - 1);
                j = std::clamp(j, 0, m.ny - 1);
                return m.heights[static_cast<std::size_t>(j * m.nx + i)];
            };
            const bool lake = p.water && p.waterLevel > m.lo;
            const float landLo = lake ? p.waterLevel : m.lo;
            const float spacing = p.printWidth / std::max(m.mmPerMetre, 1e-6f) / static_cast<float>(m.nx - 1) *
                                  static_cast<float>(step);   // between two cells drawn (m)
            for (int b = 0; b < cy; ++b)
                for (int a = 0; a < cx; ++a) {
                    const int i = a * step, j = b * step;
                    const float hh = H(i, j);
                    glm::vec3 col;
                    if (lake && hh <= p.waterLevel) {
                        col = p.waterColor;
                    } else if (p.style == Style::Terraces && !m.levels.empty()) {
                        std::size_t k = 0;
                        for (std::size_t l = 0; l < m.levels.size(); ++l)
                            if (hh >= m.levels[l]) k = l;
                        col = m.levelColors[k];
                    } else {
                        col = terrainmodel::bandColor(p, hh, landLo, m.hi, false);
                    }
                    // Lit from the north-west: ground rising away from it faces the light.
                    const float slope = ((H(i + step, j - step) - H(i - step, j + step)) * p.exaggeration) /
                                        (2.0f * spacing * 1.41421f);
                    const float shade = std::clamp(0.9f + 0.45f * slope, 0.6f, 1.15f);
                    // Row 0 is the front (south) edge: drawn at the bottom.
                    const ImVec2 q(at.x + cell * a, at.y + cell * (cy - 1 - b));
                    dl->AddRectFilled(q, ImVec2(q.x + cell + 0.5f, q.y + cell + 0.5f), colourOf(col, shade));
                }
            dl->AddText(ImVec2(o.x + 6.0f, o.y + 4.0f), IM_COL32(220, 225, 235, 220), "N");
        } else {
            const char* why = m.error.empty() ? "nothing to show" : m.error.c_str();
            const ImVec2 ts = ImGui::CalcTextSize(why);
            dl->AddText(ImVec2(o.x + 0.5f * (avail - ts.x), o.y + 0.5f * (h - ts.y)), IM_COL32(220, 160, 120, 255), why);
        }
        if (m.ok && !m.parts.empty()) {
            const glm::vec2 reg = terrainmodel::regionOf(p);
            ui::hint("%.0f x %.0f m of ground, %.0f to %.0f m high: 1 : %.0f across, heights x %.1f", reg.x, reg.y,
                     m.lo, m.hi, 1000.0f / std::max(m.mmPerMetre, 1e-6f), p.exaggeration);
            ui::hint("%.1f x %.1f x %.1f mm, %.1f cm3 of plastic", m.size.x, m.size.y, m.size.z, m.volume / 1000.0);
            if (p.style == Style::Terraces) {
                std::vector<glm::vec3> seen;
                for (const auto& part : m.parts)
                    if (std::find(seen.begin(), seen.end(), part.color) == seen.end()) seen.push_back(part.color);
                ui::hint("%zu slabs, a part each, in %zu colours: one filament a colour.", m.parts.size(), seen.size());
            } else if (!m.changes.empty()) {
                std::string s = "For colour, change filament at";
                char b[96];
                for (const auto& [z, what] : m.changes) {
                    std::snprintf(b, sizeof b, " %.1f mm (%s),", z, what.c_str());
                    s += b;
                }
                s.back() = '.';
                ui::hint("%s", s.c_str());
            }
        }
        if (std::max(m.size.x, m.size.y) > 256.0f)
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Larger than a 256 mm bed.");
    }

    // --- The ground --------------------------------------------------------------------
    if (ui::header("Ground", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (c.source == Source::Scene) {
            if (!heights()) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "This scene has no terrain yet.");
            changed |= num("East", c.centreX, 10.0f, -1.0e6f, 1.0e6f, "%.0f m");
            ImGui::SetItemTooltip("The middle of the square, world x.");
            changed |= num("South", c.centreZ, 10.0f, -1.0e6f, 1.0e6f, "%.0f m");
            ImGui::SetItemTooltip("The middle of the square, world z (the scene's +z is south).");
            if (ImGui::Button("Round the 3D cursor or where the camera looks")) {
                aimAtView();
                changed = true;
            }
            changed |= num("Width", c.width, 50.0f, 10.0f, 100000.0f, "%.0f m");
            changed |= num("Depth", c.depth, 50.0f, 10.0f, 100000.0f, "%.0f m");
            ImGui::SetItemTooltip("North to south.");
            if (ImGui::SmallButton("Square")) {
                c.depth = c.width;
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Read the terrain again")) refresh(true);
            ImGui::SetItemTooltip("After the world generator ran or the ground was sculpted.");
        } else {
            ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Browse...").x - 24.0f);
            if (ImGui::InputText("##image", m_image, sizeof m_image)) {
                c.image = m_image;
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Browse...")) {
                std::string path;
                const std::string dir = c.image.empty() ? std::string() : std::filesystem::path(c.image).parent_path().string();
                if (ed::pickFile(path, dir, "Heightmaps", "*.png;*.jpg;*.jpeg;*.tga;*.bmp")) {
                    c.image = path;
                    std::snprintf(m_image, sizeof m_image, "%s", path.c_str());
                    changed = true;
                }
            }
            ui::hint("Grey, dark low and light high; 16-bit PNGs keep the fine steps (real elevation tiles).");
            changed |= num("Covers", c.imageWidth, 100.0f, 10.0f, 1.0e6f, "%.0f m across");
            changed |= num("Black to white", c.imageRelief, 10.0f, 1.0f, 10000.0f, "%.0f m");
        }
        changed |= ImGui::Checkbox("Water", &c.water);
        ImGui::SetItemTooltip("Ground below the water level becomes a still lake (the blue slab at the bottom\n"
                              "of a contour model).");
        if (c.water) {
            ImGui::Indent();
            changed |= num("Water level", c.waterLevel, 1.0f, -100000.0f, 100000.0f, "%.1f m");
            if (c.source == Source::Scene && m_d.waterLevel) {
                ImGui::SameLine();
                if (ImGui::SmallButton("The scene's")) {
                    c.waterLevel = m_d.waterLevel();
                    changed = true;
                }
            }
            ImGui::Unindent();
        }
    }

    // --- The model -----------------------------------------------------------------------
    if (ui::header("Model", ImGuiTreeNodeFlags_DefaultOpen)) {
        int st = static_cast<int>(c.style);
        if (ImGui::RadioButton("Relief", &st, 0)) changed = true;
        ImGui::SetItemTooltip("One piece, the ground as it is; colour it by filament changes at the heights given.");
        ImGui::SameLine();
        if (ImGui::RadioButton("Contour terraces", &st, 1)) changed = true;
        ImGui::SetItemTooltip("Stacked slabs cut along the contours, each its own part in its own colour.");
        c.style = static_cast<Style>(st);
        if (c.style == Style::Terraces) {
            ImGui::Indent();
            changed |= count("Slabs", c.bands, 2, 24);
            ImGui::Unindent();
        }
        changed |= num("Print width", c.printWidth, 5.0f, 20.0f, 400.0f, "%.0f mm");
        changed |= num("Heights", c.exaggeration, 0.1f, 0.1f, 20.0f, "x %.1f");
        ImGui::SetItemTooltip("Heights stretched by this; maps of hills read better at 1.5 to 3.");
        changed |= num("Base", c.base, 0.5f, 0.6f, 30.0f, "%.1f mm");
        ImGui::SetItemTooltip("Plastic under the lowest point (the lake's surface, with water).");
        changed |= count("Detail", c.resolution, 16, 400, "%.0f samples");
        ImGui::SetItemTooltip("Samples across; at 0.4 mm a sample the printer shows all there is.");
    }

    if (ui::header("Colours")) {
        if (c.style == Style::Relief) {
            changed |= ImGui::ColorEdit3("Relief", &c.reliefColor.x, ImGuiColorEditFlags_NoInputs);
        } else {
            changed |= ImGui::ColorEdit3("Water", &c.waterColor.x, ImGuiColorEditFlags_NoInputs);
            ImGui::SameLine();
            changed |= ImGui::ColorEdit3("Low", &c.lowColor.x, ImGuiColorEditFlags_NoInputs);
            ImGui::SameLine();
            changed |= ImGui::ColorEdit3("Hills", &c.midColor.x, ImGuiColorEditFlags_NoInputs);
            changed |= ImGui::ColorEdit3("Rock", &c.highColor.x, ImGuiColorEditFlags_NoInputs);
            ImGui::SameLine();
            changed |= ImGui::ColorEdit3("Snow", &c.peakColor.x, ImGuiColorEditFlags_NoInputs);
        }
        ui::hint("The 3MF export hands each colour its own filament.");
    }

    // --- Place / rebuild -------------------------------------------------------------------
    ImGui::Separator();
    const bool live = m_liveId >= 0 && m_d.document.find(m_liveId);
    if (changed) m_pending = true;
    ImGui::BeginDisabled(!m.ok || m.parts.empty());
    if (ImGui::Button("Place new", ImVec2(avail * 0.5f - 4.0f, 34.0f))) place();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("In front of the camera, north to the north.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!live);
    if (ImGui::Button("Update placed", ImVec2(-1.0f, 34.0f))) rebuild();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Built again with these settings from the ground as it is now.");
    ImGui::Checkbox("Update the placed one on every change", &m_auto);
    const int selId = selectedId();
    ImGui::BeginDisabled(selId < 0 || selId == m_liveId);
    if (ImGui::Button("Edit selected", ImVec2(-1.0f, 30.0f))) editSelected();
    ImGui::EndDisabled();
    if (live) ImGui::TextDisabled("Editing: %s", m_d.document.find(m_liveId)->name.c_str());
    if (m_pending && m_auto && live && !ImGui::IsAnyItemActive()) rebuild();
    if (!live) m_pending = false;
    ImGui::End();
}

} // namespace terrainui
