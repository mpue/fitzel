#include "RaceHud.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "RaceSim.hpp"

namespace racehud {
namespace {

// --- Look ------------------------------------------------------------------
// One dark ink, one hairline, and three accents (gold = you / leading, cyan =
// speed + boost, green = done). Panels are translucent so the track stays
// readable through them; text always carries a drop shadow because a HUD has to
// survive a white sky and a black tunnel in the same lap.
constexpr ImU32 kInk     = IM_COL32( 10,  13,  20, 175);
constexpr ImU32 kInkDeep = IM_COL32(  8,  10,  16, 232);
constexpr ImU32 kEdge    = IM_COL32(255, 255, 255,  26);
constexpr ImU32 kText    = IM_COL32(236, 240, 247, 255);
constexpr ImU32 kDim     = IM_COL32(148, 160, 180, 255);
constexpr ImU32 kCyan    = IM_COL32( 96, 222, 255, 255);
constexpr ImU32 kGold    = IM_COL32(255, 212, 106, 255);
constexpr ImU32 kGreen   = IM_COL32(118, 244, 158, 255);
constexpr ImU32 kRed     = IM_COL32(255, 108,  94, 255);

ImU32 fade(ImU32 col, float a) {
    const float f = std::clamp(a, 0.0f, 1.0f);
    return (col & 0x00FFFFFFu) |
           (static_cast<ImU32>(((col >> 24) & 0xFF) * f) << 24);
}

// Place colours: podium first, then plain.
ImU32 placeColor(int place) {
    switch (place) {
        case 1:  return kGold;
        case 2:  return IM_COL32(214, 220, 232, 255);
        case 3:  return IM_COL32(206, 148,  92, 255);
        default: return kDim;
    }
}

const char* ordinal(int p) {
    const int t = p % 100;
    if (t >= 11 && t <= 13) return "TH";
    switch (p % 10) {
        case 1:  return "ST";
        case 2:  return "ND";
        case 3:  return "RD";
        default: return "TH";
    }
}

std::string fmtTime(float t) {
    const int m = static_cast<int>(t / 60.0f);
    const float s = t - m * 60.0f;
    char b[24];
    if (m > 0) std::snprintf(b, sizeof(b), "%d:%05.2f", m, s);
    else       std::snprintf(b, sizeof(b), "%.2f", s);
    return std::string(b);
}

// --- Draw helpers ----------------------------------------------------------
// Everything below works in pixels off one scale factor, so the HUD keeps its
// proportions from a docked editor viewport up to a 4K presentation window.
struct Ctx {
    ImDrawList* dl;
    ImFont*     font;
    float x0, y0, x1, y1;   // viewport rect
    float cx;               // horizontal centre
    float S;                // scale unit (1.0 at ~900 px of view height)
    float pad;
    float fSmall, fBody, fBig, fHuge;
    float time;             // seconds, for pulses

    float textW(float size, const char* s) const {
        return font->CalcTextSizeA(size, FLT_MAX, 0.0f, s).x;
    }
    void text(float x, float y, float size, ImU32 col, const char* s) const {
        const float o = std::max(1.0f, size * 0.06f);
        dl->AddText(font, size, ImVec2(x + o, y + o), fade(IM_COL32(0, 0, 0, 205),
                                                           ((col >> 24) & 0xFF) / 255.0f), s);
        dl->AddText(font, size, ImVec2(x, y), col, s);
    }
    void textR(float xRight, float y, float size, ImU32 col, const char* s) const {
        text(xRight - textW(size, s), y, size, col, s);
    }
    void textC(float xMid, float y, float size, ImU32 col, const char* s) const {
        text(xMid - textW(size, s) * 0.5f, y, size, col, s);
    }
    // A panel: translucent ink, hairline edge, optional accent bar down the left.
    void panel(float ax, float ay, float bx, float by, ImU32 fill,
               ImU32 accent = 0, float alpha = 1.0f) const {
        const float r = 5.0f * S;
        dl->AddRectFilled(ImVec2(ax, ay), ImVec2(bx, by), fade(fill, alpha), r);
        dl->AddRect(ImVec2(ax, ay), ImVec2(bx, by), fade(kEdge, alpha), r, 0, 1.0f);
        if (accent)
            dl->AddRectFilled(ImVec2(ax, ay), ImVec2(ax + 3.0f * S, by),
                              fade(accent, alpha), r, ImDrawFlags_RoundCornersLeft);
    }
    // Small capital label above a value -- the HUD's only "typography rule".
    void label(float x, float y, ImU32 col, const char* s) const {
        text(x, y, fSmall, col, s);
    }
};

// A rounded chip carrying the place number, podium-coloured.
void placeChip(const Ctx& g, float x, float y, float h, int place, float alpha) {
    const ImU32 col = placeColor(place);
    const float w = h * 1.15f;
    g.dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h),
                        fade(col, 0.16f * alpha), 3.0f * g.S);
    g.dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), fade(col, 0.55f * alpha),
                  3.0f * g.S, 0, 1.0f);
    char b[8]; std::snprintf(b, sizeof(b), "%d", place);
    const float fsz = h * 0.72f;
    g.text(x + (w - g.textW(fsz, b)) * 0.5f, y + (h - fsz) * 0.5f - 1.0f * g.S,
           fsz, fade(col, alpha), b);
}

