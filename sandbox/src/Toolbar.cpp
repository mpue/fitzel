#include "Toolbar.hpp"

#include <cmath>
#include <cstdio>

#include <imgui_internal.h>   // BeginViewportSideBar

#include "ToolbarIcons.hpp"
#include "ViewShade.hpp"

namespace toolbar {

void draw(const State& s) {
    ImGuiViewport* tvp = ImGui::GetMainViewport();
    // Sized from the font, so the strip scales with the display and with the
    // user's text size like every other control -- a fixed 26 px shrank to a
    // row of specks at 150 %, the opposite of the large targets this editor
    // promises.
    const float  bh   = std::round(ImGui::GetFontSize() * 1.3f);
    const ImVec2 pad(ImGui::GetStyle().WindowPadding.x, ImGui::GetStyle().WindowPadding.y * 0.5f);
    const float  barH = bh + pad.y * 2.0f + 2.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, pad);
    // At rest the buttons are just their pictures on the strip; the button body
    // shows up on hover, and as an accent wash on the ones that are on
    // (icon::button's `active`).
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    const bool barOpen = ImGui::BeginViewportSideBar(
        "##PrimToolbar", tvp, ImGuiDir_Up, barH,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDecoration);
    if (barOpen) {
        ImDrawList*  dl = ImGui::GetWindowDrawList();
        const ImVec2 bs(bh, bh);
        const float  r  = bh * 0.31f;
        ImVec2       c;
        // Between groups: a hairline divider in a gap, so the groups read as
        // groups without a label each.
        auto gap = [&] {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float  w = bh * 0.5f;
            ImGui::Dummy(ImVec2(w, bh));
            dl->AddLine({std::round(p.x + w * 0.5f), p.y + bh * 0.2f},
                        {std::round(p.x + w * 0.5f), p.y + bh * 0.8f},
                        ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
            ImGui::SameLine();
        };

        // --- Select / Create -----------------------------------------------
        // The pair that decides what a left-click on empty ground does. Select
        // is the default and the harmless one: it can only pick and deselect.
        // Create is the one that drops objects, and you have to ask for it --
        // otherwise every stray click while looking around litters the scene
        // with boxes. Esc steps back out of Create.
        auto modeToggle = [&](bool create, const char* tip) {
            const bool on  = (s.placeMode == create);
            const bool hit = icon::button(create ? "modeCreate" : "modeSelect", bs, tip, false, c, on);
            icon::pointer(dl, create, c, r, on ? icon::on() : icon::kOff);
            if (hit) s.placeMode = create;
        };
        modeToggle(false, "Select -- a click picks objects and never creates one (Esc)");
        modeToggle(true,  "Create -- a click on empty ground drops the chosen shape");
        gap();

        auto shapeBtn = [&](EntityType t, const char* id, const char* tip) {
            const bool on  = (s.newType == t);
            const bool hit = icon::button(id, bs, tip, false, c, on);
            icon::shape(dl, t, c, r, on ? icon::on() : icon::kOff);
            if (hit) {
                s.newType = t;
                // In Select mode the button itself is the create action, so it
                // drops one at the spawn point (the 3D cursor, or in view). In
                // Create mode it only arms the shape -- there the click in the
                // viewport is what places it, and getting two objects out of
                // one click surprises.
                if (!s.placeMode && s.addShape) s.addShape(t);
            }
        };
        shapeBtn(EntityType::Box,      "shapeBox",      "Box");
        shapeBtn(EntityType::Ramp,     "shapeRamp",     "Ramp");
        shapeBtn(EntityType::Cylinder, "shapeCylinder", "Cylinder");
        shapeBtn(EntityType::Sphere,   "shapeSphere",   "Sphere");
        shapeBtn(EntityType::Plane,    "shapePlane",
                 "Plane -- a flat quad, for floors, walls and backdrops");
        shapeBtn(EntityType::Light,    "shapeLight",    "Light");
        shapeBtn(EntityType::Empty,    "shapeEmpty",    "Empty (transform-only grouping node)");

        // Terrain: an object like any other, so it is added like any other. Not
        // a shapeBtn -- it is a component on an Empty, not an entity type -- and
        // disabled once the scene has ground, since a scene has one terrain.
        if (icon::button("terrainAdd", bs,
                         s.terrainOn ? "Terrain (the scene already has one)"
                                     : "Terrain (adds ground to the scene)",
                         s.terrainOn, c) &&
            s.addTerrain)
            s.addTerrain();
        icon::terrain(dl, c, r, s.terrainOn ? icon::kDim : icon::kOff);

        // Gap, then the transform-gizmo modes (Q/W/E).
        gap();
        auto modeBtn = [&](ImGuizmo::OPERATION op, const char* id, const char* tip) {
            const bool on  = (s.gizmoOp == op);
            const bool hit = icon::button(id, bs, tip, false, c, on);
            icon::gizmo(dl, op, c, r, on ? icon::on() : icon::kOff);
            if (hit) s.gizmoOp = op;
        };
        modeBtn(ImGuizmo::TRANSLATE, "gizmoMove",   "Move (Q) -- hold Ctrl to snap to the grid");
        modeBtn(ImGuizmo::ROTATE,    "gizmoRotate", "Rotate (W) -- hold Ctrl to turn in steps");
        modeBtn(ImGuizmo::SCALE,     "gizmoScale",  "Scale (E) -- hold Ctrl to scale in steps");

        // Gap, then the gizmo reference-frame toggle (local vs world).
        gap();
        {
            const bool isLocal = (s.gizmoMode == ImGuizmo::LOCAL);
            char tip[64];
            std::snprintf(tip, sizeof tip, "Gizmo space: %s  (X to toggle)",
                          isLocal ? "Local" : "World");
            // A two-way switch whose picture IS the state, so no accent: there
            // is no "off" for it to stand out from.
            const bool hit = icon::button("gizmoSpace", bs, tip, false, c);
            icon::gizmoSpace(dl, isLocal, c, r, icon::kOff);
            if (hit) s.gizmoMode = isLocal ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
        }

        // Gap, then how the viewport DRAWS the scene. Not a tool: nothing here
        // changes the scene, only what is shown of it, which is why the group
        // sits apart from the ones that edit.
        gap();
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
                 "viewport. Starts when the camera comes to rest\n"
                 "and refines until it is done.\n"
                 "Click again to pick up an edit."},
            };
            for (const ShadeBtn& b : kShades) {
                const bool on  = !s.playMode && s.viewShade == b.mode;
                const bool hit = icon::button(b.id, bs, b.tip, s.playMode, c, on);
                icon::shade(dl, b.mode, c, r,
                            s.playMode ? icon::kDim : on ? icon::on() : icon::kOff);
                if (!hit) continue;
                // Pressing the mode you are already in means "look again": the
                // trace follows the camera by itself, and this is the one thing
                // it cannot see coming.
                if (b.mode == kShadePathTraced && s.viewShade == b.mode && s.refreshTrace)
                    s.refreshTrace();
                s.viewShade = b.mode;
            }
        }

        // Gap, then the road editor: a toggle, not a one-shot action like the
        // buttons before it, so it stays lit while it owns the left mouse
        // button in the viewport.
        gap();
        {
            const bool roadOn = s.viewTool == ViewTool::Road;
            char tip[160];
            std::snprintf(tip, sizeof tip,
                          "Road editor%s\n"
                          "Click ground = add point, drag = move,\n"
                          "Ctrl+drag = raise/lower, Del = delete.",
                          roadOn ? " (on)" : "");
            const bool hit = icon::button("roadTool", bs, tip, false, c, roadOn);
            icon::road(dl, c, r, roadOn ? icon::on() : icon::kOff);
            if (hit) {
                // Takes the left button from whichever tool had it.
                takeTool(s.viewTool, ViewTool::Road, !roadOn);
                if (!roadOn) s.showRoads = true; // the tunables belong with the tool
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

} // namespace toolbar
