#include "MixerPanel.hpp"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <string>

#include <imgui.h>

#include "UiStyle.hpp"

// The mixer, drawn as a desk: one strip per channel, the aux buses after them,
// the master set apart on the right. Top to bottom a strip is: name plate, insert
// slots, sends (one per aux bus), pan, the fader with its dB scale and a stereo
// meter, mute/solo. Below the desk, the parameters of the insert last clicked.
//
// Why decibels and a tapered fader rather than a slider: a slider is a
// percentage, and a mixer is not read in percentages. -6 dB says how much
// quieter something is; "50%" does not.
//
// Everything is built to be usable WITHOUT precise dragging, which is the
// editor's standing rule and not a nicety: whole tracks are hit targets and a
// click anywhere moves the control there, the wheel steps in whole decibels (or
// small steps), a double-click returns to the default, every effect parameter
// has - and + buttons, and anything with a number takes a typed one (Ctrl+click).
namespace mixerui {
namespace {

using fitzel::mixfx::Type;

// --- Geometry ---------------------------------------------------------------
constexpr float kStripW  = 124.0f;  // one channel strip
constexpr float kPlateH  = 26.0f;   // the name plate at the top
constexpr float kFaderH  = 216.0f;  // fader travel, and the meter beside it
constexpr float kTrackW  = 32.0f;   // the fader's hit area
constexpr float kScaleW  = 34.0f;   // the dB scale, between fader and meter
constexpr float kMeterW  = 20.0f;   // two bars, left and right
constexpr float kCapH    = 18.0f;   // the fader cap
constexpr float kCapW    = 28.0f;
constexpr float kLedGap  = 20.0f;   // room above the meter for the clip flag
constexpr float kPad     = 6.0f;
constexpr float kSendMinDb = -60.0f; // a send slider's bottom: off

// --- Colours ----------------------------------------------------------------
constexpr ImU32 kStripBg   = IM_COL32( 30,  32,  37, 255);
constexpr ImU32 kStripEdge = IM_COL32( 58,  62,  70, 255);
constexpr ImU32 kDeskBg    = IM_COL32( 22,  23,  27, 255);
constexpr ImU32 kGroove    = IM_COL32( 15,  16,  19, 255);
constexpr ImU32 kGrooveLip = IM_COL32( 52,  56,  64, 255);
constexpr ImU32 kCapTop    = IM_COL32(128, 134, 147, 255);
constexpr ImU32 kCapBottom = IM_COL32( 60,  64,  74, 255);
constexpr ImU32 kCapLine   = IM_COL32(236, 238, 244, 255);
constexpr ImU32 kCapEdge   = IM_COL32( 16,  17,  20, 255);
constexpr ImU32 kTick      = IM_COL32(112, 118, 130, 190);
constexpr ImU32 kTickText  = IM_COL32(150, 156, 168, 255);
constexpr ImU32 kUnityTick = IM_COL32(202, 208, 220, 255);
constexpr ImU32 kMeterBg   = IM_COL32( 13,  14,  17, 255);
constexpr ImU32 kMeterLow  = IM_COL32( 66, 196, 110, 255);
constexpr ImU32 kMeterMid  = IM_COL32(228, 190,  62, 255);
constexpr ImU32 kMeterHigh = IM_COL32(234,  82,  60, 255);
constexpr ImU32 kHoldLine  = IM_COL32(242, 246, 253, 240);
constexpr ImU32 kLedOff    = IM_COL32( 52,  30,  30, 255);
constexpr ImU32 kLedOn     = IM_COL32(242,  72,  56, 255);
constexpr ImU32 kPlateText = IM_COL32(238, 241, 248, 255);
constexpr ImU32 kSection   = IM_COL32(120, 126, 138, 255);

ImU32 plateColour(Kind k) {
    return k == Kind::Aux    ? IM_COL32(70, 52, 104, 255)
         : k == Kind::Master ? IM_COL32(62, 66, 78, 255)
                             : IM_COL32(38, 84, 96, 255);
}
ImU32 accentColour(Kind k) {
    return k == Kind::Aux    ? IM_COL32(156, 120, 220, 230)
         : k == Kind::Master ? IM_COL32(168, 176, 192, 235)
                             : IM_COL32(72, 168, 188, 230);
}

// --- The fader taper --------------------------------------------------------
struct Stop { float db, pos; };
constexpr Stop kTaper[] = {
    { 6.0f, 1.00f}, {  0.0f, 0.90f}, { -3.0f, 0.83f}, { -6.0f, 0.76f},
    {-12.0f, 0.63f}, {-20.0f, 0.49f}, {-30.0f, 0.35f}, {-40.0f, 0.24f},
    {-60.0f, 0.09f}, {kMinDb, 0.00f},
};
constexpr int   kStops = static_cast<int>(sizeof(kTaper) / sizeof(kTaper[0]));
constexpr float kMaxDb = 6.0f;

float posFromDb(float db) {
    if (db >= kTaper[0].db) return kTaper[0].pos;
    for (int i = 1; i < kStops; ++i)
        if (db >= kTaper[i].db) {
            const float t = (db - kTaper[i].db) / (kTaper[i - 1].db - kTaper[i].db);
            return kTaper[i].pos + (kTaper[i - 1].pos - kTaper[i].pos) * t;
        }
    return 0.0f;
}

float dbFromPos(float pos) {
    if (pos >= 1.0f) return kMaxDb;
    if (pos <= 0.0f) return kMinDb;
    for (int i = 1; i < kStops; ++i)
        if (pos >= kTaper[i].pos) {
            const float t = (pos - kTaper[i].pos) / (kTaper[i - 1].pos - kTaper[i].pos);
            return kTaper[i].db + (kTaper[i - 1].db - kTaper[i].db) * t;
        }
    return kMinDb;
}

constexpr float kTicks[] = {6.0f, 0.0f, -6.0f, -12.0f, -20.0f, -30.0f, -40.0f, -60.0f};
constexpr int   kTickCount = static_cast<int>(sizeof(kTicks) / sizeof(kTicks[0]));

// --- Meter ballistics -------------------------------------------------------
// Instant rise, 30 dB/s fall, a peak that hangs 1.2 s before dropping at 14 dB/s.
void tickMeter(Meter& m, float peak, float dt, bool playing) {
    const float db = toDb(playing ? peak : 0.0f);
    m.showDb = (db >= m.showDb) ? db : std::max(db, m.showDb - dt * 30.0f);
    if (db >= m.holdDb) {
        m.holdDb  = db;
        m.holdAge = 0.0f;
    } else {
        m.holdAge += dt;
        if (m.holdAge > 1.2f) m.holdDb = std::max(db, m.holdDb - dt * 14.0f);
    }
    if (db > -0.05f) m.clipped = true;
}

ImU32 meterColour(float db) {
    if (db >= -3.0f)  return kMeterHigh;
    if (db >= -12.0f) return kMeterMid;
    return kMeterLow;
}

void textAt(ImVec2 p, float size, ImU32 col, const char* s, bool centred = false) {
    ImFont* f = ImGui::GetFont();
    if (centred) {
        const ImVec2 sz = f->CalcTextSizeA(size, FLT_MAX, 0.0f, s);
        p.x -= sz.x * 0.5f;
        p.y -= sz.y * 0.5f;
    }
    ImGui::GetWindowDrawList()->AddText(f, size, p, col, s);
}

// --- What the panel remembers between frames ---------------------------------
struct UiState {
    int  selStrip  = -1;   // the strip whose insert is open below the desk
    int  selSlot   = -1;
    int  renameUid = -1;
    char renameBuf[64] = {};
    int  removeUid = -1;   // asked "remove this strip?"
};
UiState g_ui;

// --- The parts of a strip ---------------------------------------------------

void fader(Strip& c, ImVec2 pos, ImU32 accent) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton("##fader", ImVec2(kTrackW, kFaderH));
    const bool hovered = ImGui::IsItemHovered();
    const float top    = pos.y + kCapH * 0.5f;
    const float travel = kFaderH - kCapH;

