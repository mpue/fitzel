#include "CardPanel.hpp"

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
#include "ImageDoc.hpp"   // img::systemFonts
#include "SceneGraph.hpp"
#include "Selection.hpp"
#include "UiStyle.hpp"

namespace cardui {

namespace {

ImU32 colourOf(const glm::vec3& c, float a = 1.0f) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, a));
}

const ImVec4 kWarn(1.0f, 0.75f, 0.3f, 1.0f);

// A measurement: stepped with - and +, or typed -- never dragged.
bool num(const char* label, float& v, float step, float lo, float hi, const char* fmt) {
    ImGui::PushID(label);
    const bool ch = ui::stepper("##v", v, step, lo, hi, fmt, ui::stepperWidth(fmt) + 90.0f);
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
    ImGui::PopID();
    return ch;
}

// Nine squares, one per anchor; the offset beside them, typed or stepped, and
// a button that takes it back to the anchor.
bool anchorPicker(const char* id, card::Place& at) {
    ImGui::PushID(id);
    bool ch = false;
    const float b = std::round(ImGui::GetFrameHeight() * 0.75f), gap = 3.0f;
    ImGui::BeginGroup();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int a = 0; a < 9; ++a) {
        if (a % 3) ImGui::SameLine(0.0f, gap);
        ImGui::PushID(a);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##a", ImVec2(b, b))) {
            at = {a, 0.0f, 0.0f};
            ch = true;
        }
        const bool hot = ImGui::IsItemHovered(), on = at.anchor == a;
        dl->AddRectFilled(p, ImVec2(p.x + b, p.y + b),
                          ImGui::GetColorU32(on ? ImGuiCol_CheckMark : hot ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg),
                          3.0f);
        static const char* names[9] = {"top left", "top", "top right", "left", "middle", "right",
                                       "bottom left", "bottom", "bottom right"};
        ImGui::SetItemTooltip("Put it %s.", names[a]);
        ImGui::PopID();
    }
    ImGui::EndGroup();
    ImGui::SameLine(0.0f, 12.0f);
    ImGui::BeginGroup();
    ch |= ui::stepper("##dx", at.dx, 0.5f, -500.0f, 500.0f, "%+.1f mm", 130.0f);
    ImGui::SameLine();
    ImGui::TextDisabled("right");
    ch |= ui::stepper("##dy", at.dy, 0.5f, -500.0f, 500.0f, "%+.1f mm", 130.0f);
    ImGui::SameLine();
    ImGui::TextDisabled("down");
    ImGui::BeginDisabled(at.dx == 0.0f && at.dy == 0.0f);
    if (ImGui::SmallButton("Back to the anchor")) {
        at.dx = at.dy = 0.0f;
        ch = true;
    }
    ImGui::EndDisabled();
    ImGui::EndGroup();
    ImGui::PopID();
    return ch;
}

std::string fontLabel(const std::string& path) {
    if (path.empty()) {
        const std::string d = lettering::defaultFontPath();
        return d.empty() ? "Default (none found)" : "Default (" + std::filesystem::path(d).stem().string() + ")";
    }
    return std::filesystem::path(path).stem().string();
}

} // namespace

CardTool::CardTool(Deps d) : m_d(std::move(d)) { setParams(m_cfg); }

void CardTool::setParams(const card::Params& p) {
    m_cfg = p;
    std::snprintf(m_text, sizeof m_text, "%s", m_cfg.text.c_str());
    std::snprintf(m_qrText, sizeof m_qrText, "%s", m_cfg.qrText.c_str());
}

int CardTool::selectedCardId() const {
    if (!m_d.sel.valid()) return -1;
    const Entity* e = &m_d.document.entities()[static_cast<std::size_t>(m_d.sel.index())];
    for (int depth = 0; e && depth < 64; ++depth) {
        if (e->components.get<CardComponent>()) return e->id;
        if (e->parent < 0) break;
        e = m_d.document.find(e->parent);
    }
    return -1;
}

