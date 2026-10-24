#include "ImageIcons.hpp"

#include <algorithm>
#include <cmath>

namespace imageui {

namespace {

constexpr float kPi = 3.14159265f;

ImU32 withAlpha(ImU32 col, float a) {
    const ImU32 al = ImU32(float((col >> IM_COL32_A_SHIFT) & 0xFF) * a);
    return (col & ~IM_COL32_A_MASK) | (al << IM_COL32_A_SHIFT);
}

} // namespace

void drawIcon(ImDrawList* dl, Icon ic, ImVec2 c, float s, ImU32 col) {
    const float h = s * 0.5f;
    const float lw = std::max(1.4f, s * 0.085f);
    // Points in a -1..1 square.
    auto P = [&](float x, float y) { return ImVec2(c.x + x * h, c.y + y * h); };
    auto line = [&](float x0, float y0, float x1, float y1, float k = 1.0f) { dl->AddLine(P(x0, y0), P(x1, y1), col, lw * k); };
    auto rect = [&](float x0, float y0, float x1, float y1, float k = 1.0f) { dl->AddRect(P(x0, y0), P(x1, y1), col, h * 0.06f, 0, lw * k); };
    auto fill = [&](float x0, float y0, float x1, float y1) { dl->AddRectFilled(P(x0, y0), P(x1, y1), col, h * 0.06f); };
    auto tri = [&](float x0, float y0, float x1, float y1, float x2, float y2) { dl->AddTriangleFilled(P(x0, y0), P(x1, y1), P(x2, y2), col); };
    // An arrow from (x0, y0) to (x1, y1), its head at the end.
    auto arrow = [&](float x0, float y0, float x1, float y1, float head = 0.32f) {
        const float dx = x1 - x0, dy = y1 - y0, l = std::max(1e-3f, std::sqrt(dx * dx + dy * dy));
        const float ux = dx / l, uy = dy / l;
        line(x0, y0, x1 - ux * head * 0.6f, y1 - uy * head * 0.6f);
        tri(x1, y1, x1 - ux * head - uy * head * 0.6f, y1 - uy * head + ux * head * 0.6f,
            x1 - ux * head + uy * head * 0.6f, y1 - uy * head - ux * head * 0.6f);
    };
    // A dashed outline along points (the marching ants of the marquee icons).
    auto dashed = [&](const ImVec2* pts, int n, bool closed) {
        const float dash = std::max(2.0f, s * 0.13f);
        float carry = 0.0f;
        bool on = true;
        for (int i = 0; i < (closed ? n : n - 1); ++i) {
            const ImVec2 a = pts[i], b = pts[(i + 1) % n];
            const float dx = b.x - a.x, dy = b.y - a.y, l = std::sqrt(dx * dx + dy * dy);
            float t = 0.0f;
            while (t < l) {
                const float step = std::min(dash - carry, l - t);
                if (on) dl->AddLine(ImVec2(a.x + dx * t / l, a.y + dy * t / l),
                                    ImVec2(a.x + dx * (t + step) / l, a.y + dy * (t + step) / l), col, lw);
                t += step;
                carry += step;
                if (carry >= dash - 1e-3f) { carry = 0.0f; on = !on; }
            }
        }
    };
    auto arcArrow = [&](bool cw, float r, float cx, float cy) {
        // Three quarters of a circle, the head at the end it turns towards.
        const float a0 = cw ? -kPi * 0.95f : -kPi * 0.05f, a1 = cw ? kPi * 0.45f : -kPi * 1.45f;
        dl->PathArcTo(P(cx, cy), r * h, a0, a1, 20);
        dl->PathStroke(col, 0, lw);
        const ImVec2 e(P(cx, cy).x + std::cos(a1) * r * h, P(cx, cy).y + std::sin(a1) * r * h);
        const float tx = cw ? -std::sin(a1) : std::sin(a1), ty = cw ? std::cos(a1) : -std::cos(a1);   // the way it goes
        const float k = h * 0.3f;
        dl->AddTriangleFilled(ImVec2(e.x + tx * k, e.y + ty * k), ImVec2(e.x - ty * k * 0.8f, e.y + tx * k * 0.8f),
                              ImVec2(e.x + ty * k * 0.8f, e.y - tx * k * 0.8f), col);
    };
    auto magnifier = [&] {
        dl->AddCircle(P(-0.15f, -0.15f), h * 0.52f, col, 0, lw * 1.2f);
        line(0.22f, 0.22f, 0.82f, 0.82f, 2.0f);
    };

    switch (ic) {
    case Icon::Move:
        arrow(0, 0, 0, -0.9f); arrow(0, 0, 0, 0.9f); arrow(0, 0, -0.9f, 0); arrow(0, 0, 0.9f, 0);
        break;
    case Icon::Marquee: {
        const ImVec2 p[4] = {P(-0.75f, -0.62f), P(0.75f, -0.62f), P(0.75f, 0.62f), P(-0.75f, 0.62f)};
        dashed(p, 4, true);
        break;
    }
    case Icon::MarqueeEllipse: {
        ImVec2 p[24];
        for (int i = 0; i < 24; ++i) p[i] = P(std::cos(i * kPi / 12) * 0.8f, std::sin(i * kPi / 12) * 0.6f);
        dashed(p, 24, true);
        break;
    }
    case Icon::Lasso:
        dl->AddEllipse(P(0.12f, -0.28f), ImVec2(h * 0.72f, h * 0.45f), col, -0.25f, 24, lw);
        dl->AddBezierCubic(P(-0.42f, 0.02f), P(-0.25f, 0.35f), P(-0.75f, 0.45f), P(-0.55f, 0.88f), col, lw);
        break;
    case Icon::PolyLasso: {
        const ImVec2 p[5] = {P(-0.8f, 0.25f), P(-0.35f, -0.72f), P(0.75f, -0.45f), P(0.35f, 0.35f), P(-0.15f, 0.05f)};
        dl->AddPolyline(p, 5, col, ImDrawFlags_Closed, lw);
        for (const ImVec2& q : p) dl->AddRectFilled(ImVec2(q.x - lw, q.y - lw), ImVec2(q.x + lw, q.y + lw), col);
        dl->AddBezierCubic(P(-0.8f, 0.25f), P(-0.7f, 0.6f), P(-0.4f, 0.55f), P(-0.5f, 0.9f), col, lw);
        break;
    }
    case Icon::Wand:
        line(-0.8f, 0.8f, 0.15f, -0.15f, 1.8f);
        line(0.5f, -0.9f, 0.5f, -0.25f); line(0.18f, -0.58f, 0.82f, -0.58f);
        line(0.3f, -0.78f, 0.7f, -0.38f, 0.7f); line(0.3f, -0.38f, 0.7f, -0.78f, 0.7f);
        break;
    case Icon::Crop:
        line(-0.45f, -0.9f, -0.45f, 0.45f, 1.4f); line(-0.45f, 0.45f, 0.9f, 0.45f, 1.4f);
        line(-0.9f, -0.45f, 0.45f, -0.45f, 1.4f); line(0.45f, -0.45f, 0.45f, 0.9f, 1.4f);
        break;
    case Icon::Eyedropper:
        line(-0.78f, 0.78f, 0.1f, -0.1f, 1.3f);
        dl->AddCircleFilled(P(0.42f, -0.42f), h * 0.3f, col);
        line(-0.05f, -0.35f, 0.35f, 0.05f, 2.0f);
        break;
    case Icon::Brush:
        line(0.82f, -0.82f, 0.08f, -0.08f, 1.6f);
        dl->AddEllipseFilled(P(-0.3f, 0.3f), ImVec2(h * 0.45f, h * 0.24f), col, kPi * 0.75f);
        tri(-0.55f, 0.48f, -0.48f, 0.55f, -0.88f, 0.88f);
        break;
    case Icon::Clone:
        dl->AddCircleFilled(P(0, -0.62f), h * 0.26f, col);
        fill(-0.13f, -0.45f, 0.13f, 0.1f);
        dl->AddQuadFilled(P(-0.3f, 0.05f), P(0.3f, 0.05f), P(0.78f, 0.45f), P(-0.78f, 0.45f), col);
        line(-0.85f, 0.7f, 0.85f, 0.7f, 1.3f);
        break;
    case Icon::Eraser: {
        // A block on its edge, the rubber end filled.
        const float a = -kPi * 0.22f, ca = std::cos(a), sa = std::sin(a);
        auto R = [&](float x, float y) { return P(x * ca - y * sa, x * sa + y * ca + 0.1f); };
        const ImVec2 q[4] = {R(-0.85f, -0.35f), R(0.85f, -0.35f), R(0.85f, 0.35f), R(-0.85f, 0.35f)};
        dl->AddPolyline(q, 4, col, ImDrawFlags_Closed, lw);
        dl->AddQuadFilled(R(-0.85f, -0.35f), R(-0.05f, -0.35f), R(-0.05f, 0.35f), R(-0.85f, 0.35f), col);
        break;
    }
    case Icon::Gradient:
        dl->AddRectFilledMultiColor(P(-0.8f, -0.6f), P(0.8f, 0.6f), withAlpha(col, 0.05f), col, col, withAlpha(col, 0.05f));
        rect(-0.8f, -0.6f, 0.8f, 0.6f);
        break;
    case Icon::RadialGradient:
        for (int i = 6; i >= 1; --i) dl->AddCircleFilled(c, h * 0.8f * i / 6.0f, withAlpha(col, 0.18f + (6 - i) * 0.02f));
        dl->AddCircleFilled(c, h * 0.14f, col);
        dl->AddCircle(c, h * 0.8f, col, 0, lw);
        break;
    case Icon::Bucket: {
        const ImVec2 q[4] = {P(-0.62f, -0.1f), P(0.0f, -0.72f), P(0.58f, -0.14f), P(-0.04f, 0.48f)};
        dl->AddPolyline(q, 4, col, ImDrawFlags_Closed, lw);
        dl->AddQuadFilled(P(-0.62f, -0.1f), P(-0.3f, -0.42f), P(0.27f, 0.17f), P(-0.04f, 0.48f), col);
        dl->AddBezierCubic(P(-0.3f, -0.42f), P(-0.6f, -0.95f), P(0.1f, -1.0f), P(0.0f, -0.72f), col, lw);
        dl->AddCircleFilled(P(0.66f, 0.55f), h * 0.2f, col);
        tri(0.66f, 0.12f, 0.49f, 0.5f, 0.83f, 0.5f);
        break;
    }
    case Icon::Blur:
        dl->AddCircleFilled(P(0, 0.25f), h * 0.52f, col);
        tri(0, -0.88f, -0.45f, 0.0f, 0.45f, 0.0f);
        break;
    case Icon::Sharpen:
        dl->AddTriangle(P(0, -0.85f), P(0.55f, 0.75f), P(-0.55f, 0.75f), col, lw * 1.2f);
        tri(0, -0.85f, 0.2f, -0.25f, -0.2f, -0.25f);
        break;
    case Icon::Dodge:
        // A paddle on an upright stick (the darkroom's dodging tool) -- not the zoom's slanted lens.
        dl->AddCircle(P(0, -0.38f), h * 0.45f, col, 0, lw * 1.3f);
        line(0, 0.08f, 0, 0.92f, 1.6f);
        break;
    case Icon::Burn:
        // A hand with the light through a hole: a ring round a dark disc, on the stick.
        dl->AddCircleFilled(P(0, -0.38f), h * 0.5f, col);
        line(0, 0.1f, 0, 0.92f, 1.6f);
        line(-0.45f, 0.5f, 0.45f, 0.5f, 1.2f);
        break;
    case Icon::Sponge:
    case Icon::Saturate:
        dl->AddRect(P(-0.75f, -0.55f), P(0.75f, 0.55f), col, h * 0.3f, 0, lw * 1.2f);
        if (ic == Icon::Sponge) {
            dl->AddCircleFilled(P(-0.32f, -0.15f), h * 0.12f, col);
            dl->AddCircleFilled(P(0.28f, -0.22f), h * 0.1f, col);
            dl->AddCircleFilled(P(0.05f, 0.25f), h * 0.12f, col);
            dl->AddCircleFilled(P(0.45f, 0.22f), h * 0.07f, col);
        } else {
            line(-0.32f, 0, 0.32f, 0, 1.2f);
            line(0, -0.32f, 0, 0.32f, 1.2f);
        }
        break;
    case Icon::Text:
        fill(-0.7f, -0.78f, 0.7f, -0.55f);
        fill(-0.12f, -0.78f, 0.12f, 0.78f);
        fill(-0.35f, 0.6f, 0.35f, 0.78f);
        fill(-0.7f, -0.78f, -0.52f, -0.4f);
        fill(0.52f, -0.78f, 0.7f, -0.4f);
        break;
    case Icon::Shape:
        fill(-0.8f, -0.5f, 0.25f, 0.55f);
        dl->AddCircle(P(0.35f, -0.05f), h * 0.5f, col, 0, lw);
        break;
    case Icon::Hand: {
        dl->AddRectFilled(P(-0.48f, -0.05f), P(0.48f, 0.65f), col, h * 0.3f);
        const float xs[4] = {-0.48f, -0.22f, 0.04f, 0.3f}, tops[4] = {-0.55f, -0.8f, -0.82f, -0.6f};
        for (int i = 0; i < 4; ++i) dl->AddRectFilled(P(xs[i], tops[i]), P(xs[i] + 0.19f, 0.1f), col, h * 0.1f);
        line(-0.42f, 0.35f, -0.85f, -0.1f, 2.0f);
        fill(-0.3f, 0.6f, 0.3f, 0.88f);
        break;
    }
    case Icon::Zoom: magnifier(); break;
    case Icon::ZoomIn:
        magnifier();
        line(-0.42f, -0.15f, 0.12f, -0.15f); line(-0.15f, -0.42f, -0.15f, 0.12f);
        break;
    case Icon::ZoomOut:
        magnifier();
        line(-0.42f, -0.15f, 0.12f, -0.15f);
        break;
    case Icon::Line: line(-0.75f, 0.75f, 0.75f, -0.75f, 1.4f); break;
    case Icon::RectOutline: rect(-0.75f, -0.6f, 0.75f, 0.6f, 1.3f); break;
    case Icon::RectFilled: fill(-0.75f, -0.6f, 0.75f, 0.6f); break;
    case Icon::EllipseOutline: dl->AddEllipse(c, ImVec2(h * 0.8f, h * 0.62f), col, 0.0f, 32, lw * 1.3f); break;
    case Icon::EllipseFilled: dl->AddEllipseFilled(c, ImVec2(h * 0.8f, h * 0.62f), col, 0.0f, 32); break;
    case Icon::SelReplace: fill(-0.65f, -0.65f, 0.65f, 0.65f); break;
    case Icon::SelAdd:
        fill(-0.8f, -0.8f, 0.3f, 0.3f); fill(-0.3f, -0.3f, 0.8f, 0.8f);
        break;
    case Icon::SelSubtract:
        fill(-0.8f, -0.8f, 0.3f, -0.3f); fill(-0.8f, -0.3f, -0.3f, 0.3f);
        rect(-0.3f, -0.3f, 0.8f, 0.8f, 0.8f);
        break;
    case Icon::SelIntersect:
        rect(-0.8f, -0.8f, 0.3f, 0.3f, 0.8f); rect(-0.3f, -0.3f, 0.8f, 0.8f, 0.8f);
        fill(-0.3f, -0.3f, 0.3f, 0.3f);
        break;
    case Icon::Undo: arcArrow(false, 0.6f, 0.05f, 0.1f); break;
    case Icon::Redo: arcArrow(true, 0.6f, -0.05f, 0.1f); break;
    case Icon::Fit:
        line(-0.85f, -0.85f, -0.4f, -0.85f); line(-0.85f, -0.85f, -0.85f, -0.4f);
        line(0.85f, -0.85f, 0.4f, -0.85f); line(0.85f, -0.85f, 0.85f, -0.4f);
        line(-0.85f, 0.85f, -0.4f, 0.85f); line(-0.85f, 0.85f, -0.85f, 0.4f);
        line(0.85f, 0.85f, 0.4f, 0.85f); line(0.85f, 0.85f, 0.85f, 0.4f);
        rect(-0.45f, -0.35f, 0.45f, 0.35f, 0.8f);
        break;
    case Icon::OneToOne: {
        const char* t = "1:1";
        ImFont* f = ImGui::GetFont();
        const float fs = s * 0.62f;
        const ImVec2 ts = f->CalcTextSizeA(fs, 1e9f, 0.0f, t);
        dl->AddText(f, fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), col, t);
        break;
    }
    case Icon::Swap:
        dl->PathArcTo(P(-0.2f, 0.2f), h * 0.62f, -kPi * 0.5f, 0.0f, 10);
        dl->PathStroke(col, 0, lw);
        tri(-0.2f, -0.68f, -0.2f, -0.14f, -0.55f, -0.42f);
        tri(0.42f, 0.55f, 0.15f, 0.2f, 0.69f, 0.2f);
        break;
    case Icon::Defaults:
        dl->AddRect(P(-0.1f, -0.1f), P(0.75f, 0.75f), col, 0.0f, 0, lw);
        fill(-0.75f, -0.75f, 0.1f, 0.1f);
        break;
    case Icon::Eye:
    case Icon::EyeOff: {
        const float w = h * 0.9f, e = h * 0.5f;
        dl->PathArcTo(ImVec2(c.x, c.y + e * 0.9f), w * 1.18f, -kPi * 0.78f, -kPi * 0.22f, 12);
        dl->PathStroke(col, 0, lw);
        dl->PathArcTo(ImVec2(c.x, c.y - e * 0.9f), w * 1.18f, kPi * 0.22f, kPi * 0.78f, 12);
        dl->PathStroke(col, 0, lw);
        if (ic == Icon::Eye) dl->AddCircleFilled(c, h * 0.3f, col);
        else line(-0.8f, 0.7f, 0.8f, -0.7f);
        break;
    }
    case Icon::Lock:
    case Icon::Unlock:
        fill(-0.6f, -0.1f, 0.6f, 0.8f);
        if (ic == Icon::Lock) dl->PathArcTo(P(0, -0.35f), h * 0.4f, kPi, 2 * kPi, 12);
        else dl->PathArcTo(P(0.55f, -0.45f), h * 0.4f, kPi, 2 * kPi, 12);
        dl->PathStroke(col, 0, lw * 1.3f);
        if (ic == Icon::Lock) { line(-0.4f, -0.35f, -0.4f, -0.1f, 1.3f); line(0.4f, -0.35f, 0.4f, -0.1f, 1.3f); }
        else line(0.15f, -0.45f, 0.15f, -0.1f, 1.3f);
        break;
    case Icon::LayerNew:
        rect(-0.75f, -0.75f, 0.75f, 0.75f);
        line(-0.4f, 0, 0.4f, 0, 1.2f); line(0, -0.4f, 0, 0.4f, 1.2f);
        break;
    case Icon::Duplicate:
        rect(-0.8f, -0.8f, 0.3f, 0.3f);
        fill(-0.3f, -0.3f, 0.8f, 0.8f);
        break;
    case Icon::Trash:
        fill(-0.75f, -0.62f, 0.75f, -0.48f);
        fill(-0.25f, -0.82f, 0.25f, -0.62f);
        dl->AddRectFilled(P(-0.55f, -0.38f), P(0.55f, 0.82f), col, h * 0.08f);
        break;
    case Icon::Up: arrow(0, 0.75f, 0, -0.8f, 0.55f); break;
    case Icon::Down: arrow(0, -0.75f, 0, 0.8f, 0.55f); break;
    case Icon::Left: arrow(0.75f, 0, -0.8f, 0, 0.55f); break;
    case Icon::Right: arrow(-0.75f, 0, 0.8f, 0, 0.55f); break;
    case Icon::Merge:
        line(-0.65f, -0.8f, 0, -0.15f); line(0.65f, -0.8f, 0, -0.15f);
        arrow(0, -0.15f, 0, 0.5f, 0.4f);
        fill(-0.7f, 0.62f, 0.7f, 0.82f);
        break;
    case Icon::Check: {
        const ImVec2 p[3] = {P(-0.75f, 0.0f), P(-0.25f, 0.55f), P(0.8f, -0.6f)};
        dl->AddPolyline(p, 3, col, 0, lw * 1.8f);
        break;
    }
    case Icon::Cancel:
        dl->AddCircle(c, h * 0.78f, col, 0, lw * 1.3f);
        line(-0.55f, 0.55f, 0.55f, -0.55f, 1.3f);
        break;
    case Icon::FlipH:
        tri(-0.15f, -0.7f, -0.15f, 0.7f, -0.85f, 0.7f);
        dl->AddTriangle(P(0.15f, -0.7f), P(0.15f, 0.7f), P(0.85f, 0.7f), col, lw);
        line(0, -0.95f, 0, 0.95f, 0.6f);
        break;
    case Icon::FlipV:
        tri(-0.7f, -0.15f, 0.7f, -0.15f, 0.7f, -0.85f);
        dl->AddTriangle(P(-0.7f, 0.15f), P(0.7f, 0.15f), P(0.7f, 0.85f), col, lw);
        line(-0.95f, 0, 0.95f, 0, 0.6f);
        break;
    case Icon::RotateCW:
        rect(-0.35f, -0.1f, 0.35f, 0.6f, 0.8f);
        arcArrow(true, 0.75f, 0, 0.05f);
        break;
    case Icon::RotateCCW:
        rect(-0.35f, -0.1f, 0.35f, 0.6f, 0.8f);
        arcArrow(false, 0.75f, 0, 0.05f);
        break;
    case Icon::LineMode: {
        const ImVec2 p[3] = {P(-0.8f, 0.6f), P(-0.1f, -0.55f), P(0.8f, 0.35f)};
        dl->AddPolyline(p, 3, col, 0, lw);
        for (const ImVec2& q : p) dl->AddCircleFilled(q, lw * 1.6f, col);
        break;
    }
    case Icon::Target:
        dl->AddCircle(c, h * 0.55f, col, 0, lw);
        line(-0.9f, 0, -0.3f, 0); line(0.3f, 0, 0.9f, 0); line(0, -0.9f, 0, -0.3f); line(0, 0.3f, 0, 0.9f);
        break;
    case Icon::Transform: {
        rect(-0.6f, -0.6f, 0.6f, 0.6f, 0.8f);
        const float k = 0.16f;
        for (float x : {-0.6f, 0.0f, 0.6f})
            for (float y : {-0.6f, 0.0f, 0.6f})
                if (x != 0.0f || y != 0.0f) fill(x - k, y - k, x + k, y + k);
        break;
    }
    case Icon::Chain:
        dl->AddRect(P(-0.85f, -0.3f), P(0.1f, 0.3f), col, h * 0.3f, 0, lw);
        dl->AddRect(P(-0.1f, -0.3f), P(0.85f, 0.3f), col, h * 0.3f, 0, lw);
        break;
    case Icon::Tile:
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x) fill(-0.8f + x * 0.85f, -0.8f + y * 0.85f, -0.05f + x * 0.85f, -0.05f + y * 0.85f);
        break;
    case Icon::Grid:
        rect(-0.8f, -0.8f, 0.8f, 0.8f, 0.8f);
        for (float t : {-0.27f, 0.27f}) { line(t, -0.8f, t, 0.8f, 0.8f); line(-0.8f, t, 0.8f, t, 0.8f); }
        break;
    case Icon::Count: break;
    }
}

bool iconButton(const char* id, Icon ic, const char* tip, bool on, float side) {
    const float h = side > 0.0f ? side : ImGui::GetFrameHeight();
    if (on) {
        const ImVec4 a = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
        ImGui::PushStyleColor(ImGuiCol_Button, a);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, a);
    }
    const bool pressed = ImGui::Button(id, ImVec2(h, h));
    if (on) ImGui::PopStyleColor(2);
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const bool disabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
    const ImU32 ink = disabled ? ImGui::GetColorU32(ImGuiCol_TextDisabled) : ImGui::GetColorU32(ImGuiCol_Text);
    drawIcon(ImGui::GetWindowDrawList(), ic, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), h * 0.6f, ink);
    if (tip) ImGui::SetItemTooltip("%s", tip);
    return pressed;
}

} // namespace imageui