    if (ImGui::IsItemActive()) {
        const float t = 1.0f - (ImGui::GetIO().MousePos.y - top) / travel;
        c.level = fromDb(dbFromPos(std::clamp(t, 0.0f, 1.0f)));
    }
    if (hovered) {
        if (const float wheel = ImGui::GetIO().MouseWheel; wheel != 0.0f) {
            const float step = ImGui::GetIO().KeyShift ? 0.5f : 1.0f;
            c.level = fromDb(std::clamp(toDb(c.level) + wheel * step, kMinDb, kMaxDb));
        }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) c.level = 1.0f;
        ImGui::SetTooltip("Click the track to set it there\n"
                          "Wheel: 1 dB   Shift+wheel: 0.5 dB\n"
                          "Double-click: back to 0 dB");
    }

    const float capY = top + (1.0f - posFromDb(toDb(c.level))) * travel;
    const float gx   = pos.x + kTrackW * 0.5f;
    dl->AddRectFilled(ImVec2(gx - 4.0f, pos.y), ImVec2(gx + 4.0f, pos.y + kFaderH), kGroove, 4.0f);
    dl->AddRect(ImVec2(gx - 4.0f, pos.y), ImVec2(gx + 4.0f, pos.y + kFaderH), kGrooveLip, 4.0f);
    if (capY < pos.y + kFaderH - 3.0f)
        dl->AddRectFilled(ImVec2(gx - 2.5f, capY), ImVec2(gx + 2.5f, pos.y + kFaderH), accent, 3.0f);
    const ImVec2 a(gx - kCapW * 0.5f, capY - kCapH * 0.5f);
    const ImVec2 b(gx + kCapW * 0.5f, capY + kCapH * 0.5f);
    dl->AddRectFilled(a, ImVec2(b.x, capY), kCapTop, 3.0f, ImDrawFlags_RoundCornersTop);
    dl->AddRectFilled(ImVec2(a.x, capY), b, kCapBottom, 3.0f, ImDrawFlags_RoundCornersBottom);
    dl->AddRectFilled(ImVec2(a.x + 2.0f, capY - 1.0f), ImVec2(b.x - 2.0f, capY + 1.0f), kCapLine);
    dl->AddRect(a, b, kCapEdge, 3.0f);
}

