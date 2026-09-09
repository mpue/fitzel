#include "MixerPanel.hpp"

#include <algorithm>
#include <cstdio>

#include <imgui.h>

#include "UiStyle.hpp"

// The mixer, drawn as a desk: channel strips with a tapered fader, a dB scale
// the fader and its meter share, mute/solo, a peak that hangs, a clip flag that
// latches, and a master set apart on the right.
//
// Why not the three ImGui::VSliderFloat this was: a slider is a percentage, and
// a mixer is not read in percentages. "Ambient at 50%" says nothing about how
// much quieter that is; -6 dB does, and the numbers beside the fader are the
// ones anyone who has stood in front of a desk already knows. The taper is there
// for the same reason -- a linear 0..1 slider spends half its travel between
// silence and -20 dB, where nothing is ever decided, and squeezes the range that
// is actually mixed in into the top fifth.
//
// The faders are built to be usable WITHOUT precise dragging, which is the
// editor's standing rule and not a nicety: the whole track is the hit target and
// a click anywhere on it moves the fader there, the wheel steps in whole
// decibels, a double-click returns to unity, and the readout above takes a typed
// number. Nothing here needs a steady hand on a 14-pixel cap.
namespace mixerui {
namespace {

// --- Geometry ---------------------------------------------------------------
constexpr float kStripW  = 108.0f;  // one channel strip
constexpr float kPlateH  = 26.0f;   // the name plate at the top
constexpr float kFaderH  = 216.0f;  // fader travel, and the meter beside it
constexpr float kTrackW  = 32.0f;   // the fader's hit area
constexpr float kScaleW  = 34.0f;   // the dB scale, between fader and meter
constexpr float kMeterW  = 18.0f;
constexpr float kCapH    = 18.0f;   // the fader cap
constexpr float kCapW    = 28.0f;
constexpr float kLedGap  = 20.0f;   // room above the row for the clip flag

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

// --- The fader taper --------------------------------------------------------
// Position on the track (0 at the bottom, 1 at the top) against decibels. A
// console taper, not a straight line: unity sits high but not at the very top,
// the working range from -20 dB up takes half the travel, and the bottom decade
// is compressed because there is nothing down there to decide.
struct Stop { float db, pos; };
constexpr Stop kTaper[] = {
    {  0.0f, 1.00f}, { -3.0f, 0.92f}, { -6.0f, 0.84f}, {-12.0f, 0.70f},
    {-20.0f, 0.54f}, {-30.0f, 0.38f}, {-40.0f, 0.26f}, {-60.0f, 0.10f},
    {kMinDb, 0.00f},
};
constexpr int kStops = static_cast<int>(sizeof(kTaper) / sizeof(kTaper[0]));

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
    if (pos >= 1.0f) return 0.0f;
    if (pos <= 0.0f) return kMinDb;
    for (int i = 1; i < kStops; ++i)
        if (pos >= kTaper[i].pos) {
            const float t = (pos - kTaper[i].pos) / (kTaper[i - 1].pos - kTaper[i].pos);
            return kTaper[i].db + (kTaper[i - 1].db - kTaper[i].db) * t;
        }
    return kMinDb;
}

// The labelled marks. One scale for the fader AND the meter beside it, so a bus
// asking for more than its fader is set to reads as a bar standing above the
// cap, rather than as two numbers to compare.
constexpr float kTicks[] = {0.0f, -6.0f, -12.0f, -20.0f, -30.0f, -40.0f, -60.0f};
constexpr int   kTickCount = static_cast<int>(sizeof(kTicks) / sizeof(kTicks[0]));

// --- Meter ballistics -------------------------------------------------------
// Instant rise, 30 dB/s fall, and a peak that hangs 1.2 s before dropping at
// 14 dB/s. PPM-ish numbers: fast enough to catch a thunder clap, slow enough
// that the eye can read where it got to.
void tickMeter(Channel& c, float dt, bool playing) {
    c.ping = std::max(0.0f, c.ping - dt * 2.2f);
    const float db = toDb(playing ? std::max(c.ask, c.ping) : 0.0f);

    c.showDb = (db >= c.showDb) ? db : std::max(db, c.showDb - dt * 30.0f);
    if (db >= c.holdDb) {
        c.holdDb  = db;
        c.holdAge = 0.0f;
    } else {
        c.holdAge += dt;
        if (c.holdAge > 1.2f) c.holdDb = std::max(db, c.holdDb - dt * 14.0f);
    }
    if (db > -0.05f) c.clipped = true;
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

// --- The parts of a strip ---------------------------------------------------

// Groove, travelled part, cap -- and every way of moving it that does not need
// a steady hand.
void fader(Channel& c, ImVec2 pos, ImU32 accent) {
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton("##fader", ImVec2(kTrackW, kFaderH));
    const bool hovered = ImGui::IsItemHovered();

    // The cap travels between its own half-heights, so the value maps to the
    // middle of the cap and not to the end of the track.
    const float top    = pos.y + kCapH * 0.5f;
    const float travel = kFaderH - kCapH;

    if (ImGui::IsItemActive()) {
        const float t = 1.0f - (ImGui::GetIO().MousePos.y - top) / travel;
        c.level = fromDb(dbFromPos(std::clamp(t, 0.0f, 1.0f)));
    }
    if (hovered) {
        if (const float wheel = ImGui::GetIO().MouseWheel; wheel != 0.0f) {
            const float step = ImGui::GetIO().KeyShift ? 0.5f : 1.0f;
            c.level = fromDb(std::clamp(toDb(c.level) + wheel * step, kMinDb, 0.0f));
        }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) c.level = 1.0f;
        ImGui::SetTooltip("Click the track to set it there\n"
                          "Wheel: 1 dB   Shift+wheel: 0.5 dB\n"
                          "Double-click: back to 0 dB");
    }

    const float capY = top + (1.0f - posFromDb(toDb(c.level))) * travel;
    const float gx   = pos.x + kTrackW * 0.5f;

    dl->AddRectFilled(ImVec2(gx - 4.0f, pos.y), ImVec2(gx + 4.0f, pos.y + kFaderH),
                      kGroove, 4.0f);
    dl->AddRect(ImVec2(gx - 4.0f, pos.y), ImVec2(gx + 4.0f, pos.y + kFaderH),
                kGrooveLip, 4.0f);
    if (capY < pos.y + kFaderH - 3.0f)
        dl->AddRectFilled(ImVec2(gx - 2.5f, capY), ImVec2(gx + 2.5f, pos.y + kFaderH),
                          accent, 3.0f);

    // Two halves, so the cap catches the light like a moulded one.
    const ImVec2 a(gx - kCapW * 0.5f, capY - kCapH * 0.5f);
    const ImVec2 b(gx + kCapW * 0.5f, capY + kCapH * 0.5f);
    dl->AddRectFilled(a, ImVec2(b.x, capY), kCapTop, 3.0f, ImDrawFlags_RoundCornersTop);
    dl->AddRectFilled(ImVec2(a.x, capY), b, kCapBottom, 3.0f,
                      ImDrawFlags_RoundCornersBottom);
    dl->AddRectFilled(ImVec2(a.x + 2.0f, capY - 1.0f), ImVec2(b.x - 2.0f, capY + 1.0f),
                      kCapLine);
    dl->AddRect(a, b, kCapEdge, 3.0f);
}

// The scale between fader and meter, drawn once and read by both.
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
        std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(kTicks[i]));
        textAt(ImVec2(pos.x + 14.0f, y - size * 0.62f), size,
               u ? kUnityTick : kTickText, buf);
    }
}

