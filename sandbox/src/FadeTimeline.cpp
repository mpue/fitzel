#include "FadeTimeline.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include <imgui.h>

#include "UiStyle.hpp"

namespace graphui {

namespace {

using animgraph::Transition;

ImU32 col(float r, float g, float b, float a = 1.0f) {
    return ImGui::GetColorU32(ImVec4(r, g, b, a));
}

float snapTo(float v, float step) { return std::round(v / step) * step; }

// The span the line shows, HELD while a handle is dragged. A scale worked out
// from the value being dragged moves the handle out from under the pointer as it
// goes, and the drag chases its own tail. Only one transition is edited at a
// time, so one held span is enough.
bool  g_held     = false;
float g_heldSpan = 0.0f;
// Where a drag of the new state's bar began, so it moves by the pointer's travel
// rather than jumping to put the bar's start under the pointer.
float g_offsetAtPress = 0.0f;

// Seconds between ruler ticks: the first round step that leaves room for a label.
float tickStep(float span, float px, float minPx) {
    static const float steps[] = {0.05f, 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 30.0f};
    for (float s : steps)
        if (s / span * px >= minPx) return s;
    return 60.0f;
}

} // namespace

// Seconds since the state being left was entered: the clock exit time is
// measured on (see animgraph::ready). Without exit time the arrow goes whenever
// its conditions hold, which is no particular moment -- so it is put half-way
// along, a place rather than a time.
float fadeExitAt(const Transition& t, const FadeView& v) {
    const float len = v.from.length;
    return t.hasExitTime && len > 1e-4f ? std::max(t.exitTime, 0.0f) * len
                                        : 0.5f * (len > 1e-4f ? len : 1.0f);
}

// The same arithmetic animgraph::step does, laid out along the line: the old
// state plays from 0, the new one from the exit -- at its offset, from its end
// if it runs backward -- and the old one's share falls along the curve.
FadePose fadePoseAt(const Transition& t, const FadeView& v, float at) {
    FadePose p;
    const float exitAt = fadeExitAt(t, v);
    p.fromPhase = at * v.from.rate;
    if (at < exitAt) return p;
    p.toOn    = true;
    p.toPhase = std::max(t.offset, 0.0f) * (v.to.rate < 0.0f ? -1.0f : 1.0f) +
                (at - exitAt) * v.to.rate;
    p.fromShare = t.fade > 0.0f && at < exitAt + t.fade
                ? 1.0f - animgraph::fadeShare(t.curve, (at - exitAt) / t.fade)
                : 0.0f;
    return p;
}

bool fadeTimeline(const char* id, Transition& t, const FadeView& v, FadePreview* preview) {
    const float em = ImGui::GetFontSize();
    bool changed = false;
    ImGui::PushID(id);

    const float fromLen   = v.from.length;
    const bool  exitKnown = t.hasExitTime && fromLen > 1e-4f;
    const float exitAt    = fadeExitAt(t, v);
    const float fade    = std::max(t.fade, 0.0f);
    const float fadeEnd = exitAt + fade;
    const float fromRate = std::fabs(v.from.rate);
    const float toRate   = std::fabs(v.to.rate);
    const float fromCycle = fromLen > 1e-4f && fromRate > 1e-4f ? fromLen / fromRate : 0.0f;
    // How fast the new clip's own seconds pass on this line; a state standing
    // still (speed 0) still has its offset dragged at one to one.
    const float toPace = toRate > 1e-4f ? toRate : 1.0f;

    float span = fadeEnd + std::max(0.35f * fadeEnd, 0.3f);
    // Held while dragging -- but allowed to grow when the fade is dragged up to
    // the right-hand edge, which is how a longer fade than the view is reached.
    if (g_held) span = std::max(g_heldSpan, fadeEnd * 1.08f);

    // --- Preview: the buttons, and the play head moving on ------------------
    if (preview) {
        const bool canShow = preview->target && preview->target[0];
        ImGui::BeginDisabled(!canShow);
        if (ImGui::Button(preview->playing ? "Pause##pv" : "Play##pv", ImVec2(em * 4.5f, 0.0f))) {
            preview->playing = !preview->playing;
            if (preview->playing && (preview->at < 0.0f || preview->at >= span)) preview->at = 0.0f;
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop##pv")) { preview->playing = false; preview->at = -1.0f; }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(em * 5.0f);
        int sp = preview->speed < 0.375f ? 2 : preview->speed < 0.75f ? 1 : 0;
        if (ImGui::Combo("##pvspeed", &sp, "1x\0" "0.5x\0" "0.25x\0"))
            preview->speed = sp == 0 ? 1.0f : sp == 1 ? 0.5f : 0.25f;
        ImGui::SetItemTooltip("Preview speed: a fade is easier to judge slowed down");
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (canShow)
            ui::hint("Preview on %s -- or click the ruler to scrub.", preview->target);
        else
            ui::hint("No figure runs this graph: give one an Animation Graph component "
                     "(or select it) to preview the fade on it.");
        if (!canShow) { preview->playing = false; preview->at = -1.0f; }
        // Round and round: the whole line, so the lead-up to the exit is seen
        // every time, not only the hand-over itself.
        if (preview->playing) {
            preview->at += ImGui::GetIO().DeltaTime * preview->speed;
            if (preview->at >= span) preview->at = 0.0f;
        }
    }

    // --- Geometry ---------------------------------------------------------
    const ImVec2 p0     = ImGui::GetCursorScreenPos();
    const float  W      = std::max(ImGui::GetContentRegionAvail().x, em * 20.0f);
    const float  labelW = em * 6.5f;
    const float  rulerH = em * 1.3f, rowH = em * 1.6f, gap = em * 0.35f, curveH = em * 3.2f;
    const float  H      = rulerH + rowH + gap + rowH + gap + curveH + em * 0.5f;
    const float  tx0 = p0.x + labelW, tx1 = p0.x + W - em * 0.8f;
    const float  yRuler = p0.y, yFrom = yRuler + rulerH, yTo = yFrom + rowH + gap,
                 yCurve = yTo + rowH + gap;
    const float  pxPerSec = (tx1 - tx0) / span;
    auto X = [&](float s) { return tx0 + s * pxPerSec; };
    auto T = [&](float x) { return (x - tx0) / pxPerSec; };
    const float hw = std::max(em * 0.45f, 5.0f);   // half a handle's grab width
    const ImVec2 mouse = ImGui::GetIO().MousePos;

    // --- The handles, submitted before anything is drawn so the drawing shows
    // this frame's values. The first item under the pointer wins it, so the
    // narrow handles go before the wide bar they sit on.

    // Exit: in the upper half, on the ruler and the state being left -- the end
    // of the old bar is where the arrow goes.
    ImGui::SetCursorScreenPos(ImVec2(X(exitAt) - hw, yRuler));
    ImGui::InvisibleButton("##exit", ImVec2(hw * 2.0f, yFrom + rowH - yRuler));
    const bool exitHot = ImGui::IsItemHovered(), exitActive = ImGui::IsItemActive();
    if (exitKnown) {
        if (exitHot || exitActive) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (exitActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            // In whole percent, like the slider beside it.
            const float e = std::clamp(snapTo(T(mouse.x) / fromLen, 0.01f), 0.0f, 1.0f);
            if (e != t.exitTime) { t.exitTime = e; changed = true; }
        }
    }

    // Fade end: in the lower half, on the new bar and the curve.
    ImGui::SetCursorScreenPos(ImVec2(X(fadeEnd) - hw, yTo));
    ImGui::InvisibleButton("##fadeEnd", ImVec2(hw * 2.0f, yCurve + curveH - yTo));
    const bool fadeHot = ImGui::IsItemHovered(), fadeActive = ImGui::IsItemActive();
    if (fadeHot || fadeActive) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (fadeActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        // In the stepper's 0.05 s, and inside its range.
        const float f = std::clamp(snapTo(T(mouse.x) - exitAt, 0.05f), 0.0f, 2.0f);
        if (f != t.fade) { t.fade = f; changed = true; }
    }

    // The new bar itself: slid left, the new clip starts further in. Its start
    // stays at the exit -- that is when the state is entered -- and what moves is
    // which part of the clip lies there; the part skipped is drawn as a ghost.
    const float ghostAt = exitAt - std::max(t.offset, 0.0f) / toPace;
    const float bodyX0  = std::max(tx0, std::min(X(ghostAt), X(exitAt)));
    ImGui::SetCursorScreenPos(ImVec2(bodyX0, yTo));
    ImGui::InvisibleButton("##toBar", ImVec2(std::max(tx1 - bodyX0, 1.0f), rowH));
    const bool bodyHot = ImGui::IsItemHovered(), bodyActive = ImGui::IsItemActive();
    if (bodyHot || bodyActive) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActivated()) g_offsetAtPress = t.offset;
    if (bodyActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const float dx  = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).x;
        const float top = v.to.length > 1e-4f ? v.to.length : 10.0f;
        const float o = std::clamp(snapTo(g_offsetAtPress - dx / pxPerSec * toPace, 0.01f),
                                   0.0f, top);
        if (o != t.offset) { t.offset = o; changed = true; }
    }
    // A double-click puts the new clip back at its start.
    if (bodyHot && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && t.offset != 0.0f) {
        t.offset = 0.0f;
        changed = true;
    }