// --- Boost: a segmented tank gauge under the speed bar ----------------------
// Segments rather than a smooth fill because a boost is spent in decisions, not
// in millilitres: discrete blocks let the pilot see "two more presses" at a
// glance, mid-corner, without reading a number. The part-full block at the head
// of the row is drawn faded so the bar still moves continuously as it drains --
// a purely blocky gauge looks broken while it is refilling.
void drawBoostBar(const Ctx& g, const racesim::RaceState& st, float x0, float x1,
                  float y) {
    const int   n    = std::clamp(st.boostSegments, 1, 40);
    const float cap  = std::max(st.boostCapacity, 1.0f);
    const float fill = std::clamp(st.boostCharge / cap, 0.0f, 1.0f) * n; // in segments
    const float gap  = 2.0f * g.S;
    const float segW = ((x1 - x0) - gap * (n - 1)) / n;
    const float segH = 5.0f * g.S;
    const float r    = 1.5f * g.S;

    // Running dry flashes the whole row red; while boosting it pulses, so the
    // gauge reads as "in use" from peripheral vision.
    const bool  dry   = st.boostDryFlash > 0.0f;
    const float pulse = st.boostActive
                            ? 0.75f + 0.25f * std::sin(g.time * 18.0f)
                            : 1.0f;
    const ImU32 on    = dry ? IM_COL32(255, 96, 96, 255)
                            : (st.boostActive ? kCyan : IM_COL32(120, 220, 255, 235));

    for (int i = 0; i < n; ++i) {
        const float sx = x0 + i * (segW + gap);
        const ImVec2 a(sx, y), b(sx + segW, y + segH);
        g.dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 28), r);
        const float f = std::clamp(fill - static_cast<float>(i), 0.0f, 1.0f);
        if (f > 0.02f)
            g.dl->AddRectFilled(a, b, fade(on, f * pulse), r);
    }
    g.label(x0, y - g.fSmall - 2.0f * g.S, dry ? IM_COL32(255, 120, 120, 255) : kDim,
            "BOOST");
}

// --- Speed: big number + a bar that runs past the top-speed tick on boost ----
void drawSpeed(const Ctx& g, const racesim::RaceState& st) {
    const float kmh  = st.gliderSpeedMps * 3.6f;
    const float top  = std::max(st.gliderTopSpeed, 1.0f);
    const float barMax = top * 1.35f;                 // headroom for boost
    // Taller than the plain speed panel: the boost gauge lives inside it rather
    // than floating on its own, so the two numbers a pilot reads mid-corner are
    // in one place.
    const float w  = 250.0f * g.S, h = 118.0f * g.S;
    const float ax = g.x0 + g.pad, ay = g.y1 - g.pad - h;
    const bool  boosting = st.gliderBoosting;
    g.panel(ax, ay, ax + w, ay + h, kInk, boosting ? kCyan : kGold);

    char num[16]; std::snprintf(num, sizeof(num), "%.0f", std::max(kmh, 0.0f));
    const float ix = ax + 14.0f * g.S;
    g.label(ix, ay + 9.0f * g.S, kDim, "SPEED");
    g.text(ix, ay + 20.0f * g.S, g.fBig, boosting ? kCyan : kText, num);
    g.text(ix + g.textW(g.fBig, num) + 6.0f * g.S,
           ay + 20.0f * g.S + g.fBig - g.fSmall * 1.3f, g.fSmall, kDim, "KM/H");

    // Bar: base fill to the top-speed tick, the boost overrun beyond it in cyan.
    const float bx0 = ix, bx1 = ax + w - 14.0f * g.S;
    const float by0 = ay + h - 48.0f * g.S, by1 = by0 + 7.0f * g.S;
    g.dl->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by1),
                        IM_COL32(255, 255, 255, 30), 3.0f * g.S);
    const float f = std::clamp(st.gliderSpeedMps / barMax, 0.0f, 1.0f);
    if (f > 0.001f)
        g.dl->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx0 + (bx1 - bx0) * f, by1),
                            boosting ? kCyan : kGold, 3.0f * g.S);
    const float tick = bx0 + (bx1 - bx0) * (top / barMax);
    g.dl->AddLine(ImVec2(tick, by0 - 2.0f * g.S), ImVec2(tick, by1 + 2.0f * g.S),
                  IM_COL32(255, 255, 255, 120), 1.0f);

    drawBoostBar(g, st, bx0, bx1, ay + h - 13.0f * g.S);
}