// What the bus is being asked to play, on the scale beside it, with a peak that
// hangs and a clip flag above it that latches until it is cleared.
void meter(Channel& c, ImVec2 pos) {
    ImDrawList* dl   = ImGui::GetWindowDrawList();
    const float top  = pos.y + kCapH * 0.5f;
    const float trav = kFaderH - kCapH;

    dl->AddRectFilled(ImVec2(pos.x, top), ImVec2(pos.x + kMeterW, top + trav),
                      kMeterBg, 2.0f);

    // Three bands rather than a gradient, so the colour change lands on the mark
    // it belongs to instead of somewhere nobody can read a number off.
    if (c.showDb > kMinDb) {
        constexpr float kBands[] = {-12.0f, -3.0f, 0.0f};
        float from = kMinDb;
        for (const float band : kBands) {
            const float to = std::min(c.showDb, band);
            if (to > from) {
                const float y0 = top + (1.0f - posFromDb(from)) * trav;
                const float y1 = top + (1.0f - posFromDb(to)) * trav;
                dl->AddRectFilled(ImVec2(pos.x + 1.5f, y1),
                                  ImVec2(pos.x + kMeterW - 1.5f, y0),
                                  meterColour(band - 0.1f));
            }
            from = band;
            if (c.showDb <= band) break;
        }
    }
    if (c.holdDb > kMinDb) {
        const float y = top + (1.0f - posFromDb(c.holdDb)) * trav;
        dl->AddLine(ImVec2(pos.x + 1.0f, y), ImVec2(pos.x + kMeterW - 1.0f, y),
                    kHoldLine, 2.0f);
    }
    dl->AddRect(ImVec2(pos.x, top), ImVec2(pos.x + kMeterW, top + trav),
                kGrooveLip, 2.0f);

    // The clip flag. It latches and clears on click, the way the lamp on a desk
    // does: a peak that happened while you were looking somewhere else has to
    // still be there when you look up, or it may as well not be shown at all.
    ImGui::SetCursorScreenPos(ImVec2(pos.x - 2.0f, pos.y - kLedGap + 2.0f));
    ImGui::InvisibleButton("##clip", ImVec2(kMeterW + 4.0f, kLedGap - 6.0f));
    if (ImGui::IsItemClicked()) c.clipped = false;
    const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
    dl->AddRectFilled(ImVec2(lo.x + 2.0f, lo.y + 2.0f), ImVec2(hi.x - 2.0f, hi.y - 2.0f),
                      c.clipped ? kLedOn : kLedOff, 2.0f);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(c.clipped
                              ? "Clipped: this bus was asked for more than 0 dB.\n"
                                "Click to clear."
                              : "Clip flag -- lights when the bus is asked for\n"
                                "more than 0 dB, and stays lit.");
}