void CardTool::placeAt(const glm::vec3& at, int parentId) {
    const card::Palette pal = card::ensurePalette(m_d.document.materials(), m_cfg);
    std::vector<Entity> es = card::entities(m_cfg, pal, m_d.entityCounter, at);
    if (es.empty()) return;
    if (const Entity* parent = parentId >= 0 ? m_d.document.find(parentId) : nullptr) {
        Entity& root = es.front();
        root.parent = parent->id;
        const glm::mat4 pw = scenegraph::worldOf(*parent);
        scenegraph::setWorld(root, root.center, root.rotation, &pw);
    }
    m_liveId = es.front().id;
    m_d.history.push(std::make_unique<AddEntitiesCmd>(std::move(es), "Card"), m_d.document);
    m_d.sel.select(m_liveId);
    char b[96];
    std::snprintf(b, sizeof b, "Placed a card, %.0f x %.0f mm.", m_cfg.width, m_cfg.height);
    m_d.status = b;
}

void CardTool::place() { placeAt(m_d.spawnPoint(0.6f)); }

void CardTool::rebuild() {
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
    const card::Palette pal = card::ensurePalette(m_d.document.materials(), m_cfg);
    std::vector<Entity> es = card::entities(m_cfg, pal, m_d.entityCounter, old.localCenter);
    if (es.empty()) return;
    // Keep where it hangs and how it turns; a name the panel gave follows the text.
    const auto* was = old.components.get<CardComponent>();
    const bool autoName = was && old.name == card::autoName(was->params);
    if (!autoName) es.front().name = old.name;
    es.front().parent        = old.parent;
    es.front().localRotation = old.localRotation;
    es.front().rotation      = old.rotation;
    es.front().center        = old.center;
    m_liveId = es.front().id;
    m_d.history.push(std::make_unique<ReplaceEntitiesCmd>(m_d.document, ids, std::move(es), "Card"),
                     m_d.document);
    m_d.sel.select(m_liveId);
}

void CardTool::editSelected() {
    const int id = selectedCardId();
    const Entity* e = id >= 0 ? m_d.document.find(id) : nullptr;
    if (!e) return;
    setParams(e->components.get<CardComponent>()->params);
    m_liveId = id;
    m_d.sel.select(id);
    m_d.status = "Editing " + e->name + ".";
}