// --- Energy: the hull bar, bottom centre ------------------------------------
// A single continuous bar, not the boost gauge's blocks: energy is not spent in
// decisions, it is taken away, and what a pilot needs from it is "how much is
// left" as one length. Bottom centre because that is where the eye already is
// (it sits right under the craft), and because the number matters most at the
// moment there is no time to look anywhere else.
//
// It carries three states, all readable from the corner of an eye: the fill's
// colour walks green -> gold -> red as it drains, a hit slams a white flash
// across the whole bar with what it cost written beside it, and below the
// warning threshold the whole panel pulses and says so in words.
void drawEnergy(const Ctx& g, const racesim::RaceState& st) {
    const float cap  = std::max(st.energyCapacity, 1.0f);
    const float frac = std::clamp(st.energy / cap, 0.0f, 1.0f);
    const float w = 360.0f * g.S, h = 76.0f * g.S;
    // Centred -- but never on top of the speed block in a narrow (docked-editor)
    // viewport, and never off the right edge either.
    const float left = g.x0 + g.pad + 262.0f * g.S;
    const float ax = std::min(std::max(g.cx - w * 0.5f, left), g.x1 - g.pad - w);
    const float ay = g.y1 - g.pad - h;

    // Colour: the bar tells the story before any number is read.
    const ImU32 lit = st.energyOut ? kRed
                    : frac > 0.5f  ? kGreen
                    : frac > 0.25f ? kGold
                                   : kRed;
    // Low: the panel breathes. Empty: it stops breathing and stays red.
    const float pulse = st.energyLow ? 0.62f + 0.38f * std::sin(g.time * 7.0f) : 1.0f;
    const float flash = std::clamp(st.energyHitFlash / 0.7f, 0.0f, 1.0f);

    g.panel(ax, ay, ax + w, ay + h, kInk, lit);
    if (flash > 0.01f)   // the hit itself: a wash of red over the whole panel
        g.dl->AddRectFilled(ImVec2(ax, ay), ImVec2(ax + w, ay + h),
                            fade(IM_COL32(255, 70, 60, 90), flash), 5.0f * g.S);

    const float ix = ax + 16.0f * g.S, rx = ax + w - 16.0f * g.S;
    g.label(ix, ay + 9.0f * g.S,
            st.energyLow ? fade(kRed, pulse) : kDim, "ENERGY");
    char num[16];
    std::snprintf(num, sizeof(num), "%.0f%%", frac * 100.0f);
    g.textR(rx, ay + 7.0f * g.S, g.fBody, fade(lit, pulse), num);

    // The bar. The unfilled part stays visible as a dark track, so the gauge
    // reads as "how much of the hull is left", not as a shrinking stripe.
    const float by0 = ay + 32.0f * g.S, by1 = by0 + 14.0f * g.S;
    g.dl->AddRectFilled(ImVec2(ix, by0), ImVec2(rx, by1),
                        IM_COL32(255, 255, 255, 26), 3.0f * g.S);
    if (frac > 0.002f)
        g.dl->AddRectFilled(ImVec2(ix, by0), ImVec2(ix + (rx - ix) * frac, by1),
                            fade(lit, pulse), 3.0f * g.S);
    // Quarter ticks: a length is easier to judge against marks than on its own.
    for (int i = 1; i < 4; ++i) {
        const float tx = ix + (rx - ix) * (i * 0.25f);
        g.dl->AddLine(ImVec2(tx, by0), ImVec2(tx, by1),
                      IM_COL32(0, 0, 0, 110), 1.0f);
    }
    if (flash > 0.01f)   // ...and a white edge on the fill as it is knocked back
        g.dl->AddRect(ImVec2(ix, by0), ImVec2(rx, by1),
                      fade(IM_COL32(255, 255, 255, 255), flash), 3.0f * g.S, 0, 2.0f);

    // Bottom line: the standing state on the left, the last hit on the right for
    // as long as its flash lasts.
    const float ly = by1 + 4.0f * g.S;
    if (st.energyOut)
        g.text(ix, ly, g.fSmall, kRed, "HULL DOWN");
    else if (st.energyLow)
        g.text(ix, ly, g.fSmall, fade(kRed, pulse), "ENERGY LOW");
    else
        g.text(ix, ly, g.fSmall, kDim, "HULL");
    if (flash > 0.01f && st.energyLastHit > 0.0f) {
        char d[24];
        std::snprintf(d, sizeof(d), "-%.0f", st.energyLastHit);
        g.textR(rx, ly, g.fSmall, fade(kRed, flash), d);
    }
}