void scale(ImVec2 pos) {
    ImDrawList* dl   = ImGui::GetWindowDrawList();
    const float top  = pos.y + kCapH * 0.5f;
    const float trav = kFaderH - kCapH;
    const float size = ImGui::GetFontSize() * 0.70f;
    char buf[8];
    for (int i = 0; i < kTickCount; ++i) {
        const float y = top + (1.0f - posFromDb(kTicks[i])) * trav;
        const bool  u = kTicks[i] == 0.0f;
        dl->AddLine(ImVec2(pos.x + 2.0f, y), ImVec2(pos.x + (u ? 12.0f : 8.0f), y),
                    u ? kUnityTick : kTick, u ? 1.6f : 1.0f);
        std::snprintf(buf, sizeof(buf), "%+d", static_cast<int>(kTicks[i]));
        if (kTicks[i] == 0.0f) std::snprintf(buf, sizeof(buf), "0");
        textAt(ImVec2(pos.x + 14.0f, y - size * 0.62f), size, u ? kUnityTick : kTickText, buf);
    }
}

void meterBar(const Meter& m, float x0, float x1, float top, float trav) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (m.showDb > kMinDb) {
        constexpr float kBands[] = {-12.0f, -3.0f, kMaxDb};
        float from = kMinDb;
        for (const float band : kBands) {
            const float to = std::min(m.showDb, band);
            if (to > from) {
                const float y0 = top + (1.0f - posFromDb(from)) * trav;
                const float y1 = top + (1.0f - posFromDb(to)) * trav;
                dl->AddRectFilled(ImVec2(x0, y1), ImVec2(x1, y0), meterColour(band - 0.1f));
            }
            from = band;
            if (m.showDb <= band) break;
        }
    }
    if (m.holdDb > kMinDb) {
        const float y = top + (1.0f - posFromDb(m.holdDb)) * trav;
        dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), kHoldLine, 2.0f);
    }
}

