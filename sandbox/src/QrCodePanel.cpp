#include "QrCodePanel.hpp"

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

namespace qrui {

namespace {

ImU32 colourOf(const glm::vec3& c) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, 1.0f));
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

// The same in millimetres for a value kept in metres.
bool mm(const char* label, float& metres, float step, float lo, float hi, const char* fmt) {
    float v = metres * 1000.0f;
    if (!num(label, v, step, lo, hi, fmt)) return false;
    metres = v / 1000.0f;
    return true;
}

// The code drawn square into the box at `o`, `side` pixels: the plate (when
// there is one), the frame (when there is one), the dark modules as runs along
// each row. Whole pixels a module where they fit, so the preview is not blurred
// into uneven stripes.
void drawCode(ImDrawList* dl, const qrencode::Code& c, const qrcode::Params& p, const qrcode::Layout& l,
              ImVec2 o, float side) {
    float px = side * l.pitch / l.width;
    if (px >= 2.0f) px = std::floor(px);
    const float k = px / l.pitch;   // pixels a metre
    const float w = l.width * k;
    const ImVec2 a(std::round(o.x + 0.5f * (side - w)), std::round(o.y + 0.5f * (side - w)));
    const ImVec2 b(a.x + w, a.y + w);
    const ImU32 ink = colourOf(p.inkColor);
    if (l.plate > 0.0f) dl->AddRectFilled(a, b, colourOf(p.plateColor));
    if (l.frame > 0.0f) {
        const float f = std::max(1.0f, std::round(l.frame * k));
        dl->AddRectFilled(a, ImVec2(b.x, a.y + f), ink);
        dl->AddRectFilled(ImVec2(a.x, b.y - f), b, ink);
        dl->AddRectFilled(ImVec2(a.x, a.y + f), ImVec2(a.x + f, b.y - f), ink);
        dl->AddRectFilled(ImVec2(b.x - f, a.y + f), ImVec2(b.x, b.y - f), ink);
    }
    const ImVec2 q(std::round(a.x + l.inset * k), std::round(a.y + l.inset * k));
    for (int y = 0; y < c.size; ++y)
        for (int x = 0; x < c.size;) {
            if (!c.at(x, y)) { ++x; continue; }
            int e = x;
            while (e < c.size && c.at(e, y)) ++e;
            dl->AddRectFilled(ImVec2(q.x + px * static_cast<float>(x), q.y + px * static_cast<float>(y)),
                              ImVec2(q.x + px * static_cast<float>(e), q.y + px * static_cast<float>(y + 1)), ink);
            x = e;
        }
}

} // namespace

QrCodeTool::QrCodeTool(Deps d) : m_d(std::move(d)) {
    std::snprintf(m_text, sizeof m_text, "%s", m_cfg.text.c_str());
}

int QrCodeTool::selectedCodeId() const {
    if (!m_d.sel.valid()) return -1;
    const Entity* e = &m_d.document.entities()[static_cast<std::size_t>(m_d.sel.index())];
    for (int depth = 0; e && depth < 64; ++depth) {
        if (e->components.get<QrCodeComponent>()) return e->id;
        if (e->parent < 0) break;
        e = m_d.document.find(e->parent);
    }
    return -1;
}

void QrCodeTool::placeAt(const glm::vec3& at, int parentId) {
    const qrcode::Palette pal = qrcode::ensurePalette(m_d.document.materials(), m_cfg);
    std::vector<Entity> es = qrcode::entities(m_cfg, pal, m_d.entityCounter, at);
    if (es.empty()) {
        m_d.status = "The text is too long for a QR code.";
        return;
    }
    if (const Entity* parent = parentId >= 0 ? m_d.document.find(parentId) : nullptr) {
        Entity& root = es.front();
        root.parent = parent->id;
        const glm::mat4 pw = scenegraph::worldOf(*parent);
        scenegraph::setWorld(root, root.center, root.rotation, &pw);
    }
    m_liveId = es.front().id;
    m_d.history.push(std::make_unique<AddEntitiesCmd>(std::move(es), "QR code"), m_d.document);
    m_d.sel.select(m_liveId);
    char b[96];
    std::snprintf(b, sizeof b, "Placed a QR code, %.0f mm wide.", m_cfg.size * 1000.0f);
    m_d.status = b;
}

