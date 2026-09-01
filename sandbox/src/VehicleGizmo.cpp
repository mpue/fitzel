#include "VehicleGizmo.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "Component.hpp"

namespace vehiclegizmo {
namespace {

// Pixel radius the cursor grabs a handle within. Deliberately much larger than
// the dot it grabs, for the same reason the water tool's is: this has to be
// usable by someone who cannot land the cursor on a five-pixel circle.
constexpr float kGrabRadius = 16.0f;

// Arrow-key nudge, in metres (Shift for the coarse step). The point of the
// nudge is that it needs no aim at all -- select once, then adjust without
// holding a button down.
constexpr float kNudgeFine   = 0.01f;
constexpr float kNudgeCoarse = 0.05f;

// Colours: the wheels and axles in one family, the collision box in another, so
// the two things that are easy to confuse never read as one shape.
constexpr ImU32 kWheelCol  = IM_COL32(120, 210, 255, 220);
constexpr ImU32 kAxleCol   = IM_COL32(120, 210, 255, 130);
constexpr ImU32 kBoxCol    = IM_COL32(255, 190,  90, 190);
constexpr ImU32 kComCol    = IM_COL32(255, 110, 140, 235);
constexpr ImU32 kNoseCol   = IM_COL32(150, 255, 170, 220);
constexpr ImU32 kHandleCol = IM_COL32(240, 245, 255, 235);
constexpr ImU32 kHotCol    = IM_COL32(255, 235, 120, 255);
constexpr ImU32 kShadow    = IM_COL32(0, 0, 0, 200);

// One draggable number: where its dot sits, which way it slides, and what it
// currently reads. Rebuilt every frame from the component, so a value changed in
// the panel moves its handle immediately.
struct Grip {
    glm::vec3   pos{0.0f};      // model-local
    glm::vec3   axis{1, 0, 0};  // model-local, unit
    float       value = 0.0f;
    const char* label = "";
    const char* fmt   = "%.2f m";
};

// The parameter along a line (A + s*axis) closest to the eye ray (O + t*dir).
// This is the whole of "drag along one axis": the handle follows the point on
// its own line nearest to where the user is pointing, so a shaky cross-axis
// wobble contributes nothing at all.
bool closestOnAxis(const glm::vec3& A, const glm::vec3& axis,
                   const glm::vec3& O, const glm::vec3& dir, float& s) {
    const glm::vec3 w0 = A - O;
    const float a = glm::dot(axis, axis);
    const float b = glm::dot(axis, dir);
    const float c = glm::dot(dir, dir);
    const float d = glm::dot(axis, w0);
    const float e = glm::dot(dir, w0);
    const float den = a * c - b * b;
    if (std::abs(den) < 1e-7f) return false; // sighting down the axis: no answer
    s = (b * e - c * d) / den;
    return true;
}

} // namespace

bool handle(const Context& c) {
    VehicleComponent& vc = c.vc;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // The model's nose: the component's Z values live in the chassis frame
    // (+Z forward), which is the model frame yawed 180 when the model was built
    // nose-down-Z. Everything below is model-local, so it goes through fs.
    const float fs = (vc.forward == 1) ? -1.0f : 1.0f;
    const float boxY = c.boxCenterY ? c.boxCenterY(vc)
                                    : vc.chassisHalf.y + vc.chassisY + vc.wheelY;

    auto toWorld = [&](const glm::vec3& lp) {
        return glm::vec3(c.world * glm::vec4(lp, 1.0f));
    };
    auto toScreen = [&](const glm::vec3& wp, ImVec2& out) {
        const glm::vec4 clip = c.viewProj * glm::vec4(wp, 1.0f);
        if (clip.w <= 1e-4f) return false;
        const glm::vec3 n = glm::vec3(clip) / clip.w;
        if (n.z > 1.0f) return false;
        out = ImVec2(c.origin.x + (n.x * 0.5f + 0.5f) * c.viewW,
                     c.origin.y + (1.0f - (n.y * 0.5f + 0.5f)) * c.viewH);
        return true;
    };
    auto localLine = [&](const glm::vec3& a, const glm::vec3& b, ImU32 col,
                         float th = 1.5f) {
        ImVec2 s0, s1;
        if (toScreen(toWorld(a), s0) && toScreen(toWorld(b), s1))
            dl->AddLine(s0, s1, col, th);
    };

    // --- The geometry, as the numbers describe it ----------------------------
    const float wr = std::max(vc.wheelRadius, 0.01f);
    const float ww = std::max(vc.wheelWidth, 0.01f);
    const glm::vec3 ch = glm::max(vc.chassisHalf, glm::vec3(0.02f));

    // Wheel centres, model-local: FL FR RL RR in the component's own order.
    const glm::vec3 wheelC[4] = {
        {-vc.halfTrack, vc.wheelY, vc.frontZ * fs},
        { vc.halfTrack, vc.wheelY, vc.frontZ * fs},
        {-vc.halfTrack, vc.wheelY, vc.rearZ  * fs},
        { vc.halfTrack, vc.wheelY, vc.rearZ  * fs},
    };

    // Wheels: two rims joined at the cardinal points. The axle runs along local
    // X, so the disc lies in the YZ plane -- drawing it any other way would show
    // a car with its wheels on backwards, which is exactly the mistake the
    // `forward` flag exists to make and this drawing exists to catch.
    constexpr int kSeg = 20;
    for (const glm::vec3& wc : wheelC) {
        for (int side = 0; side < 2; ++side) {
            const float x = wc.x + (side ? 0.5f : -0.5f) * ww;
            ImVec2 prev;
            bool   prevOk = false;
            for (int i = 0; i <= kSeg; ++i) {
                const float t = (6.2831853f * i) / kSeg;
                const glm::vec3 p(x, wc.y + std::sin(t) * wr, wc.z + std::cos(t) * wr);
                ImVec2 s;
                const bool ok = toScreen(toWorld(p), s);
                if (ok && prevOk) dl->AddLine(prev, s, kWheelCol, 1.6f);
                prev = s; prevOk = ok;
            }
        }
        for (int i = 0; i < 4; ++i) { // join the rims at N/E/S/W
            const float t = 1.5707963f * i;
            const float dy = std::sin(t) * wr, dz = std::cos(t) * wr;
            localLine({wc.x - 0.5f * ww, wc.y + dy, wc.z + dz},
                      {wc.x + 0.5f * ww, wc.y + dy, wc.z + dz}, kWheelCol, 1.2f);
        }
    }
    // Axles and the wheelbase, so track and axle spacing read as distances.
    localLine(wheelC[0], wheelC[1], kAxleCol, 2.0f);
    localLine(wheelC[2], wheelC[3], kAxleCol, 2.0f);
    localLine(0.5f * (wheelC[0] + wheelC[1]), 0.5f * (wheelC[2] + wheelC[3]),
              kAxleCol, 1.2f);

    // The collision box. It is NOT the model's bounds: it rides a suspension
    // travel above the wheel line, and that gap is the single most misread thing
    // in this component -- so it gets drawn with its gap visible.
    const glm::vec3 bc(0.0f, boxY, 0.0f);
    for (int i = 0; i < 4; ++i) {
        const float sx = (i & 1) ? 1.0f : -1.0f;
        const float sz = (i & 2) ? 1.0f : -1.0f;
        const glm::vec3 a(bc.x + sx * ch.x, bc.y - ch.y, bc.z + sz * ch.z);
        const glm::vec3 b(bc.x + sx * ch.x, bc.y + ch.y, bc.z + sz * ch.z);
        localLine(a, b, kBoxCol);                                  // uprights
        const float nx = (i & 1) ? -1.0f : 1.0f;
        localLine(a, {bc.x + nx * ch.x, a.y, a.z}, kBoxCol);       // bottom rails
        localLine(b, {bc.x + nx * ch.x, b.y, b.z}, kBoxCol);       // top rails
        const float nz = (i & 2) ? -1.0f : 1.0f;
        localLine(a, {a.x, a.y, bc.z + nz * ch.z}, kBoxCol);
        localLine(b, {b.x, b.y, bc.z + nz * ch.z}, kBoxCol);
    }

    // Centre of mass: the anti-rollover lever, with a dropline to the wheels so
    // "how far down is it, really" is a picture instead of a fraction.
    const glm::vec3 com(0.0f, boxY - glm::clamp(vc.comLower, 0.0f, 1.0f) * ch.y, 0.0f);
    localLine({com.x, com.y, com.z}, {0.0f, vc.wheelY, 0.0f},
              IM_COL32(255, 110, 140, 90), 1.0f);
    ImVec2 comS;
    if (toScreen(toWorld(com), comS)) {
        dl->AddCircleFilled(comS, 4.5f, kComCol, 12);
        dl->AddCircle(comS, 8.0f, kComCol, 16, 1.2f);
    }

    // Which way the car thinks it is pointing. A vehicle set up with the nose
    // backwards drives, steers and rolls wrongly in ways that all look like
    // handling bugs, so the arrow is worth its pixels.
    {
        const float zNose = bc.z + ch.z * fs;
        const glm::vec3 tip(0.0f, boxY, zNose + 0.6f * fs);
        localLine({0.0f, boxY, zNose}, tip, kNoseCol, 2.0f);
        localLine(tip, {0.18f, boxY, zNose + 0.35f * fs}, kNoseCol, 2.0f);
        localLine(tip, {-0.18f, boxY, zNose + 0.35f * fs}, kNoseCol, 2.0f);
    }

    if (!c.editable) return false;

    // --- The handles ---------------------------------------------------------
    Grip g[kCount];
    g[kTrack]    = {{ vc.halfTrack, vc.wheelY, vc.frontZ * fs}, {1, 0, 0},
                    vc.halfTrack, "Half track"};
    g[kFrontZ]   = {{0.0f, vc.wheelY, vc.frontZ * fs}, {0, 0, fs},
                    vc.frontZ, "Front axle"};
    g[kRearZ]    = {{0.0f, vc.wheelY, vc.rearZ * fs}, {0, 0, fs},
                    vc.rearZ, "Rear axle"};
    g[kWheelY]   = {{-vc.halfTrack, vc.wheelY, vc.frontZ * fs}, {0, 1, 0},
                    vc.wheelY, "Wheel height"};
    g[kRadius]   = {{-vc.halfTrack, vc.wheelY + wr, vc.rearZ * fs}, {0, 1, 0},
                    wr, "Wheel radius"};
    g[kChassisX] = {{bc.x + ch.x, bc.y, bc.z}, {1, 0, 0}, ch.x, "Box width"};
    g[kChassisY] = {{bc.x, bc.y + ch.y, bc.z}, {0, 1, 0}, ch.y, "Box height"};
    g[kChassisZ] = {{bc.x, bc.y, bc.z + ch.z * fs}, {0, 0, fs}, ch.z, "Box length"};
    // The floor of the box is its POSITION, the roof is its SIZE. Splitting them
    // that way is what makes the pair draggable at all: pull the floor and the
    // body rides higher on its springs without changing shape, pull the roof and
    // it grows upward from a floor that stays put.
    g[kChassisPos] = {{bc.x, bc.y - ch.y, bc.z}, {0, 1, 0}, vc.chassisY,
                      "Chassis Y"};
    g[kCom]      = {com, {0, 1, 0}, glm::clamp(vc.comLower, 0.0f, 1.0f),
                    "COM drop", "%.2f"};

    ImVec2 gs[kCount];
    bool   gOk[kCount];
    for (int i = 0; i < kCount; ++i) gOk[i] = toScreen(toWorld(g[i].pos), gs[i]);

    // What the cursor is over. Nearest wins, so overlapping handles on a small
    // car still resolve to whichever one is actually closer to the cursor.
    int hot = kNone;
    if (c.hovered && !c.dragging) {
        float bestD = kGrabRadius;
        for (int i = 0; i < kCount; ++i) {
            if (!gOk[i]) continue;
            const float d = std::hypot(gs[i].x - c.mousePos.x, gs[i].y - c.mousePos.y);
            if (d < bestD) { bestD = d; hot = i; }
        }
    }

    // --- Grab, drag, release -------------------------------------------------
    if (hot != kNone && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        c.sel = hot;
        c.dragging = true;
        if (c.beginEdit) c.beginEdit();
    }
    if (c.dragging && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        c.dragging = false;
        if (c.endEdit) c.endEdit("Vehicle setup");
    }

    if (c.dragging && c.sel > kNone && c.sel < kCount) {
        // The eye ray through the cursor, in world space.
        const glm::mat4 invVP = glm::inverse(c.viewProj);
        glm::vec4 far4 = invVP * glm::vec4(c.mouseNdc.x, c.mouseNdc.y, 1.0f, 1.0f);
        if (std::abs(far4.w) > 1e-6f) {
            const glm::vec3 farP = glm::vec3(far4) / far4.w;
            const glm::vec3 dir  = glm::normalize(farP - c.cameraPos);

            const Grip& gr = g[c.sel];
            // The handle's line, in world space: its own axis through its anchor.
            const glm::vec3 A  = toWorld(gr.pos);
            const glm::vec3 Ax = glm::normalize(
                glm::vec3(c.world * glm::vec4(gr.axis, 0.0f)));
            float s = 0.0f;
            if (closestOnAxis(A, Ax, c.cameraPos, dir, s)) {
                // `s` is the signed slide from where the handle sits right now,
                // so most handles just add it to the value they already show --
                // no world-to-local bookkeeping per handle, and no jump on grab.
                const float v = gr.value + s;
                switch (c.sel) {
                    case kTrack:    vc.halfTrack  = glm::clamp(v, 0.05f, 6.0f); break;
                    case kFrontZ:   vc.frontZ     = glm::clamp(v, -20.0f, 20.0f); break;
                    case kRearZ:    vc.rearZ      = glm::clamp(v, -20.0f, 20.0f); break;
                    case kWheelY:   vc.wheelY     = glm::clamp(v, -10.0f, 10.0f); break;
                    case kRadius:   vc.wheelRadius= glm::clamp(v, 0.05f, 3.0f); break;
                    case kChassisX: vc.chassisHalf.x = glm::clamp(v, 0.05f, 12.0f); break;
                    // The roof is the one handle that is not its own value: the
                    // box centre rides ON the half height (boxY grows with it), so
                    // the top face moves two metres per metre of half height. Half
                    // the slide, or the roof runs away at twice the cursor.
                    case kChassisY: vc.chassisHalf.y =
                                        glm::clamp(gr.value + 0.5f * s, 0.05f, 12.0f); break;
                    case kChassisZ: vc.chassisHalf.z = glm::clamp(v, 0.05f, 20.0f); break;
                    case kChassisPos: vc.chassisY = glm::clamp(v, 0.05f, 3.0f); break;
                    // comLower is a FRACTION of the box half height, and it counts
                    // downward -- so the slide is divided by that half height and
                    // negated, and dragging the marker down drops the mass.
                    case kCom:      vc.comLower = glm::clamp(
                                        gr.value - s / std::max(ch.y, 1e-3f),
                                        0.0f, 1.0f); break;
                    default: break;
                }
            }
        }
    }

    // --- The keyboard nudge --------------------------------------------------
    // Whole steps, no aim required, and the whole burst of key repeats lands in
    // one undo entry rather than fifty.
    if (c.sel > kNone && c.sel < kCount && !c.dragging && c.hovered) {
        const float step = ImGui::GetIO().KeyShift ? kNudgeCoarse : kNudgeFine;
        float d = 0.0f;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow,    true)) d += step;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow,  true)) d -= step;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) d += step;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow,  true)) d -= step;
        if (d != 0.0f) {
            if (c.beginEdit && (!c.editOpen || !c.editOpen())) c.beginEdit();
            const float v = g[c.sel].value + d;
            switch (c.sel) {
                case kTrack:    vc.halfTrack     = glm::clamp(v, 0.05f, 6.0f); break;
                case kFrontZ:   vc.frontZ        = glm::clamp(v, -20.0f, 20.0f); break;
                case kRearZ:    vc.rearZ         = glm::clamp(v, -20.0f, 20.0f); break;
                case kWheelY:   vc.wheelY        = glm::clamp(v, -10.0f, 10.0f); break;
                case kRadius:   vc.wheelRadius   = glm::clamp(v, 0.05f, 3.0f); break;
                case kChassisX: vc.chassisHalf.x = glm::clamp(v, 0.05f, 12.0f); break;
                case kChassisY: vc.chassisHalf.y = glm::clamp(v, 0.05f, 12.0f); break;
                case kChassisZ: vc.chassisHalf.z = glm::clamp(v, 0.05f, 20.0f); break;
                case kChassisPos: vc.chassisY    = glm::clamp(v, 0.05f, 3.0f); break;
                case kCom:      vc.comLower      = glm::clamp(v, 0.0f, 1.0f); break;
                default: break;
            }
        }
        const bool anyDown = ImGui::IsKeyDown(ImGuiKey_UpArrow) ||
                             ImGui::IsKeyDown(ImGuiKey_DownArrow) ||
                             ImGui::IsKeyDown(ImGuiKey_LeftArrow) ||
                             ImGui::IsKeyDown(ImGuiKey_RightArrow);
        if (!anyDown && c.editOpen && c.editOpen() && c.endEdit)
            c.endEdit("Vehicle setup");
    }

    // --- Draw the handles ----------------------------------------------------
    for (int i = 0; i < kCount; ++i) {
        if (!gOk[i]) continue;
        const bool sel = (c.sel == i);
        const bool warm = sel || hot == i;
        const float r = warm ? 6.5f : 4.5f;
        // The axis the handle slides on, as a stub through it: the gesture is
        // legible before it starts, so nobody drags to find out what moves.
        if (warm) {
            const glm::vec3 ax = g[i].axis * 0.35f;
            localLine(g[i].pos - ax, g[i].pos + ax, kHotCol, 2.0f);
        }
        dl->AddCircleFilled(gs[i], r + 1.5f, kShadow, 14);
        dl->AddCircleFilled(gs[i], r, warm ? kHotCol : kHandleCol, 14);

        // The number, next to the thing it measures. This is the whole point of
        // the exercise -- the value and the shape it produces in one glance.
        if (warm) {
            char val[32];
            std::snprintf(val, sizeof(val), g[i].fmt, g[i].value);
            char buf[80];
            std::snprintf(buf, sizeof(buf), "%s %s", g[i].label, val);
            const ImVec2 at(gs[i].x + r + 5.0f, gs[i].y - 7.0f);
            dl->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f), kShadow, buf);
            dl->AddText(at, IM_COL32(255, 245, 200, 245), buf);
        }
    }

    return c.dragging;
}

} // namespace vehiclegizmo