float stripHeight() {
    const float frame = ImGui::GetFrameHeight();
    return kPlateH + 8.0f + frame + kLedGap + kFaderH + 12.0f
         + frame + 6.0f + frame + 8.0f + ImGui::GetFontSize() * 0.74f + 8.0f;
}

// One complete strip. Everything it changes, it changes in `c`.
void strip(Channel& c, const char* name, ImU32 plate, ImU32 accent,
           bool withSolo, bool live, float dt, bool playing) {
    ImGui::PushID(name);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    tickMeter(c, dt, playing);

    const ImVec2 p0    = ImGui::GetCursorScreenPos();
    const float  h     = stripHeight();
    const float  frame = ImGui::GetFrameHeight();

    dl->AddRectFilled(p0, ImVec2(p0.x + kStripW, p0.y + h), kStripBg, 6.0f);
    dl->AddRect(p0, ImVec2(p0.x + kStripW, p0.y + h), kStripEdge, 6.0f);

    // Name plate.
    dl->AddRectFilled(ImVec2(p0.x + 1.0f, p0.y + 1.0f),
                      ImVec2(p0.x + kStripW - 1.0f, p0.y + kPlateH), plate,
                      5.0f, ImDrawFlags_RoundCornersTop);
    {
        ImFont* bold = ui::boldFont();
        const float size = ImGui::GetFontSize() * 0.84f;
        if (bold) ImGui::PushFont(bold, size);
        textAt(ImVec2(p0.x + kStripW * 0.5f, p0.y + kPlateH * 0.5f + 1.0f), size,
               kPlateText, name, /*centred=*/true);
        if (bold) ImGui::PopFont();
    }

    // The readout is also how an exact value gets typed in.
    float db = toDb(c.level);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 6.0f, p0.y + kPlateH + 8.0f));
    ImGui::SetNextItemWidth(kStripW - 12.0f);
    if (ImGui::DragFloat("##db", &db, 0.25f, kMinDb, 0.0f,
                         db <= kMinDb ? "-inf" : "%.1f dB"))
        c.level = fromDb(db);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The fader, in decibels. Drag it, or Ctrl+click to type.");

    // Fader, scale, meter: one row, placed from the strip's own origin so every
    // strip lines up whatever the UI font size is.
    const float rowY = p0.y + kPlateH + 8.0f + frame + kLedGap;
    const float padX = (kStripW - (kTrackW + kScaleW + kMeterW)) * 0.5f;
    fader(c, ImVec2(p0.x + padX, rowY), live ? accent : kGrooveLip);
    scale(ImVec2(p0.x + padX + kTrackW, rowY));
    meter(c, ImVec2(p0.x + padX + kTrackW + kScaleW, rowY));

    // Mute and solo, stacked and full width: two big targets beat two small ones
    // side by side, and the labels fit at any font size.
    const float btnY = rowY + kFaderH + 12.0f;
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 6.0f, btnY));
    if (c.mute) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(186, 62, 56, 255));
    if (ImGui::Button(c.mute ? "MUTE" : "Mute", ImVec2(kStripW - 12.0f, 0.0f)))
        c.mute = !c.mute;
    if (c.mute) ImGui::PopStyleColor();

    if (withSolo) {
        ImGui::SetCursorScreenPos(ImVec2(p0.x + 6.0f, btnY + frame + 6.0f));
        if (c.solo) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(206, 158, 48, 255));
        if (ImGui::Button(c.solo ? "SOLO" : "Solo", ImVec2(kStripW - 12.0f, 0.0f)))
            c.solo = !c.solo;
        if (c.solo) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Listen to this bus alone. The other one goes\n"
                              "silent until solo is off again.");
    } else {
        // The master has nothing to solo against, so its second button is the
        // thing a master fader is asked for more often than any other: put it
        // back where it belongs. Double-clicking the fader does the same, but a
        // button is the version that can be found without being told.
        ImGui::SetCursorScreenPos(ImVec2(p0.x + 6.0f, btnY + frame + 6.0f));
        if (ImGui::Button("0 dB", ImVec2(kStripW - 12.0f, 0.0f))) c.level = 1.0f;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Back to unity gain.");
    }

    // The peak, in the same unit as everything above it.
    char buf[24];
    if (c.holdDb <= kMinDb) std::snprintf(buf, sizeof(buf), "peak  --");
    else                    std::snprintf(buf, sizeof(buf), "peak %+.1f", c.holdDb);
    const float small = ImGui::GetFontSize() * 0.74f;
    textAt(ImVec2(p0.x + kStripW * 0.5f, btnY + frame * 2.0f + 14.0f + small * 0.5f),
           small, kTickText, buf, /*centred=*/true);

    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(ImVec2(kStripW, h));
    ImGui::PopID();
}

} // namespace