// The crash itself, on the whole view: a red rim that flares on impact and
// fades. A bar alone is a thing to be read; a pilot has to FEEL the hit while
// looking at the corner they are about to miss.
void drawHitFlash(const Ctx& g, const racesim::RaceState& st) {
    const float a = std::clamp(st.energyHitFlash / 0.7f, 0.0f, 1.0f);
    if (a <= 0.01f) return;
    const float t = std::min((g.x1 - g.x0), (g.y1 - g.y0)) * 0.09f;
    // Four soft bands rather than one border: a rim reads as damage, a rectangle
    // outline reads as a selection.
    const ImU32 c0 = fade(IM_COL32(255, 60, 50, 150), a * a);
    const ImU32 c1 = IM_COL32(255, 60, 50, 0);
    g.dl->AddRectFilledMultiColor(ImVec2(g.x0, g.y0), ImVec2(g.x0 + t, g.y1), c0, c1, c1, c0);
    g.dl->AddRectFilledMultiColor(ImVec2(g.x1 - t, g.y0), ImVec2(g.x1, g.y1), c1, c0, c0, c1);
    g.dl->AddRectFilledMultiColor(ImVec2(g.x0, g.y0), ImVec2(g.x1, g.y0 + t), c0, c0, c1, c1);
    g.dl->AddRectFilledMultiColor(ImVec2(g.x0, g.y1 - t), ImVec2(g.x1, g.y1), c1, c1, c0, c0);
}

// --- Lap + times, and the position badge under it ---------------------------
void drawLapBlock(const Ctx& g, const racesim::RaceState& st, float topInset) {
    const float w = 268.0f * g.S;
    const float ax = g.x0 + g.pad, ay = g.y0 + g.pad + topInset;
    const bool  pips = st.cpTotal > 0;
    const float h = (pips ? 104.0f : 86.0f) * g.S;
    g.panel(ax, ay, ax + w, ay + h, kInk, kGold);

    const float ix = ax + 14.0f * g.S, rx = ax + w - 14.0f * g.S;
    g.label(ix, ay + 9.0f * g.S, kDim, "LAP");
    char lap[24];
    if (st.raceLaps > 0) std::snprintf(lap, sizeof(lap), "%d/%d", st.raceLap + 1, st.raceLaps);
    else                 std::snprintf(lap, sizeof(lap), "%d", st.raceLap + 1);
    g.text(ix, ay + 20.0f * g.S, g.fBig, kText, lap);
    // The running lap time is the number a driver actually watches -> gold, right.
    g.textR(rx, ay + 9.0f * g.S, g.fSmall, kDim, "TIME");
    g.textR(rx, ay + 20.0f * g.S, g.fBig, kGold, fmtTime(st.lapClock).c_str());

    const float ty = ay + 20.0f * g.S + g.fBig + 4.0f * g.S;
    char b[48];
    std::snprintf(b, sizeof(b), "BEST %s", st.bestLap > 0.0f ? fmtTime(st.bestLap).c_str() : "--");
    g.text(ix, ty, g.fSmall, st.bestLap > 0.0f ? kCyan : kDim, b);
    std::snprintf(b, sizeof(b), "LAST %s", st.lastLap > 0.0f ? fmtTime(st.lastLap).c_str() : "--");
    g.textR(rx, ty, g.fSmall, kDim, b);

    // Checkpoint pips: one per gate, lit as it is flown. Reads at a glance,
    // which "CP 3/5" never did.
    if (pips) {
        const int done = static_cast<int>(st.cpPassed.size());
        const float r = 4.0f * g.S, step = 13.0f * g.S;
        const float py = ay + h - 14.0f * g.S;
        for (int i = 0; i < st.cpTotal; ++i) {
            const float px = ix + r + i * step;
            if (i < done) g.dl->AddCircleFilled(ImVec2(px, py), r, kCyan, 12);
            else          g.dl->AddCircle(ImVec2(px, py), r, IM_COL32(255, 255, 255, 90), 12, 1.0f);
        }
    }

    // Position badge: the other number a driver watches. Sits right under the
    // lap block so the whole left column is one glance.
    if (st.playerPlace > 0 && st.standings.size() > 1) {
        const float bw = 132.0f * g.S, bh = 62.0f * g.S;
        const float by = ay + h + 8.0f * g.S;
        g.panel(ax, by, ax + bw, by + bh, kInk, placeColor(st.playerPlace));
        g.label(ax + 14.0f * g.S, by + 8.0f * g.S, kDim, "POSITION");
        char p[8]; std::snprintf(p, sizeof(p), "%d", st.playerPlace);
        const float px = ax + 14.0f * g.S, py = by + 18.0f * g.S;
        g.text(px, py, g.fBig, placeColor(st.playerPlace), p);
        const float ow = g.textW(g.fBig, p);
        g.text(px + ow + 2.0f * g.S, py + 3.0f * g.S, g.fSmall,
               placeColor(st.playerPlace), ordinal(st.playerPlace));
        char of[16]; std::snprintf(of, sizeof(of), "/ %d", static_cast<int>(st.standings.size()));
        g.textR(ax + bw - 14.0f * g.S, py + g.fBig - g.fSmall * 1.2f, g.fSmall, kDim, of);
    }
}

