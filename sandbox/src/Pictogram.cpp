#include "Pictogram.hpp"

#include <algorithm>
#include <cmath>

namespace picto {

namespace {

constexpr float kPi = 3.14159265f;

// The accent a chosen button's picture is drawn in -- the same amber the synth
// panel uses for "this one is on".
const ImU32 kAccent = IM_COL32(236, 172, 64, 255);

float stroke(float r) { return std::max(1.5f, r * 0.15f); }

ImVec2 at(ImVec2 c, float r, float x, float y) { return ImVec2(c.x + x * r, c.y + y * r); }

// A curve y = f(x) for x in -1..1, drawn across the icon. `f` returns -1..1
// with +1 at the TOP, the way a graph is read, not the way a screen counts.
template <class F>
void plot(ImDrawList* dl, ImVec2 c, float r, ImU32 col, F f, int steps = 28, float h = 0.62f) {
    ImVec2 pts[64];
    const int n = std::min(steps, 63);
    for (int i = 0; i <= n; ++i) {
        const float x = -0.9f + 1.8f * static_cast<float>(i) / static_cast<float>(n);
        pts[i] = at(c, r, x, -f(x) * h);
    }
    dl->AddPolyline(pts, n + 1, col, ImDrawFlags_None, stroke(r));
}

void arrowHead(ImDrawList* dl, ImVec2 tip, ImVec2 dir, float s, ImU32 col) {
    const ImVec2 n(-dir.y, dir.x);
    dl->AddTriangleFilled(tip,
                          ImVec2(tip.x - dir.x * s + n.x * s * 0.8f, tip.y - dir.y * s + n.y * s * 0.8f),
                          ImVec2(tip.x - dir.x * s - n.x * s * 0.8f, tip.y - dir.y * s - n.y * s * 0.8f),
                          col);
}

// The curved arrow of undo (and, mirrored, redo): over the top from one side to
// the other, the head coming down on the side the step goes back to.
void turnArrow(ImDrawList* dl, ImVec2 c, float r, ImU32 col, bool back) {
    const ImVec2 o = at(c, r, 0.0f, 0.3f);
    const float  R = r * 0.62f;
    dl->PathArcTo(o, R, kPi, 2.0f * kPi, 18);
    dl->PathStroke(col, ImDrawFlags_None, stroke(r));
    const float  sx  = back ? -1.0f : 1.0f;
    const ImVec2 end(o.x + sx * R, o.y + r * 0.05f);
    arrowHead(dl, ImVec2(end.x, end.y + r * 0.42f), ImVec2(0.0f, 1.0f), r * 0.42f, col);
}

void plus(ImDrawList* dl, ImVec2 p, float s, ImU32 col, float t) {
    dl->AddLine(ImVec2(p.x - s, p.y), ImVec2(p.x + s, p.y), col, t);
    dl->AddLine(ImVec2(p.x, p.y - s), ImVec2(p.x, p.y + s), col, t);
}

ImU32 fade(ImU32 col, float k) {
    const unsigned a = (col >> IM_COL32_A_SHIFT) & 0xFFu;
    return (col & ~IM_COL32_A_MASK) |
           (static_cast<ImU32>(static_cast<float>(a) * k) << IM_COL32_A_SHIFT);
}

// A box seen from a little above and to the right: the shape every modelling
// icon is drawn on, so "a corner", "an edge" and "a face" are all shown on the
// same object. The far edges are faint, the way a wireframe shows hidden lines.
struct Box {
    ImVec2 f[4], b[4];   // front face, back face: top-left, top-right, bottom-right, bottom-left
};
Box box(ImVec2 c, float r) {
    Box x;
    x.f[0] = at(c, r, -0.82f, -0.32f); x.f[1] = at(c, r, 0.32f, -0.32f);
    x.f[2] = at(c, r, 0.32f, 0.82f);   x.f[3] = at(c, r, -0.82f, 0.82f);
    x.b[0] = at(c, r, -0.32f, -0.82f); x.b[1] = at(c, r, 0.82f, -0.82f);
    x.b[2] = at(c, r, 0.82f, 0.32f);   x.b[3] = at(c, r, -0.32f, 0.32f);
    return x;
}
void drawBox(ImDrawList* dl, const Box& x, ImU32 col, float t) {
    for (int i = 0; i < 4; ++i) dl->AddLine(x.f[i], x.f[(i + 1) % 4], col, t);
    dl->AddLine(x.b[0], x.b[1], col, t);
    dl->AddLine(x.b[1], x.b[2], col, t);
    for (int i = 0; i < 3; ++i) dl->AddLine(x.f[i], x.b[i], col, t);
    const ImU32 hidden = fade(col, 0.35f);
    dl->AddLine(x.b[2], x.b[3], hidden, t * 0.7f);
    dl->AddLine(x.b[3], x.b[0], hidden, t * 0.7f);
    dl->AddLine(x.f[3], x.b[3], hidden, t * 0.7f);
}

// A dashed rectangle: "a selection", the marching ants every paint program has.
void dashedRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float t, float dash) {
    auto seg = [&](ImVec2 p, ImVec2 q) {
        const float len = std::hypot(q.x - p.x, q.y - p.y);
        const int   n   = std::max(1, static_cast<int>(len / (dash * 2.0f)));
        for (int i = 0; i < n; ++i) {
            const float u0 = (2.0f * i) / (2.0f * n), u1 = (2.0f * i + 1.0f) / (2.0f * n);
            dl->AddLine(ImVec2(p.x + (q.x - p.x) * u0, p.y + (q.y - p.y) * u0),
                        ImVec2(p.x + (q.x - p.x) * u1, p.y + (q.y - p.y) * u1), col, t);
        }
    };
    seg(a, ImVec2(b.x, a.y));
    seg(ImVec2(b.x, a.y), b);
    seg(b, ImVec2(a.x, b.y));
    seg(ImVec2(a.x, b.y), a);
}

// A straight arrow from `p` to `q`, head at `q`.
void arrow(ImDrawList* dl, ImVec2 p, ImVec2 q, float head, ImU32 col, float t) {
    const float  len = std::max(1e-3f, std::hypot(q.x - p.x, q.y - p.y));
    const ImVec2 d((q.x - p.x) / len, (q.y - p.y) / len);
    dl->AddLine(p, ImVec2(q.x - d.x * head * 0.6f, q.y - d.y * head * 0.6f), col, t);
    arrowHead(dl, q, d, head, col);
}

} // namespace