// Left and right as measured after the fader, on the scale beside them, with a
// clip flag above that latches until it is clicked.
void meter(Strip& c, ImVec2 pos) {
    ImDrawList* dl   = ImGui::GetWindowDrawList();
    const float top  = pos.y + kCapH * 0.5f;
    const float trav = kFaderH - kCapH;
    dl->AddRectFilled(ImVec2(pos.x, top), ImVec2(pos.x + kMeterW, top + trav), kMeterBg, 2.0f);
    const float mid = pos.x + kMeterW * 0.5f;
    meterBar(c.meter[0], pos.x + 1.5f, mid - 0.75f, top, trav);
    meterBar(c.meter[1], mid + 0.75f, pos.x + kMeterW - 1.5f, top, trav);
    dl->AddRect(ImVec2(pos.x, top), ImVec2(pos.x + kMeterW, top + trav), kGrooveLip, 2.0f);

    const bool clipped = c.meter[0].clipped || c.meter[1].clipped;
    ImGui::SetCursorScreenPos(ImVec2(pos.x - 2.0f, pos.y - kLedGap + 2.0f));
    ImGui::InvisibleButton("##clip", ImVec2(kMeterW + 4.0f, kLedGap - 6.0f));
    if (ImGui::IsItemClicked()) c.meter[0].clipped = c.meter[1].clipped = false;
    const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
    dl->AddRectFilled(ImVec2(lo.x + 2.0f, lo.y + 2.0f), ImVec2(hi.x - 2.0f, hi.y - 2.0f),
                      clipped ? kLedOn : kLedOff, 2.0f);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(clipped ? "Clipped: this strip went over 0 dB.\nClick to clear."
                                  : "Clip flag -- lights when the strip goes over\n0 dB, and stays lit.");
}

// A row the strip lays out at a fixed place, so every strip lines up.
void rowAt(ImVec2 p0, float y) { ImGui::SetCursorScreenPos(ImVec2(p0.x + kPad, y)); }
float rowW() { return kStripW - kPad * 2.0f; }