// --- The field, in running order (top right) --------------------------------
void drawStandings(const Ctx& g, const racesim::RaceState& st) {
    const std::vector<racesim::RaceState::Standing>& rows = st.standings;
    const float rh = 24.0f * g.S, w = 264.0f * g.S;
    const float h  = 30.0f * g.S + rh * rows.size() + 8.0f * g.S;
    const float ax = g.x1 - g.pad - w, ay = g.y0 + g.pad;
    g.panel(ax, ay, ax + w, ay + h, kInk);
    g.label(ax + 12.0f * g.S, ay + 8.0f * g.S, kDim, "POSITION");
    g.dl->AddLine(ImVec2(ax + 12.0f * g.S, ay + 26.0f * g.S),
                  ImVec2(ax + w - 12.0f * g.S, ay + 26.0f * g.S), kEdge, 1.0f);

    float y = ay + 30.0f * g.S;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const racesim::RaceState::Standing& s = rows[i];
        const int place = static_cast<int>(i) + 1;
        if (s.isPlayer) {   // your own row, lit so it is found without reading
            g.dl->AddRectFilled(ImVec2(ax + 4.0f * g.S, y), ImVec2(ax + w - 4.0f * g.S, y + rh),
                                IM_COL32(255, 212, 106, 26), 3.0f * g.S);
            g.dl->AddRectFilled(ImVec2(ax + 4.0f * g.S, y), ImVec2(ax + 6.0f * g.S, y + rh), kGold);
        }
        placeChip(g, ax + 12.0f * g.S, y + 3.0f * g.S, rh - 6.0f * g.S, place, 1.0f);
        const float nx = ax + 12.0f * g.S + (rh - 6.0f * g.S) * 1.15f + 8.0f * g.S;
        g.text(nx, y + (rh - g.fBody) * 0.5f, g.fBody, s.isPlayer ? kGold : kText,
               s.name.c_str());
        // Right column: finish time, or how far back this racer is running.
        char right[32];
        if (s.finished)    std::snprintf(right, sizeof(right), "%s", fmtTime(s.totalTime).c_str());
        else if (place == 1) std::snprintf(right, sizeof(right), "L%d", s.lap + 1);
        else if (s.gap >= 1000.0f) std::snprintf(right, sizeof(right), "+%.1fkm", s.gap * 0.001f);
        else               std::snprintf(right, sizeof(right), "+%.0fm", s.gap);
        g.textR(ax + w - 12.0f * g.S, y + (rh - g.fSmall) * 0.5f, g.fSmall,
                s.finished ? kGreen : kDim, right);
        y += rh;
    }
}

// --- Centre-of-screen moments: boost, missed gate, countdown ----------------
void drawBanners(const Ctx& g, const racesim::RaceState& st, float topInset) {
    float y = g.y0 + g.pad + topInset;
    if (st.gliderBoosting) {
        // Pulsing chip -- a boost is a felt event, so it moves.
        const float p = 0.72f + 0.28f * std::sin(g.time * 12.0f);
        const char* s = "> > >   B O O S T   > > >";
        const float tw = g.textW(g.fBody, s);
        const float bw = tw + 26.0f * g.S, bh = g.fBody + 14.0f * g.S;
        const float ax = g.cx - bw * 0.5f;
        g.dl->AddRectFilled(ImVec2(ax, y), ImVec2(ax + bw, y + bh),
                            fade(IM_COL32(12, 40, 56, 220), p), 4.0f * g.S);
        g.dl->AddRect(ImVec2(ax, y), ImVec2(ax + bw, y + bh), fade(kCyan, p), 4.0f * g.S, 0, 1.5f);
        g.textC(g.cx, y + 7.0f * g.S, g.fBody, fade(kCyan, p), s);
        y += bh + 8.0f * g.S;
    }
    if (st.raceMissedFlash > 0.0f) {
        const float a = std::clamp(st.raceMissedFlash / 1.2f, 0.0f, 1.0f);
        const char* s = "MISSED A CHECKPOINT";
        const float bw = g.textW(g.fBody, s) + 26.0f * g.S, bh = g.fBody + 14.0f * g.S;
        const float ax = g.cx - bw * 0.5f;
        g.dl->AddRectFilled(ImVec2(ax, y), ImVec2(ax + bw, y + bh),
                            fade(IM_COL32(58, 14, 12, 225), a), 4.0f * g.S);
        g.dl->AddRect(ImVec2(ax, y), ImVec2(ax + bw, y + bh), fade(kRed, a), 4.0f * g.S, 0, 1.5f);
        g.textC(g.cx, y + 7.0f * g.S, g.fBody, fade(kRed, a), s);
        y += bh + 8.0f * g.S;
    }
    // Idle hint before the first crossing (only when the scene has a line).
    if (!st.raceActive && !st.raceFinished && st.raceHasLine &&
        st.raceCountdown <= 0.0f && !st.onGrid)
        g.textC(g.cx, y, g.fBody, IM_COL32(220, 224, 232, 200), "CROSS THE START LINE TO BEGIN");
}

