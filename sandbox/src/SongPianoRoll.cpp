// The pane under the Song Sequencer's lanes (SongPanel.hpp): a piano roll for
// the MIDI clip being edited, the settings and the waveform of an audio one.

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>

#include <imgui.h>

#include "SongPanelState.hpp"
#include "UiStyle.hpp"

namespace songui {

using sg::Clip;
using sg::Note;
using sg::Track;
using sg::TrackKind;
using sg::InstrumentKind;
using State = Panel::State;

namespace {

bool blackKey(int p) {
    const int k = ((p % 12) + 12) % 12;
    return k == 1 || k == 3 || k == 6 || k == 8 || k == 10;
}

std::int64_t floorTo(std::int64_t t, std::int64_t q) {
    if (q <= 1) return t;
    std::int64_t r = t / q;
    if (t < 0 && t % q) --r;
    return r * q;
}

// The notes the pane works on, selected or (if none is) all of them.
template <class F>
void forSelected(Clip& c, bool allIfNone, F f) {
    const bool any = std::any_of(c.notes.begin(), c.notes.end(), [](const Note& n) { return n.selected; });
    for (Note& n : c.notes)
        if (n.selected || (allIfNone && !any)) f(n);
}

void quantize(State& st, Clip& c) {
    const std::int64_t q = st.gridOf(st.prSnap);
    st.checkpoint();
    forSelected(c, true, [&](Note& n) { n.tick = std::llround(static_cast<double>(n.tick) / q) * q; });
    c.sortNotes();
    st.edited();
}

void transpose(State& st, Clip& c, int by) {
    bool any = false;
    forSelected(c, true, [&](Note&) { any = true; });
    if (!any) return;
    st.checkpoint();
    forSelected(c, true, [&](Note& n) { n.pitch = std::clamp(n.pitch + by, 0, 127); });
    st.edited();
}

void deleteNotes(State& st, Clip& c) {
    if (std::none_of(c.notes.begin(), c.notes.end(), [](const Note& n) { return n.selected; })) return;
    st.checkpoint();
    c.notes.erase(std::remove_if(c.notes.begin(), c.notes.end(), [](const Note& n) { return n.selected; }), c.notes.end());
    st.edited();
}

// The content tick the playhead is on, when it is inside the clip.
bool playheadIn(const State& st, const Clip& c, double& ct) {
    const double p = st.playTicks();
    if (p < static_cast<double>(c.start) || p >= static_cast<double>(c.end())) return false;
    ct = static_cast<double>(c.offset) + (p - static_cast<double>(c.start));
    if (c.loop > 0) ct = std::fmod(ct, static_cast<double>(c.loop));
    return true;
}

// The clip's place, length and loop, stepped a grid unit at a time.
void clipBar(State& st, Track& t, Clip& c) {
    const float em = ImGui::GetFontSize();
    sg::Song& s = st.song;
    const std::int64_t bar = s.ticksPerBar(), beat = s.ticksPerBeat();
    if (ImFont* bold = ui::boldFont()) ImGui::PushFont(bold);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted((c.name.empty() ? t.name : c.name).c_str());
    if (ui::boldFont()) ImGui::PopFont();
    ImGui::SameLine(0, 10);
    const std::int64_t g = std::max<std::int64_t>(1, st.gridOf(st.arrSnap));
    float start = static_cast<float>(static_cast<double>(c.start) / bar) + 1.0f;
    if (ui::stepper("##cstart", start, static_cast<float>(static_cast<double>(g) / bar), 1.0f, 100000.0f, "Bar %.2f", em * 8.0f)) {
        st.checkpoint();
        c.start = std::max<std::int64_t>(0, std::llround((start - 1.0f) * static_cast<double>(bar) / g) * g);
        t.sortClips();
        st.edited();
    }
    ImGui::SetItemTooltip("Where the clip starts (a click moves it a grid step)");
    ImGui::SameLine(0, 4);
    float len = static_cast<float>(static_cast<double>(c.length) / beat);
    if (ui::stepper("##clen", len, static_cast<float>(static_cast<double>(g) / beat), 0.0625f, 100000.0f, "%.2f beats", em * 9.0f)) {
        st.checkpoint();
        c.length = std::max<std::int64_t>(1, std::llround(len * static_cast<double>(beat)));
        st.edited();
    }
    ImGui::SetItemTooltip("How long the clip is");
    ImGui::SameLine(0, 4);
    bool mute = c.mute;
    if (ImGui::Checkbox("Mute", &mute)) {
        st.checkpoint();
        c.mute = mute;
        st.edited();
    }
}

// --- The piano roll ------------------------------------------------------------------------

void noteTools(State& st, Track& t, Clip& c) {
    const float em = ImGui::GetFontSize();
    sg::Song& s = st.song;
    const std::int64_t beat = s.ticksPerBeat();
    ImGui::SameLine(0, 10);
    bool loop = c.loop > 0;
    if (ImGui::Checkbox("Loop", &loop)) {
        st.checkpoint();
        c.loop = loop ? std::max<std::int64_t>(beat, c.length) : 0;
        st.edited();
    }
    ImGui::SetItemTooltip("Repeat the content for as long as the clip is");
    if (c.loop > 0) {
        ImGui::SameLine(0, 4);
        float lb = static_cast<float>(static_cast<double>(c.loop) / beat);
        if (ui::stepper("##loopl", lb, 1.0f, 0.25f, 4096.0f, "every %.2f beats", em * 10.0f)) {
            st.checkpoint();
            c.loop = std::max<std::int64_t>(beat / 4, std::llround(lb * static_cast<double>(beat)));
            st.edited();
        }
    }
    ImGui::SameLine(0, 12);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Grid");
    ImGui::SameLine(0, 4);
    ImGui::SetNextItemWidth(em * 4.5f);
    if (ImGui::BeginCombo("##prsnap", snaps()[static_cast<std::size_t>(st.prSnap)].label)) {
        for (int i = 0; i < static_cast<int>(snaps().size()); ++i)
            if (ImGui::Selectable(snaps()[static_cast<std::size_t>(i)].label, st.prSnap == i)) {
                st.prSnap = i;
                st.lastLen = std::max<std::int64_t>(st.gridOf(i), sg::kPpq / 32);
            }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("What notes snap to (hold Alt to place freely)");
    ImGui::SameLine(0, 4);
    if (ImGui::Button("Quantize")) quantize(st, c);
    ImGui::SetItemTooltip("The selected notes (or all) onto the grid (Q)");
    ImGui::SameLine(0, 10);
    if (ImGui::Button("-12")) transpose(st, c, -12);
    ImGui::SameLine(0, 2);
    if (ImGui::Button("-1")) transpose(st, c, -1);
    ImGui::SameLine(0, 2);
    if (ImGui::Button("+1")) transpose(st, c, 1);
    ImGui::SameLine(0, 2);
    if (ImGui::Button("+12")) transpose(st, c, 12);
    ImGui::SetItemTooltip("Transpose the selected notes (or all): Up/Down, Shift for an octave");
    // The velocity of what is selected, or the one the next note gets.
    ImGui::SameLine(0, 10);
    int selVel = -1, nSel = 0;
    for (const Note& n : c.notes)
        if (n.selected) { selVel = selVel < 0 ? n.velocity : selVel; ++nSel; }
    float v = static_cast<float>(nSel ? selVel : st.lastVel);
    if (ui::stepper("##vel", v, 5.0f, 1.0f, 127.0f, nSel ? "Velocity %.0f" : "New: vel %.0f", em * 10.0f)) {
        if (nSel) {
            st.checkpoint();
            for (Note& n : c.notes)
                if (n.selected) n.velocity = static_cast<int>(v);
            st.edited();
        }
        st.lastVel = static_cast<int>(v);
    }
    ImGui::SetItemTooltip(nSel ? "The velocity of the selected notes" : "The velocity a drawn note gets");
    ImGui::SameLine(0, 4);
    if (ImGui::Button("x1/2")) {
        st.checkpoint();
        forSelected(c, true, [](Note& n) { n.length = std::max<std::int64_t>(sg::kPpq / 32, n.length / 2); });
        st.edited();
    }
    ImGui::SetItemTooltip("Halve the selected notes' lengths");
    ImGui::SameLine(0, 2);
    if (ImGui::Button("x2")) {
        st.checkpoint();
        forSelected(c, true, [](Note& n) { n.length *= 2; });
        st.edited();
    }
    ImGui::SetItemTooltip("Double the selected notes' lengths");
    ImGui::SameLine(0, 2);
    if (ImGui::Button("Legato")) {
        st.checkpoint();
        c.sortNotes();
        for (std::size_t i = 0; i < c.notes.size(); ++i) {
            if (!c.notes[i].selected && std::any_of(c.notes.begin(), c.notes.end(), [](const Note& n) { return n.selected; })) continue;
            for (std::size_t j = i + 1; j < c.notes.size(); ++j)
                if (c.notes[j].tick > c.notes[i].tick) { c.notes[i].length = c.notes[j].tick - c.notes[i].tick; break; }
        }
        st.edited();
    }
    ImGui::SetItemTooltip("Each note held until the next one starts");
    (void)t;
}

void pianoRoll(State& st, const Host& h, Track& t, Clip& c, ImVec2 size) {
    sg::Song& s = st.song;
    ImGuiIO& io = ImGui::GetIO();
    const float em = ImGui::GetFontSize();
    const bool drums = t.kind == TrackKind::Instrument && t.instrument.kind == InstrumentKind::Drums;
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float kw = std::round(em * (drums ? 7.5f : 3.6f));
    const float rulerH = std::round(em * 1.4f);
    const float velH = std::round(std::min(em * 4.5f, size.y * 0.25f));
    const float gx0 = o.x + kw, gx1 = o.x + size.x - 4.0f;
    const float gy0 = o.y + rulerH, gy1 = o.y + size.y - velH - 4.0f;
    const float gw = std::max(40.0f, gx1 - gx0), gh = std::max(20.0f, gy1 - gy0);
    const std::int64_t bar = s.ticksPerBar(), beat = s.ticksPerBeat();
    const std::int64_t grid = std::max<std::int64_t>(1, st.gridOf(st.prSnap));
    // A MIDI track's notes are heard through the instrument it plays: the
    // engine routes a hand-played note by the track it is played on.
    const int trackUid = t.uid;

    // The view fitted to a clip the first time it is shown.
    const std::int64_t contentEnd = c.loop > 0 ? c.loop : c.offset + c.length;
    const std::int64_t contentStart = c.loop > 0 ? 0 : c.offset;
    if (st.prFitFor != c.uid || st.prPpt <= 0.0) {
        st.prFitFor = c.uid;
        const double span = static_cast<double>(std::max<std::int64_t>(contentEnd - contentStart, bar)) * 1.06;
        st.prPpt = static_cast<double>(gw) / span;
        st.prView0 = static_cast<double>(contentStart) - span * 0.01;
        int hi = 0, lo = 127;
        for (const Note& n : c.notes) { hi = std::max(hi, n.pitch); lo = std::min(lo, n.pitch); }
        const float rows = gh / st.rowH;
        if (hi >= lo) st.prTop = std::min(127.0f, static_cast<float>((hi + lo) / 2) + rows * 0.5f);
        else st.prTop = drums ? 52.0f : 78.0f;
    }
    st.prTop = std::clamp(st.prTop, std::min(127.0f, gh / st.rowH), 127.0f);
    auto xOf = [&](double ct) { return gx0 + static_cast<float>((ct - st.prView0) * st.prPpt); };
    auto tOf = [&](float x) { return st.prView0 + static_cast<double>(x - gx0) / st.prPpt; };
    auto yOf = [&](int p) { return gy0 + (st.prTop - static_cast<float>(p)) * st.rowH; };
    auto pitchAt = [&](float y) { return std::clamp(static_cast<int>(std::floor(st.prTop - (y - gy0) / st.rowH)) + 1, 0, 127); };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = trackColor(s, t);

    // --- The rows and the grid -------------------------------------------------------------
    dl->AddRectFilled(ImVec2(gx0, gy0), ImVec2(gx1, gy1), IM_COL32(26, 28, 34, 255));
    dl->PushClipRect(ImVec2(gx0, gy0), ImVec2(gx1, gy1), true);
    for (int p = 127; p >= 0; --p) {
        const float y = yOf(p);
        if (y > gy1 || y + st.rowH < gy0) continue;
        if (blackKey(p)) dl->AddRectFilled(ImVec2(gx0, y), ImVec2(gx1, y + st.rowH), IM_COL32(21, 23, 28, 255));
        if (p % 12 == 0) dl->AddLine(ImVec2(gx0, y + st.rowH), ImVec2(gx1, y + st.rowH), IM_COL32(56, 60, 72, 255));
    }
    {
        const double t0 = tOf(gx0), t1 = tOf(gx1);
        const bool showGrid = grid * st.prPpt > 5.0;
        const std::int64_t step = showGrid ? grid : beat;
        for (std::int64_t k = floorTo(static_cast<std::int64_t>(t0), step); static_cast<double>(k) <= t1; k += step) {
            const float x = xOf(static_cast<double>(k));
            const ImU32 lc = k % bar == 0 ? IM_COL32(80, 86, 100, 255) : k % beat == 0 ? IM_COL32(50, 54, 64, 255) : IM_COL32(38, 41, 49, 255);
            dl->AddLine(ImVec2(x, gy0), ImVec2(x, gy1), lc);
        }
    }
    // Outside the clip's content: shaded -- notes there are not played.
    dl->AddRectFilled(ImVec2(gx0, gy0), ImVec2(xOf(static_cast<double>(contentStart)), gy1), IM_COL32(0, 0, 0, 90));
    dl->AddRectFilled(ImVec2(xOf(static_cast<double>(contentEnd)), gy0), ImVec2(gx1, gy1), IM_COL32(0, 0, 0, 90));

    // --- The notes ----------------------------------------------------------------------------
    struct NoteRect { int index; ImVec2 a, b; };
    std::vector<NoteRect> rects;
    for (std::size_t i = 0; i < c.notes.size(); ++i) {
        const Note& n = c.notes[i];
        const float x0 = xOf(static_cast<double>(n.tick)), x1 = std::max(x0 + 3.0f, xOf(static_cast<double>(n.end())));
        const float y0 = yOf(n.pitch);
        if (x1 < gx0 || x0 > gx1 || y0 > gy1 || y0 + st.rowH < gy0) continue;
        const ImVec2 a(x0, y0 + 0.5f), b(x1 - 0.5f, y0 + st.rowH - 0.5f);
        rects.push_back({static_cast<int>(i), a, b});
        const bool outside = n.tick < contentStart || n.tick >= contentEnd;
        const float k = 0.55f + 0.75f * static_cast<float>(n.velocity) / 127.0f;
        ImU32 fill = n.selected ? IM_COL32(250, 236, 180, 255) : mul(col, k);
        if (outside) fill = (fill & 0x00FFFFFF) | (90u << 24);
        dl->AddRectFilled(a, b, fill, 2.0f);
        dl->AddRect(a, b, n.selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 140), 2.0f);
        if (b.x - a.x > em * 2.2f && st.rowH >= em * 0.8f)
            dl->AddText(ImVec2(a.x + 3.0f, a.y + (st.rowH - em) * 0.5f), IM_COL32(20, 20, 24, 220), sg::noteName(n.pitch).c_str());
        if (h.probe) h.probe("pr:note:" + std::to_string(i), a, b);
    }
    // The loop's ghosts: the content again, where it will play again.
    if (c.loop > 0)
        for (std::int64_t k = c.loop; static_cast<double>(k) < tOf(gx1) && k < c.loop * 64; k += c.loop)
            for (const Note& n : c.notes) {
                if (n.tick >= c.loop) continue;
                const float x0 = xOf(static_cast<double>(n.tick + k)), x1 = xOf(static_cast<double>(n.end() + k));
                if (x0 > gx1) break;
                const float y0 = yOf(n.pitch);
                dl->AddRect(ImVec2(x0, y0 + 0.5f), ImVec2(x1, y0 + st.rowH - 0.5f), mul(col, 1.0f, 0.35f), 2.0f);
            }
    if (st.pr.mode == PrDrag::Rubber) dl->AddRect(st.pr.press, io.MousePos, IM_COL32(255, 255, 255, 170));
    double ph = 0.0;
    if (playheadIn(st, c, ph)) {
        const float x = xOf(ph);
        dl->AddLine(ImVec2(x, gy0), ImVec2(x, gy1), IM_COL32(255, 255, 255, 200), 1.5f);
    }
    dl->PopClipRect();

    // --- The ruler: bars of the content, and its end to drag -----------------------------------------
    {
        const ImVec2 a(gx0, o.y), b(gx1, gy0);
        dl->AddRectFilled(a, b, IM_COL32(30, 33, 40, 255));
        dl->PushClipRect(a, b, true);
        const double t0 = tOf(gx0), t1 = tOf(gx1);
        for (std::int64_t k = floorTo(static_cast<std::int64_t>(t0), bar); static_cast<double>(k) <= t1; k += bar) {
            const float x = xOf(static_cast<double>(k));
            dl->AddLine(ImVec2(x, a.y + rulerH * 0.4f), ImVec2(x, b.y), IM_COL32(150, 156, 168, 200));
            char n[16];
            std::snprintf(n, sizeof n, "%lld", static_cast<long long>(k / bar + 1));
            dl->AddText(ImVec2(x + 3.0f, a.y + 1.0f), IM_COL32(200, 204, 214, 255), n);
        }
        const float ex = xOf(static_cast<double>(contentEnd));
        dl->AddRectFilled(ImVec2(xOf(static_cast<double>(contentStart)), b.y - 4.0f), ImVec2(ex, b.y),
                          c.loop > 0 ? IM_COL32(90, 150, 230, 230) : mul(col, 1.0f));
        dl->AddTriangleFilled(ImVec2(ex - 6.0f, a.y + 2.0f), ImVec2(ex, a.y + 2.0f), ImVec2(ex, b.y), IM_COL32(240, 240, 240, 220));
        dl->PopClipRect();
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("##prruler", ImVec2(b.x - a.x, b.y - a.y));
        if (ImGui::IsItemHovered() && std::fabs(io.MousePos.x - ex) < 7.0f) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActivated()) {
            st.prFocus = true;
            if (std::fabs(io.MousePos.x - ex) < 7.0f) {
                st.pr = {};
                st.pr.mode = PrDrag::ClipEnd;
                st.pr.base = s;
            } else {
                // The ruler moves the playhead, in the song: content tick -> song tick.
                const double ct = tOf(io.MousePos.x);
                st.seek(static_cast<std::int64_t>(std::max(0.0, static_cast<double>(c.start) + ct - static_cast<double>(c.offset))));
            }
        }
        if (ImGui::IsItemActive() && st.pr.mode == PrDrag::ClipEnd) {
            const std::int64_t e = std::max(contentStart + grid, st.snapTo(static_cast<std::int64_t>(tOf(io.MousePos.x)), st.prSnap));
            if (c.loop > 0) c.loop = e;
            else c.length = std::max<std::int64_t>(1, e - c.offset);
            st.pr.moved = true;
            st.edited();
        }
        if (ImGui::IsItemDeactivated() && st.pr.mode == PrDrag::ClipEnd) {
            if (st.pr.moved) st.checkpoint(st.pr.base);
            st.pr = {};
        }
    }