float size() { return std::round(ImGui::GetFrameHeight() * 1.45f); }

void draw(ImDrawList* dl, Icon icon, ImVec2 c, float r, ImU32 col) {
    const float t = stroke(r);
    switch (icon) {
    // --- files and history -------------------------------------------------
    case Icon::New: {
        // A blank sheet with its corner folded, and a plus: "a new one".
        const ImVec2 tl = at(c, r, -0.62f, -0.88f), br = at(c, r, 0.62f, 0.88f);
        const float  f  = r * 0.42f;
        const ImVec2 pts[] = {tl, ImVec2(br.x - f, tl.y), ImVec2(br.x, tl.y + f), br,
                              ImVec2(tl.x, br.y)};
        dl->AddPolyline(pts, 5, col, ImDrawFlags_Closed, t);
        dl->AddLine(ImVec2(br.x - f, tl.y), ImVec2(br.x - f, tl.y + f), col, t * 0.8f);
        dl->AddLine(ImVec2(br.x - f, tl.y + f), ImVec2(br.x, tl.y + f), col, t * 0.8f);
        plus(dl, at(c, r, 0.0f, 0.18f), r * 0.32f, col, t);
        break;
    }
    case Icon::Open: {
        // A folder with its front swung open.
        const ImVec2 tab[] = {at(c, r, -0.9f, -0.35f), at(c, r, -0.9f, -0.7f),
                              at(c, r, -0.35f, -0.7f), at(c, r, -0.2f, -0.5f),
                              at(c, r, 0.72f, -0.5f), at(c, r, 0.72f, -0.1f)};
        dl->AddPolyline(tab, 6, col, ImDrawFlags_None, t);
        const ImVec2 front[] = {at(c, r, -0.9f, -0.35f), at(c, r, -0.9f, 0.75f),
                                at(c, r, 0.62f, 0.75f), at(c, r, 0.95f, -0.1f),
                                at(c, r, -0.55f, -0.1f), at(c, r, -0.9f, 0.75f)};
        dl->AddPolyline(front, 6, col, ImDrawFlags_None, t);
        break;
    }
    case Icon::Save: {
        // The floppy disk. Nobody has used one in twenty years, and everybody
        // still reads it as "save" -- which is exactly what an icon is for.
        const ImVec2 a = at(c, r, -0.85f, -0.85f), b = at(c, r, 0.85f, 0.85f);
        dl->AddRect(a, b, col, r * 0.12f, 0, t);
        dl->AddRect(at(c, r, -0.45f, -0.85f), at(c, r, 0.4f, -0.3f), col, 0.0f, 0, t * 0.9f);
        dl->AddRectFilled(at(c, r, 0.12f, -0.75f), at(c, r, 0.28f, -0.4f), col);
        dl->AddRect(at(c, r, -0.55f, 0.1f), at(c, r, 0.55f, 0.85f), col, 0.0f, 0, t * 0.9f);
        break;
    }
    case Icon::Undo: turnArrow(dl, c, r, col, true);  break;
    case Icon::Redo: turnArrow(dl, c, r, col, false); break;
    case Icon::Tidy: {
        // Four boxes squared up in a grid: "put these in order".
        const float s = r * 0.62f, g = r * 0.16f;
        for (int i = 0; i < 4; ++i) {
            const float x = (i % 2 == 0) ? -g * 0.5f - s : g * 0.5f;
            const float y = (i < 2) ? -g * 0.5f - s : g * 0.5f;
            dl->AddRectFilled(ImVec2(c.x + x, c.y + y), ImVec2(c.x + x + s, c.y + y + s), col,
                              r * 0.1f);
        }
        break;
    }

    // --- sound --------------------------------------------------------------
    case Icon::Play:
        dl->AddTriangleFilled(at(c, r, -0.5f, -0.78f), at(c, r, -0.5f, 0.78f),
                              at(c, r, 0.82f, 0.0f), col);
        break;
    case Icon::Stop:
        dl->AddRectFilled(at(c, r, -0.62f, -0.62f), at(c, r, 0.62f, 0.62f), col, r * 0.1f);
        break;
    case Icon::Speaker: {
        dl->AddRectFilled(at(c, r, -0.9f, -0.3f), at(c, r, -0.48f, 0.3f), col);
        dl->AddQuadFilled(at(c, r, -0.48f, -0.3f), at(c, r, -0.05f, -0.72f),
                          at(c, r, -0.05f, 0.72f), at(c, r, -0.48f, 0.3f), col);
        for (int k = 0; k < 2; ++k) {
            dl->PathArcTo(at(c, r, -0.05f, 0.0f), r * (0.45f + 0.38f * k), -0.75f, 0.75f, 12);
            dl->PathStroke(col, ImDrawFlags_None, t);
        }
        break;
    }
    case Icon::Keys: {
        // A few piano keys.
        const ImVec2 a = at(c, r, -0.95f, -0.62f), b = at(c, r, 0.95f, 0.62f);
        dl->AddRect(a, b, col, r * 0.08f, 0, t);
        for (int i = 1; i < 4; ++i) {
            const float x = a.x + (b.x - a.x) * static_cast<float>(i) / 4.0f;
            dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), col, t * 0.7f);
            dl->AddRectFilled(ImVec2(x - r * 0.14f, a.y), ImVec2(x + r * 0.14f, c.y + r * 0.08f),
                              col);
        }
        break;
    }
    case Icon::Lock:
    case Icon::LockOpen: {
        // Held: the shackle is shut. Free: it is lifted off one side.
        const bool   open = icon == Icon::LockOpen;
        const ImVec2 o    = at(c, r, 0.0f, open ? -0.42f : -0.12f);
        dl->PathArcTo(o, r * 0.4f, kPi, 2.0f * kPi, 14);
        dl->PathStroke(col, ImDrawFlags_None, t);
        dl->AddLine(ImVec2(o.x - r * 0.4f, o.y), at(c, r, -0.4f, -0.05f), col, t);
        dl->AddLine(ImVec2(o.x + r * 0.4f, o.y), at(c, r, 0.4f, open ? -0.3f : -0.05f), col, t);
        dl->AddRectFilled(at(c, r, -0.62f, -0.08f), at(c, r, 0.62f, 0.82f), col, r * 0.1f);
        break;
    }

    // --- editing ------------------------------------------------------------
    case Icon::AddDial: {
        const ImVec2 k = at(c, r, -0.18f, 0.18f);
        dl->AddCircle(k, r * 0.6f, col, 20, t);
        dl->AddLine(k, ImVec2(k.x - r * 0.3f, k.y - r * 0.36f), col, t);
        plus(dl, at(c, r, 0.62f, -0.6f), r * 0.28f, col, t);
        break;
    }
    case Icon::Trash: {
        dl->AddLine(at(c, r, -0.78f, -0.55f), at(c, r, 0.78f, -0.55f), col, t);
        dl->AddRect(at(c, r, -0.25f, -0.82f), at(c, r, 0.25f, -0.55f), col, 0.0f, 0, t * 0.8f);
        const ImVec2 body[] = {at(c, r, -0.58f, -0.4f), at(c, r, 0.58f, -0.4f),
                               at(c, r, 0.45f, 0.85f), at(c, r, -0.45f, 0.85f)};
        dl->AddPolyline(body, 4, col, ImDrawFlags_Closed, t);
        dl->AddLine(at(c, r, -0.16f, -0.2f), at(c, r, -0.12f, 0.65f), col, t * 0.7f);
        dl->AddLine(at(c, r, 0.16f, -0.2f), at(c, r, 0.12f, 0.65f), col, t * 0.7f);
        break;
    }

    // --- waves: two periods of the shape itself ---------------------------
    case Icon::WaveSine:
        plot(dl, c, r, col, [](float x) { return std::sin((x + 0.9f) / 1.8f * 4.0f * kPi); });
        break;
    case Icon::WaveSaw: {
        const ImVec2 pts[] = {at(c, r, -0.9f, 0.55f), at(c, r, 0.0f, -0.55f),
                              at(c, r, 0.0f, 0.55f), at(c, r, 0.9f, -0.55f),
                              at(c, r, 0.9f, 0.55f)};
        dl->AddPolyline(pts, 5, col, ImDrawFlags_None, t);
        break;
    }
    case Icon::WavePulse: {
        const ImVec2 pts[] = {at(c, r, -0.9f, 0.55f), at(c, r, -0.9f, -0.55f),
                              at(c, r, -0.45f, -0.55f), at(c, r, -0.45f, 0.55f),
                              at(c, r, 0.0f, 0.55f), at(c, r, 0.0f, -0.55f),
                              at(c, r, 0.45f, -0.55f), at(c, r, 0.45f, 0.55f),
                              at(c, r, 0.9f, 0.55f)};
        dl->AddPolyline(pts, 9, col, ImDrawFlags_None, t);
        break;
    }
    case Icon::WaveNoise: {
        // Fixed "random" heights, so the icon does not flicker frame to frame.
        static const float h[] = {0.1f, -0.6f, 0.45f, -0.2f, 0.7f, -0.5f, 0.2f,
                                  -0.7f, 0.5f, -0.1f, 0.6f, -0.4f, 0.3f};
        ImVec2 pts[13];
        for (int i = 0; i < 13; ++i)
            pts[i] = at(c, r, -0.9f + 1.8f * static_cast<float>(i) / 12.0f, h[i] * 0.8f);
        dl->AddPolyline(pts, 13, col, ImDrawFlags_None, t);
        break;
    }

    // --- filters: what gets through, low notes on the left ------------------
    case Icon::LowPass:
    case Icon::HighPass:
    case Icon::BandPass: {
        dl->AddLine(at(c, r, -0.9f, 0.72f), at(c, r, 0.9f, 0.72f), col & 0x60FFFFFF, t * 0.7f);
        if (icon == Icon::LowPass) {
            dl->AddLine(at(c, r, -0.9f, -0.4f), at(c, r, 0.05f, -0.4f), col, t);
            dl->AddBezierCubic(at(c, r, 0.05f, -0.4f), at(c, r, 0.45f, -0.4f),
                               at(c, r, 0.5f, 0.7f), at(c, r, 0.9f, 0.7f), col, t);
        } else if (icon == Icon::HighPass) {
            dl->AddBezierCubic(at(c, r, -0.9f, 0.7f), at(c, r, -0.5f, 0.7f),
                               at(c, r, -0.45f, -0.4f), at(c, r, -0.05f, -0.4f), col, t);
            dl->AddLine(at(c, r, -0.05f, -0.4f), at(c, r, 0.9f, -0.4f), col, t);
        } else {
            dl->AddBezierCubic(at(c, r, -0.9f, 0.7f), at(c, r, -0.35f, 0.7f),
                               at(c, r, -0.3f, -0.55f), at(c, r, 0.0f, -0.55f), col, t);
            dl->AddBezierCubic(at(c, r, 0.0f, -0.55f), at(c, r, 0.3f, -0.55f),
                               at(c, r, 0.35f, 0.7f), at(c, r, 0.9f, 0.7f), col, t);
        }
        break;
    }

    // --- distortion: what the shape does to a signal (in across, out up) ---
    case Icon::ShapeSoft:
        plot(dl, c, r, col, [](float x) { return std::tanh(x * 2.2f) / std::tanh(2.0f); });
        break;
    case Icon::ShapeHard:
        plot(dl, c, r, col, [](float x) { return std::clamp(x * 2.0f, -1.0f, 1.0f); });
        break;
    case Icon::ShapeAtan:
        plot(dl, c, r, col, [](float x) { return std::atan(x * 1.4f) / std::atan(1.3f); });
        break;
    case Icon::ShapeFold:
        plot(dl, c, r, col,
             [](float x) {
                 float y = x * 2.4f;
                 while (y > 1.0f || y < -1.0f) {
                     if (y > 1.0f)  y =  2.0f - y;
                     if (y < -1.0f) y = -2.0f - y;
                 }
                 return y;
             },
             40);
        break;

    // --- modelling: what is picked -----------------------------------------
    case Icon::ModeVertex:
    case Icon::MakeEditable: {
        const Box x = box(c, r);
        drawBox(dl, x, fade(col, 0.55f), t * 0.7f);
        const ImVec2 dots[] = {x.f[0], x.f[1], x.f[2], x.f[3], x.b[0], x.b[1], x.b[2]};
        for (const ImVec2& d : dots) dl->AddCircleFilled(d, r * 0.17f, col, 10);
        break;
    }
    case Icon::ModeEdge: {
        const Box x = box(c, r);
        drawBox(dl, x, fade(col, 0.55f), t * 0.7f);
        dl->AddLine(x.f[1], x.b[1], col, t * 2.0f);
        dl->AddLine(x.f[0], x.f[1], col, t * 2.0f);
        break;
    }
    case Icon::ModeFace: {
        const Box x = box(c, r);
        dl->AddQuadFilled(x.f[0], x.f[1], x.f[2], x.f[3], fade(col, 0.55f));
        drawBox(dl, x, fade(col, 0.55f), t * 0.7f);
        dl->AddQuad(x.f[0], x.f[1], x.f[2], x.f[3], col, t * 1.4f);
        break;
    }
    case Icon::SelectAdd: {
        // The pointer, with a plus: "this click adds".
        const ImVec2 cur[] = {at(c, r, -0.62f, -0.85f), at(c, r, -0.62f, 0.42f),
                              at(c, r, -0.3f, 0.12f),   at(c, r, -0.08f, 0.62f),
                              at(c, r, 0.1f, 0.54f),    at(c, r, -0.12f, 0.05f),
                              at(c, r, 0.3f, 0.05f)};
        dl->AddConvexPolyFilled(cur, 3, col);   // the head...
        dl->AddQuadFilled(cur[2], cur[3], cur[4], cur[5], col);   // ...and the tail
        dl->AddTriangleFilled(cur[0], cur[5], cur[6], col);
        plus(dl, at(c, r, 0.55f, 0.55f), r * 0.3f, col, t);
        break;
    }
    case Icon::SelectAll:
    case Icon::SelectNone: {
        dashedRect(dl, at(c, r, -0.88f, -0.88f), at(c, r, 0.88f, 0.88f), col, t * 0.8f, r * 0.14f);
        for (int i = 0; i < 4; ++i) {
            const ImVec2 p = at(c, r, (i % 2) ? 0.38f : -0.38f, (i < 2) ? -0.38f : 0.38f);
            if (icon == Icon::SelectAll) dl->AddCircleFilled(p, r * 0.2f, col, 10);
            else                         dl->AddCircle(p, r * 0.2f, fade(col, 0.6f), 10, t * 0.7f);
        }
        break;
    }

    // --- modelling: what is done to it -------------------------------------
    case Icon::Extrude: {
        // A face, and a copy of it pulled up out of it.
        const ImVec2 lo[] = {at(c, r, -0.85f, 0.62f), at(c, r, 0.3f, 0.62f),
                             at(c, r, 0.85f, 0.25f), at(c, r, -0.3f, 0.25f)};
        ImVec2 hi[4];
        for (int i = 0; i < 4; ++i) hi[i] = ImVec2(lo[i].x, lo[i].y - r * 0.85f);
        dl->AddQuad(lo[0], lo[1], lo[2], lo[3], fade(col, 0.6f), t * 0.8f);
        for (int i = 0; i < 4; ++i) dl->AddLine(lo[i], hi[i], fade(col, i == 3 ? 0.35f : 0.8f), t * 0.8f);
        dl->AddQuadFilled(hi[0], hi[1], hi[2], hi[3], fade(col, 0.55f));
        dl->AddQuad(hi[0], hi[1], hi[2], hi[3], col, t);
        break;
    }
    case Icon::MoveNormal: {
        const ImVec2 f[] = {at(c, r, -0.85f, 0.2f), at(c, r, 0.3f, 0.2f),
                            at(c, r, 0.85f, -0.15f), at(c, r, -0.3f, -0.15f)};
        dl->AddQuadFilled(f[0], f[1], f[2], f[3], fade(col, 0.5f));
        dl->AddQuad(f[0], f[1], f[2], f[3], col, t);
        arrow(dl, at(c, r, 0.0f, 0.1f), at(c, r, 0.0f, -0.95f), r * 0.35f, col, t);
        arrow(dl, at(c, r, 0.0f, 0.05f), at(c, r, 0.0f, 0.95f), r * 0.35f, col, t);
        break;
    }
    case Icon::Inset: {
        const ImVec2 o0 = at(c, r, -0.88f, -0.88f), o1 = at(c, r, 0.88f, 0.88f);
        const ImVec2 i0 = at(c, r, -0.42f, -0.42f), i1 = at(c, r, 0.42f, 0.42f);
        dl->AddRect(o0, o1, col, 0.0f, 0, t);
        dl->AddRectFilled(i0, i1, fade(col, 0.6f));
        dl->AddRect(i0, i1, col, 0.0f, 0, t);
        dl->AddLine(o0, i0, fade(col, 0.6f), t * 0.7f);
        dl->AddLine(o1, i1, fade(col, 0.6f), t * 0.7f);
        dl->AddLine(ImVec2(o1.x, o0.y), ImVec2(i1.x, i0.y), fade(col, 0.6f), t * 0.7f);
        dl->AddLine(ImVec2(o0.x, o1.y), ImVec2(i0.x, i1.y), fade(col, 0.6f), t * 0.7f);
        break;
    }
    case Icon::ScaleFace: {
        dl->AddRectFilled(at(c, r, -0.35f, -0.35f), at(c, r, 0.35f, 0.35f), fade(col, 0.6f));
        dl->AddRect(at(c, r, -0.35f, -0.35f), at(c, r, 0.35f, 0.35f), col, 0.0f, 0, t);
        for (int i = 0; i < 4; ++i) {
            const float sx = (i % 2) ? 1.0f : -1.0f, sy = (i < 2) ? -1.0f : 1.0f;
            arrow(dl, at(c, r, sx * 0.45f, sy * 0.45f), at(c, r, sx * 0.92f, sy * 0.92f),
                  r * 0.28f, col, t * 0.9f);
        }
        break;
    }
    case Icon::LoopCutH:
    case Icon::LoopCutV: {
        // A box with a new edge running right round it.
        const Box x = box(c, r);
        drawBox(dl, x, fade(col, 0.55f), t * 0.7f);
        if (icon == Icon::LoopCutH) {
            const ImVec2 a(x.f[0].x, (x.f[0].y + x.f[3].y) * 0.5f);
            const ImVec2 b(x.f[1].x, a.y);
            const ImVec2 e(x.b[1].x, (x.b[1].y + x.b[2].y) * 0.5f);
            dl->AddLine(a, b, col, t * 1.7f);
            dl->AddLine(b, e, col, t * 1.7f);
        } else {
            const ImVec2 a((x.f[0].x + x.f[1].x) * 0.5f, x.f[3].y);
            const ImVec2 b(a.x, x.f[0].y);
            const ImVec2 e((x.b[0].x + x.b[1].x) * 0.5f, x.b[0].y);
            dl->AddLine(a, b, col, t * 1.7f);
            dl->AddLine(b, e, col, t * 1.7f);
        }
        break;
    }
    case Icon::Subdivide: {
        const ImVec2 a = at(c, r, -0.85f, -0.85f), b = at(c, r, 0.85f, 0.85f);
        dl->AddRect(a, b, col, 0.0f, 0, t);
        dl->AddLine(ImVec2(c.x, a.y), ImVec2(c.x, b.y), col, t * 0.8f);
        dl->AddLine(ImVec2(a.x, c.y), ImVec2(b.x, c.y), col, t * 0.8f);
        break;
    }
    case Icon::Merge: {
        // Two corners drawn together into one.
        dl->AddCircle(at(c, r, -0.72f, -0.62f), r * 0.17f, col, 10, t * 0.8f);
        dl->AddCircle(at(c, r, 0.72f, -0.62f), r * 0.17f, col, 10, t * 0.8f);
        arrow(dl, at(c, r, -0.55f, -0.45f), at(c, r, -0.12f, 0.3f), r * 0.3f, col, t);
        arrow(dl, at(c, r, 0.55f, -0.45f), at(c, r, 0.12f, 0.3f), r * 0.3f, col, t);
        dl->AddCircleFilled(at(c, r, 0.0f, 0.62f), r * 0.24f, col, 12);
        break;
    }
    case Icon::SplitEdge: {
        // An edge with a new corner in its middle.
        dl->AddLine(at(c, r, -0.85f, 0.0f), at(c, r, 0.85f, 0.0f), col, t);
        dl->AddCircleFilled(at(c, r, -0.85f, 0.0f), r * 0.13f, col, 8);
        dl->AddCircleFilled(at(c, r, 0.85f, 0.0f), r * 0.13f, col, 8);
        dl->AddCircleFilled(c, r * 0.26f, col, 12);
        plus(dl, at(c, r, 0.0f, -0.62f), r * 0.24f, col, t * 0.9f);
        break;
    }
    case Icon::Collapse: {
        // An edge squeezed down to a single corner.
        arrow(dl, at(c, r, -0.92f, 0.0f), at(c, r, -0.3f, 0.0f), r * 0.3f, col, t);
        arrow(dl, at(c, r, 0.92f, 0.0f), at(c, r, 0.3f, 0.0f), r * 0.3f, col, t);
        dl->AddCircleFilled(c, r * 0.2f, col, 12);
        break;
    }
    case Icon::Dissolve: {
        // Two faces with the edge between them fading out: one face.
        const ImVec2 a = at(c, r, -0.9f, -0.55f), b = at(c, r, 0.9f, 0.55f);
        dl->AddRectFilled(a, b, fade(col, 0.25f));
        dl->AddRect(a, b, col, 0.0f, 0, t);
        const float y0 = a.y, y1 = b.y;
        for (int i = 0; i < 4; ++i) {
            const float u0 = y0 + (y1 - y0) * (i * 2.0f) / 7.0f;
            const float u1 = y0 + (y1 - y0) * (i * 2.0f + 1.0f) / 7.0f;
            dl->AddLine(ImVec2(c.x, u0), ImVec2(c.x, u1), fade(col, 0.55f), t);
        }
        break;
    }
    case Icon::Bevel: {
        // A corner of a block with its edge cut away: the chamfer, filled.
        const ImVec2 p0 = at(c, r, -0.85f, 0.85f), p1 = at(c, r, -0.85f, -0.85f);
        const ImVec2 p2 = at(c, r, 0.2f, -0.85f),  p3 = at(c, r, 0.85f, -0.2f);
        const ImVec2 p4 = at(c, r, 0.85f, 0.85f);
        const ImVec2 poly[] = {p0, p1, p2, p3, p4};
        dl->AddConvexPolyFilled(poly, 5, fade(col, 0.3f));
        dl->AddPolyline(poly, 5, col, ImDrawFlags_Closed, t);
        dl->AddLine(p2, p3, col, t * 2.2f);
        dl->AddLine(at(c, r, 0.2f, -0.85f), at(c, r, 0.85f, -0.85f), fade(col, 0.4f), t * 0.7f);
        dl->AddLine(at(c, r, 0.85f, -0.85f), at(c, r, 0.85f, -0.2f), fade(col, 0.4f), t * 0.7f);
        break;
    }
    case Icon::MakeFace: {
        // Four corners, and the face they close.
        const ImVec2 q[] = {at(c, r, -0.75f, -0.55f), at(c, r, 0.6f, -0.8f),
                            at(c, r, 0.8f, 0.6f), at(c, r, -0.55f, 0.75f)};
        dl->AddQuadFilled(q[0], q[1], q[2], q[3], fade(col, 0.45f));
        dl->AddQuad(q[0], q[1], q[2], q[3], col, t * 0.8f);
        for (const ImVec2& d : q) dl->AddCircleFilled(d, r * 0.17f, col, 10);
        break;
    }
    case Icon::FillHole: {
        // A frame with a patch going into its hole.
        const ImVec2 a = at(c, r, -0.9f, -0.9f), b = at(c, r, 0.9f, 0.9f);
        const ImVec2 i0 = at(c, r, -0.45f, -0.45f), i1 = at(c, r, 0.45f, 0.45f);
        dl->AddRectFilled(a, ImVec2(b.x, i0.y), fade(col, 0.35f));
        dl->AddRectFilled(ImVec2(a.x, i1.y), b, fade(col, 0.35f));
        dl->AddRectFilled(ImVec2(a.x, i0.y), ImVec2(i0.x, i1.y), fade(col, 0.35f));
        dl->AddRectFilled(ImVec2(i1.x, i0.y), ImVec2(b.x, i1.y), fade(col, 0.35f));
        dl->AddRect(a, b, col, 0.0f, 0, t * 0.8f);
        dashedRect(dl, i0, i1, col, t * 0.8f, r * 0.1f);
        plus(dl, c, r * 0.28f, col, t);
        break;
    }
    case Icon::Connect: {
        // A face with a new edge drawn corner to corner across it.
        const ImVec2 a = at(c, r, -0.85f, -0.85f), b = at(c, r, 0.85f, 0.85f);
        dl->AddRect(a, b, fade(col, 0.6f), 0.0f, 0, t * 0.8f);
        dl->AddLine(ImVec2(a.x, b.y), ImVec2(b.x, a.y), col, t * 1.6f);
        dl->AddCircleFilled(ImVec2(a.x, b.y), r * 0.2f, col, 10);
        dl->AddCircleFilled(ImVec2(b.x, a.y), r * 0.2f, col, 10);
        break;
    }
    case Icon::Flip: {
        // A face, and the arrow out of it turning round.
        const ImVec2 f[] = {at(c, r, -0.85f, 0.35f), at(c, r, 0.3f, 0.35f),
                            at(c, r, 0.85f, 0.0f), at(c, r, -0.3f, 0.0f)};
        dl->AddQuadFilled(f[0], f[1], f[2], f[3], fade(col, 0.45f));
        dl->AddQuad(f[0], f[1], f[2], f[3], col, t * 0.8f);
        arrow(dl, at(c, r, -0.25f, 0.1f), at(c, r, -0.25f, -0.9f), r * 0.3f, fade(col, 0.45f), t * 0.9f);
        arrow(dl, at(c, r, 0.25f, 0.2f), at(c, r, 0.25f, 0.95f), r * 0.3f, col, t);
        break;
    }
    case Icon::Weld: {
        // Three corners close together, drawn into one.
        const ImVec2 d[] = {at(c, r, -0.7f, -0.55f), at(c, r, 0.7f, -0.55f), at(c, r, 0.0f, 0.75f)};
        for (const ImVec2& p : d) {
            dl->AddCircle(p, r * 0.16f, fade(col, 0.7f), 10, t * 0.7f);
            arrow(dl, p, ImVec2(c.x + (p.x - c.x) * 0.3f, c.y + (p.y - c.y) * 0.3f),
                  r * 0.24f, col, t * 0.8f);
        }
        dl->AddCircleFilled(c, r * 0.2f, col, 12);
        break;
    }
    case Icon::Grow: {
        // A picked square with the ring around it joining in.
        dl->AddRectFilled(at(c, r, -0.3f, -0.3f), at(c, r, 0.3f, 0.3f), col);
        dashedRect(dl, at(c, r, -0.88f, -0.88f), at(c, r, 0.88f, 0.88f), col, t * 0.8f, r * 0.14f);
        for (int i = 0; i < 4; ++i) {
            const float sx = (i == 0) ? -1.0f : (i == 1) ? 1.0f : 0.0f;
            const float sy = (i == 2) ? -1.0f : (i == 3) ? 1.0f : 0.0f;
            arrow(dl, at(c, r, sx * 0.35f, sy * 0.35f), at(c, r, sx * 0.78f, sy * 0.78f),
                  r * 0.22f, col, t * 0.8f);
        }
        break;
    }
    case Icon::Pencil: {
        const ImVec2 p0 = at(c, r, 0.72f, -0.52f), p1 = at(c, r, 0.52f, -0.72f);
        const ImVec2 p2 = at(c, r, -0.48f, 0.28f), p3 = at(c, r, -0.28f, 0.48f);
        dl->AddQuadFilled(p0, p1, p2, p3, col);
        dl->AddTriangleFilled(p2, p3, at(c, r, -0.78f, 0.78f), col);
        break;
    }

    // --- directions ---------------------------------------------------------
    case Icon::Nudge:
        arrow(dl, c, at(c, r, 0.0f, -0.95f), r * 0.3f, col, t);
        arrow(dl, c, at(c, r, 0.0f, 0.95f), r * 0.3f, col, t);
        arrow(dl, c, at(c, r, -0.95f, 0.0f), r * 0.3f, col, t);
        arrow(dl, c, at(c, r, 0.95f, 0.0f), r * 0.3f, col, t);
        break;
    case Icon::ArrowLeft:  arrow(dl, at(c, r, 0.75f, 0.0f), at(c, r, -0.85f, 0.0f), r * 0.5f, col, t * 1.3f); break;
    case Icon::ArrowRight: arrow(dl, at(c, r, -0.75f, 0.0f), at(c, r, 0.85f, 0.0f), r * 0.5f, col, t * 1.3f); break;
    case Icon::ArrowUp:    arrow(dl, at(c, r, 0.0f, 0.75f), at(c, r, 0.0f, -0.85f), r * 0.5f, col, t * 1.3f); break;
    case Icon::ArrowDown:  arrow(dl, at(c, r, 0.0f, -0.75f), at(c, r, 0.0f, 0.85f), r * 0.5f, col, t * 1.3f); break;
    // Depth has no direction on a flat screen, so it is drawn the way the box
    // above is: away is up and to the right, towards you is down and to the left.
    case Icon::ArrowIn:    arrow(dl, at(c, r, 0.6f, -0.6f), at(c, r, -0.7f, 0.7f), r * 0.5f, col, t * 1.3f); break;
    case Icon::ArrowOut:   arrow(dl, at(c, r, -0.6f, 0.6f), at(c, r, 0.7f, -0.7f), r * 0.5f, col, t * 1.3f); break;
    }
}