// The grid hold: lined up, still, waiting to be asked for.
//
// Deliberately quiet, and low on the screen. What the moment is FOR is the craft
// on the grid and the camera going round it; a HUD that competes with that has
// misunderstood which of the two the player came to look at.
void drawGridHold(const Ctx& g, const racesim::RaceState& st) {
    if (!st.onGrid) return;
    // Fade in over the first second so it does not snap on over the cut.
    const float a  = std::clamp(st.gridTime, 0.0f, 1.0f);
    const float cy = g.y0 + (g.y1 - g.y0) * 0.78f;
    g.textC(g.cx, cy - g.fBig * 1.5f, g.fBig, fade(kGold, a), "ON THE GRID");
    // A slow pulse under the prompt -- enough to read as "waiting for you"
    // without turning into something that has to be looked away from.
    const float pulse = 0.70f + 0.30f * std::sin(st.gridTime * 2.6f);
    g.textC(g.cx, cy, g.fBody, fade(kText, a * pulse),
            "PRESS ENTER  /  PAD A  TO START");
}

// Ready / Set / Go: three lights over one big word, both popping on the change.
void drawCountdown(const Ctx& g, const racesim::RaceState& st) {
    if (st.raceCountdown <= 0.0f && st.goFlash <= 0.0f) return;
    const char* txt; ImU32 col; int lit; float phaseT; float alpha = 1.0f;
    if (st.raceCountdown > 1.5f)      { txt = "READY"; col = kRed;   lit = 1; phaseT = 3.0f - st.raceCountdown; }
    else if (st.raceCountdown > 0.0f) { txt = "SET";   col = kGold;  lit = 2; phaseT = 1.5f - st.raceCountdown; }
    else                              { txt = "GO!";   col = kGreen; lit = 3; phaseT = 1.3f - st.goFlash;
                                        alpha = std::clamp(st.goFlash / 0.6f, 0.0f, 1.0f); }
    // Pop: overshoot on the phase change, settling in ~0.3 s.
    const float pop = 1.0f + 0.28f * std::exp(-phaseT * 7.0f);
    const float cy  = g.y0 + (g.y1 - g.y0) * 0.42f;

    const float r = 9.0f * g.S, step = 30.0f * g.S;
    for (int i = 0; i < 3; ++i) {
        const ImVec2 p(g.cx + (i - 1) * step, cy - 34.0f * g.S);
        if (i < lit) {
            g.dl->AddCircleFilled(p, r, fade(col, alpha), 20);
            g.dl->AddCircle(p, r * 1.6f, fade(col, 0.35f * alpha), 20, 2.0f);
        } else {
            g.dl->AddCircle(p, r, fade(IM_COL32(255, 255, 255, 110), alpha), 20, 1.5f);
        }
    }
    const float fsz = g.fHuge * pop;
    g.dl->AddText(g.font, fsz, ImVec2(g.cx - g.textW(fsz, txt) * 0.5f + 3.0f * g.S, cy + 3.0f * g.S),
                  fade(IM_COL32(0, 0, 0, 205), alpha), txt);
    g.dl->AddText(g.font, fsz, ImVec2(g.cx - g.textW(fsz, txt) * 0.5f, cy),
                  fade(col, alpha), txt);
}