    // --- The keyboard --------------------------------------------------------------------------
    {
        const ImVec2 a(o.x, gy0), b(gx0 - 1.0f, gy1);
        dl->PushClipRect(a, b, true);
        for (int p = 127; p >= 0; --p) {
            const float y = yOf(p);
            if (y > gy1 || y + st.rowH < gy0) continue;
            const bool black = blackKey(p);
            const bool down = st.pr.mode == PrDrag::Keys && st.pr.lastPitch == p;
            const ImU32 kc = down ? IM_COL32(250, 200, 110, 255) : black ? IM_COL32(40, 42, 48, 255) : IM_COL32(214, 216, 222, 255);
            dl->AddRectFilled(ImVec2(a.x, y), ImVec2(black ? a.x + (b.x - a.x) * 0.62f : b.x, y + st.rowH - 1.0f), kc);
            const char* dn = drums ? drumName(p) : nullptr;
            if (dn && st.rowH >= em * 0.75f) {
                dl->AddText(ImVec2(a.x + 3.0f, y + (st.rowH - em) * 0.5f), black ? IM_COL32(230, 230, 236, 255) : IM_COL32(30, 30, 36, 255), dn);
            } else if (p % 12 == 0 && st.rowH >= em * 0.6f) {
                dl->AddText(ImVec2(b.x - ImGui::CalcTextSize("C-1").x - 3.0f, y + (st.rowH - em) * 0.5f), IM_COL32(60, 60, 70, 255),
                            sg::noteName(p).c_str());
            }
        }
        dl->PopClipRect();
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("##prkeys", ImVec2(b.x - a.x, b.y - a.y));
        if (h.probe) h.probe("pr:keys", a, b);
        if (ImGui::IsItemActivated()) {
            st.prFocus = true;
            st.pr = {};
            st.pr.mode = PrDrag::Keys;
            st.pr.lastPitch = pitchAt(io.MousePos.y);
            st.liveNote(trackUid, st.pr.lastPitch, st.lastVel, true);
        }
        if (ImGui::IsItemActive() && st.pr.mode == PrDrag::Keys) {
            const int p = pitchAt(io.MousePos.y);
            if (p != st.pr.lastPitch) {   // a glissando down the keys
                st.liveNote(trackUid, st.pr.lastPitch, 0, false);
                st.liveNote(trackUid, p, st.lastVel, true);
                st.pr.lastPitch = p;
            }
        }
        if (ImGui::IsItemDeactivated() && st.pr.mode == PrDrag::Keys) {
            st.liveNote(trackUid, st.pr.lastPitch, 0, false);
            st.pr = {};
        }
    }