bool button(const char* id, Icon icon, const char* tip, bool enabled, bool active, float sz) {
    if (sz <= 0.0f) sz = size();
    return buttonSized(id, icon, tip, enabled, active, ImVec2(sz, sz));
}

bool buttonSized(const char* id, Icon icon, const char* tip, bool enabled, bool active,
                 ImVec2 bsz) {
    ImGui::PushID(id);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (active) {
        // The chosen one of a group sits on a wash of the accent, so what is on
        // reads from the button and not only from the thin lines of its picture.
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.92f, 0.67f, 0.25f, 0.28f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.92f, 0.67f, 0.25f, 0.40f));
    }
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::Button("##p", bsz);
    ImGui::EndDisabled();
    if (active) ImGui::PopStyleColor(2);
    if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tip);

    const ImU32 col = !enabled ? ImGui::GetColorU32(ImGuiCol_TextDisabled)
                      : active ? kAccent
                               : ImGui::GetColorU32(ImGuiCol_Text);
    draw(ImGui::GetWindowDrawList(), icon, ImVec2(p0.x + bsz.x * 0.5f, p0.y + bsz.y * 0.5f),
         std::min(bsz.x, bsz.y) * 0.32f, col);
    ImGui::PopID();
    return clicked && enabled;
}

} // namespace picto