// A big menu button. Selected = gold fill and border; hovering selects it too,
// so the mouse never has to hit anything small or be held steady while clicking.
bool endButton(const Ctx& g, float ax, float ay, float bx, float by,
               const char* label, bool selected, bool mouseUsable, int* hovered) {
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 m = io.MousePos;
    const bool over = mouseUsable && m.x >= ax && m.x <= bx && m.y >= ay && m.y <= by;
    if (over && hovered) *hovered = 1;
    const bool lit = selected || over;
    g.dl->AddRectFilled(ImVec2(ax, ay), ImVec2(bx, by),
                        lit ? IM_COL32(255, 212, 106, 40) : IM_COL32(14, 18, 26, 220),
                        6.0f * g.S);
    g.dl->AddRect(ImVec2(ax, ay), ImVec2(bx, by), lit ? kGold : kEdge,
                  6.0f * g.S, 0, lit ? 2.0f : 1.0f);
    g.textC((ax + bx) * 0.5f, (ay + by) * 0.5f - g.fBody * 0.5f, g.fBody,
            lit ? kGold : kText, label);
    return over && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

// --- Final classification, then the "what now?" question --------------------
EndAction drawResults(const Ctx& g, const racesim::RaceState& st,
                      EndPrompt& p, const EndInput& in) {
    const std::vector<racesim::RaceState::Standing>& rows = st.standings;
    p.boardT += ImGui::GetIO().DeltaTime;
    // Dim the world so the board is the only thing to look at.
    g.dl->AddRectFilled(ImVec2(g.x0, g.y0), ImVec2(g.x1, g.y1), IM_COL32(4, 6, 10, 140));

    const float rh = 30.0f * g.S, w = 420.0f * g.S;
    const float head = 96.0f * g.S, foot = 44.0f * g.S;
    const float h = head + rh * std::max<std::size_t>(rows.size(), 1) + foot;
    // The question sits under the board; centre board + question as one block.
    const float extra = (p.asked ? 104.0f : 40.0f) * g.S;
    const float ax = g.cx - w * 0.5f;
    const float ay = g.y0 + (g.y1 - g.y0) * 0.5f - (h + extra) * 0.5f;
    g.panel(ax, ay, ax + w, ay + h, kInkDeep, kGold);

    // Losing the hull is its own ending, and it has to say so plainly: the board
    // is otherwise the same one that comes up after a clean race, and a pilot who
    // reads "RACE OVER" learns nothing about why they are looking at it.
    const bool lost = st.energyOut;
    g.label(ax + 18.0f * g.S, ay + 12.0f * g.S, lost ? kRed : kDim,
            lost ? "OUT OF ENERGY" : "RACE OVER");
    const std::string win = lost ? std::string("HULL DESTROYED")
                          : st.winnerName.empty() ? std::string("--")
                                                  : st.winnerName;
    g.text(ax + 18.0f * g.S, ay + 26.0f * g.S, g.fBig,
           lost ? kRed : (st.winnerIsPlayer ? kGold : kText), win.c_str());
    g.textR(ax + w - 18.0f * g.S, ay + 30.0f * g.S, g.fSmall, kDim,
            lost ? "RETIRED" : "WINNER");
    if (!lost && st.winnerTime > 0.0f)
        g.textR(ax + w - 18.0f * g.S, ay + 30.0f * g.S + g.fSmall * 1.4f, g.fSmall,
                kGreen, fmtTime(st.winnerTime).c_str());
    g.dl->AddLine(ImVec2(ax + 18.0f * g.S, ay + head - 10.0f * g.S),
                  ImVec2(ax + w - 18.0f * g.S, ay + head - 10.0f * g.S), kEdge, 1.0f);

    float y = ay + head;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const racesim::RaceState::Standing& s = rows[i];
        const int place = static_cast<int>(i) + 1;
        if (s.isPlayer) {
            g.dl->AddRectFilled(ImVec2(ax + 8.0f * g.S, y), ImVec2(ax + w - 8.0f * g.S, y + rh),
                                IM_COL32(255, 212, 106, 30), 3.0f * g.S);
            g.dl->AddRectFilled(ImVec2(ax + 8.0f * g.S, y), ImVec2(ax + 10.5f * g.S, y + rh), kGold);
        }
        placeChip(g, ax + 18.0f * g.S, y + 4.0f * g.S, rh - 8.0f * g.S, place, 1.0f);
        const float nx = ax + 18.0f * g.S + (rh - 8.0f * g.S) * 1.15f + 10.0f * g.S;
        g.text(nx, y + (rh - g.fBody) * 0.5f, g.fBody, s.isPlayer ? kGold : kText, s.name.c_str());
        char right[40];
        if (s.finished)      std::snprintf(right, sizeof(right), "%s", fmtTime(s.totalTime).c_str());
        else                 std::snprintf(right, sizeof(right), "LAP %d  +%.0fm", s.lap + 1, s.gap);
        g.textR(ax + w - 18.0f * g.S, y + (rh - g.fBody) * 0.5f, g.fBody,
                s.finished ? kGreen : kDim, right);
        // The best lap of the race, alongside whoever set it.
        if (s.bestLap > 0.0f) {
            char bl[24]; std::snprintf(bl, sizeof(bl), "best %s", fmtTime(s.bestLap).c_str());
            g.textR(ax + w - 18.0f * g.S - 132.0f * g.S, y + (rh - g.fSmall) * 0.5f,
                    g.fSmall, kDim, bl);
        }
        y += rh;
    }
    if (st.playerPlace > 0) {
        char pb[64];
        std::snprintf(pb, sizeof(pb), "YOUR PLACE   %d%s  OF %d", st.playerPlace,
                      ordinal(st.playerPlace), static_cast<int>(rows.size()));
        g.textC(g.cx, y + 12.0f * g.S, g.fBody, kGold, pb);
    }

    // --- Read the board first, decide second --------------------------------
    // A short arming delay keeps a button still held from the last corner from
    // skipping straight past the summary.
    const float qy = ay + h + 14.0f * g.S;
    if (!p.asked) {
        if (p.boardT > 1.0f) {
            const float blink = 0.55f + 0.45f * std::sin(g.time * 3.0f);
            g.textC(g.cx, qy, g.fSmall, fade(kDim, blink),
                    "PRESS ENTER  /  PAD A  TO CONTINUE");
        }
        if (in.confirm && p.boardT > 0.8f) { p.asked = true; p.boardT = 0.0f; }
        return EndAction::None;
    }

    // The question. Two targets, deliberately large: this is answered with the
    // keyboard or a pad as readily as with the mouse, and hovering picks.
    if (in.prev) p.choice = 0;
    if (in.next) p.choice = 1;
    p.choice = std::clamp(p.choice, 0, 1);
    g.textC(g.cx, qy, g.fSmall, kDim, "RACE AGAIN, OR BACK TO THE START SCREEN?");

    const float bw = 200.0f * g.S, bh = 52.0f * g.S, gap = 16.0f * g.S;
    const float bx0 = g.cx - bw - gap * 0.5f, by0 = qy + 22.0f * g.S;
    const bool  mouse = !ImGui::GetIO().WantCaptureMouse;
    int hoverA = 0, hoverB = 0;
    const bool clickA = endButton(g, bx0, by0, bx0 + bw, by0 + bh,
                                  "RACE AGAIN", p.choice == 0, mouse, &hoverA);
    const float bx1 = g.cx + gap * 0.5f;
    const bool clickB = endButton(g, bx1, by0, bx1 + bw, by0 + bh,
                                  "START SCREEN", p.choice == 1, mouse, &hoverB);
    // Hovering moves the selection, so a confirm key always fires what is lit.
    if (hoverA) p.choice = 0;
    if (hoverB) p.choice = 1;

    if (clickA) return EndAction::RaceAgain;
    if (clickB) return EndAction::StartScreen;
    if (in.confirm && p.boardT > 0.25f)
        return p.choice == 0 ? EndAction::RaceAgain : EndAction::StartScreen;
    return EndAction::None;
}

} // namespace

