#include "SculptPanel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <imgui.h>

#include "EditorContext.hpp"

namespace sculptui {

void drawPanel(const PanelState& s) {
    if (!s.show) return;
    if (ImGui::Begin("Terrain Sculpt", &s.show)) {
        ImGui::Checkbox("Sculpt mode", &s.sculptMode);
        if (s.sculptMode)
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.6f, 1.0f),
                s.brush.tool == 8 ? "Press and drag to pull the ground out"
              : s.brush.tool == 9 ? "Click for a shower | hold to keep it raining there"
                            : "Hold LMB to sculpt | Alt inverts raise/lower");
        else
            ImGui::TextDisabled("Enable to reshape the ground with the brush");

        const char* tools[] = {"Raise", "Lower", "Smooth", "Flatten",
                               "Erode", "Stamp", "Noise", "Carve", "Pull", "Rain"};
        ImGui::Combo("Tool", &s.brush.tool, tools, IM_ARRAYSIZE(tools));
        ImGui::SliderFloat("Radius", &s.brush.radius, 1.0f, 40.0f, "%.1f m");
        // Pull has no rate: it is not a dab repeated while you hold the button,
        // it is one gesture whose height is wherever you let go. So the strength
        // slider would be a knob that does nothing, which is worse than no knob.
        if (s.brush.tool != 8)
            ImGui::SliderFloat("Strength", &s.brush.strength, 0.05f, 1.0f);
        if (s.brush.tool == 3)
            ImGui::SliderFloat("Flatten height", &s.brush.flattenHeight,
                               -40.0f, 60.0f, "%.1f m");
        if (s.brush.tool == 4)
            ImGui::TextDisabled("Weathers slopes: material slides downhill.");
        if (s.brush.tool == 9) {
            ImGui::TextDisabled("Raindrops run downhill, cut gullies into the slope\n"
                                "and leave the silt in fans where it levels out.");
            ImGui::TextDisabled("The rain stays where you pressed, however the\n"
                                "cursor moves while you hold.");
        }
        if (s.brush.tool == 6)
            ImGui::SliderFloat("Feature size", &s.brush.noiseFreq, 0.08f, 1.0f, "%.2f");
        if (s.brush.tool == 7) {
            ImGui::SliderFloat("Valley depth", &s.brush.carveDepth, 2.0f, 40.0f, "%.1f m");
            ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f),
                               "Drag to carve a valley | Alt raises a ridge");
        }
        if (s.brush.tool == 5) {
            const char* shapes[] = {"Dome", "Cone", "Plateau", "Crater",
                                    "Ridge", "Range"};
            ImGui::Combo("Shape", &s.brush.stampShape, shapes, IM_ARRAYSIZE(shapes));
            ImGui::SliderFloat("Stamp height", &s.brush.stampHeight, -30.0f, 40.0f, "%.1f m");
            // Ridge (4) and mountain range (5) are directional -> offer rotation.
            if (s.brush.stampShape == 4 || s.brush.stampShape == 5) {
                float deg = glm::degrees(s.brush.stampRot);
                if (ImGui::SliderFloat("Rotation", &deg, -180.0f, 180.0f, "%.0f deg"))
                    s.brush.stampRot = glm::radians(deg);
            }
            ImGui::TextColored(ImVec4(0.8f, 0.6f, 1.0f, 1.0f),
                               "Click to stamp | Alt inverts (digs in)");
        }

        if (s.brush.tool == 8) {
            ImGui::SliderFloat("Proportion", &s.brush.pullFalloff, 0.5f, 6.0f, "%.2f");
            ImGui::TextDisabled(
                s.brush.pullFalloff < 0.85f ? "Broad shoulder: the whole disc comes up"
              : s.brush.pullFalloff > 1.6f  ? "Sharp peak: only the middle follows"
                                      : "Smooth hill");
            // Steppers, not a drag. This is the whole point of the field: a
            // click on the ground pulls it out by exactly this much, so the tool
            // can be used without ever holding a button down and moving -- and a
            // negative number pushes in, which is why there is no modifier for
            // that either.
            ImGui::InputFloat("Height", &s.brush.pullHeight, 0.5f, 2.0f, "%.2f m");
            s.brush.pullHeight = glm::clamp(s.brush.pullHeight, -200.0f, 200.0f);
            ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f),
                "Click the ground to pull it out by that much");
            ImGui::TextDisabled("Or hold and move up/down to dial it in "
                                "(Shift = fine). Either way the ground follows "
                                "the number -- it never piles up.");
        }

        // Grid spacing only makes sense to change before any edits exist (the map
        // keys are cell indices at the current spacing).
        float grid = s.work.cell;
        ImGui::BeginDisabled(!s.work.deltas.empty());
        if (ImGui::SliderFloat("Grid", &grid, 0.5f, 4.0f, "%.2f m"))
            s.work.cell = grid;
        ImGui::EndDisabled();

        ImGui::Text("Edited cells: %d", static_cast<int>(s.work.deltas.size()));
        ImGui::BeginDisabled(s.work.deltas.empty());
        if (ImGui::Button("Clear sculpt")) {
            s.work.deltas.clear();
            s.publish();
            s.streamer.rebuild(); // drop all edits -> regenerate the terrain
            s.grassDirty = true;
        }
        ImGui::EndDisabled();
    }
    ImGui::End();
}

