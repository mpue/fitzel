#include "ViewBar.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <imgui.h>

#include "ToolbarIcons.hpp"
#include "ViewShade.hpp"
#include "ViewportNav.hpp"

namespace viewbar {

namespace {

float buttonSize() { return std::round(ImGui::GetFontSize() * 1.15f); }
float padY()       { return std::max(2.0f, std::round(ImGui::GetFontSize() * 0.15f)); }

} // namespace

float height() { return buttonSize() + padY() * 2.0f; }

void draw(const char* id, const State& s) {
    ImGui::PushID(id);
    const float  bh = buttonSize();
    const float  py = padY();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float  w  = ImGui::GetContentRegionAvail().x;
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + height()),
                      ImGui::GetColorU32(ImGuiCol_MenuBarBg));

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(std::round(bh * 0.15f), ImGui::GetStyle().ItemSpacing.y));
    ImGui::SetCursorScreenPos(ImVec2(p0.x + py * 2.0f, p0.y + py));
    const ImVec2 bs(bh, bh);
    const float  r = bh * 0.31f;
    ImVec2       c;

    // How the view DRAWS the scene. Not a tool: nothing here changes the
    // scene, only what this view shows of it.
    {
        struct ShadeBtn { int mode; const char* id; const char* tip; };
        static const ShadeBtn kShades[] = {
            {kShadeWireframe, "shadeWire",
             "Wireframe -- edges only, and you can see through it"},
            {kShadeSolid, "shadeSolid",
             "Solid -- one clay surface under a fixed studio light.\n"
             "Shape stays readable whatever the scene's lighting does."},
            {kShadeSolidLit, "shadeSolidLit",
             "Solid lit -- the scene's own light and shadows,\n"
             "without the textures arguing with them."},
            {kShadeTextured, "shadeTextured",
             "Textured -- the game: materials, sky, water, plants."},
            {kShadePathTraced, "shadePath",
             "Pathtraced -- the offline renderer, live in the\n"
             "view. Starts when the camera comes to rest\n"
             "and refines until it is done.\n"
             "Click again to pick up an edit."},
        };
        for (const ShadeBtn& b : kShades) {
            const bool on  = !s.playMode && s.shade == b.mode;
            const bool hit = icon::button(b.id, bs, b.tip, s.playMode, c, on);
            icon::shade(dl, b.mode, c, r,
                        s.playMode ? icon::kDim : on ? icon::on() : icon::kOff);
            if (!hit) continue;
            // Pressing the mode you are already in means "look again": the
            // trace follows the camera by itself, and this is the one thing
            // it cannot see coming.
            if (b.mode == kShadePathTraced && s.shade == b.mode && s.refreshTrace)
                s.refreshTrace();
            // The post look belongs to the modes that show the game; the
            // plain ones show shape, and a bloom on clay helps nobody.
            if (s.shade != b.mode)
                s.postFx = (b.mode == kShadeTextured || b.mode == kShadePathTraced);
            s.shade = b.mode;
        }

        // Post FX, at the end of the group: it is part of how the scene is
        // DRAWN. In Play it shows as on and cannot be touched -- the game
        // always has its whole look.
        const bool fxOn = s.playMode || s.postFx;
        const bool hit = icon::button(
            "postFx", bs,
            fxOn ? "Post FX: on -- bloom, AO, colour grade, depth of field ...\n"
                   "Right-click: Post processing panel"
                 : "Post FX: off -- the bare frame, exposure and tonemap only\n"
                   "Right-click: Post processing panel",
            s.playMode, c, fxOn && !s.playMode);
        const bool rightHit = ImGui::IsItemClicked(ImGuiMouseButton_Right);
        icon::postFx(dl, c, r, s.playMode ? icon::kDim : fxOn ? icon::on() : icon::kOff);
        if (hit) s.postFx = !s.postFx;
        if (rightHit) s.showPost = true;
    }

    // A hairline between the groups, as in the main toolbar.
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float  gw = bh * 0.5f;
        ImGui::Dummy(ImVec2(gw, bh));
        dl->AddLine({std::round(p.x + gw * 0.5f), p.y + bh * 0.2f},
                    {std::round(p.x + gw * 0.5f), p.y + bh * 0.8f},
                    ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
        ImGui::SameLine();
    }

    // Where it looks from: the standard viewpoints and the lens, by name. The
    // name is the button -- "Front · Ortho" says what the view is and where
    // to change it in one place.
    {
        const char* v = viewnav::label(s.nav.current());
        char name[64];
        std::snprintf(name, sizeof name, "%s%s%s", v ? v : (s.ortho ? "User" : "Perspective"),
                      s.ortho ? " \xC2\xB7 Ortho" : "", "##viewpoint");
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(ImGui::GetStyle().FramePadding.x,
                                   std::max(0.0f, (bh - ImGui::GetFontSize()) * 0.5f)));
        if (ImGui::Button(name)) ImGui::OpenPopup("viewpoints");
        ImGui::PopStyleVar();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Viewpoint and lens of this view\n"
                              "(Num 1 / 3 / 7, Ctrl for the opposite side, Num 5)");
        if (ImGui::BeginPopup("viewpoints")) {
            s.nav.drawItems();
            ImGui::EndPopup();
        }
    }

    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    // Leave the cursor under the strip, at its left edge.
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + height()));
    ImGui::PopID();
}

} // namespace viewbar