    // --- Pointer on the grid ---------------------------------------------------------------------
    ImGui::SetCursorScreenPos(ImVec2(gx0, gy0));
    ImGui::InvisibleButton("##prgrid", ImVec2(gw, gh), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    if (h.probe) h.probe("pr", ImVec2(gx0, gy0), ImVec2(gx1, gy1));
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    enum Zone { Body, Left, Right };
    auto hit = [&](ImVec2 p, Zone& z) -> int {
        for (auto it = rects.rbegin(); it != rects.rend(); ++it) {
            if (p.x < it->a.x || p.x > it->b.x || p.y < it->a.y || p.y > it->b.y) continue;
            const float edge = std::min(6.0f, (it->b.x - it->a.x) * 0.3f);
            z = p.x > it->b.x - edge ? Right : p.x < it->a.x + edge ? Left : Body;
            return it->index;
        }
        return -1;
    };
    Zone hz = Body;
    const int under = hovered ? hit(io.MousePos, hz) : -1;
    if (under >= 0 && st.pr.mode == PrDrag::None && hz != Body) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    PrDrag& d = st.pr;
    const auto mt = static_cast<std::int64_t>(std::floor(tOf(io.MousePos.x)));
    const int mp = pitchAt(io.MousePos.y);
    const std::int64_t minLen = std::max<std::int64_t>(sg::kPpq / 32, std::min<std::int64_t>(grid, sg::kPpq / 4));
    auto addNote = [&](std::int64_t at, int pitch) {
        for (Note& n : c.notes) n.selected = false;
        Note n;
        n.tick = at;
        n.length = st.lastLen;
        n.pitch = pitch;
        n.velocity = st.lastVel;
        n.selected = true;
        c.notes.push_back(n);
        c.sortNotes();
        st.edited();
        st.audition(trackUid, pitch, st.lastVel);
        for (std::size_t i = 0; i < c.notes.size(); ++i)
            if (c.notes[i].selected) return static_cast<int>(i);
        return -1;
    };

    if (ImGui::IsItemActivated()) {
        st.prFocus = true;
        d = {};
        d.press = io.MousePos;
        d.pressTick = mt;
        d.pressPitch = mp;
        d.base = s;
        const bool right = ImGui::IsMouseClicked(ImGuiMouseButton_Right);
        Zone z = Body;
        const int ni = hit(io.MousePos, z);
        if (right || st.tool == Tool::Erase) {
            d.mode = PrDrag::Erase;
            if (ni >= 0) {
                c.notes.erase(c.notes.begin() + ni);
                d.moved = true;
                st.edited();
                rects.clear();   // they name indices, stale now
            }
        } else if (ni >= 0) {
            Note& n = c.notes[static_cast<std::size_t>(ni)];
            if (io.KeyCtrl) d.copy = true;
            else if (io.KeyShift) n.selected = true;
            else if (!n.selected) {
                for (Note& o2 : c.notes) o2.selected = false;
                n.selected = true;
            }
            d.base = s;   // with the selection as it is now
            d.note = ni;
            d.mode = z == Right ? PrDrag::ResizeR : z == Left ? PrDrag::ResizeL : PrDrag::Move;
            d.lastPitch = n.pitch;
            st.lastLen = n.length;
            st.lastVel = n.velocity;
            st.audition(trackUid, n.pitch, n.velocity);
        } else if (st.tool == Tool::Draw) {
            d.note = addNote(io.KeyAlt ? mt : floorTo(mt, grid), mp);
            d.mode = PrDrag::Draw;
            d.moved = true;
        } else {
            d.mode = PrDrag::Rubber;
            if (!io.KeyShift && !io.KeyCtrl)
                for (Note& n : c.notes) n.selected = false;
            d.base = s;
        }
    }

    if (ImGui::IsItemActive() && d.mode != PrDrag::None) {
        const bool far = std::fabs(io.MousePos.x - d.press.x) > 3.0f || std::fabs(io.MousePos.y - d.press.y) > 3.0f;
        switch (d.mode) {
        case PrDrag::Move: {
            if (!far && !d.moved) break;
            const Track* bt = nullptr;
            const Clip* bc = d.base.clip(c.uid, &bt);
            if (!bc || d.note < 0 || d.note >= static_cast<int>(bc->notes.size())) break;
            d.moved = true;
            const Note& n0 = bc->notes[static_cast<std::size_t>(d.note)];
            std::int64_t dt = st.snapTo(n0.tick + (mt - d.pressTick), st.prSnap) - n0.tick;
            int dp = mp - d.pressPitch;
            int lo = 127, hi = 0;
            std::int64_t first = INT64_MAX;
            for (const Note& n : bc->notes)
                if (n.selected) { lo = std::min(lo, n.pitch); hi = std::max(hi, n.pitch); first = std::min(first, n.tick); }
            dp = std::clamp(dp, -lo, 127 - hi);
            if (first != INT64_MAX) dt = std::max(dt, -first);
            std::vector<Note> notes;
            for (const Note& n : bc->notes) {
                if (n.selected) {
                    Note m = n;
                    m.tick += dt;
                    m.pitch += dp;
                    if (d.copy) {
                        Note keep = n;
                        keep.selected = false;
                        notes.push_back(keep);
                    }
                    notes.push_back(m);
                } else {
                    notes.push_back(n);
                }
            }
            c.notes = std::move(notes);
            c.sortNotes();
            st.edited();
            if (n0.pitch + dp != d.lastPitch) {
                d.lastPitch = n0.pitch + dp;
                st.audition(trackUid, d.lastPitch, n0.velocity);
            }
            break;
        }
        case PrDrag::ResizeR:
        case PrDrag::ResizeL: {
            const Clip* bc = d.base.clip(c.uid);
            if (!bc || d.note < 0 || d.note >= static_cast<int>(bc->notes.size())) break;
            d.moved = true;
            const Note& n0 = bc->notes[static_cast<std::size_t>(d.note)];
            std::vector<Note> notes = bc->notes;
            if (d.mode == PrDrag::ResizeR) {
                const std::int64_t e = std::max(n0.tick + minLen, st.snapTo(mt, st.prSnap));
                const std::int64_t dl2 = e - n0.end();
                for (Note& n : notes)
                    if (n.selected) n.length = std::max(minLen, n.length + dl2);
                st.lastLen = std::max(minLen, n0.length + dl2);
            } else {
                const std::int64_t ns = std::min(n0.end() - minLen, st.snapTo(mt, st.prSnap));
                const std::int64_t ds = ns - n0.tick;
                for (Note& n : notes)
                    if (n.selected) {
                        const std::int64_t e = n.end();
                        n.tick = std::min(n.tick + ds, e - minLen);
                        n.length = e - n.tick;
                    }
            }
            c.notes = std::move(notes);
            c.sortNotes();
            st.edited();
            break;
        }
        case PrDrag::Draw: {
            for (Note& n : c.notes)
                if (n.selected) {
                    n.length = std::max(grid, st.snapTo(mt, st.prSnap) - n.tick);
                    st.lastLen = n.length;
                }
            st.edited();
            break;
        }
        case PrDrag::Erase: {
            Zone z;
            const int ni = hit(io.MousePos, z);
            if (ni >= 0) {
                c.notes.erase(c.notes.begin() + ni);
                d.moved = true;
                st.edited();
                // The rects name indices; the rest of this frame works on stale ones.
                rects.clear();
            }
            break;
        }
        case PrDrag::Rubber: {
            if (!far) break;
            d.moved = true;
            const ImVec2 a(std::min(d.press.x, io.MousePos.x), std::min(d.press.y, io.MousePos.y));
            const ImVec2 b(std::max(d.press.x, io.MousePos.x), std::max(d.press.y, io.MousePos.y));
            const Clip* bc = d.base.clip(c.uid);
            for (std::size_t i = 0; i < c.notes.size(); ++i)
                c.notes[i].selected = bc && i < bc->notes.size() && bc->notes[i].selected;
            for (const NoteRect& r : rects)
                if (r.b.x >= a.x && r.a.x <= b.x && r.b.y >= a.y && r.a.y <= b.y) c.notes[static_cast<std::size_t>(r.index)].selected = true;
            break;
        }
        default: break;
        }
    }

    if (ImGui::IsItemDeactivated() && d.mode != PrDrag::None) {
        switch (d.mode) {
        case PrDrag::Move:
            if (d.moved) st.checkpoint(d.base);
            else if (d.copy) {
                if (d.note >= 0 && d.note < static_cast<int>(c.notes.size())) c.notes[static_cast<std::size_t>(d.note)].selected ^= true;
            } else if (!io.KeyShift && d.note >= 0 && d.note < static_cast<int>(c.notes.size())) {
                for (Note& n : c.notes) n.selected = false;
                c.notes[static_cast<std::size_t>(d.note)].selected = true;
            }
            break;
        case PrDrag::ResizeL:
        case PrDrag::ResizeR:
        case PrDrag::Draw:
        case PrDrag::Erase:
            if (d.moved) st.checkpoint(d.base);
            break;
        case PrDrag::Rubber:
            if (!d.moved && !st.playing()) {
                // A click on the grid: the playhead to it, in the song.
                st.seek(static_cast<std::int64_t>(std::max(0.0, static_cast<double>(c.start) + st.snapTo(d.pressTick, st.prSnap) - static_cast<double>(c.offset))));
            }
            break;
        default: break;
        }
        d = {};
    }

    // Double-click on the empty grid: a note there.
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && st.tool == Tool::Select) {
        Zone z;
        if (hit(io.MousePos, z) < 0) {
            const sg::Song before = s;
            addNote(io.KeyAlt ? mt : floorTo(mt, grid), mp);
            st.checkpoint(before);
        }
        d = {};
    }