void brushViewport(Brush& brush, const ViewportFrame& view, fitzel::TerrainEditField& work,
                   fitzel::TerrainStreamer& streamer, const std::function<void()>& publish,
                   bool& grassDirty, float dt) {
    const glm::mat4& vp = view.viewProj;
    const ImVec2 org = view.origin;
    glm::vec3 center;
    const bool onGround = view.hovered &&
                          view.pickTerrain(view.mouseNdc, vp, center);

    // Grab the flatten target from the surface on press.
    if (onGround && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        brush.flattenHeight = center.y;

    // --- Pull: one gesture, absolute height ------------------
    // Anchored on the press and driven by the mouse from there on
    // -- deliberately NOT by where the cursor lands on the ground,
    // because the ground it would be asking is the ground this
    // gesture is busy moving, and a tool that reads its own output
    // runs away from you.
    if (brush.tool == 8) {
        if (onGround && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            brush.pullActive  = true;
            brush.pullCenter  = glm::vec2(center.x, center.z);
            brush.pullRadius  = brush.radius;
            brush.pullShape   = brush.pullFalloff;
            brush.pullApplied = 0.0f;
            brush.pullStartY  = ImGui::GetIO().MousePos.y;
            // How many metres of world one pixel of drag is worth,
            // measured AT THE ANCHOR: project a metre of height
            // there and see how tall it comes out on screen. So
            // the peak keeps up with the cursor whether the anchor
            // is at your feet or across the valley, which is the
            // difference between a tool that feels like pulling
            // and one that feels like a slider in disguise.
            const glm::vec4 a = vp * glm::vec4(center, 1.0f);
            const glm::vec4 b = vp * glm::vec4(center + glm::vec3(0.0f, 1.0f, 0.0f), 1.0f);
            const float pxPerM = (a.w > 1e-4f && b.w > 1e-4f)
                ? std::fabs((b.y / b.w - a.y / a.w)) * 0.5f * view.h
                : 0.0f;
            // Clamped, because the measurement degenerates: from
            // straight overhead a metre of height is worth almost
            // no pixels at all, and an unclamped scale there turns
            // a twitch into a mountain.
            brush.pullScale = glm::clamp(
                (pxPerM > 0.5f) ? 1.0f / pxPerM : 0.5f, 0.01f, 0.5f);
        }
        if (brush.pullActive && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // A drag ends by becoming the new click height, so
            // the next hill matches the one just made without
            // anybody having to read a number off the screen.
            if (std::fabs(brush.pullApplied) > 1e-3f) brush.pullHeight = brush.pullApplied;
            brush.pullActive = false;
        }
        if (brush.pullActive) {
            const ImGuiIO& io = ImGui::GetIO();
            // The click is worth `brush.pullHeight` on its own; moving
            // up or down from there adjusts it. Up the screen is
            // up the world, and Shift is the fine gear -- the same
            // gesture over four times the travel.
            float travel = (brush.pullStartY - io.MousePos.y) * brush.pullScale;
            if (io.KeyShift) travel *= 0.25f;
            const float want = brush.pullHeight + travel;
            const float step = want - brush.pullApplied;
            if (std::fabs(step) > 1e-4f) {
                work.pull(brush.pullCenter, brush.pullRadius, step, brush.pullShape);
                brush.pullApplied = want;
                publish();
                const float m = brush.pullRadius + 3.0f * work.cell;
                streamer.editsChanged(
                    glm::vec2(brush.pullCenter.x - m, brush.pullCenter.y - m),
                    glm::vec2(brush.pullCenter.x + m, brush.pullCenter.y + m));
                grassDirty = true;
            }
        }
    }

    // --- Rain: showers at the anchor ----------------------------
    if (brush.tool == 9) {
        if (onGround && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            brush.rainActive = true;
            brush.rainCenter = glm::vec2(center.x, center.z);
            brush.rainRadius = brush.radius;
            brush.rainClock  = 0.0f;           // the click itself is a shower
        }
        if (brush.rainActive && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            brush.rainActive = false;
        if (brush.rainActive) {
            brush.rainClock -= dt;
            if (brush.rainClock <= 0.0f) {
                brush.rainClock = std::max(brush.rainClock + 0.1f, 0.0f);
                // About 0.06 drops per square metre per shower at full
                // strength: ten showers a second cut clear gullies in
                // a couple of seconds of holding, while one click only
                // roughens the slope a little.
                const float area = 3.14159265f * brush.rainRadius * brush.rainRadius;
                const int drops = std::clamp(
                    static_cast<int>(area * brush.strength * 0.06f), 4, 40000);
                const float reach = work.rain(streamer.settings(),
                    brush.rainCenter, brush.rainRadius, drops, brush.rainSeed++);
                publish();
                const float m = reach + 3.0f * work.cell;
                streamer.editsChanged(
                    glm::vec2(brush.rainCenter.x - m, brush.rainCenter.y - m),
                    glm::vec2(brush.rainCenter.x + m, brush.rainCenter.y + m));
                grassDirty = true;
            }
        }
    }

    // Stamp drops a landform once per click; the other tools apply
    // continuously while the button is held.
    const bool stampTool = (brush.tool == 5);
    const bool apply = onGround && brush.tool != 8 && brush.tool != 9 &&
        (stampTool ? ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                   : ImGui::IsMouseDown(ImGuiMouseButton_Left));
    if (apply) {
        const glm::vec2 c(center.x, center.z);
        const bool invert = ImGui::GetIO().KeyAlt;
        switch (brush.tool) {
            case 0: case 1: {                 // raise / lower
                float dir = (brush.tool == 1) ? -1.0f : 1.0f;
                if (invert) dir = -dir;
                work.raise(c, brush.radius,
                                 dir * brush.strength * 14.0f * dt);
                break;
            }
            case 2:                           // smooth
                work.smooth(streamer.settings(), c, brush.radius,
                    glm::clamp(brush.strength * 5.0f * dt, 0.0f, 1.0f));
                break;
            case 3:                           // flatten to grabbed height
                work.flatten(streamer.settings(), c, brush.radius,
                    glm::clamp(brush.strength * 5.0f * dt, 0.0f, 1.0f),
                    brush.flattenHeight);
                break;
            case 4:                           // erode (weathering)
                work.erode(streamer.settings(), c, brush.radius,
                    glm::clamp(brush.strength * 6.0f * dt, 0.0f, 1.0f));
                break;
            case 5:                           // stamp a landform
                work.stamp(c, brush.radius,
                    invert ? -brush.stampHeight : brush.stampHeight,
                    brush.stampShape, brush.stampRot);
                break;
            case 6:                           // noise / roughen
                work.roughen(c, brush.radius,
                    brush.strength * 3.0f * dt, brush.noiseFreq, brush.noiseSeed);
                brush.noiseSeed += 1.7f;            // decorrelate next dab
                break;
            case 7:                           // carve valley (Alt: ridge)
                work.carve(streamer.settings(), c, brush.radius,
                    glm::clamp(brush.strength * 4.0f * dt, 0.0f, 1.0f),
                    invert ? -brush.carveDepth : brush.carveDepth);
                break;
        }
        // Publish the new shape, then rebuild the touched chunks.
        // Erosion/stamp reach a little past the disc, so pad the
        // rebuilt rectangle beyond the radius.
        publish();
        const float m = brush.radius + 3.0f * work.cell;
        streamer.editsChanged(glm::vec2(c.x - m, c.y - m),
                              glm::vec2(c.x + m, c.y + m));
        grassDirty = true; // vegetation re-drapes on the new ground
    }

    // Brush cursor: a ground-hugging ring, coloured per tool.
    // Once a pull is under way the ring stays on its ANCHOR --
    // that is where the edit is, and a ring that followed the
    // cursor would be pointing at ground the tool is not touching.
    const bool ringHere = onGround || brush.pullActive || brush.rainActive;
    const glm::vec2 ringAt = brush.pullActive ? brush.pullCenter
                           : brush.rainActive ? brush.rainCenter
                                        : glm::vec2(center.x, center.z);
    const float ringR = brush.pullActive ? brush.pullRadius
                      : brush.rainActive ? brush.rainRadius : brush.radius;
    if (ringHere) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 col = brush.tool == 9 ? IM_COL32(70, 140, 235, 235)
                        : brush.tool == 8 ? IM_COL32(150, 255, 210, 235)
                        : brush.tool == 2 ? IM_COL32(120, 200, 255, 225)
                        : brush.tool == 3 ? IM_COL32(255, 210, 90, 225)
                        : brush.tool == 4 ? IM_COL32(200, 150, 110, 225)
                        : brush.tool == 5 ? IM_COL32(200, 140, 255, 225)
                        : brush.tool == 6 ? IM_COL32(180, 180, 190, 225)
                        : brush.tool == 7 ? IM_COL32(90, 170, 255, 225)
                        : brush.tool == 1 ? IM_COL32(255, 130, 90, 225)
                                          : IM_COL32(140, 235, 140, 225);
        const int SEG = 56;
        ImVec2 prev; bool have = false;
        auto toScreen = [&](float wx, float wy, float wz, ImVec2& out) {
            const glm::vec4 cc = vp * glm::vec4(wx, wy, wz, 1.0f);
            if (cc.w <= 1e-4f) return false;
            const glm::vec3 n = glm::vec3(cc) / cc.w;
            out = ImVec2(org.x + (n.x * 0.5f + 0.5f) * view.w,
                         org.y + (1.0f - (n.y * 0.5f + 0.5f)) * view.h);
            return true;
        };
        for (int i = 0; i <= SEG; ++i) {
            const float a  = static_cast<float>(i) / SEG * 6.2831853f;
            const float wx = ringAt.x + std::cos(a) * ringR;
            const float wz = ringAt.y + std::sin(a) * ringR;
            ImVec2 sp;
            if (!toScreen(wx, streamer.heightAt(wx, wz) + 0.05f, wz, sp)) {
                have = false; continue;
            }
            if (have) dl->AddLine(prev, sp, col, 2.0f);
            prev = sp; have = true;
        }
        // A pull also draws its own stem and says how far it has
        // come. Reading the height off the silhouette of a hill
        // you are in the middle of making is guesswork, and this
        // tool is here so that a height can be aimed at.
        if (brush.pullActive) {
            const float ground = streamer.heightAt(ringAt.x, ringAt.y);
            ImVec2 foot, tip;
            if (toScreen(ringAt.x, ground - brush.pullApplied, ringAt.y, foot) &&
                toScreen(ringAt.x, ground, ringAt.y, tip)) {
                dl->AddLine(foot, tip, col, 2.0f);
                dl->AddCircleFilled(tip, 4.0f, col);
                char lbl[32];
                std::snprintf(lbl, sizeof lbl, "%+.2f m", brush.pullApplied);
                dl->AddText(ImVec2(tip.x + 8.0f, tip.y - 8.0f), col, lbl);
            }
        }
    }
}

} // namespace sculptui