// The insert slots: one button per effect (click: edit it below the desk,
// right-click: bypass, move, remove), then "+ Insert".
void insertsSection(Desk& d, Strip& c, ImVec2 p0, float y, int rows) {
    const float frame = ImGui::GetFrameHeight();
    for (int i = 0; i < static_cast<int>(c.inserts.size()); ++i) {
        Insert& in = c.inserts[i];
        ImGui::PushID(i);
        rowAt(p0, y + i * (frame + 3.0f));
        const bool sel = g_ui.selStrip == c.uid && g_ui.selSlot == i;
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (in.bypass) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::Button(fitzel::mixfx::name(in.type), ImVec2(rowW(), 0.0f))) {
            g_ui.selStrip = c.uid;
            g_ui.selSlot  = i;
        }
        if (in.bypass) ImGui::PopStyleColor();
        if (sel) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s%s\nClick: edit below the desk. Right-click: bypass, move, remove.",
                              fitzel::mixfx::name(in.type), in.bypass ? " (bypassed)" : "");
        if (ImGui::BeginPopupContextItem("##insctx")) {
            if (ImGui::MenuItem("Bypass", nullptr, in.bypass)) in.bypass = !in.bypass;
            if (ImGui::MenuItem("Move up", nullptr, false, i > 0)) {
                std::swap(c.inserts[i], c.inserts[i - 1]);
                if (sel) g_ui.selSlot = i - 1;
            }
            if (ImGui::MenuItem("Move down", nullptr, false, i + 1 < static_cast<int>(c.inserts.size()))) {
                std::swap(c.inserts[i], c.inserts[i + 1]);
                if (sel) g_ui.selSlot = i + 1;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Remove")) {
                c.inserts.erase(c.inserts.begin() + i);
                if (g_ui.selStrip == c.uid) g_ui.selSlot = -1;
                ImGui::EndPopup();
                ImGui::PopID();
                return;
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    rowAt(p0, y + static_cast<int>(c.inserts.size()) * (frame + 3.0f));
    if (ImGui::Button("+ Insert", ImVec2(rowW(), 0.0f))) ImGui::OpenPopup("##addfx");
    if (ImGui::BeginPopup("##addfx")) {
        for (int t = 0; t < static_cast<int>(Type::Count); ++t)
            if (ImGui::Selectable(fitzel::mixfx::name(static_cast<Type>(t)))) {
                Insert in;
                in.type = static_cast<Type>(t);
                for (const auto& p : fitzel::mixfx::params(in.type)) in.values.push_back(p.def);
                c.inserts.push_back(in);
                g_ui.selStrip = c.uid;
                g_ui.selSlot  = static_cast<int>(c.inserts.size()) - 1;
            }
        ImGui::EndPopup();
    }
    (void)d; (void)rows;
}

// One send per aux bus: a slider in dB (all the way down = off), wheel steps,
// double-click toggles off/0 dB, right-click picks pre or post fader.
void sendsSection(Desk& d, Strip& c, ImVec2 p0, float y) {
    const float frame = ImGui::GetFrameHeight();
    int row = 0;
    for (const Strip& a : d.strips) {
        if (a.kind != Kind::Aux) continue;
        ImGui::PushID(a.uid);
        rowAt(p0, y + row * (frame + 3.0f));
        Send& sd = c.sends[a.uid];
        float db = sd.level <= 0.0f ? kSendMinDb : std::max(kSendMinDb, toDb(sd.level));
        char fmt[96];
        if (sd.level <= 0.0f) std::snprintf(fmt, sizeof(fmt), "%s off", a.name.c_str());
        else std::snprintf(fmt, sizeof(fmt), "%s%s %%.0f dB", a.name.c_str(), sd.pre ? " pre" : "");
        ImGui::SetNextItemWidth(rowW());
        if (ImGui::SliderFloat("##send", &db, kSendMinDb, 6.0f, fmt))
            sd.level = db <= kSendMinDb + 0.01f ? 0.0f : fromDb(db);
        if (ImGui::IsItemHovered()) {
            if (const float wheel = ImGui::GetIO().MouseWheel; wheel != 0.0f) {
                const float cur = sd.level <= 0.0f ? kSendMinDb : toDb(sd.level);
                const float nx  = std::clamp(cur + wheel * (cur <= kSendMinDb ? 30.0f : 1.0f), kSendMinDb, 6.0f);
                sd.level = nx <= kSendMinDb + 0.01f ? 0.0f : fromDb(nx);
            }
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                sd.level = sd.level > 0.0f ? 0.0f : 1.0f;
            ImGui::SetTooltip("Send to the %s bus, in dB (all the way left = off).\n"
                              "Wheel: 1 dB   Double-click: off / 0 dB\n"
                              "Right-click: before or after the fader.", a.name.c_str());
        }
        if (ImGui::BeginPopupContextItem("##sendctx")) {
            if (ImGui::MenuItem("Post fader", nullptr, !sd.pre)) sd.pre = false;
            if (ImGui::MenuItem("Pre fader", nullptr, sd.pre)) sd.pre = true;
            ImGui::Separator();
            if (ImGui::MenuItem("Off")) sd.level = 0.0f;
            ImGui::EndPopup();
        }
        ImGui::PopID();
        ++row;
    }
}

int auxCount(const Desk& d) {
    int n = 0;
    for (const Strip& s : d.strips) if (s.kind == Kind::Aux) ++n;
    return n;
}

struct Layout {
    int   insertRows = 1;
    int   sendRows   = 0;
    float insertsY = 0, sendsY = 0, panY = 0, dbY = 0, rowY = 0, btnY = 0, height = 0;
};

Layout layout(const Desk& d) {
    Layout L;
    const float frame = ImGui::GetFrameHeight();
    std::size_t most = d.master.inserts.size();
    for (const Strip& s : d.strips) most = std::max(most, s.inserts.size());
    L.insertRows = static_cast<int>(most) + 1;
    L.sendRows   = auxCount(d);
    const float small = ImGui::GetFontSize() * 0.74f;
    float y = kPlateH + 6.0f;
    L.insertsY = y + small + 4.0f;
    y = L.insertsY + L.insertRows * (frame + 3.0f) + 4.0f;
    L.sendsY = y + small + 4.0f;
    y = L.sendsY + std::max(1, L.sendRows) * (frame + 3.0f) + 6.0f;
    L.panY = y;
    L.dbY  = L.panY + frame + 6.0f;
    L.rowY = L.dbY + frame + kLedGap;
    L.btnY = L.rowY + kFaderH + 12.0f;
    L.height = L.btnY + (frame + 6.0f) * 3.0f + 4.0f;
    return L;
}

// One complete strip.
void strip(Desk& d, Strip& c, const Layout& L, float dt, bool playing) {
    ImGui::PushID(c.uid);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    tickMeter(c.meter[0], c.peak[0], dt, playing);
    tickMeter(c.meter[1], c.peak[1], dt, playing);
    c.peak[0] = c.peak[1] = 0.0f;

    const ImVec2 p0    = ImGui::GetCursorScreenPos();
    const float  frame = ImGui::GetFrameHeight();
    const float  small = ImGui::GetFontSize() * 0.74f;
    const bool   live  = d.gainOf(c) > 0.0f;

    dl->AddRectFilled(p0, ImVec2(p0.x + kStripW, p0.y + L.height), kStripBg, 6.0f);
    dl->AddRect(p0, ImVec2(p0.x + kStripW, p0.y + L.height), kStripEdge, 6.0f);

    // Name plate: double-click to rename.
    dl->AddRectFilled(ImVec2(p0.x + 1.0f, p0.y + 1.0f), ImVec2(p0.x + kStripW - 1.0f, p0.y + kPlateH),
                      plateColour(c.kind), 5.0f, ImDrawFlags_RoundCornersTop);
    {
        ImFont* bold = ui::boldFont();
        const float size = ImGui::GetFontSize() * 0.84f;
        if (bold) ImGui::PushFont(bold, size);
        textAt(ImVec2(p0.x + kStripW * 0.5f, p0.y + kPlateH * 0.5f + 1.0f), size, kPlateText,
               c.name.c_str(), /*centred=*/true);
        if (bold) ImGui::PopFont();
    }
    ImGui::SetCursorScreenPos(p0);
    ImGui::InvisibleButton("##plate", ImVec2(kStripW, kPlateH));
    if (c.kind != Kind::Master) {
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s\nDouble-click to rename.",
                              c.kind == Kind::Aux ? "Aux bus" : "Channel");
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                g_ui.renameUid = c.uid;
                std::snprintf(g_ui.renameBuf, sizeof(g_ui.renameBuf), "%s", c.name.c_str());
                ImGui::OpenPopup("##rename");
            }
        }
        if (ImGui::BeginPopup("##rename")) {
            ImGui::TextUnformatted("Name");
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            const bool enter = ImGui::InputText("##nm", g_ui.renameBuf, sizeof(g_ui.renameBuf),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
            if (enter || ImGui::Button("OK", ImVec2(120.0f, 0.0f))) {
                d.rename(c.uid, g_ui.renameBuf);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    textAt(ImVec2(p0.x + kPad, p0.y + L.insertsY - small - 3.0f), small, kSection, "INSERTS");
    insertsSection(d, c, p0, p0.y + L.insertsY, L.insertRows);

    textAt(ImVec2(p0.x + kPad, p0.y + L.sendsY - small - 3.0f), small, kSection,
           c.kind == Kind::Channel ? "SENDS" : "");
    if (c.kind == Kind::Channel) {
        if (L.sendRows == 0)
            textAt(ImVec2(p0.x + kPad, p0.y + L.sendsY + 3.0f), small, kTick, "(no aux bus)");
        sendsSection(d, c, p0, p0.y + L.sendsY);
    }

    // Pan.
    rowAt(p0, p0.y + L.panY);
    {
        char fmt[24];
        const int pc = static_cast<int>(std::lround(std::fabs(c.pan) * 100.0f));
        if (pc == 0) std::snprintf(fmt, sizeof(fmt), "Pan C");
        else std::snprintf(fmt, sizeof(fmt), "Pan %c%d", c.pan < 0.0f ? 'L' : 'R', pc);
        ImGui::SetNextItemWidth(rowW());
        ImGui::SliderFloat("##pan", &c.pan, -1.0f, 1.0f, fmt);
        if (ImGui::IsItemHovered()) {
            if (const float wheel = ImGui::GetIO().MouseWheel; wheel != 0.0f)
                c.pan = std::clamp(c.pan + wheel * 0.05f, -1.0f, 1.0f);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) c.pan = 0.0f;
            ImGui::SetTooltip("Left / right. Wheel: 5%%   Double-click: centre");
        }
    }

    // The readout is also how an exact value gets typed in.
    float db = toDb(c.level);
    rowAt(p0, p0.y + L.dbY);
    ImGui::SetNextItemWidth(rowW());
    if (ImGui::DragFloat("##db", &db, 0.25f, kMinDb, kMaxDb, db <= kMinDb ? "-inf" : "%.1f dB"))
        c.level = fromDb(db);
    ImGui::SetItemTooltip("The fader, in decibels. Drag it, or Ctrl+click to type.");

    const float rowY = p0.y + L.rowY;
    const float padX = (kStripW - (kTrackW + kScaleW + kMeterW)) * 0.5f;
    fader(c, ImVec2(p0.x + padX, rowY), live ? accentColour(c.kind) : kGrooveLip);
    scale(ImVec2(p0.x + padX + kTrackW, rowY));
    meter(c, ImVec2(p0.x + padX + kTrackW + kScaleW, rowY));

    // Mute, then solo (channels) or back to 0 dB (buses, master), then remove.
    const float btnY = p0.y + L.btnY;
    rowAt(p0, btnY);
    if (c.mute) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(186, 62, 56, 255));
    if (ImGui::Button(c.mute ? "MUTE" : "Mute", ImVec2(rowW(), 0.0f))) c.mute = !c.mute;
    if (c.mute) ImGui::PopStyleColor();
    rowAt(p0, btnY + frame + 6.0f);
    if (c.kind == Kind::Channel) {
        if (c.solo) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(206, 158, 48, 255));
        if (ImGui::Button(c.solo ? "SOLO" : "Solo", ImVec2(rowW(), 0.0f))) c.solo = !c.solo;
        if (c.solo) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Listen to this channel alone (several may be soloed).\n"
                              "Aux buses stay on, so its reverb is still heard.");
    } else {
        if (ImGui::Button("0 dB", ImVec2(rowW(), 0.0f))) c.level = 1.0f;
        ImGui::SetItemTooltip("Back to unity gain.");
    }
    if (c.kind != Kind::Master) {
        rowAt(p0, btnY + (frame + 6.0f) * 2.0f);
        if (ImGui::Button("Remove", ImVec2(rowW(), 0.0f))) {
            g_ui.removeUid = c.uid;
            ImGui::OpenPopup("##remove");
        }
        if (ImGui::BeginPopup("##remove")) {
            ImGui::Text("Remove \"%s\"?", c.name.c_str());
            ImGui::TextDisabled(c.kind == Kind::Aux
                                    ? "Every send to it goes with it."
                                    : "Sounds routed here play on the default channel.");
            if (ImGui::Button("Remove", ImVec2(110.0f, 0.0f))) {
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                ImGui::SetCursorScreenPos(p0);
                ImGui::Dummy(ImVec2(kStripW, L.height));
                ImGui::PopID();
                d.remove(c.uid);     // c is gone after this
                return;
            }
            ImGui::SameLine();
            if (ImGui::Button("Keep", ImVec2(110.0f, 0.0f))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(ImVec2(kStripW, L.height));
    ImGui::PopID();
}

// The selected insert's parameters, under the desk: every value a slider (a
// click sets it), with - and + beside it, and a stepped choice as buttons.
void insertEditor(Desk& d) {
    Strip* s = d.find(g_ui.selStrip);
    if (!s || g_ui.selSlot < 0 || g_ui.selSlot >= static_cast<int>(s->inserts.size())) return;
    Insert& in = s->inserts[g_ui.selSlot];
    const auto& defs = fitzel::mixfx::params(in.type);
    ImGui::SeparatorText((s->name + "  >  " + fitzel::mixfx::name(in.type) + "  (slot " +
                          std::to_string(g_ui.selSlot + 1) + ")").c_str());
    ImGui::Checkbox("Bypass", &in.bypass);
    ImGui::SameLine();
    if (ImGui::Button("Defaults"))
        for (std::size_t i = 0; i < defs.size(); ++i) in.values[i] = defs[i].def;
    ImGui::SameLine();
    if (ImGui::Button("Close")) { g_ui.selSlot = -1; return; }

    const float labelW = ImGui::GetFontSize() * 7.0f;
    const float btnW   = ImGui::GetFrameHeight() * 1.4f;
    for (std::size_t i = 0; i < defs.size() && i < in.values.size(); ++i) {
        const auto& p = defs[i];
        float& v = in.values[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(p.name);
        ImGui::SameLine(labelW);
        if (p.labels) {
            // A stepped choice: one button per option, the current one lit.
            std::string all = p.labels;
            int idx = 0;
            std::size_t start = 0;
            while (start <= all.size()) {
                const std::size_t bar = all.find('|', start);
                const std::string lab = all.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
                const bool on = static_cast<int>(std::lround(v)) == idx;
                if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                if (ImGui::Button(lab.c_str())) v = static_cast<float>(idx);
                if (on) ImGui::PopStyleColor();
                ImGui::SameLine();
                ++idx;
                if (bar == std::string::npos) break;
                start = bar + 1;
            }
            ImGui::NewLine();
        } else {
            // Steps: 2% of the range, or a 6% ratio on a logarithmic one.
            const auto step = [&](float dir) {
                if (p.log) v = std::clamp(v * (dir > 0 ? 1.06f : 1.0f / 1.06f), p.min, p.max);
                else       v = std::clamp(v + dir * (p.max - p.min) * 0.02f, p.min, p.max);
            };
            if (ImGui::Button("-", ImVec2(btnW, 0.0f))) step(-1.0f);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - btnW - 8.0f));
            ImGui::SliderFloat("##v", &v, p.min, p.max, p.fmt,
                               p.log ? ImGuiSliderFlags_Logarithmic : ImGuiSliderFlags_None);
            if (ImGui::IsItemHovered()) {
                if (const float wheel = ImGui::GetIO().MouseWheel; wheel != 0.0f) step(wheel > 0 ? 1.0f : -1.0f);
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) v = p.def;
                ImGui::SetTooltip("Click to set, wheel or -/+ to step,\nCtrl+click to type, double-click: default.");
            }
            ImGui::SameLine();
            if (ImGui::Button("+", ImVec2(btnW, 0.0f))) step(1.0f);
        }
        ImGui::PopID();
    }
}

} // namespace

