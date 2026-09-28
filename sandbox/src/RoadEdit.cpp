#include "RoadEdit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "RoadJunction.hpp"
#include "RoadLoop.hpp"
#include "RoadSet.hpp"
#include "RoadSystem.hpp"

namespace roadedit {

bool removePoint(RoadSystem& road, int k) {
    if (k < 0 || k >= static_cast<int>(road.roadPts.size())) return false;
    road.erasePoint(k);
    std::vector<RoadSystem::BridgeSpec> keep;
    for (RoadSystem::BridgeSpec b : road.bridges) {
        if (b.a == k || b.b == k) continue;
        if (b.a > k) --b.a;
        if (b.b > k) --b.b;
        keep.push_back(b);
    }
    road.bridges.swap(keep);
    // Tunnels name their ends the same way, so they need the same
    // bookkeeping -- see the loop note below.
    std::vector<RoadSystem::BridgeSpec> keepT;
    for (RoadSystem::BridgeSpec t : road.tunnels) {
        if (t.a == k || t.b == k) continue;
        if (t.a > k) --t.a;
        if (t.b > k) --t.b;
        keepT.push_back(t);
    }
    road.tunnels.swap(keepT);
    // Loops name their ends the same way a bridge does, so they need the
    // same bookkeeping -- a loop left pointing at a deleted point would
    // silently move to whatever slid into its place.
    std::vector<roadloop::Spec> keepL;
    for (roadloop::Spec l : road.loops) {
        if (l.a == k || l.b == k) continue;
        if (l.a > k) --l.a;
        if (l.b > k) --l.b;
        keepL.push_back(l);
    }
    road.loops.swap(keepL);
    road.needsBuild = true;
    return true;
}

int insertPoint(RoadSystem& road, int at, glm::vec2 p) {
    at = glm::clamp(at, 0, static_cast<int>(road.roadPts.size()));
    // A point dropped between two others inherits their height, so
    // inserting into a raised stretch doesn't punch a hole in it.
    float lift = 0.0f;
    const int n = static_cast<int>(road.roadPts.size());
    if (n > 0) {
        const int a = std::clamp(at - 1, 0, n - 1);
        const int b = std::clamp(at,     0, n - 1);
        lift = 0.5f * (road.liftOf(a) + road.liftOf(b));
    }
    // ...and its cross-fall, for the same reason: dropping a point into a
    // banked corner should not flatten it at that one station.
    float bank = 0.0f;
    if (n > 0) {
        const int a = std::clamp(at - 1, 0, n - 1);
        const int b = std::clamp(at,     0, n - 1);
        bank = 0.5f * (road.bankOf(a) + road.bankOf(b));
    }
    road.insertPoint(at, p, lift, bank);
    for (RoadSystem::BridgeSpec& b : road.bridges) {
        if (b.a >= at) ++b.a;
        if (b.b >= at) ++b.b;
    }
    for (RoadSystem::BridgeSpec& t : road.tunnels) {
        if (t.a >= at) ++t.a;
        if (t.b >= at) ++t.b;
    }
    for (roadloop::Spec& l : road.loops) {
        if (l.a >= at) ++l.a;
        if (l.b >= at) ++l.b;
    }
    road.needsBuild = true;
    return at;
}

int insertIndex(const RoadSystem& road, glm::vec2 P) {
    const int n = static_cast<int>(road.roadPts.size());
    if (n < 2) return n; // 0 or 1 points: nothing to insert between -> append
    float bestD = 1e30f, bestT = 0.0f;
    int   bestSeg = 0;
    const int segs = road.closed ? n : n - 1; // closed loops wrap last->first
    for (int i = 0; i < segs; ++i) {
        const glm::vec2 a = road.roadPts[i];
        const glm::vec2 b = road.roadPts[(i + 1) % n];
        const glm::vec2 ab = b - a;
        const float len2 = glm::dot(ab, ab);
        const float t = len2 > 1e-6f
            ? glm::clamp(glm::dot(P - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
        const float d = glm::distance(P, a + ab * t);
        if (d < bestD) { bestD = d; bestSeg = i; bestT = t; }
    }
    if (!road.closed) {
        if (bestSeg == 0     && bestT <= 0.0f) return 0; // before the start
        if (bestSeg == n - 2 && bestT >= 1.0f) return n; // past the end
    }
    return bestSeg + 1;
}

void handle(const Context& c) {
    // The handles belong to the road the Roads panel has selected;
    // the others are drawn by the renderer and are not grabbable
    // (there is one set of point indices, and a bridge names its
    // ends by index). Bound here rather than per use: the selection
    // cannot change in the middle of a viewport gesture.
    RoadSystem& road = c.roads.active();
    const glm::mat4& vp = c.view.viewProj;
    // Handles sit on the road, not on the ground: a lifted point
    // has to be grabbable where its road actually runs.
    auto handleWorld = [&](int i) {
        return glm::vec3(road.roadPts[i].x,
                         c.view.groundAt(road.roadPts[i].x, road.roadPts[i].y)
                             + 0.10f + road.liftOf(i),
                         road.roadPts[i].y);
    };
    // False behind the camera or past the far plane.
    auto toScreen = [&](const glm::vec3& wp, ImVec2& out) { return c.view.project(wp, out); };

    // Handle under the cursor, or -1. Computed every frame (not
    // only on click) so the drawing below can show what a click
    // would grab -- and so picking and highlighting can never
    // disagree about which point that is.
    int roadHover = -1;
    if (c.view.hovered && !c.dragging) {
        float bestD = 12.0f; // pixel grab radius
        for (int i = 0; i < static_cast<int>(road.roadPts.size()); ++i) {
            ImVec2 sp;
            if (!toScreen(handleWorld(i), sp)) continue;
            const float d = std::hypot(sp.x - c.view.mousePos.x, sp.y - c.view.mousePos.y);
            if (d < bestD) { bestD = d; roadHover = i; }
        }
    }

    // A loop needs a grip of its own. It is NAMED by two control
    // points, but it stands twenty metres over them -- so the thing
    // to point at is the crown of the turn, where the loop actually
    // is, and picking it selects the pair whose row the panel edits.
    // Control points win the contest: they are smaller targets and
    // there are far more of them.
    const std::vector<roadloop::Loop>& builtLoops = road.loopGeometry();
    auto loopCrown = [](const roadloop::Loop& lp) {
        glm::vec3 top = lp.frames.empty() ? glm::vec3(0.0f)
                                          : lp.frames.front().pos;
        for (const roadloop::Frame& f : lp.frames)
            if (f.pos.y > top.y) top = f.pos;
        return top;
    };
    int loopHover = -1;
    if (c.view.hovered && !c.dragging && roadHover < 0) {
        float bestD = 15.0f; // pixel grab radius, a touch larger
        for (int i = 0; i < static_cast<int>(builtLoops.size()); ++i) {
            if (builtLoops[i].frames.empty()) continue;
            ImVec2 sp;
            if (!toScreen(loopCrown(builtLoops[i]), sp)) continue;
            const float d = std::hypot(sp.x - c.view.mousePos.x, sp.y - c.view.mousePos.y);
            if (d < bestD) { bestD = d; loopHover = i; }
        }
    }

    // Pick / add on click.
    if (c.view.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const int best = roadHover;
        if (best >= 0) {
            // Shift-click marks the far end of a bridge instead of
            // re-selecting; plain click picks (and starts a drag);
            // Ctrl+drag raises/lowers the point instead of moving
            // it across the ground.
            if (ImGui::GetIO().KeyShift && c.sel >= 0 && best != c.sel)
                c.sel2 = best;
            else {
                c.sel = best; c.sel2 = -1; c.dragging = true;
                c.dragHeight = ImGui::GetIO().KeyCtrl;
                // Opened here, pushed on mouse release: the whole
                // drag is one undo step.
                c.beginEdit();
            }
        } else if (loopHover >= 0) {
            // Selecting the turn selects the pair that names it, so
            // the panel's row for THIS loop is the live one and its
            // radius is one field away -- rather than a list of
            // "#2 -> #1" to count out against the viewport.
            c.sel  = builtLoops[loopHover].pa;
            c.sel2 = builtLoops[loopHover].pb;
        } else {
            glm::vec3 h;
            if (c.view.pickTerrain(c.view.mouseNdc, vp, h)) {
                // With an END of the road selected, extend from
                // THAT end. Otherwise insert at the nearest
                // segment, so a click on an existing road drops a
                // waypoint in the middle.
                //
                // Nearest-in-plan-view is the wrong question once
                // roads cross in three dimensions: drawing a road
                // UNDER a bridge puts every click right beside the
                // stretch flying overhead, and the new point gets
                // spliced into that stretch instead of continuing
                // the one being drawn. Having picked an end, the
                // author has already said which end they mean --
                // and since the new point takes the selection, a
                // run of clicks lays out a road point by point.
                const int n = static_cast<int>(road.roadPts.size());
                const bool ends = !road.closed && n >= 2;
                const int at = (ends && c.sel == n - 1) ? n
                             : (ends && c.sel == 0)     ? 0
                             : insertIndex(road, {h.x, h.z});
                c.addPoint(at, glm::vec2(h.x, h.z));
            }
        }
    }
    // Drag the selected handle: across the terrain, or (Ctrl)
    // straight up and down.
    if (c.dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        c.sel >= 0 && c.sel < static_cast<int>(road.roadPts.size())) {
        if (c.dragHeight) {
            // Metres per pixel at the handle's own depth, so the
            // point tracks the cursor instead of drifting away
            // from it as you zoom in or out.
            const glm::vec3 hw = handleWorld(c.sel);
            const float mpp = c.view.metresPerPixel(hw);
            const float dy = ImGui::GetIO().MouseDelta.y;
            if (dy != 0.0f)
                road.setLift(c.sel,
                             road.liftOf(c.sel) - dy * mpp);
        } else {
            glm::vec3 h;
            if (c.view.pickTerrain(c.view.mouseNdc, vp, h)) {
                road.roadPts[c.sel] = glm::vec2(h.x, h.z);
                road.needsBuild = true;
            }
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && c.dragging) {
        c.dragging = false;
        c.endEdit(c.dragHeight ? "Raise point" : "Move point");
    }
    // Delete the selected point.
    if (c.sel >= 0 && c.sel < static_cast<int>(road.roadPts.size()) &&
        ImGui::IsKeyPressed(ImGuiKey_Delete))
        c.deletePoint(c.sel);

    // Keyboard nudge for the selected point: the arrows move it
    // across the ground, PageUp/Down raise and lower it. Camera
    // relative, because "left" means what you see, not where the
    // world's X axis happens to point. Held keys repeat, and the
    // whole burst is bracketed into one undo step (released ->
    // committed below).
    if (c.sel >= 0 && c.sel < static_cast<int>(road.roadPts.size()) &&
        !ImGui::GetIO().WantTextInput && !c.dragging) {
        const float step = (ImGui::GetIO().KeyShift ? 2.5f : 0.25f);
        glm::vec3 f = c.view.cameraFront;
        f.y = 0.0f;
        if (glm::length(f) < 1e-4f) f = glm::vec3(0, 0, -1);
        f = glm::normalize(f);
        const glm::vec3 r(-f.z, 0.0f, f.x); // right-hand perp in XZ
        glm::vec2 d(0.0f);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow,    true)) d += glm::vec2(f.x, f.z);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow,  true)) d -= glm::vec2(f.x, f.z);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) d += glm::vec2(r.x, r.z);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow,  true)) d -= glm::vec2(r.x, r.z);
        float dh = 0.0f;
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp,   true)) dh += step;
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown, true)) dh -= step;
        if (d != glm::vec2(0.0f) || dh != 0.0f) {
            c.beginEdit();
            if (d != glm::vec2(0.0f)) {
                road.roadPts[c.sel] += d * step;
                road.needsBuild = true;
            }
            if (dh != 0.0f)
                road.setLift(c.sel, road.liftOf(c.sel) + dh);
        }
        // Burst over (no nudge key still down) -> close the step.
        const bool held =
            ImGui::IsKeyDown(ImGuiKey_UpArrow)   || ImGui::IsKeyDown(ImGuiKey_DownArrow) ||
            ImGui::IsKeyDown(ImGuiKey_LeftArrow) || ImGui::IsKeyDown(ImGuiKey_RightArrow) ||
            ImGui::IsKeyDown(ImGuiKey_PageUp)    || ImGui::IsKeyDown(ImGuiKey_PageDown);
        if (!held && c.editOpen()) c.endEdit("Nudge point");
    }

    // Live preview: the smoothed spline as it will be built -- the
    // curved centreline plus its left/right edges at the road width.
    // Yellow = not yet built, cyan = matches the committed road.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const RoadSystem::Preview pv = road.previewGeometry();
    const ImU32 edgeCol = road.needsBuild ? IM_COL32(255, 210, 70, 200)
                                          : IM_COL32(90, 210, 190, 190);
    const ImU32 midCol  = road.needsBuild ? IM_COL32(255, 235, 140, 150)
                                          : IM_COL32(150, 235, 220, 130);
    auto drawPolyline = [&](const std::vector<glm::vec3>& line,
                            ImU32 col, float th) {
        ImVec2 prev; bool have = false;
        for (const glm::vec3& wp : line) {
            ImVec2 sp;
            if (!toScreen(wp, sp)) { have = false; continue; }
            if (have) dl->AddLine(prev, sp, col, th);
            prev = sp; have = true;
        }
    };
    drawPolyline(pv.left,  edgeCol, 2.0f);
    drawPolyline(pv.right, edgeCol, 2.0f);
    drawPolyline(pv.center, midCol, 1.5f);

    // Which stretch of centreline belongs to which pair of control
    // points, so a bridge can be shown where it actually runs
    // instead of only as "#3 -> #7" in the panel. The candidate
    // pair (selected + shift-clicked) is drawn the same way before
    // it exists, which is what makes picking the two ends
    // something you can see rather than count out.
    auto drawSpan = [&](int pa, int pb, ImU32 col, float th) {
        const int n = static_cast<int>(pv.ptSample.size());
        const int a = std::min(pa, pb), b = std::max(pa, pb);
        if (a < 0 || b >= n) return;
        ImVec2 prev; bool have = false;
        for (int i = pv.ptSample[a];
             i <= pv.ptSample[b] && i < static_cast<int>(pv.center.size()); ++i) {
            ImVec2 sp;
            if (!toScreen(pv.center[i], sp)) { have = false; continue; }
            if (have) dl->AddLine(prev, sp, col, th);
            prev = sp; have = true;
        }
    };
    // The same, addressed by centreline SAMPLE rather than by
    // control point -- a loop's footprint starts at a point but ends
    // wherever its own geometry comes back down.
    auto drawFootprint = [&](int i0, int i1, ImU32 col, float th) {
        ImVec2 prev; bool have = false;
        for (int i = std::max(0, i0);
             i <= i1 && i < static_cast<int>(pv.center.size()); ++i) {
            ImVec2 sp;
            if (!toScreen(pv.center[i], sp)) { have = false; continue; }
            if (have) dl->AddLine(prev, sp, col, th);
            prev = sp; have = true;
        }
    };
    for (const RoadSystem::BridgeSpec& b : road.bridges)
        drawSpan(b.a, b.b, IM_COL32(255, 140, 60, 220), 4.0f);
    // Bored stretches in a cooler colour: a road can carry both,
    // and one orange for two opposite structures reads as neither.
    for (const RoadSystem::BridgeSpec& t : road.tunnels)
        drawSpan(t.a, t.b, IM_COL32(90, 190, 255, 220), 4.0f);
    if (c.sel >= 0 && c.sel2 >= 0 && c.sel != c.sel2)
        drawSpan(c.sel, c.sel2, IM_COL32(255, 255, 255, 200), 3.0f);

    // --- Loops -----------------------------------------------
    // A loop gets a colour of its own: it is neither a deck nor a
    // bore, and borrowing one of theirs would make three structures
    // read as two. The ring is the turn's own centreline projected,
    // so it is drawn where the loop stands -- which is what makes a
    // loop something to look at and click rather than a row in a
    // list naming two numbers.
    auto drawRing = [&](const roadloop::Loop& lp, ImU32 col, float th,
                        bool dashed) {
        ImVec2 prev; bool have = false;
        for (std::size_t i = 0; i < lp.frames.size(); ++i) {
            ImVec2 sp;
            if (!toScreen(lp.frames[i].pos, sp)) { have = false; continue; }
            if (have && (!dashed || (i / 4) % 2 == 0))
                dl->AddLine(prev, sp, col, th);
            prev = sp; have = true;
        }
    };
    auto shadowText = [&](ImVec2 at, ImU32 col, const char* txt) {
        dl->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f),
                    IM_COL32(0, 0, 0, 200), txt);
        dl->AddText(at, col, txt);
    };
    for (int i = 0; i < static_cast<int>(builtLoops.size()); ++i) {
        const roadloop::Loop& lp = builtLoops[i];
        if (lp.frames.empty()) continue;
        const bool lsel = (lp.pa == c.sel && lp.pb == c.sel2) ||
                          (lp.pa == c.sel2 && lp.pb == c.sel);
        const bool lhov = (i == loopHover);
        const ImU32 col = lsel ? IM_COL32(255, 190, 255, 255)
                        : lhov ? IM_COL32(240, 205, 255, 250)
                               : IM_COL32(190, 130, 255, 205);
        // The ground the turn stands in place of -- its FOOTPRINT,
        // not the pair's whole stretch: a turn advances only half
        // its radius while it goes round, so the road past its feet
        // is still road. This is the same span the ribbon leaves
        // out, so it answers "where did my road go?" exactly.
        drawFootprint(lp.sa, lp.sb, (col & 0x00FFFFFFu) | 0x55000000u,
                      4.0f);
        drawRing(lp, col, (lsel || lhov) ? 3.0f : 2.0f, /*dashed=*/false);
        ImVec2 sp;
        if (toScreen(loopCrown(lp), sp)) {
            const float rad = (lsel || lhov) ? 7.0f : 5.5f;
            dl->AddCircleFilled(sp, rad, col);
            dl->AddCircle(sp, rad, IM_COL32(0, 0, 0, 190), 0, 1.5f);
            char lb[80];
            std::snprintf(lb, sizeof(lb), "Loop #%d-#%d  %.0f m tall%s",
                          lp.pa, lp.pb, lp.radius * 2.0f,
                          lp.inverts ? "" : "  (a hump: too long to turn over)");
            shadowText(ImVec2(sp.x + rad + 3.0f, sp.y - rad - 2.0f),
                       IM_COL32(240, 220, 255, 245), lb);
        }
    }

    // --- Junctions -------------------------------------------
    // Nothing about a junction is authored, so this is the only
    // place the author gets to see what the build FOUND: the apron
    // it laid and which road it belongs to. A fourth colour,
    // because a junction is neither a deck, a bore nor a turn --
    // and the ones this road is not part of are drawn faint rather
    // than not at all, since "why is there no junction here?" is
    // answered by seeing where the others are.
    for (const roadjunction::Crossing& jx : c.roads.junctions()) {
        if (jx.plate.size() < 3) continue;
        const int me = c.roads.selected();
        const bool mine = (jx.roadA == me || jx.roadB == me);
        const ImU32 col = mine ? IM_COL32(120, 240, 190, 235)
                               : IM_COL32(120, 240, 190, 165);
        ImVec2 prev; bool have = false;
        for (std::size_t i = 0; i <= jx.plate.size(); ++i) {
            const glm::vec2& p = jx.plate[i % jx.plate.size()];
            ImVec2 sp;
            if (!toScreen(glm::vec3(p.x, jx.y + 0.10f, p.y), sp)) {
                have = false;
                continue;
            }
            if (have) dl->AddLine(prev, sp, col, mine ? 2.5f : 1.5f);
            prev = sp; have = true;
        }
        ImVec2 sp;
        if (!mine || !toScreen(glm::vec3(jx.at.x, jx.y + 0.10f, jx.at.y), sp))
            continue;
        const int other = (jx.roadA == me) ? jx.roadB : jx.roadA;
        const float deg = glm::degrees(
            std::asin(glm::clamp(jx.sinAngle, 0.0f, 1.0f)));
        char lb[96];
        if (other == me)
            std::snprintf(lb, sizeof(lb), "%s  with itself  %.0f\xC2\xB0",
                          jx.tee ? "T" : "Junction", deg);
        else if (other >= 0 && other < c.roads.count())
            std::snprintf(lb, sizeof(lb), "%s  %s  %.0f\xC2\xB0",
                          jx.tee ? "T" : "Junction",
                          c.roads.at(other).name.c_str(), deg);
        else
            continue;
        shadowText(ImVec2(sp.x + 8.0f, sp.y - 9.0f),
                   IM_COL32(180, 255, 225, 245), lb);
    }

    // What "Create loop" would give you, drawn before it is asked
    // for: with two points picked and no loop on them yet, the turn
    // that button would build is ghosted in. Planned by the SAME
    // routine the build runs, so the ghost cannot promise a shape
    // the road would not produce -- including refusing to: a pair
    // too close together plans nothing, and says so where the eye
    // already is, instead of leaving a button that quietly does
    // nothing.
    if (c.sel >= 0 && c.sel2 >= 0 && c.sel != c.sel2 &&
        pv.center.size() >= 2) {
        bool already = false;
        for (const roadloop::Spec& sp : road.loops)
            already = already ||
                      (sp.a == c.sel && sp.b == c.sel2) ||
                      (sp.a == c.sel2 && sp.b == c.sel);
        if (!already) {
            std::vector<glm::vec2> gcen(pv.center.size());
            std::vector<float>     gprof(pv.center.size());
            for (std::size_t i = 0; i < pv.center.size(); ++i) {
                gcen[i]  = glm::vec2(pv.center[i].x, pv.center[i].z);
                gprof[i] = pv.center[i].y;
            }
            roadloop::Spec want;
            want.a = c.sel; want.b = c.sel2;
            const std::vector<roadloop::Loop> ghost =
                roadloop::plan(gcen, gprof, pv.ptSample, {want},
                               road.width);
            const ImU32 ghostCol = IM_COL32(230, 180, 255, 190);
            if (!ghost.empty() && !ghost.front().frames.empty()) {
                const roadloop::Loop& lp = ghost.front();
                drawRing(lp, ghostCol, 2.0f, /*dashed=*/true);
                ImVec2 sp;
                if (toScreen(loopCrown(lp), sp)) {
                    // How far the turn travels while it goes round
                    // is the distance between the two points, so
                    // both numbers a loop is judged by are on screen
                    // while they are still being chosen.
                    float run = 0.0f;
                    for (int k = lp.sa; k < lp.sb &&
                         k + 1 < static_cast<int>(pv.center.size()); ++k)
                        run += glm::length(
                            glm::vec2(pv.center[k + 1].x - pv.center[k].x,
                                      pv.center[k + 1].z - pv.center[k].z));
                    char lb[128];
                    if (!lp.inverts)
                        std::snprintf(lb, sizeof(lb),
                                      "%.0f m of road is too far for a %.0f m "
                                      "radius: a hump, not a loop",
                                      run, lp.radius);
                    else if (lp.sway > 0.05f)
                        std::snprintf(lb, sizeof(lb),
                                      "Loop here: %.0f m tall, %.0f m of road, "
                                      "swaying %.0f m to clear itself",
                                      lp.radius * 2.0f, run, lp.sway);
                    else
                        std::snprintf(lb, sizeof(lb),
                                      "Loop here: %.0f m tall, %.0f m of road",
                                      lp.radius * 2.0f, run);
                    shadowText(ImVec2(sp.x + 9.0f, sp.y - 9.0f),
                               lp.inverts ? ghostCol
                                          : IM_COL32(255, 190, 120, 235), lb);
                }
            } else {
                const int pa = std::min(c.sel, c.sel2);
                ImVec2 sp;
                if (pa < static_cast<int>(pv.ptSample.size()) &&
                    pv.ptSample[pa] < static_cast<int>(pv.center.size()) &&
                    toScreen(pv.center[pv.ptSample[pa]] +
                                 glm::vec3(0.0f, 1.5f, 0.0f), sp))
                    shadowText(ImVec2(sp.x + 9.0f, sp.y - 9.0f),
                               IM_COL32(255, 150, 130, 235),
                               "Too close together for a loop");
            }
        }
    }

    // Handles. A point the terrain hides is drawn faint rather
    // than dropped: it still has to be findable behind a ridge,
    // just not compete with the ones you can actually see.
    auto occluded = [&](const glm::vec3& wp) {
        const glm::vec3 eye = c.view.cameraPos;
        const glm::vec3 d   = wp - eye;
        for (int s = 1; s < 24; ++s) { // skip the endpoints
            const glm::vec3 p = eye + d * (static_cast<float>(s) / 24.0f);
            if (c.view.groundAt(p.x, p.z) > p.y + 0.25f) return true;
        }
        return false;
    };
    const int ptCount = static_cast<int>(road.roadPts.size());
    for (int i = 0; i < ptCount; ++i) {
        ImVec2 sp;
        const glm::vec3 hw = handleWorld(i);
        if (!toScreen(hw, sp)) continue;
        const bool sel   = (i == c.sel);
        const bool mate  = (i == c.sel2);
        const bool hover = (i == roadHover);
        const float rad  = sel ? 7.0f : (hover ? 6.5f : 5.0f);
        ImU32 col = sel   ? IM_COL32(255, 210,  60, 255)
                  : mate  ? IM_COL32(255, 140,  60, 245)
                  : hover ? IM_COL32(210, 235, 255, 255)
                          : IM_COL32( 90, 180, 255, 235);
        if (occluded(hw) && !sel && !hover)
            col = (col & 0x00FFFFFF) | 0x50000000; // keep hue, drop alpha
        // A raised or sunken point gets a stalk down to the ground
        // it left: without it a lifted handle just looks like a
        // point somewhere else on the terrain.
        if (road.liftOf(i) != 0.0f) {
            ImVec2 gp;
            const glm::vec3 g(hw.x, hw.y - road.liftOf(i), hw.z);
            if (toScreen(g, gp)) {
                dl->AddLine(gp, sp, IM_COL32(255, 210, 60, 140), 1.5f);
                dl->AddCircle(gp, 2.5f, IM_COL32(255, 210, 60, 160), 0, 1.5f);
            }
        }
        dl->AddCircleFilled(sp, rad, col);
        dl->AddCircle(sp, rad, IM_COL32(0, 0, 0, 190), 0, 1.5f);
        // Index labels: all of them while the road is short enough
        // to read, otherwise only the ones a bridge or the cursor
        // is about to involve -- a hundred numbers along a long
        // road is noise, not information.
        if (ptCount <= 30 || sel || mate || hover) {
            char lbl[16];
            std::snprintf(lbl, sizeof(lbl), "%d", i);
            const ImVec2 at(sp.x + rad + 2.0f, sp.y - rad - 2.0f);
            dl->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f),
                        IM_COL32(0, 0, 0, 200), lbl);
            dl->AddText(at, IM_COL32(235, 240, 250, 235), lbl);
        }
        // The selected point also states its height, so a stretch
        // can be set to a round number without hunting in the panel.
        if (sel && road.liftOf(i) != 0.0f) {
            char hb[32];
            std::snprintf(hb, sizeof(hb), "%+.2f m", road.liftOf(i));
            const ImVec2 at(sp.x + rad + 2.0f, sp.y + 2.0f);
            dl->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f),
                        IM_COL32(0, 0, 0, 200), hb);
            dl->AddText(at, IM_COL32(255, 225, 140, 245), hb);
        }
    }
}

} // namespace roadedit