EndAction draw(ImDrawList* dl, const ImVec2& vmin, const ImVec2& vsize,
               const racesim::RaceState& st, float topInset,
               EndPrompt& prompt, const EndInput& in) {
    if (!dl) return EndAction::None;
    Ctx g;
    g.dl   = dl;
    g.font = ImGui::GetFont();
    g.x0 = vmin.x; g.y0 = vmin.y;
    g.x1 = vmin.x + vsize.x; g.y1 = vmin.y + vsize.y;
    g.cx = vmin.x + vsize.x * 0.5f;
    g.S  = std::clamp(vsize.y / 900.0f, 0.55f, 2.2f);
    g.pad    = 20.0f * g.S;
    g.fSmall = 14.0f * g.S;
    g.fBody  = 18.0f * g.S;
    g.fBig   = 34.0f * g.S;
    g.fHuge  = 86.0f * g.S;
    g.time   = static_cast<float>(ImGui::GetTime());

    // The speed readout and the energy bar belong to the craft, not the race:
    // they show whenever a glider is flown, race or not. Losing the hull is not
    // a race event either -- it ends a practice run just as it ends a race.
    drawSpeed(g, st);
    drawEnergy(g, st);
    drawHitFlash(g, st);
    // Before the early-out below: the grid hold is the one moment where there is
    // a race to talk about and no race data to draw yet.
    drawGridHold(g, st);

    const bool inRace = st.raceHasLine || st.raceActive || st.raceFinished ||
                        !st.standings.empty();
    if (!inRace && !st.energyOut) return EndAction::None;

    EndAction act = EndAction::None;
    const bool over = st.raceFinished || st.raceOver || st.energyOut;
    if (over) {
        act = drawResults(g, st, prompt, in);
    } else {
        drawLapBlock(g, st, topInset);
        if (st.standings.size() > 1) drawStandings(g, st);
        drawBanners(g, st, topInset);
    }
    drawCountdown(g, st);
    return act;
}

} // namespace racehud