void QrCodeTool::place() { placeAt(m_d.spawnPoint(1.5f)); }

void QrCodeTool::setParams(const qrcode::Params& p) {
    m_cfg = p;
    std::snprintf(m_text, sizeof m_text, "%s", m_cfg.text.c_str());
}

void QrCodeTool::rebuild() {
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
    const qrcode::Palette pal = qrcode::ensurePalette(m_d.document.materials(), m_cfg);
    std::vector<Entity> es = qrcode::entities(m_cfg, pal, m_d.entityCounter, old.localCenter);
    if (es.empty()) return;   // too long: the placed code stays as it was
    // Keep what the user did to the placed code: where it hangs, how it turns,
    // how it is scaled. A name the panel gave follows the text; one typed stays.
    const auto* was = old.components.get<QrCodeComponent>();
    const bool autoName = was && old.name == qrcode::autoName(was->params);
    if (!autoName) es.front().name = old.name;
    es.front().parent        = old.parent;
    es.front().localRotation = old.localRotation;
    es.front().rotation      = old.rotation;
    es.front().center        = old.center;
    m_liveId = es.front().id;
    m_d.history.push(std::make_unique<ReplaceEntitiesCmd>(m_d.document, ids, std::move(es), "QR code"),
                     m_d.document);
    m_d.sel.select(m_liveId);
}

void QrCodeTool::editSelected() {
    const int id = selectedCodeId();
    const Entity* e = id >= 0 ? m_d.document.find(id) : nullptr;
    if (!e) return;
    m_cfg    = e->components.get<QrCodeComponent>()->params;
    m_liveId = id;
    std::snprintf(m_text, sizeof m_text, "%s", m_cfg.text.c_str());
    m_d.sel.select(id);
    m_d.status = "Editing " + e->name + ".";
}

// A Wi-Fi network as phones read it from a code: WIFI:T:WPA;S:name;P:pass;;
// with \ ; , : " escaped by a backslash.
std::string wifiText(const char* ssid, const char* pass, int security, bool hidden) {
    auto esc = [](const char* s) {
        std::string o;
        for (; *s; ++s) {
            if (std::strchr("\\;,:\"", *s)) o += '\\';
            o += *s;
        }
        return o;
    };
    static const char* kinds[] = {"WPA", "WEP", "nopass"};
    std::string t = std::string("WIFI:T:") + kinds[std::clamp(security, 0, 2)] + ";S:" + esc(ssid) + ";";
    if (security != 2) t += "P:" + esc(pass) + ";";
    if (hidden) t += "H:true;";
    return t + ";";
}