// The card drawn to scale. The text and the code can be taken with the mouse
// and moved; let go near an anchor and they sit on it. Returns true when
// something moved.
bool CardTool::preview(const card::Params& p, const card::Layout& L, float width) {
    const float maxH = 300.0f, pad = 10.0f;
    const float k = std::min((width - 2.0f * pad) / p.width, (maxH - 2.0f * pad) / p.height);   // px a mm
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const ImVec2 size(width, p.height * k + 2.0f * pad);
    ImGui::InvisibleButton("##card", size);
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    const ImVec2 c(o.x + 0.5f * width, o.y + 0.5f * size.y);
    auto px = [&](glm::dvec2 mm) {
        return ImVec2(c.x + static_cast<float>(mm.x) * k, c.y - static_cast<float>(mm.y) * k);
    };
    auto rectOf = [&](const card::Box& b) { return std::make_pair(px({b.lo.x, b.hi.y}), px({b.hi.x, b.lo.y})); };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bg = IM_COL32(104, 116, 128, 255);
    dl->AddRectFilled(o, ImVec2(o.x + size.x, o.y + size.y), bg, 4.0f);

    // The card, its shadow, the hole, the frame.
    {
        const auto [a, b] = rectOf(L.card);
        dl->AddRectFilled(ImVec2(a.x + 3, a.y + 4), ImVec2(b.x + 3, b.y + 4), IM_COL32(0, 0, 0, 60), p.corner * k);
        dl->AddRectFilled(a, b, colourOf(p.cardColor), p.corner * k);
    }
    if (L.holeRadius > 0.0) dl->AddCircleFilled(px(L.holeAt), static_cast<float>(L.holeRadius) * k, bg, 48);
    const ImU32 ink = colourOf(p.inkColor);
    if (p.frame) {
        const double mid = p.frameInset + 0.5 * p.frameWidth;
        const card::Box fb{L.card.lo + glm::dvec2(mid), L.card.hi - glm::dvec2(mid)};
        const auto [a, b] = rectOf(fb);
        dl->AddRect(a, b, ink, std::max(0.0f, static_cast<float>(p.corner - mid)) * k, 0,
                    std::max(1.0f, p.frameWidth * k));
    }
    // The code, row by row in runs.
    if (L.code.valid()) {
        const float m = static_cast<float>(L.qrPitch) * k;
        const ImVec2 q = px({L.qrBox.lo.x, L.qrBox.hi.y});
        for (int y = 0; y < L.code.size; ++y)
            for (int x = 0; x < L.code.size;) {
                if (!L.code.at(x, y)) { ++x; continue; }
                int e = x;
                while (e < L.code.size && L.code.at(e, y)) ++e;
                dl->AddRectFilled(ImVec2(q.x + m * x, q.y + m * y), ImVec2(q.x + m * e, q.y + m * (y + 1)), ink);
                x = e;
            }
    }
    // The text: its own triangles. Anti-aliased edges would draw a hairline of
    // card colour along every seam between them.
    {
        const ImDrawListFlags keep = dl->Flags;
        dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
        const glm::dvec2 origin(L.textBox.lo.x, L.textBox.hi.y);
        for (const lettering::Placed& g : L.text.glyphs) {
            auto to = [&](glm::dvec2 u) { return px(origin + g.at + g.scale * u); };
            const polyfill::Fill& f = g.shape->fill;
            for (const glm::ivec3& t : f.tris)
                dl->AddTriangleFilled(to(f.points[static_cast<std::size_t>(t.x)]), to(f.points[static_cast<std::size_t>(t.y)]),
                                      to(f.points[static_cast<std::size_t>(t.z)]), ink);
            for (const polyfill::Trap& t : g.shape->traps)
                dl->AddQuadFilled(to({t.xl0, t.y0}), to({t.xr0, t.y0}), to({t.xr1, t.y1}), to({t.xl1, t.y1}), ink);
        }
        dl->Flags = keep;
    }

    // Taking and moving.
    const ImGuiIO& io = ImGui::GetIO();
    const glm::dvec2 mouse((io.MousePos.x - c.x) / k, (c.y - io.MousePos.y) / k);
    auto under = [&](const card::Box& b) {
        return mouse.x >= b.lo.x && mouse.x <= b.hi.x && mouse.y >= b.lo.y && mouse.y <= b.hi.y;
    };
    const int hot = L.code.valid() && under(L.qrBox) ? 2 : !L.text.glyphs.empty() && under(L.textBox) ? 1 : 0;
    if (ImGui::IsItemActivated()) {
        m_drag = hot;
        m_dragLo = hot == 2 ? L.qrBox.lo : L.textBox.lo;
    }
    if (!active) m_drag = 0;
    bool moved = false;
    if (m_drag && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
        m_dragLo += glm::dvec2(io.MouseDelta.x / k, -io.MouseDelta.y / k);
        const card::Box& b = m_drag == 2 ? L.qrBox : L.textBox;
        const double snap = std::max(1.5, 6.0 / k);
        (m_drag == 2 ? m_cfg.qrAt : m_cfg.textAt) = card::placeAt(L.area, b.size(), m_dragLo, snap);
        moved = true;
    }
    const int shown = m_drag ? m_drag : hovered ? hot : 0;
    if (shown) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        const auto [a, b] = rectOf(shown == 2 ? L.qrBox : L.textBox);
        dl->AddRect(ImVec2(a.x - 3, a.y - 3), ImVec2(b.x + 3, b.y + 3), ImGui::GetColorU32(ImGuiCol_CheckMark), 3.0f, 0,
                    1.5f);
        if (!m_drag) ImGui::SetTooltip("Drag to move it -- it snaps to the nine anchors.");
    }
    // While dragging: the area inside the margin and its anchors, the one in use lit.
    if (m_drag) {
        const auto [a, b] = rectOf(L.area);
        dl->AddRect(a, b, IM_COL32(60, 140, 255, 140), 0.0f, 0, 1.0f);
        const card::Place& at = m_drag == 2 ? m_cfg.qrAt : m_cfg.textAt;
        for (int i = 0; i < 9; ++i) {
            const glm::dvec2 q(i % 3 == 0 ? L.area.lo.x : i % 3 == 1 ? L.area.centre().x : L.area.hi.x,
                               i / 3 == 0 ? L.area.hi.y : i / 3 == 1 ? L.area.centre().y : L.area.lo.y);
            const bool on = i == at.anchor && at.dx == 0.0f && at.dy == 0.0f;
            dl->AddCircleFilled(px(q), on ? 5.0f : 3.0f, on ? IM_COL32(60, 140, 255, 255) : IM_COL32(60, 140, 255, 150));
        }
    }
    return moved;
}

