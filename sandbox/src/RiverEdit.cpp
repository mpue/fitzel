#include "RiverEdit.hpp"

#include <cmath>
#include <cstdio>

#include "RiverSystem.hpp"

namespace riveredit {
namespace {

// Pixel radius the cursor grabs a handle within. Deliberately larger than the
// dot it grabs -- the tool has to be usable by someone who cannot put the cursor
// exactly on a five-pixel circle, and nothing else in the viewport is competing
// for the click while this mode is on.
constexpr float kGrabRadius = 14.0f;

// Where a control point's handle sits in the world: ON THE WATER where the
// course has been solved, on the bare ground before it has. A channel cut two
// metres into a hillside has its surface two metres down with it, and a handle
// left up on the original slope is a handle for a river that is not there.
glm::vec3 handleWorld(const Context& c, int path, int i) {
    const RiverSystem::Path& p = c.rivers.paths[path];
    const glm::vec2 q = p.points[i];
    float y = 0.0f;
    if (!c.rivers.handleHeight(path, i, y)) {
        y = (c.groundAt ? c.groundAt(q.x, q.y) : 0.0f) +
            (i < static_cast<int>(p.bias.size()) ? p.bias[i] : 0.0f);
    }
    return glm::vec3(q.x, y + 0.15f, q.y);
}

// Which control point a click at `P` (world XZ) should be spliced in front of:
// the nearest segment of the control polygon, or one of the ends.
int insertIndex(const RiverSystem::Path& p, glm::vec2 P) {
    const int n = static_cast<int>(p.points.size());
    if (n < 2) return n;
    float bestD = 1e30f, bestT = 0.0f;
    int   bestSeg = 0;
    for (int i = 0; i + 1 < n; ++i) {
        const glm::vec2 a = p.points[i], b = p.points[i + 1];
        const glm::vec2 ab = b - a;
        const float len2 = glm::dot(ab, ab);
        const float t = len2 > 1e-6f
            ? glm::clamp(glm::dot(P - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
        const float d = glm::distance(P, a + ab * t);
        if (d < bestD) { bestD = d; bestSeg = i; bestT = t; }
    }
    if (bestSeg == 0     && bestT <= 0.0f) return 0;
    if (bestSeg == n - 2 && bestT >= 1.0f) return n;
    return bestSeg + 1;
}

} // namespace

void handle(const Context& c) {
    RiverSystem& rv = c.rivers;
    if (rv.paths.empty()) return;
    if (c.sel >= static_cast<int>(rv.paths.size())) { c.sel = -1; c.ptSel = -1; }

    auto toScreen = [&](const glm::vec3& wp, ImVec2& out) {
        const glm::vec4 clip = c.viewProj * glm::vec4(wp, 1.0f);
        if (clip.w <= 1e-4f) return false;
        const glm::vec3 n = glm::vec3(clip) / clip.w;
        if (n.z > 1.0f) return false;
        out = ImVec2(c.origin.x + (n.x * 0.5f + 0.5f) * c.viewW,
                     c.origin.y + (1.0f - (n.y * 0.5f + 0.5f)) * c.viewH);
        return true;
    };

    // --- What the cursor is over ---------------------------------------------
    // Handles of EVERY watercourse are pickable, not just the selected one's:
    // clicking a neighbouring brook's point selects that brook, which is how a
    // valley with three streams in it stays navigable.
    int hoverPath = -1, hoverPt = -1;
    if (c.hovered && !c.dragging) {
        float bestD = kGrabRadius;
        for (int pi = 0; pi < static_cast<int>(rv.paths.size()); ++pi) {
            if (!rv.paths[pi].enabled) continue;
            for (int i = 0; i < static_cast<int>(rv.paths[pi].points.size()); ++i) {
                ImVec2 s;
                if (!toScreen(handleWorld(c, pi, i), s)) continue;
                const float d = std::hypot(s.x - c.mousePos.x, s.y - c.mousePos.y);
                if (d < bestD) { bestD = d; hoverPath = pi; hoverPt = i; }
            }
        }
    }

    // --- Click: pick a handle, or lay a new point ----------------------------
    if (c.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hoverPath >= 0) {
            c.sel        = hoverPath;
            c.ptSel      = hoverPt;
            c.dragging   = true;
            c.dragHeight = ImGui::GetIO().KeyCtrl;
            c.beginEdit();   // opened here, pushed on release: one drag, one step
        } else if (c.sel >= 0) {
            glm::vec3 h;
            if (c.pickTerrain && c.pickTerrain(c.mouseNdc, c.viewProj, h)) {
                RiverSystem::Path& p = rv.paths[c.sel];
                const int n = static_cast<int>(p.points.size());
                // With an END selected, extend from THAT end -- so a run of
                // clicks lays a course out point by point, which is the gesture
                // this tool is for. Otherwise splice into the nearest segment.
                const bool ends = n >= 2;
                const int at = (ends && c.ptSel == n - 1) ? n
                             : (ends && c.ptSel == 0)     ? 0
                             : insertIndex(p, glm::vec2(h.x, h.z));
                c.beginEdit();
                rv.insertPoint(c.sel, at, glm::vec2(h.x, h.z));
                c.endEdit("Add point");
                c.ptSel = at;   // the new point takes the selection
            }
        }
    }

    // --- Drag ----------------------------------------------------------------
    if (c.dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left) && c.sel >= 0 &&
        c.ptSel >= 0 && c.ptSel < static_cast<int>(rv.paths[c.sel].points.size())) {
        if (c.dragHeight) {
            // Metres per pixel at the handle's own depth, so the point tracks the
            // cursor instead of drifting away from it as you zoom in or out.
            const glm::vec3 hw = handleWorld(c, c.sel, c.ptSel);
            const float dist = glm::length(hw - c.cameraPos);
            const float mpp = 2.0f * dist *
                              std::tan(glm::radians(c.cameraFov * 0.5f)) /
                              std::max(1.0f, c.viewH);
            const float dy = ImGui::GetIO().MouseDelta.y;
            if (dy != 0.0f)
                rv.setBias(c.sel, c.ptSel, rv.biasOf(c.sel, c.ptSel) - dy * mpp);
        } else {
            glm::vec3 h;
            if (c.pickTerrain && c.pickTerrain(c.mouseNdc, c.viewProj, h)) {
                rv.paths[c.sel].points[c.ptSel] = glm::vec2(h.x, h.z);
                rv.touch(c.sel);
            }
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && c.dragging) {
        c.dragging = false;
        c.endEdit(c.dragHeight ? "Water level at point" : "Move point");
    }

    // --- Delete + nudge ------------------------------------------------------
    if (c.sel >= 0 && c.ptSel >= 0 &&
        c.ptSel < static_cast<int>(rv.paths[c.sel].points.size())) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            c.beginEdit();
            rv.erasePoint(c.sel, c.ptSel);
            c.endEdit("Delete point");
            c.ptSel = -1;
        }
    }
    if (c.sel >= 0 && c.ptSel >= 0 && !c.dragging &&
        c.ptSel < static_cast<int>(rv.paths[c.sel].points.size()) &&
        !ImGui::GetIO().WantTextInput) {
        // Camera relative, because "left" means what you see, not where the
        // world's X axis happens to point. Held keys repeat, and the whole burst
        // is one undo step -- and therefore one cut.
        const float step = ImGui::GetIO().KeyShift ? 2.5f : 0.25f;
        glm::vec3 fwd(c.cameraFront.x, 0.0f, c.cameraFront.z);
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
                rv.paths[c.sel].points[c.ptSel] += d * step;
                rv.touch(c.sel);
            }
            if (dh != 0.0f)
                rv.setBias(c.sel, c.ptSel, rv.biasOf(c.sel, c.ptSel) + dh);
        }
        const bool held =
            ImGui::IsKeyDown(ImGuiKey_UpArrow)   || ImGui::IsKeyDown(ImGuiKey_DownArrow) ||
            ImGui::IsKeyDown(ImGuiKey_LeftArrow) || ImGui::IsKeyDown(ImGuiKey_RightArrow) ||
            ImGui::IsKeyDown(ImGuiKey_PageUp)    || ImGui::IsKeyDown(ImGuiKey_PageDown);
        if (!held && c.editOpen && c.editOpen()) c.endEdit("Nudge point");
    }

    // --- Draw ----------------------------------------------------------------
    ImDrawList* dl = ImGui::GetWindowDrawList();

    for (int pi = 0; pi < static_cast<int>(rv.paths.size()); ++pi) {
        if (!rv.paths[pi].enabled) continue;
        const rivergen::Course& co = rv.course(pi);
        const int n = static_cast<int>(co.line.size());
        if (n < 2) continue;
        const bool active = pi == c.sel;

        // The two waterlines, so the channel reads as a channel rather than as a
        // line with a width typed into a panel somewhere else.
        for (int side = -1; side <= 1; side += 2) {
            ImVec2 prev; bool have = false;
            for (int i = 0; i < n; ++i) {
                const glm::vec2 nrm(co.dir[i].y, -co.dir[i].x);
                const glm::vec3 w = co.line[i] +
                    glm::vec3(nrm.x, 0.0f, nrm.y) * (co.half[i] * static_cast<float>(side));
                ImVec2 s;
                if (!toScreen(w, s)) { have = false; continue; }
                if (have)
                    dl->AddLine(prev, s,
                                active ? IM_COL32(120, 200, 255, 150)
                                       : IM_COL32(110, 150, 185, 80),
                                1.0f);
                prev = s; have = true;
            }
        }

        // The centreline, coloured by what the solve made of each stretch: calm
        // water blue, whitewater white. This is the tool's feedback loop -- the
        // author never typed "waterfall here", so it has to be visible that the
        // ground put one there.
        ImVec2 prev; bool have = false;
        for (int i = 0; i < n; ++i) {
            ImVec2 s;
            if (!toScreen(co.line[i], s)) { have = false; continue; }
            if (have) {
                const float w = co.white[i];
                const int   r = static_cast<int>(glm::mix(70.0f,  255.0f, w));
                const int   g = static_cast<int>(glm::mix(170.0f, 255.0f, w));
                const int   b = 255;
                const int   a = active ? 235 : 120;
                dl->AddLine(prev, s, IM_COL32(r, g, b, a),
                            active ? (1.8f + 1.6f * w) : 1.3f);
            }
            prev = s; have = true;
        }

        // Which way it flows. The direction is DECIDED by the terrain, not by the
        // author, so it has to be on screen -- a chevron every twenty-five metres
        // is enough to read it at a glance and few enough not to clutter.
        if (active) {
            float next = 0.0f;
            for (int i = 1; i < n; ++i) {
                if (co.s[i] < next) continue;
                next = co.s[i] + 25.0f;
                ImVec2 a, b;
                if (!toScreen(co.line[i - 1], a) || !toScreen(co.line[i], b)) continue;
                const float dx = b.x - a.x, dy = b.y - a.y;
                const float len = std::hypot(dx, dy);
                if (len < 6.0f) continue;
                const float ux = dx / len, uy = dy / len;
                const float k = 7.0f;
                const ImVec2 tip(b.x, b.y);
                dl->AddLine(tip, ImVec2(tip.x - (ux * 0.87f - uy * 0.5f) * k,
                                        tip.y - (uy * 0.87f + ux * 0.5f) * k),
                            IM_COL32(255, 255, 255, 190), 1.6f);
                dl->AddLine(tip, ImVec2(tip.x - (ux * 0.87f + uy * 0.5f) * k,
                                        tip.y - (uy * 0.87f - ux * 0.5f) * k),
                            IM_COL32(255, 255, 255, 190), 1.6f);
            }
        }
    }

    // --- Handles -------------------------------------------------------------
    for (int pi = 0; pi < static_cast<int>(rv.paths.size()); ++pi) {
        const RiverSystem::Path& p = rv.paths[pi];
        if (!p.enabled) continue;
        const bool active = pi == c.sel;
        for (int i = 0; i < static_cast<int>(p.points.size()); ++i) {
            const glm::vec3 hw = handleWorld(c, pi, i);
            ImVec2 s;
            if (!toScreen(hw, s)) continue;
            const bool sel   = active && i == c.ptSel;
            const bool hover = pi == hoverPath && i == hoverPt;
            const float rad  = sel ? 7.0f : (hover ? 6.5f : (active ? 5.0f : 3.5f));
            const ImU32 col  = sel   ? IM_COL32(255, 210,  60, 255)
                             : hover ? IM_COL32(210, 235, 255, 255)
                             : active? IM_COL32( 90, 200, 255, 235)
                                     : IM_COL32(120, 165, 205, 150);
            // A stalk down to the ground the water left. Without it a handle on
            // water standing above the hillside is just a dot in mid-air -- and
            // standing above the hillside is exactly what a dammed pool does.
            const float g = c.groundAt ? c.groundAt(hw.x, hw.z) : hw.y;
            if (std::fabs(g - (hw.y - 0.15f)) > 0.05f) {
                ImVec2 gp;
                if (toScreen(glm::vec3(hw.x, g, hw.z), gp)) {
                    dl->AddLine(gp, s, IM_COL32(120, 200, 255, 130), 1.5f);
                    dl->AddCircle(gp, 2.5f, IM_COL32(120, 200, 255, 150), 0, 1.5f);
                }
            }
            dl->AddCircleFilled(s, rad, col);
            dl->AddCircle(s, rad, IM_COL32(0, 0, 0, 190), 0, 1.5f);
            if (sel) {
                const float bias = c.rivers.biasOf(pi, i);
                if (bias != 0.0f) {
                    char hb[32];
                    std::snprintf(hb, sizeof(hb), "%+.2f m", bias);
                    const ImVec2 at(s.x + rad + 2.0f, s.y + 2.0f);
                    dl->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f),
                                IM_COL32(0, 0, 0, 200), hb);
                    dl->AddText(at, IM_COL32(180, 230, 255, 245), hb);
                }
            }
        }
    }
}

} // namespace riveredit