    // Scrubbing: the ruler and the curve, wherever no handle has the pointer.
    // Holding still on a frame of the fade is how you see whether the pose in
    // the middle of it is one anybody would choose.
    bool scrubActive = false;
    if (preview && preview->target && preview->target[0]) {
        for (int zone = 0; zone < 2; ++zone) {
            const float y0 = zone == 0 ? yRuler : yCurve;
            const float h  = zone == 0 ? rulerH : curveH;
            ImGui::SetCursorScreenPos(ImVec2(tx0, y0));
            ImGui::InvisibleButton(zone == 0 ? "##scrubR" : "##scrubC", ImVec2(tx1 - tx0, h));
            if (ImGui::IsItemActive()) {
                scrubActive = true;
                preview->playing = false;
                preview->at = std::clamp(T(mouse.x), 0.0f, span);
            }
        }
    }

    const bool anyActive = exitActive || fadeActive || bodyActive || scrubActive;
    if (anyActive) { g_held = true; g_heldSpan = span; }
    else           g_held = false;

    // Reserve the space, now that the handles are placed inside it.
    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(ImVec2(W, H));

    // --- Drawing (with the values as they are after this frame's drags) -----
    const float exitNow = exitKnown ? std::max(t.exitTime, 0.0f) * fromLen : exitAt;
    const float fadeNow = std::max(t.fade, 0.0f);
    const float endNow  = exitNow + fadeNow;
    const float ghostNow = exitNow - std::max(t.offset, 0.0f) / toPace;
    const bool  mixes   = v.from.model && v.to.model;
    auto share = [&](float s) {   // the NEW state's share of the pose at time s
        if (s <= exitNow) return 0.0f;
        if (fadeNow <= 0.0f || s >= endNow) return 1.0f;
        return animgraph::fadeShare(t.curve, (s - exitNow) / fadeNow);
    };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + W, p0.y + H), true);
    dl->AddRectFilled(p0, ImVec2(p0.x + W, p0.y + H), col(0.09f, 0.10f, 0.12f), 4.0f);

    // Ruler, and a faint line through the rows at every tick.
    const float step = tickStep(span, tx1 - tx0, em * 3.2f);
    for (float s = 0.0f; s <= span + 1e-4f; s += step) {
        const float x = X(s);
        dl->AddLine(ImVec2(x, yRuler + rulerH * 0.6f), ImVec2(x, yFrom),
                    col(1.0f, 1.0f, 1.0f, 0.35f));
        dl->AddLine(ImVec2(x, yFrom), ImVec2(x, yCurve + curveH), col(1.0f, 1.0f, 1.0f, 0.05f));
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.2g s", s);
        dl->AddText(ImVec2(x + 3.0f, yRuler + 1.0f), col(0.62f, 0.66f, 0.74f), buf);
    }

    // The row captions: which state is which.
    auto caption = [&](float y, float h, const char* text, ImU32 c) {
        dl->PushClipRect(ImVec2(p0.x, y), ImVec2(tx0 - em * 0.4f, y + h), true);
        dl->AddText(ImVec2(p0.x + em * 0.5f, y + (h - em) * 0.5f), c, text);
        dl->PopClipRect();
    };
    caption(yFrom, rowH, v.from.name, col(0.70f, 0.80f, 1.0f));
    caption(yTo, rowH, v.to.name, col(0.66f, 0.92f, 0.70f));
    caption(yCurve, curveH, "share", col(0.62f, 0.66f, 0.74f));

    // A bar whose opacity follows a function of time -- the fade drawn as what
    // it is, a share of the pose changing as it plays.
    auto bar = [&](float s0, float s1, float y, ImVec4 c, auto alphaAt) {
        const float x0 = std::max(X(s0), tx0 - 1.0f), x1 = std::min(X(s1), tx1);
        if (x1 <= x0) return;
        const int n = std::max(1, static_cast<int>((x1 - x0) / 3.0f));
        for (int i = 0; i < n; ++i) {
            const float a = x0 + (x1 - x0) * static_cast<float>(i) / static_cast<float>(n);
            const float b = x0 + (x1 - x0) * static_cast<float>(i + 1) / static_cast<float>(n);
            const float al = alphaAt(T((a + b) * 0.5f));
            dl->AddRectFilled(ImVec2(a, y + 2.0f), ImVec2(b + 0.5f, y + rowH - 2.0f),
                              col(c.x, c.y, c.z, c.w * al));
        }
    };
    // Where a clip comes round again (a tick) or, played once, ends and holds
    // its last frame (dimmed from there on).
    auto clipMarks = [&](float firstEnd, float cycle, bool loop, float until, float y) {
        if (cycle <= 1e-4f) return;
        for (float s = firstEnd; s < until && s < span; s += cycle) {
            if (s <= 0.0f) continue;
            if (!loop) {
                dl->AddRectFilled(ImVec2(X(s), y + 2.0f),
                                  ImVec2(std::min(X(until), tx1), y + rowH - 2.0f),
                                  col(0.0f, 0.0f, 0.0f, 0.45f));
                if (X(until) - X(s) > em * 3.0f)
                    dl->AddText(ImVec2(X(s) + 4.0f, y + (rowH - em) * 0.5f),
                                col(1.0f, 1.0f, 1.0f, 0.55f), "holds");
                break;
            }
            dl->AddLine(ImVec2(X(s), y + 3.0f), ImVec2(X(s), y + rowH - 3.0f),
                        col(0.0f, 0.0f, 0.0f, 0.6f), 2.0f);
        }
    };

    const ImVec4 fromCol(0.36f, 0.52f, 0.86f, 1.0f), toCol(0.36f, 0.74f, 0.46f, 1.0f);
    // The state being left: whole until the exit, then fading out.
    bar(0.0f, endNow > exitNow ? endNow : exitNow, yFrom, fromCol, [&](float s) {
        return 0.12f + 0.88f * (1.0f - share(s));
    });
    clipMarks(fromCycle, fromCycle, v.from.loop, endNow, yFrom);

    // The state entered: the skipped start as a ghost, then fading in.
    if (ghostNow < exitNow)
        bar(ghostNow, exitNow, yTo, toCol, [](float) { return 0.16f; });
    bar(exitNow, span, yTo, toCol, [&](float s) { return 0.12f + 0.88f * share(s); });
    if (v.to.length > 1e-4f && toRate > 1e-4f) {
        // The new clip's seconds at line time s are offset + (s - exit) x rate,
        // so it comes round where that reaches a whole clip.
        const float cycle = v.to.length / toRate;
        clipMarks(exitNow + (v.to.length - std::max(t.offset, 0.0f)) / toRate, cycle,
                  v.to.loop, span, yTo);
    }
    if (t.offset > 0.0f && X(exitNow) - X(ghostNow) > em * 3.5f) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "+%.2g s", t.offset);
        dl->AddText(ImVec2(X(ghostNow) + 4.0f, yTo + (rowH - em) * 0.5f),
                    col(1.0f, 1.0f, 1.0f, 0.6f), buf);
    }

    // The curve: each state's share of the pose, the new one filled.
    {
        const float pad = em * 0.35f;
        const float cy0 = yCurve + pad, cy1 = yCurve + curveH - pad;
        dl->AddRectFilled(ImVec2(tx0, yCurve), ImVec2(tx1, yCurve + curveH),
                          col(1.0f, 1.0f, 1.0f, 0.03f), 3.0f);
        const float dim = mixes || fadeNow <= 0.0f ? 1.0f : 0.45f;
        std::vector<ImVec2> rise, fall;
        const int n = std::max(2, static_cast<int>((tx1 - tx0) / 2.0f));
        for (int i = 0; i <= n; ++i) {
            const float x = tx0 + (tx1 - tx0) * static_cast<float>(i) / static_cast<float>(n);
            const float w = share(T(x));
            rise.push_back(ImVec2(x, cy1 - w * (cy1 - cy0)));
            fall.push_back(ImVec2(x, cy0 + w * (cy1 - cy0)));
        }
        for (int i = 0; i < n; ++i)
            dl->AddQuadFilled(rise[i], rise[i + 1], ImVec2(rise[i + 1].x, cy1),
                              ImVec2(rise[i].x, cy1), col(toCol.x, toCol.y, toCol.z, 0.22f * dim));
        dl->AddPolyline(fall.data(), static_cast<int>(fall.size()),
                        col(fromCol.x, fromCol.y, fromCol.z, dim), 0, 2.0f);
        dl->AddPolyline(rise.data(), static_cast<int>(rise.size()),
                        col(toCol.x + 0.2f, toCol.y + 0.2f, toCol.z + 0.2f, dim), 0, 2.0f);

        char buf[64];
        if (fadeNow <= 0.0f)
            std::snprintf(buf, sizeof(buf), "cut");
        else
            std::snprintf(buf, sizeof(buf), "%.2f s %s", fadeNow, animgraph::curveName(t.curve));
        const float tw = ImGui::CalcTextSize(buf).x;
        float lx = (X(exitNow) + X(endNow)) * 0.5f - tw * 0.5f;
        lx = std::clamp(lx, tx0 + 2.0f, tx1 - tw - 2.0f);
        // On a plate, so the curves running under it do not cross it out.
        const ImVec2 la(lx - 3.0f, cy0 - pad * 0.5f);
        dl->AddRectFilled(la, ImVec2(lx + tw + 3.0f, la.y + em), col(0.09f, 0.10f, 0.12f, 0.85f), 3.0f);
        dl->AddText(ImVec2(lx, la.y), col(0.88f, 0.91f, 0.97f, 0.9f), buf);
        // Saying so beats letting the author tune a fade that never happens.
        if (!mixes && fadeNow > 0.0f) {
            const char* why = "cuts in the game: only model animations fade";
            const ImVec2 wa(tx0 + 4.0f, cy1 - em);
            dl->AddRectFilled(ImVec2(wa.x - 3.0f, wa.y),
                              ImVec2(wa.x + ImGui::CalcTextSize(why).x + 3.0f, wa.y + em),
                              col(0.09f, 0.10f, 0.12f, 0.85f), 3.0f);
            dl->AddText(wa, col(0.95f, 0.76f, 0.32f), why);
        }
    }

    // The handles, drawn over everything they move.
    {
        const float x = X(exitNow);
        const bool  lit = exitKnown && (exitHot || exitActive);
        const ImU32 c = exitKnown ? col(0.95f, 0.76f, 0.32f, lit ? 1.0f : 0.85f)
                                  : col(0.75f, 0.78f, 0.85f, 0.6f);
        if (exitKnown) {
            dl->AddLine(ImVec2(x, yFrom), ImVec2(x, yCurve + curveH), c, lit ? 2.5f : 1.5f);
        } else {
            for (float y = yFrom; y < yCurve + curveH; y += 8.0f)   // dashed: a place, not a time
                dl->AddLine(ImVec2(x, y), ImVec2(x, std::min(y + 4.0f, yCurve + curveH)), c, 1.5f);
        }
        const float k = em * 0.45f;
        dl->AddTriangleFilled(ImVec2(x - k, yFrom - k * 1.3f), ImVec2(x + k, yFrom - k * 1.3f),
                              ImVec2(x, yFrom), c);
    }
    {
        const float x  = X(endNow);
        const bool  lit = fadeHot || fadeActive;
        const ImU32 c  = col(0.88f, 0.91f, 0.97f, lit ? 1.0f : 0.75f);
        dl->AddLine(ImVec2(x, yTo), ImVec2(x, yCurve + curveH), c, lit ? 2.5f : 1.5f);
        const float k = em * 0.4f, ym = yTo + rowH * 0.5f;
        dl->AddQuadFilled(ImVec2(x, ym - k), ImVec2(x + k, ym), ImVec2(x, ym + k),
                          ImVec2(x - k, ym), c);
    }
    if (bodyHot || bodyActive)
        dl->AddRect(ImVec2(bodyX0, yTo + 1.0f), ImVec2(tx1, yTo + rowH - 1.0f),
                    col(1.0f, 1.0f, 1.0f, bodyActive ? 0.6f : 0.3f), 3.0f);

    // The selected object, crossing it.
    auto playhead = [&](float s) {
        const float x = X(std::clamp(s, 0.0f, span));
        dl->AddLine(ImVec2(x, yFrom), ImVec2(x, yCurve + curveH), col(0.45f, 0.95f, 0.55f), 2.0f);
        dl->AddCircleFilled(ImVec2(x, yFrom), em * 0.25f, col(0.45f, 0.95f, 0.55f));
    };
    if (v.liveInFade >= 0.0f)      playhead(exitNow + v.liveInFade);
    else if (v.liveInFrom >= 0.0f) playhead(std::min(v.liveInFrom, exitNow));

    // The preview's head: white, with a handle on the ruler to say it can be held.
    if (preview && preview->at >= 0.0f) {
        const float x = X(std::clamp(preview->at, 0.0f, span));
        const ImU32 c = col(1.0f, 1.0f, 1.0f, 0.95f);
        dl->AddLine(ImVec2(x, yRuler + rulerH * 0.4f), ImVec2(x, yCurve + curveH), c, 1.5f);
        const float k = em * 0.35f;
        dl->AddRectFilled(ImVec2(x - k, yRuler + 1.0f), ImVec2(x + k, yRuler + rulerH * 0.55f), c, 2.0f);
    }
    dl->PopClipRect();

    // --- What the pointer is over -----------------------------------------
    if (exitActive && exitKnown)
        ImGui::SetTooltip("Leaves %s %.0f%% through its clip (%.2f s)",
                          v.from.name, t.exitTime * 100.0f, exitNow);
    else if (fadeActive)
        ImGui::SetTooltip("Fade %.2f s", fadeNow);
    else if (bodyActive)
        ImGui::SetTooltip("%s starts %.2f s into its clip", v.to.name, t.offset);
    else if (scrubActive && preview) {
        const FadePose pp = fadePoseAt(t, v, preview->at);
        ImGui::SetTooltip("%.2f s\n%s %.0f%%   %s %.0f%%", preview->at, v.from.name,
                          (pp.toOn ? pp.fromShare : 1.0f) * 100.0f, v.to.name,
                          (pp.toOn ? 1.0f - pp.fromShare : 0.0f) * 100.0f);
    }
    else if (exitHot && !exitKnown)
        ImGui::SetTooltip("Goes whenever its conditions hold.\n"
                          "Tick \"Wait for the clip\" to place it on %s's clip.",
                          v.from.name);
    else if (exitHot)
        ImGui::SetTooltip("Drag: when %s may be left", v.from.name);
    else if (fadeHot)
        ImGui::SetTooltip("Drag: how long the hand-over takes");
    else if (bodyHot)
        ImGui::SetTooltip("Drag sideways: where in its clip %s starts\n"
                          "Double-click: from the beginning", v.to.name);
    else if (ImGui::IsMouseHoveringRect(ImVec2(tx0, yFrom), ImVec2(tx1, yCurve + curveH)) &&
             ImGui::IsWindowHovered()) {
        const float s = T(mouse.x);
        const float w = share(s);
        dl->AddLine(ImVec2(mouse.x, yFrom), ImVec2(mouse.x, yCurve + curveH),
                    col(1.0f, 1.0f, 1.0f, 0.3f));
        ImGui::SetTooltip("%.2f s\n%s %.0f%%   %s %.0f%%", s, v.from.name,
                          (1.0f - w) * 100.0f, v.to.name, w * 100.0f);
    }

    ImGui::PopID();
    return changed;
}

} // namespace graphui