    // The wheel: up and down the keys; along with Shift; zoom with Ctrl; row
    // height with Alt.
    if (ImGui::IsMouseHoveringRect(ImVec2(o.x, o.y), ImVec2(gx1, gy1)) && (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f)) {
        if (io.KeyCtrl) {
            const double keep = tOf(io.MousePos.x);
            st.prPpt = std::clamp(st.prPpt * std::pow(1.18, static_cast<double>(io.MouseWheel)), 0.005, 4.0);
            st.prView0 = keep - static_cast<double>(io.MousePos.x - gx0) / st.prPpt;
        } else if (io.KeyAlt) {
            st.rowH = std::clamp(st.rowH + io.MouseWheel, 6.0f, 32.0f);
        } else if (io.KeyShift || io.MouseWheelH != 0.0f) {
            const float w = io.MouseWheelH != 0.0f ? io.MouseWheelH : io.MouseWheel;
            st.prView0 -= static_cast<double>(w) * gw * 0.12 / st.prPpt;
        } else {
            st.prTop = std::clamp(st.prTop + io.MouseWheel * 3.0f, gh / st.rowH, 127.0f);
        }
    }

    // --- Velocities -----------------------------------------------------------------------------
    {
        const ImVec2 a(gx0, gy1 + 4.0f), b(gx1, o.y + size.y);
        dl->AddRectFilled(a, b, IM_COL32(22, 24, 30, 255));
        dl->AddText(ImVec2(o.x + 4.0f, a.y + 2.0f), IM_COL32(140, 146, 158, 255), "Velocity");
        dl->PushClipRect(a, b, true);
        const float vh = b.y - a.y - 4.0f;
        for (const Note& n : c.notes) {
            const float x = xOf(static_cast<double>(n.tick));
            if (x < gx0 - 4.0f || x > gx1) continue;
            const float top = b.y - 2.0f - vh * static_cast<float>(n.velocity) / 127.0f;
            const ImU32 vc = n.selected ? IM_COL32(250, 236, 180, 255) : mul(col, 1.1f);
            dl->AddLine(ImVec2(x + 1.0f, b.y - 2.0f), ImVec2(x + 1.0f, top), vc, 2.0f);
            dl->AddCircleFilled(ImVec2(x + 1.0f, top), 2.5f, vc);
        }
        dl->PopClipRect();
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("##prvel", ImVec2(b.x - a.x, b.y - a.y));
        if (h.probe) h.probe("pr:vel", a, b);
        if (ImGui::IsItemActivated()) {
            st.prFocus = true;
            st.pr = {};
            st.pr.mode = PrDrag::Velocity;
            st.pr.base = s;
        }
        if (ImGui::IsItemActive() && st.pr.mode == PrDrag::Velocity) {
            // Painted: each stick the pointer passes takes its height.
            const int v = std::clamp(static_cast<int>(std::lround((b.y - 2.0f - io.MousePos.y) / vh * 127.0f)), 1, 127);
            const bool anySel = std::any_of(c.notes.begin(), c.notes.end(), [](const Note& n) { return n.selected; });
            for (Note& n : c.notes) {
                if (anySel && !n.selected) continue;
                const float x = xOf(static_cast<double>(n.tick));
                if (std::fabs(x - io.MousePos.x) <= 5.0f && n.velocity != v) {
                    n.velocity = v;
                    st.pr.moved = true;
                    st.edited();
                }
            }
        }
        if (ImGui::IsItemDeactivated() && st.pr.mode == PrDrag::Velocity) {
            if (st.pr.moved) st.checkpoint(st.pr.base);
            st.pr = {};
        }
    }
}

// --- An audio clip ----------------------------------------------------------------------------

void audioPane(State& st, const Host& h, Track& t, Clip& c, ImVec2 size) {
    sg::Song& s = st.song;
    const float em = ImGui::GetFontSize();
    ImGui::SameLine(0, 10);
    ImGui::SetNextItemWidth(em * 12.0f);
    if (ImGui::BeginCombo("##file", c.file.c_str())) {
        const auto names = h.soundNames ? h.soundNames() : std::vector<std::string>{};
        for (const std::string& n : names)
            if (ImGui::Selectable(n.c_str(), n == c.file)) {
                st.checkpoint();
                c.file = n;
                st.edited();
            }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("The sound this clip plays");
    ImGui::SameLine(0, 6);
    float db = c.gain > 1e-4f ? 20.0f * std::log10(c.gain) : -60.0f;
    if (ui::stepper("##gain", db, 0.5f, -60.0f, 12.0f, "Gain %+.1f dB", em * 10.0f)) {
        st.checkpoint();
        c.gain = db <= -60.0f ? 0.0f : std::pow(10.0f, db / 20.0f);
        st.edited();
    }
    ImGui::SameLine(0, 4);
    float fi = c.fadeIn, fo = c.fadeOut;
    if (ui::stepper("##fi", fi, 0.05f, 0.0f, 60.0f, "Fade in %.2f s", em * 10.0f)) { st.checkpoint(); c.fadeIn = fi; st.edited(); }
    ImGui::SameLine(0, 4);
    if (ui::stepper("##fo", fo, 0.05f, 0.0f, 60.0f, "Fade out %.2f s", em * 10.0f)) { st.checkpoint(); c.fadeOut = fo; st.edited(); }
    ImGui::SameLine(0, 4);
    float off = static_cast<float>(c.sourceOffset);
    if (ui::stepper("##off", off, 0.05f, 0.0f, 36000.0f, "From %.2f s", em * 10.0f)) {
        st.checkpoint();
        c.sourceOffset = off;
        st.edited();
    }
    ImGui::SetItemTooltip("Where in the file the clip starts");
    ImGui::SameLine(0, 4);
    const double secs = st.fileLength(c.file);
    if (ImGui::Button("Rest of the file") && secs > c.sourceOffset) {
        st.checkpoint();
        c.length = std::max<std::int64_t>(1, s.tickAt(secs - c.sourceOffset));
        st.edited();
    }
    ImGui::SetItemTooltip("Make the clip as long as what is left of its file");

    // The whole file, the part the clip plays bright.
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const ImVec2 a(o.x + 4.0f, o.y + 4.0f), b(o.x + size.x - 4.0f, o.y + std::max(30.0f, size.y - ImGui::GetFrameHeight() - 12.0f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, IM_COL32(22, 24, 30, 255), 3.0f);
    const fitzel::Waveform* w = st.waveform(c.file);
    if (!w || w->seconds <= 0.0f) {
        dl->AddText(ImVec2(a.x + 8.0f, a.y + 8.0f), IM_COL32(200, 200, 210, 160), w ? "This file could not be read" : "Reading the file...");
        return;
    }
    const ImU32 col = trackColor(s, t);
    const float mid = (a.y + b.y) * 0.5f, half = (b.y - a.y) * 0.46f;
    const float spp = w->seconds / (b.x - a.x);
    const double c0 = c.sourceOffset, c1 = c.sourceOffset + s.seconds(c.length);
    for (float x = a.x; x < b.x; x += 1.0f) {
        const float t0 = (x - a.x) * spp;
        const bool in = t0 >= c0 && t0 < c1;
        const float pk = std::min(1.0f, w->peakOver(t0, t0 + spp) * (in ? c.gain : 1.0f));
        const float rm = std::min(1.0f, w->rmsOver(t0, t0 + spp) * (in ? c.gain : 1.0f));
        dl->AddLine(ImVec2(x, mid - pk * half), ImVec2(x, mid + pk * half), in ? mul(col, 0.9f) : IM_COL32(70, 74, 84, 255));
        dl->AddLine(ImVec2(x, mid - rm * half), ImVec2(x, mid + rm * half), in ? mul(col, 1.3f) : IM_COL32(96, 100, 112, 255));
    }
    const float x0 = a.x + static_cast<float>(c0 / spp), x1 = a.x + static_cast<float>(std::min(c1, static_cast<double>(w->seconds)) / spp);
    dl->AddRect(ImVec2(x0, a.y), ImVec2(x1, b.y), IM_COL32(255, 255, 255, 180), 2.0f, 0, 1.5f);
    if (c.fadeIn > 0.0f) dl->AddLine(ImVec2(x0, b.y), ImVec2(x0 + c.fadeIn / spp, a.y), IM_COL32(255, 255, 255, 160), 1.5f);
    if (c.fadeOut > 0.0f) dl->AddLine(ImVec2(x1 - c.fadeOut / spp, a.y), ImVec2(x1, b.y), IM_COL32(255, 255, 255, 160), 1.5f);
    char info[96];
    std::snprintf(info, sizeof info, "%s, %.2f s", c.file.c_str(), static_cast<double>(w->seconds));
    dl->AddText(ImVec2(a.x + 6.0f, a.y + 4.0f), IM_COL32(220, 222, 230, 200), info);
    ImGui::Dummy(ImVec2(size.x, b.y - o.y));
}

} // namespace

void drawEditorPane(State& st, const Host& h, ImVec2 size) {
    ImGui::BeginChild("##editorpane", size, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) st.prFocus = true;
    Track* t = nullptr;
    Clip* c = st.editClip >= 0 ? st.song.clip(st.editClip, &t) : nullptr;
    if (!c || !t) {
        ImGui::Dummy(ImVec2(1.0f, ImGui::GetFontSize() * 0.5f));
        ui::hint("Click a clip to edit it here. Double-click an empty spot on an instrument or MIDI track for a new "
                 "clip, or draw one with Draw (2); drop a sound or a MIDI file on the lanes. Space plays.");
        ImGui::EndChild();
        return;
    }
    clipBar(st, *t, *c);
    if (t->kind == TrackKind::Audio) {
        audioPane(st, h, *t, *c, ImVec2(size.x, size.y - ImGui::GetFrameHeight() - 6.0f));
    } else {
        noteTools(st, *t, *c);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        pianoRoll(st, h, *t, *c, ImVec2(size.x, std::max(40.0f, size.y - (at.y - ImGui::GetWindowPos().y) - 2.0f)));
    }
    ImGui::EndChild();
}

bool editorPaneKeys(State& st) {
    Track* t = nullptr;
    Clip* c = st.editClip >= 0 ? st.song.clip(st.editClip, &t) : nullptr;
    if (!c || !t || t->kind == TrackKind::Audio) return false;
    ImGuiIO& io = ImGui::GetIO();
    const bool anySel = std::any_of(c->notes.begin(), c->notes.end(), [](const Note& n) { return n.selected; });
    if (io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_A, false)) {
            for (Note& n : c->notes) n.selected = true;
            return true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_C, false) || ImGui::IsKeyPressed(ImGuiKey_X, false)) {
            st.noteBoard.clear();
            std::int64_t first = INT64_MAX;
            for (const Note& n : c->notes)
                if (n.selected) first = std::min(first, n.tick);
            for (const Note& n : c->notes)
                if (n.selected) {
                    Note k = n;
                    k.tick -= first;
                    st.noteBoard.push_back(k);
                }
            if (ImGui::IsKeyPressed(ImGuiKey_X, false)) deleteNotes(st, *c);
            st.say(std::to_string(st.noteBoard.size()) + " note(s) copied");
            return true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_V, false) && !st.noteBoard.empty()) {
            // At the playhead when it is in the clip, else after the last note.
            double ct = 0.0;
            std::int64_t at;
            if (playheadIn(st, *c, ct)) at = st.snapTo(static_cast<std::int64_t>(ct), st.prSnap);
            else {
                at = 0;
                for (const Note& n : c->notes) at = std::max(at, n.end());
                at = floorTo(at + st.gridOf(st.prSnap) - 1, st.gridOf(st.prSnap));
            }
            st.checkpoint();
            for (Note& n : c->notes) n.selected = false;
            for (Note n : st.noteBoard) {
                n.tick += at;
                n.selected = true;
                c->notes.push_back(n);
            }
            c->sortNotes();
            st.edited();
            return true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_D, false) && anySel) {
            std::int64_t a = INT64_MAX, b = 0;
            for (const Note& n : c->notes)
                if (n.selected) { a = std::min(a, n.tick); b = std::max(b, n.end()); }
            const std::int64_t q = std::max<std::int64_t>(1, st.gridOf(st.prSnap));
            const std::int64_t span = ((b - a + q - 1) / q) * q;
            st.checkpoint();
            std::vector<Note> add;
            for (Note& n : c->notes)
                if (n.selected) {
                    Note k = n;
                    k.tick += span;
                    add.push_back(k);
                    n.selected = false;
                }
            c->notes.insert(c->notes.end(), add.begin(), add.end());
            c->sortNotes();
            st.edited();
            return true;
        }
        return false;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
        deleteNotes(st, *c);
        return true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        for (Note& n : c->notes) n.selected = false;
        return true;
    }
    if (st.qwerty) return false;
    if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) { quantize(st, *c); return true; }
    if (!anySel) return false;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) { transpose(st, *c, io.KeyShift ? 12 : 1); return true; }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) { transpose(st, *c, io.KeyShift ? -12 : -1); return true; }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        const std::int64_t step = io.KeyShift ? st.song.ticksPerBar() : st.gridOf(st.prSnap);
        const std::int64_t by = ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ? -step : step;
        st.checkpoint();
        forSelected(*c, false, [&](Note& n) { n.tick = std::max<std::int64_t>(0, n.tick + by); });
        c->sortNotes();
        st.edited();
        return true;
    }
    return false;
}

} // namespace songui