bool CardTool::fontCombo(float width) {
    bool changed = false;
    ImGui::SetNextItemWidth(width);
    bool open = false;
    if (ImGui::BeginCombo("##font", fontLabel(m_cfg.font).c_str(), ImGuiComboFlags_HeightLarge)) {
        open = true;
        if (ImGui::IsWindowAppearing()) {
            m_fontSearch[0] = '\0';
            ImGui::SetKeyboardFocusHere();
        }
        ui::searchBox("##search", m_fontSearch, sizeof m_fontSearch, "Search fonts...");
        std::vector<const img::FontFile*> hits;
        for (const img::FontFile& f : img::systemFonts())
            if (ui::icontains(f.name.c_str(), m_fontSearch)) hits.push_back(&f);
        std::string hover;
        if (m_fontSearch[0] == '\0') {
            if (ImGui::Selectable(fontLabel("").c_str(), m_cfg.font.empty())) {
                m_cfg.font.clear();
                changed = true;
            }
        }
        ImGui::BeginChild("##fonts", ImVec2(0.0f, 300.0f));
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(hits.size()));
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                const img::FontFile& f = *hits[static_cast<std::size_t>(i)];
                if (ImGui::Selectable(f.name.c_str(), f.path == m_cfg.font)) {
                    m_cfg.font = f.path;
                    changed = true;
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::IsItemHovered()) hover = f.path;
            }
        ImGui::EndChild();
        ImGui::EndCombo();
        m_hoverFont = hover;
    }
    if (!open) m_hoverFont.clear();
    ImGui::SetItemTooltip("Any font the system has. Resting on one in the list shows it on the card.");
    return changed;
}

