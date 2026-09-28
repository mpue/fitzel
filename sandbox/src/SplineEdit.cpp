#include "SplineEdit.hpp"

#include <cmath>
#include <cstdio>

#include <glm/gtc/constants.hpp>

#include "SplinePlace.hpp"
#include "SplineSystem.hpp"

namespace splineedit {
namespace {

// World point -> viewport pixel. False when it is behind the camera or past the
// far plane, so callers can skip it rather than draw it somewhere absurd.
bool toScreen(const Context& c, const glm::vec3& wp, ImVec2& out) {
    return c.view.project(wp, out);
}

// Every path's centreline. The selected path in warm yellow, the rest in a cool
// grey-blue: which run a click will affect is the one thing that must be
// readable at a glance in a scene with a dozen of them.
void drawLines(const Context& c, ImDrawList* dl) {
    const SplineSystem& sp = c.splines;
    for (int pi = 0; pi < static_cast<int>(sp.paths.size()); ++pi) {
        const bool active = pi == c.sel;
        const ImU32 col = active ? IM_COL32(255, 210, 70, 220) : IM_COL32(130, 170, 210, 130);
        const float th  = active ? 2.0f : 1.5f;
        ImVec2 prev; bool have = false;
        for (const glm::vec3& wp : sp.line(pi)) {
            ImVec2 s;
            if (!toScreen(c, wp, s)) { have = false; continue; }
            if (have) dl->AddLine(prev, s, col, th);
            prev = s; have = true;
        }
    }
}

// Where "Place along path" would put its copies: a dot on the ground and a
// stroke the way the copy will face. Shown before anything is placed, so the
// spacing and the side offset can be judged in the scene rather than by number.
void drawPreview(const Context& c, ImDrawList* dl) {
    if (!c.preview) return;
    for (const splineplace::Spot& s : *c.preview) {
        ImVec2 at, tip;
        if (!toScreen(c, s.pos, at)) continue;
        const float yaw = glm::radians(s.yawDeg);
        const glm::vec3 ahead = s.pos + glm::vec3(std::sin(yaw), 0.0f, std::cos(yaw)) * 1.5f;
        if (toScreen(c, ahead, tip)) {
            dl->AddLine(at, tip, IM_COL32(0, 0, 0, 160), 3.5f);
            dl->AddLine(at, tip, IM_COL32(255, 160, 60, 235), 2.0f);
        }
        dl->AddCircleFilled(at, 5.0f, IM_COL32(255, 160, 60, 235));
        dl->AddCircle(at, 5.0f, IM_COL32(0, 0, 0, 200), 0, 1.5f);
    }
}

// Pixel radius the cursor grabs a handle within. Deliberately larger than the
// dot it grabs -- the tool has to be usable by someone who cannot put the cursor
// exactly on a five-pixel circle, and there is nothing else in the viewport
// competing for the click while this mode is on.
constexpr float kGrabRadius = 14.0f;

// Where a control point's handle sits in the world: on the run itself, not on
// the bare ground, so a raised point is grabbable where its fence actually is.
glm::vec3 handleWorld(const Context& c, int i) {
    return c.splines.pointWorld(c.sel, i) + glm::vec3(0.0f, 0.15f, 0.0f);
}

// Which control point a click at `P` (world XZ) should be spliced in front of:
// the nearest segment of the control polygon, or one of the ends.
int insertIndex(const SplineSystem::Path& p, glm::vec2 P) {
    const int n = static_cast<int>(p.points.size());
    if (n < 2) return n;
    float bestD = 1e30f, bestT = 0.0f;
    int   bestSeg = 0;
    const int segs = p.closed ? n : n - 1;
    for (int i = 0; i < segs; ++i) {
        const glm::vec2 a = p.points[i], b = p.points[(i + 1) % n];
        const glm::vec2 ab = b - a;
        const float len2 = glm::dot(ab, ab);
        const float t = len2 > 1e-6f
            ? glm::clamp(glm::dot(P - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
        const float d = glm::distance(P, a + ab * t);
        if (d < bestD) { bestD = d; bestSeg = i; bestT = t; }
    }
    if (!p.closed) {
        if (bestSeg == 0     && bestT <= 0.0f) return 0;
        if (bestSeg == n - 2 && bestT >= 1.0f) return n;
    }
    return bestSeg + 1;
}

} // namespace

void draw(const Context& c) {
    if (c.splines.paths.empty()) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    drawLines(c, dl);
    drawPreview(c, dl);
}

void handle(const Context& c) {
    SplineSystem& sp = c.splines;
    if (sp.paths.empty()) return;
    if (c.sel >= static_cast<int>(sp.paths.size())) { c.sel = -1; c.ptSel = -1; }

    auto toScreen = [&](const glm::vec3& wp, ImVec2& out) {
        return splineedit::toScreen(c, wp, out);
    };

    // --- What the cursor is over ---------------------------------------------
    // Handles of EVERY path are pickable, not just the selected one's: clicking a
    // neighbouring fence's point selects that fence, which is how a scene with a
    // dozen runs stays navigable without going back to the list each time.
    int hoverPath = -1, hoverPt = -1;
    if (c.view.hovered && !c.dragging) {
        float bestD = kGrabRadius;
        for (int pi = 0; pi < static_cast<int>(sp.paths.size()); ++pi) {
            const SplineSystem::Path& p = sp.paths[pi];
            if (!p.enabled) continue;
            for (int i = 0; i < static_cast<int>(p.points.size()); ++i) {
                ImVec2 s;
                if (!toScreen(sp.pointWorld(pi, i) + glm::vec3(0.0f, 0.15f, 0.0f), s)) continue;
                const float d = std::hypot(s.x - c.view.mousePos.x, s.y - c.view.mousePos.y);
                if (d < bestD) { bestD = d; hoverPath = pi; hoverPt = i; }
            }
        }
    }

    // --- Click: pick a handle, or lay a new point ----------------------------
    if (c.view.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hoverPath >= 0) {
            c.sel        = hoverPath;
            c.ptSel      = hoverPt;
            c.dragging   = true;
            c.dragHeight = ImGui::GetIO().KeyCtrl;
            c.beginEdit();   // opened here, pushed on release: one drag, one step
        } else if (c.sel >= 0) {
            glm::vec3 h;
            if (c.view.pickTerrain && c.view.pickTerrain(c.view.mouseNdc, c.view.viewProj, h)) {
                SplineSystem::Path& p = sp.paths[c.sel];
                const int n = static_cast<int>(p.points.size());
                // With an END selected, extend from THAT end -- so a run of clicks
                // lays a path out point by point, which is the gesture this tool is
                // for. Otherwise splice into the nearest segment.
                const bool ends = !p.closed && n >= 2;
                const int at = (ends && c.ptSel == n - 1) ? n
                             : (ends && c.ptSel == 0)     ? 0
                             : insertIndex(p, glm::vec2(h.x, h.z));
                c.beginEdit();
                sp.insertPoint(c.sel, at, glm::vec2(h.x, h.z));
                c.endEdit("Add point");
                c.ptSel = at;   // the new point takes the selection
            }
        }
    }

    // --- Drag ----------------------------------------------------------------
    if (c.dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left) && c.sel >= 0 &&
        c.ptSel >= 0 && c.ptSel < static_cast<int>(sp.paths[c.sel].points.size())) {
        if (c.dragHeight) {
            // Metres per pixel at the handle's own depth, so the point tracks the
            // cursor instead of drifting away from it as you zoom in or out.
            const glm::vec3 hw = handleWorld(c, c.ptSel);
            const float mpp = c.view.metresPerPixel(hw);
            const float dy = ImGui::GetIO().MouseDelta.y;
            if (dy != 0.0f)
                sp.setLift(c.sel, c.ptSel, sp.liftOf(c.sel, c.ptSel) - dy * mpp);
        } else {
            glm::vec3 h;
            if (c.view.pickTerrain && c.view.pickTerrain(c.view.mouseNdc, c.view.viewProj, h)) {
                sp.paths[c.sel].points[c.ptSel] = glm::vec2(h.x, h.z);
                sp.touch(c.sel);
            }
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && c.dragging) {
        c.dragging = false;
        c.endEdit(c.dragHeight ? "Raise point" : "Move point");
    }

    // --- Delete + nudge ------------------------------------------------------
    if (c.sel >= 0 && c.ptSel >= 0 &&
        c.ptSel < static_cast<int>(sp.paths[c.sel].points.size())) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            c.beginEdit();
            sp.erasePoint(c.sel, c.ptSel);
            c.endEdit("Delete point");
            c.ptSel = -1;
        }
    }
    if (c.sel >= 0 && c.ptSel >= 0 && !c.dragging &&
        c.ptSel < static_cast<int>(sp.paths[c.sel].points.size()) &&
        !ImGui::GetIO().WantTextInput) {
        // Camera relative, because "left" means what you see, not where the
        // world's X axis happens to point. Held keys repeat, and the whole burst
        // is one undo step.
        const float step = ImGui::GetIO().KeyShift ? 2.5f : 0.25f;
        glm::vec3 fwd(c.view.cameraFront.x, 0.0f, c.view.cameraFront.z);
        if (glm::length(fwd) < 1e-4f) fwd = glm::vec3(0.0f, 0.0f, -1.0f);
        fwd = glm::normalize(fwd);
        const glm::vec3 right(-fwd.z, 0.0f, fwd.x);
        glm::vec2 d(0.0f);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow,    true)) d += glm::vec2(fwd.x, fwd.z);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow,  true)) d -= glm::vec2(fwd.x, fwd.z);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) d += glm::vec2(right.x, right.z);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow,  true)) d -= glm::vec2(right.x, right.z);
        float dh = 0.0f;
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp,   true)) dh += step;
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown, true)) dh -= step;
        if (d != glm::vec2(0.0f) || dh != 0.0f) {
            c.beginEdit();
            if (d != glm::vec2(0.0f)) {
                sp.paths[c.sel].points[c.ptSel] += d * step;
                sp.touch(c.sel);
            }
            if (dh != 0.0f)
                sp.setLift(c.sel, c.ptSel, sp.liftOf(c.sel, c.ptSel) + dh);
        }
        const bool held =
            ImGui::IsKeyDown(ImGuiKey_UpArrow)   || ImGui::IsKeyDown(ImGuiKey_DownArrow) ||
            ImGui::IsKeyDown(ImGuiKey_LeftArrow) || ImGui::IsKeyDown(ImGuiKey_RightArrow) ||
            ImGui::IsKeyDown(ImGuiKey_PageUp)    || ImGui::IsKeyDown(ImGuiKey_PageDown);
        if (!held && c.editOpen && c.editOpen()) c.endEdit("Nudge point");
    }

    // --- Draw ----------------------------------------------------------------
    ImDrawList* dl = ImGui::GetWindowDrawList();
    drawLines(c, dl);
    drawPreview(c, dl);

    for (int pi = 0; pi < static_cast<int>(sp.paths.size()); ++pi) {
        const SplineSystem::Path& p = sp.paths[pi];
        if (!p.enabled) continue;
        const bool active = pi == c.sel;
        const int  n = static_cast<int>(p.points.size());
        for (int i = 0; i < n; ++i) {
            const glm::vec2 q = p.points[i];
            const float g = c.view.groundAt ? c.view.groundAt(q.x, q.y) : 0.0f;
            const float lift = i < static_cast<int>(p.lifts.size()) ? p.lifts[i] : 0.0f;
            // On the run itself -- for a bridge that is its deck, however far
            // above the ground the point is.
            const glm::vec3 hw = sp.pointWorld(pi, i) + glm::vec3(0.0f, 0.15f, 0.0f);
            ImVec2 s;
            if (!toScreen(hw, s)) continue;
            const bool sel   = active && i == c.ptSel;
            const bool hover = pi == hoverPath && i == hoverPt;
            const float rad  = sel ? 7.0f : (hover ? 6.5f : (active ? 5.0f : 3.5f));
            const ImU32 col  = sel   ? IM_COL32(255, 210,  60, 255)
                             : hover ? IM_COL32(210, 235, 255, 255)
                             : active? IM_COL32( 90, 180, 255, 235)
                                     : IM_COL32(130, 170, 210, 150);
            // A raised point gets a stalk down to the ground it left: without it a
            // lifted handle just looks like a point somewhere else on the terrain.
            if (std::abs(hw.y - 0.15f - g) > 0.05f) {
                ImVec2 gp;
                if (toScreen(glm::vec3(hw.x, g + 0.15f, hw.z), gp)) {
                    dl->AddLine(gp, s, IM_COL32(255, 210, 60, 140), 1.5f);
                    dl->AddCircle(gp, 2.5f, IM_COL32(255, 210, 60, 160), 0, 1.5f);
                }
            }
            dl->AddCircleFilled(s, rad, col);
            dl->AddCircle(s, rad, IM_COL32(0, 0, 0, 190), 0, 1.5f);
            if (sel && lift != 0.0f) {
                char hb[32];
                std::snprintf(hb, sizeof(hb), "%+.2f m", lift);
                const ImVec2 at(s.x + rad + 2.0f, s.y + 2.0f);
                dl->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f), IM_COL32(0, 0, 0, 200), hb);
                dl->AddText(at, IM_COL32(255, 225, 140, 245), hb);
            }
        }
    }
}

} // namespace splineedit