bool wifiPopup(const char* id, WifiForm& w, std::string& text) {
    if (!ImGui::BeginPopup(id)) return false;
    bool used = false;
    ui::sectionText("Wi-Fi network");
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputText("Network name", w.ssid, sizeof w.ssid);
    ImGui::BeginDisabled(w.security == 2);
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputText("Password", w.pass, sizeof w.pass);
    ImGui::EndDisabled();
    ImGui::RadioButton("WPA / WPA2 / WPA3", &w.security, 0);
    ImGui::SameLine();
    ImGui::RadioButton("WEP", &w.security, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Open", &w.security, 2);
    ImGui::Checkbox("Hidden network", &w.hidden);
    ui::hint("A phone that scans it offers to join the network.");
    ImGui::BeginDisabled(w.ssid[0] == '\0');
    if (ImGui::Button("Use as the text", ImVec2(-1.0f, 0.0f))) {
        text = wifiText(w.ssid, w.pass, w.security, w.hidden);
        used = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::EndPopup();
    return used;
}

void QrCodeTool::panel(bool& show) {
    ImGui::SetNextWindowSize(ImVec2(420, 760), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("QR codes", &show)) { ImGui::End(); return; }
    if (m_liveId >= 0 && m_d.document.indexOf(m_liveId) < 0) m_liveId = -1;
    bool changed = false;
    const float avail = ImGui::GetContentRegionAvail().x;
    const qrcode::Params p = qrcode::sane(m_cfg);
    const qrencode::Code code = qrencode::encode(p.text, p.ecc);

    // --- The preview -------------------------------------------------------------
    {
        const float side = std::min(avail, 260.0f);
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(avail, side));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(o, ImVec2(o.x + avail, o.y + side), IM_COL32(112, 128, 140, 255), 4.0f);
        const qrcode::Layout l = qrcode::layoutOf(p, code.size);
        if (code.valid())
            drawCode(dl, code, p, l, ImVec2(o.x + 0.5f * (avail - side) + 8.0f, o.y + 8.0f), side - 16.0f);
        if (code.valid()) {
            const float pitch = l.pitch * 1000.0f;
            ui::hint("Version %d: %d x %d modules of %.2f mm (code %.1f mm), %s, %d of %d codewords",
                     code.version, code.size, code.size, pitch, l.code * 1000.0f, qrencode::modeName(code.mode),
                     code.dataBytes, code.capacity);
            const ImVec4 warn(1.0f, 0.75f, 0.3f, 1.0f);
            if (pitch < 1.0f)
                ImGui::TextColored(warn, "Modules under 1 mm are finer than a 0.4 mm nozzle prints cleanly.");
            if (p.frame && p.border < 2)
                ImGui::TextColored(warn, "Scanners need light round the code: keep a quiet zone of 2 or "
                                         "more inside the frame.");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "Too long: %zu bytes, %d fit at level %c.",
                               p.text.size(), qrencode::byteCapacity(40, p.ecc), qrencode::eccLetter(p.ecc));
        }
    }

    // --- The text ------------------------------------------------------------------
    ui::sectionText("Text");
    if (ImGui::InputTextMultiline("##text", m_text, sizeof m_text,
                                  ImVec2(-1.0f, ImGui::GetTextLineHeight() * 4.5f))) {
        m_cfg.text = m_text;
        changed = true;
    }
    ImGui::SetItemTooltip("A link, a phone number, any text -- read as UTF-8.");
    if (ImGui::Button("Wi-Fi network...")) ImGui::OpenPopup("##wifi");
    ImGui::SetItemTooltip("Make the text a Wi-Fi login phones offer to join.");
    if (std::string t; wifiPopup("##wifi", m_wifi, t)) {
        std::snprintf(m_text, sizeof m_text, "%s", t.c_str());
        m_cfg.text = m_text;
        m_pending = true;
    }
    ImGui::SameLine();
    ui::hint("%zu bytes", std::strlen(m_text));

    // --- Error correction ------------------------------------------------------------
    ui::sectionText("Error correction");
    {
        int e = static_cast<int>(m_cfg.ecc);
        for (int i = 0; i < static_cast<int>(qrencode::Ecc::Count); ++i) {
            if (i) ImGui::SameLine();
            if (ImGui::RadioButton(qrencode::eccName(static_cast<qrencode::Ecc>(i)), &e, i)) {
                m_cfg.ecc = static_cast<qrencode::Ecc>(e);
                changed = true;
            }
        }
        ui::hint("How much may be scratched off or covered and still scan. Higher: more modules.");
    }

    // --- Size and thickness ------------------------------------------------------------
    if (ui::header("Size and thickness", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= mm("Width", m_cfg.size, 5.0f, 5.0f, 5000.0f, "%.0f mm");
        ImGui::SetItemTooltip("The outer side: frame and quiet zone included.");
        float border = static_cast<float>(m_cfg.border);
        if (num("Quiet zone", border, 1.0f, 0.0f, 16.0f, "%.0f modules")) {
            m_cfg.border = static_cast<int>(std::lround(border));
            changed = true;
        }
        ImGui::SetItemTooltip("Light modules round the code. Scanners want 4; 2 often does.");
        changed |= mm("Code height", m_cfg.relief, 0.2f, 0.1f, 100.0f, "%.1f mm");
        ImGui::SetItemTooltip("How far the dark modules -- and the frame -- stand up.");

        changed |= ImGui::Checkbox("Background plate", &m_cfg.background);
        ImGui::SetItemTooltip("Off: no background, the modules alone -- for a wall that is its own\n"
                              "background, an inlay, or a second colour printed apart.");
        if (m_cfg.background) {
            ImGui::Indent();
            changed |= mm("Plate thickness", m_cfg.plate, 0.2f, 0.2f, 100.0f, "%.1f mm");
            ImGui::Unindent();
        }
        changed |= ImGui::Checkbox("Frame", &m_cfg.frame);
        ImGui::SetItemTooltip("A rim round the quiet zone, in the code's colour and height.");
        if (m_cfg.frame) {
            ImGui::Indent();
            changed |= mm("Frame width", m_cfg.frameWidth, 0.5f, 0.5f, 0.45f * m_cfg.size * 1000.0f, "%.1f mm");
            ImGui::Unindent();
        }
        if (p.background)
            ui::hint("%.1f mm thick in all. For a two-colour print change the filament at %.1f mm.",
                     (p.plate + p.relief) * 1000.0f, p.plate * 1000.0f);
        else
            ui::hint("%.1f mm thick, no plate: the modules are separate pieces%s.", p.relief * 1000.0f,
                     p.frame ? ", the frame one more" : "");
    }

    // --- Colours -------------------------------------------------------------------------
    if (ui::header("Colours")) {
        ImGui::BeginDisabled(!m_cfg.background);
        changed |= ImGui::ColorEdit3("Plate", &m_cfg.plateColor.x, ImGuiColorEditFlags_NoInputs);
        ImGui::EndDisabled();
        ImGui::SameLine();
        changed |= ImGui::ColorEdit3("Code", &m_cfg.inkColor.x, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        if (ImGui::SmallButton("Swap")) {
            std::swap(m_cfg.plateColor, m_cfg.inkColor);
            changed = true;
        }
        ui::hint("The 3MF export hands each colour its own filament.");
    }

    // --- Place / rebuild ---------------------------------------------------------------
    ImGui::Separator();
    const bool live = m_liveId >= 0;
    if (changed) m_pending = true;
    ImGui::BeginDisabled(!code.valid());
    if (ImGui::Button("Place new code", ImVec2(avail * 0.5f - 4.0f, 34.0f))) place();
    ImGui::SetItemTooltip("At the 3D cursor if it is shown, else in front of the camera.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!live);
    if (ImGui::Button("Update placed code", ImVec2(-1.0f, 34.0f))) rebuild();
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::Checkbox("Update the placed code on every change", &m_auto);
    const int selCode = selectedCodeId();
    ImGui::BeginDisabled(selCode < 0 || selCode == m_liveId);
    if (ImGui::Button("Edit selected code", ImVec2(-1.0f, 30.0f))) editSelected();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Load the selected code's text and sizes into this panel.");
    if (live) {
        const Entity* e = m_d.document.find(m_liveId);
        ImGui::TextDisabled("Editing: %s", e ? e->name.c_str() : "?");
    }
    // Rebuild once the edit settles (typing a text is one undo step, not one per key).
    if (m_pending && m_auto && live && code.valid() && !ImGui::IsAnyItemActive()) rebuild();
    if (!live) m_pending = false;
    ImGui::End();
}

} // namespace qrui