void CardTool::panel(bool& show) {
    ImGui::SetNextWindowSize(ImVec2(480, 860), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Cards", &show)) { ImGui::End(); return; }
    if (m_liveId >= 0 && m_d.document.indexOf(m_liveId) < 0) m_liveId = -1;
    bool changed = false;
    const float avail = ImGui::GetContentRegionAvail().x;

    // --- The card as it will print -------------------------------------------------
    card::Params shown = card::sane(m_cfg);
    if (!m_hoverFont.empty()) shown.font = m_hoverFont;
    const card::Layout L = card::layoutOf(shown);
    changed |= preview(shown, L, avail);
    {
        std::string info;
        char b[160];
        std::snprintf(b, sizeof b, "%.1f x %.1f mm, %.1f mm thick", shown.width, shown.height,
                      shown.thickness + shown.relief);
        info = b;
        if (!L.text.glyphs.empty()) {
            std::snprintf(b, sizeof b, " -- text %.1f mm%s", shown.capHeight * L.textScale,
                          L.textScale < 0.999 ? " (shrunk to fit)" : "");
            info += b;
        }
        if (L.code.valid()) {
            std::snprintf(b, sizeof b, " -- code version %d, modules %.2f mm", L.code.version, L.qrPitch);
            info += b;
        }
        ImGui::PushTextWrapPos(0.0f);
        ui::hint("%s", info.c_str());
        for (const std::string& w : L.warnings) ImGui::TextColored(kWarn, "%s", w.c_str());
        ImGui::PopTextWrapPos();
    }

    // --- The card -------------------------------------------------------------------
    if (ui::header("Card", ImGuiTreeNodeFlags_DefaultOpen)) {
        const float sp = ImGui::GetStyle().ItemSpacing.x;
        float x = 0.0f;
        for (const card::Preset& pr : card::presets()) {
            const float w = ImGui::CalcTextSize(pr.name).x + 2.0f * ImGui::GetStyle().FramePadding.x;
            if (x > 0.0f && x + w <= avail) ImGui::SameLine();
            else x = 0.0f;
            x += w + sp;
            const bool on = std::abs(m_cfg.width - pr.width) < 0.01f && std::abs(m_cfg.height - pr.height) < 0.01f;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetColorU32(ImGuiCol_ButtonActive));
            if (ImGui::Button(pr.name)) {
                m_cfg.width  = pr.width;
                m_cfg.height = pr.height;
                m_cfg.corner = pr.corner;
                changed = true;
            }
            if (on) ImGui::PopStyleColor();
            ImGui::SetItemTooltip("%.1f x %.1f mm", pr.width, pr.height);
        }
        if (ImGui::Button("Turn it")) {
            std::swap(m_cfg.width, m_cfg.height);
            changed = true;
        }
        ImGui::SetItemTooltip("Landscape to portrait and back.");
        changed |= num("Width", m_cfg.width, 1.0f, 10.0f, 2000.0f, "%.1f mm");
        changed |= num("Height", m_cfg.height, 1.0f, 10.0f, 2000.0f, "%.1f mm");
        changed |= num("Corners", m_cfg.corner, 0.5f, 0.0f, 100.0f, "%.1f mm");
        changed |= num("Margin", m_cfg.margin, 0.5f, 0.0f, 100.0f, "%.1f mm");
        ImGui::SetItemTooltip("How far the text and the code keep from the edge (or the frame).");
        changed |= num("Plate thickness", m_cfg.thickness, 0.2f, 0.2f, 50.0f, "%.1f mm");
        changed |= num("Raised by", m_cfg.relief, 0.1f, 0.1f, 20.0f, "%.1f mm");
        ImGui::SetItemTooltip("How far the text, the code and the frame stand up from the card.");
        ui::hint("For a two-colour print change the filament at %.1f mm.", card::sane(m_cfg).thickness);
    }

    // --- The text -------------------------------------------------------------------
    if (ui::header("Text", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::InputTextMultiline("##text", m_text, sizeof m_text,
                                      ImVec2(-1.0f, ImGui::GetTextLineHeight() * 5.5f))) {
            m_cfg.text = m_text;
            changed = true;
        }
        changed |= fontCombo(avail);
        changed |= num("Size", m_cfg.capHeight, 0.25f, 0.5f, 300.0f, "%.2f mm");
        ImGui::SetItemTooltip("The height of a capital letter.");
        changed |= ImGui::Checkbox("First line larger", &m_cfg.headline);
        if (m_cfg.headline) {
            ImGui::SameLine();
            ImGui::PushID("hl");
            changed |= ui::stepper("##s", m_cfg.headlineScale, 0.1f, 1.0f, 4.0f, "x %.1f", 150.0f);
            ImGui::PopID();
        }
        int al = static_cast<int>(m_cfg.align);
        for (int i = 0; i < static_cast<int>(lettering::Align::Count); ++i) {
            if (i) ImGui::SameLine();
            if (ImGui::RadioButton(lettering::alignName(static_cast<lettering::Align>(i)), &al, i)) {
                m_cfg.align = static_cast<lettering::Align>(al);
                changed = true;
            }
        }
        changed |= num("Line spacing", m_cfg.spacing, 0.05f, 0.6f, 3.0f, "x %.2f");
        changed |= ImGui::Checkbox("Shrink to fit", &m_cfg.fit);
        ImGui::SetItemTooltip("Make the text smaller until it keeps clear of the code, the hole and the margin.");
        ui::sectionText("Where");
        changed |= anchorPicker("textAt", m_cfg.textAt);
    }

    // --- The QR code ----------------------------------------------------------------
    ImGui::PushID("qr");
    if (ui::header("QR code", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= ImGui::Checkbox("A QR code on the card", &m_cfg.qr);
        if (m_cfg.qr) {
            ImGui::SetNextItemWidth(avail - 130.0f);
            if (ImGui::InputText("##qr", m_qrText, sizeof m_qrText)) {
                m_cfg.qrText = m_qrText;
                changed = true;
            }
            ImGui::SetItemTooltip("A link, a phone number, any text.");
            ImGui::SameLine();
            if (ImGui::Button("Wi-Fi...", ImVec2(-1.0f, 0.0f))) ImGui::OpenPopup("##wifi");
            ImGui::SetItemTooltip("A Wi-Fi login phones offer to join -- the guests' card.");
            if (std::string t; qrui::wifiPopup("##wifi", m_wifi, t)) {
                std::snprintf(m_qrText, sizeof m_qrText, "%s", t.c_str());
                m_cfg.qrText = m_qrText;
                changed = true;
            }
            int e = static_cast<int>(m_cfg.ecc);
            for (int i = 0; i < static_cast<int>(qrencode::Ecc::Count); ++i) {
                if (i) ImGui::SameLine();
                if (ImGui::RadioButton(qrencode::eccName(static_cast<qrencode::Ecc>(i)), &e, i)) {
                    m_cfg.ecc = static_cast<qrencode::Ecc>(e);
                    changed = true;
                }
            }
            changed |= num("Size", m_cfg.qrSize, 1.0f, 5.0f, 2000.0f, "%.0f mm");
            ui::sectionText("Where");
            changed |= anchorPicker("qrAt", m_cfg.qrAt);
        }
    }
    ImGui::PopID();

    // --- Frame and hole ---------------------------------------------------------------
    if (ui::header("Frame and hole")) {
        changed |= ImGui::Checkbox("Frame", &m_cfg.frame);
        ImGui::SetItemTooltip("A raised rim along the edge, its corners following the card's.");
        if (m_cfg.frame) {
            ImGui::Indent();
            changed |= num("Frame width", m_cfg.frameWidth, 0.2f, 0.2f, 50.0f, "%.1f mm");
            changed |= num("From the edge", m_cfg.frameInset, 0.5f, 0.0f, 50.0f, "%.1f mm");
            ImGui::Unindent();
        }
        changed |= ImGui::Checkbox("Hole for a key ring", &m_cfg.hole);
        if (m_cfg.hole) {
            ImGui::Indent();
            changed |= num("Diameter", m_cfg.holeDiameter, 0.5f, 1.0f, 100.0f, "%.1f mm");
            static const char* corners[4] = {"Top left", "Top right", "Bottom left", "Bottom right"};
            for (int i = 0; i < 4; ++i) {
                if (i % 2) ImGui::SameLine();
                changed |= ImGui::RadioButton(corners[i], &m_cfg.holeCorner, i);
            }
            ImGui::Unindent();
        }
    }

    // --- Colours ----------------------------------------------------------------------
    if (ui::header("Colours")) {
        changed |= ImGui::ColorEdit3("Card", &m_cfg.cardColor.x, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        changed |= ImGui::ColorEdit3("Print", &m_cfg.inkColor.x, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        if (ImGui::SmallButton("Swap")) {
            std::swap(m_cfg.cardColor, m_cfg.inkColor);
            changed = true;
        }
        ui::hint("The 3MF export hands each colour its own filament.");
    }

    // --- Place / rebuild ----------------------------------------------------------------
    ImGui::Separator();
    const bool live = m_liveId >= 0;
    if (changed) m_pending = true;
    if (ImGui::Button("Place new card", ImVec2(avail * 0.5f - 4.0f, 34.0f))) place();
    ImGui::SetItemTooltip("At the 3D cursor if it is shown, else in front of the camera.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!live);
    if (ImGui::Button("Update placed card", ImVec2(-1.0f, 34.0f))) rebuild();
    ImGui::EndDisabled();
    ImGui::Checkbox("Update the placed card on every change", &m_auto);
    const int selCard = selectedCardId();
    ImGui::BeginDisabled(selCard < 0 || selCard == m_liveId);
    if (ImGui::Button("Edit selected card", ImVec2(-1.0f, 30.0f))) editSelected();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Load the selected card into this panel.");
    if (live) {
        const Entity* e = m_d.document.find(m_liveId);
        ImGui::TextDisabled("Editing: %s", e ? e->name.c_str() : "?");
    }
    // Rebuild once the edit settles (a word typed, a drag let go: one undo step).
    if (m_pending && m_auto && live && !ImGui::IsAnyItemActive()) rebuild();
    if (!live) m_pending = false;
    ImGui::End();
}

} // namespace cardui