void selectInsert(int stripUid, int slot) {
    g_ui.selStrip = stripUid;
    g_ui.selSlot  = slot;
}

void drawPanel(const PanelState& s) {
    Desk& d = s.desk;
    const Layout L = layout(d);
    const float needH = L.height + 140.0f;
    ImGui::SetNextWindowSize(ImVec2(kStripW * 5.0f + 80.0f, needH + 220.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Mixer", &s.show)) { ImGui::End(); return; }

    if (ImGui::Button("+ Channel")) d.add(Kind::Channel, "Channel");
    ImGui::SetItemTooltip("A new channel. Audio Sources pick it by name (Inspector),\n"
                          "scripts with game.sound(..., channel).");
    ImGui::SameLine();
    if (ImGui::Button("+ Aux bus")) d.add(Kind::Aux, "Aux");
    ImGui::SetItemTooltip("A bus the channels can send to -- put a reverb or a delay on it.");
    ImGui::SameLine();
    if (!s.audioOk)       ui::hint("No output device -- the desk draws, nothing plays.");
    else if (!s.playing)  ui::hint("Stopped. The editor stays silent, so the meters rest.");
    else if (d.anySolo()) ui::hint("Playing -- SOLO is engaged.");
    else                  ui::hint("Playing.");

    // The desk scrolls sideways: as many strips as anyone wants.
    ImGui::BeginChild("##desk", ImVec2(0.0f, L.height + 24.0f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float  w = (static_cast<float>(d.strips.size()) + 1.0f) * (kStripW + 8.0f) + 24.0f;
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x - 6.0f, p.y - 4.0f),
                                                  ImVec2(p.x + w, p.y + L.height + 6.0f), kDeskBg, 8.0f);
    }
    // By uid, not by reference: removing a strip mid-loop reshapes the vector.
    std::vector<int> order;
    for (const Strip& st : d.strips) order.push_back(st.uid);
    bool first = true;
    for (int uid : order) {
        Strip* st = d.find(uid);
        if (!st) continue;
        if (!first) ImGui::SameLine(0.0f, 8.0f);
        first = false;
        strip(d, *st, L, s.dt, s.playing);
    }
    // The master set apart, as on a desk: not one more strip, where they all end.
    if (!first) ImGui::SameLine(0.0f, 9.0f);
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + 5.0f, p.y + 6.0f),
                                            ImVec2(p.x + 5.0f, p.y + L.height - 6.0f), kStripEdge, 1.0f);
        ImGui::Dummy(ImVec2(10.0f, 1.0f));
    }
    ImGui::SameLine(0.0f, 0.0f);
    strip(d, d.master, L, s.dt, s.playing);
    ImGui::EndChild();

    insertEditor(d);
    if (g_ui.selSlot < 0) {
        ImGui::Spacing();
        ui::hint("Every sound feeds one channel: Audio Sources pick theirs in the Inspector,\n"
                 "weather and zone loops play on Ambient, game.sound and vehicles on SFX,\n"
                 "songs and synths on Music. Click an insert to edit it here.");
    }
    ImGui::End();
}

} // namespace mixerui