void drawPanel(const PanelState& s) {
    const float needW = kStripW * 3.0f + 66.0f;
    const float needH = stripHeight() + 196.0f;   // + status line and footnotes
    ImGui::SetNextWindowSizeConstraints(ImVec2(needW, needH),
                                        ImVec2(FLT_MAX, FLT_MAX));
    ImGui::SetNextWindowSize(ImVec2(needW, needH), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Mixer", &s.show)) { ImGui::End(); return; }

    Desk& d = s.desk;

    // Where the signal is, in one line: the strip names alone do not say whether
    // anything is coming through them.
    if (!s.audioOk)
        ui::hint("No output device -- the desk draws, nothing plays.");
    else if (!s.playing)
        ui::hint("Stopped. The editor stays silent, so the meters rest.");
    else if (d.anySolo())
        ui::hint("Playing -- SOLO is engaged, the other bus is muted.");
    else
        ui::hint("Playing.");

    // A darker deck under the strips, so the desk reads as one object rather
    // than as three cards that happen to be next to each other.
    ImGui::Spacing();
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float  w = kStripW * 3.0f + 16.0f + 19.0f + 12.0f;
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(p.x - 6.0f, p.y - 4.0f),
            ImVec2(p.x + w, p.y + stripHeight() + 8.0f), kDeskBg, 8.0f);
    }

    strip(d.ambient, "AMBIENT", IM_COL32(38, 84, 96, 255),
          IM_COL32(72, 168, 188, 230), true, d.ambientGain() > 0.0f,
          s.dt, s.playing);
    ImGui::SameLine(0.0f, 8.0f);
    strip(d.sfx, "SFX", IM_COL32(96, 72, 34, 255),
          IM_COL32(214, 152, 58, 230), true, d.sfxGain() > 0.0f,
          s.dt, s.playing);

    // The master is set apart, as it is on a desk: not one more bus, but where
    // the buses end up.
    ImGui::SameLine(0.0f, 9.0f);
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(p.x + 5.0f, p.y + 6.0f),
            ImVec2(p.x + 5.0f, p.y + stripHeight() - 6.0f), kStripEdge, 1.0f);
        ImGui::Dummy(ImVec2(10.0f, 1.0f));
    }
    ImGui::SameLine(0.0f, 0.0f);

    // What the master is asked for is what came up the two buses -- after their
    // faders, and after its own.
    d.master.ask = std::max(d.ambient.ask, d.sfx.ask) * d.masterGain();
    strip(d.master, "MASTER", IM_COL32(62, 66, 78, 255),
          IM_COL32(168, 176, 192, 235), false, d.masterGain() > 0.0f,
          s.dt, s.playing);

    ImGui::Spacing();
    ui::hint("Ambient: weather and zone loops. SFX: one-shots and vehicles. "
             "Master: the device.");
    ui::hint("The meters show what the desk ASKS its voices for, not a "
             "measurement of the output.");
    ImGui::End();
}

} // namespace mixerui
