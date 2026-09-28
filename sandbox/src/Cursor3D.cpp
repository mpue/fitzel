#include "Cursor3D.hpp"

#include <cmath>
#include <utility>
#include <vector>

#include <imgui.h>

#include "EditorContext.hpp"
#include "SceneGraph.hpp"
#include "UiStyle.hpp"
#include "ViewportOverlay.hpp"

namespace cursor3d {

glm::vec3 snapToGrid(const glm::vec3& p, float step) {
    if (step <= 0.0f) return p;
    return glm::vec3(std::round(p.x / step) * step, std::round(p.y / step) * step,
                     std::round(p.z / step) * step);
}

namespace {

// Move the selected entity to a world position (via the local source of truth,
// so it respects any parent -- same path the gizmo/inspector use), as one undo
// step. Its children follow through the hierarchy, so the entity alone is what
// changed.
void moveSelectionTo(EditorContext& ed, const glm::vec3& wPos, const char* label) {
    if (!ed.sel.valid()) return;
    Entity& b = ed.entities[ed.sel.index()];
    const std::vector<int> ids{b.id};
    std::vector<Entity>    before = ed.snapshot(ids);
    const glm::mat4 pw = scenegraph::parentWorld(ed.entities, b);
    scenegraph::setWorld(b, wPos, b.rotation, b.parent >= 0 ? &pw : nullptr);
    ed.commitEdit(std::move(before), ids, label);
}

} // namespace

void snap(Snap op, EditorContext& ed, Cursor& c,
          const std::function<float(float, float)>& groundAt) {
    const bool haveSel = ed.sel.valid();
    switch (op) {
        case Snap::CursorToOrigin: c.pos = glm::vec3(0.0f); break;
        case Snap::CursorToGrid:   c.pos = snapToGrid(c.pos, c.grid); break;
        case Snap::CursorToTerrain:
            if (groundAt) c.pos.y = groundAt(c.pos.x, c.pos.z);
            break;
        case Snap::CursorToSelection:
            if (haveSel) c.pos = ed.entities[ed.sel.index()].center;
            break;
        case Snap::SelectionToCursor: moveSelectionTo(ed, c.pos, "Selection to cursor"); break;
        case Snap::SelectionToGrid:
            if (haveSel)
                moveSelectionTo(ed, snapToGrid(ed.entities[ed.sel.index()].center, c.grid),
                                "Selection to grid");
            break;
    }
}

void viewport(const ViewportFrame& view, Cursor& c, bool editing) {
    // Shift+Right-click drops the cursor onto the terrain (the look control
    // ignores right-drag while Shift is held).
    if (editing && view.hovered && ImGui::GetIO().KeyShift &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        glm::vec3 h;
        if (view.pickTerrain && view.pickTerrain(view.mouseNdc, view.viewProj, h)) c.pos = h;
    }
    // Draw it: a red/white split ring with crosshair ticks, always on top (2D
    // overlay), so it reads like Blender's cursor.
    if (c.visible && editing) overlay::cursorMark(view, c.pos);
}

void snapMenu(EditorContext& ed, const ViewportFrame& view, Cursor& c, bool editing) {
    const ImGuiIO& io = ImGui::GetIO();
    if (editing && view.hovered && !io.WantTextInput && io.KeyShift && !io.KeyCtrl &&
        ImGui::IsKeyPressed(ImGuiKey_S))
        ImGui::OpenPopup("##snapMenu");
    // Moderate outer padding; the menu labels get an explicit left (and
    // matching right) inset via Indent, since MenuItem renders its label flush
    // to the window's inner edge otherwise.
    const ImVec2 basePad = ImGui::GetStyle().WindowPadding;
    const float  inset   = basePad.x * 0.9f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(basePad.x, basePad.y * 1.7f));
    if (ImGui::BeginPopup("##snapMenu")) {
        const bool haveSel = ed.sel.valid();
        auto run = [&](Snap op) { snap(op, ed, c, view.groundAt); };
        ImGui::Indent(inset);
        ImGui::TextDisabled("Snap");
        ImGui::Unindent(inset);
        ImGui::Separator();
        ImGui::Indent(inset);
        // Trailing spaces reserve right-edge room so the label isn't flush
        // against the popup's right border either.
        if (ImGui::MenuItem("Cursor to World Origin      ")) run(Snap::CursorToOrigin);
        if (ImGui::MenuItem("Cursor to Grid      "))         run(Snap::CursorToGrid);
        if (ImGui::MenuItem("Cursor to Terrain      "))      run(Snap::CursorToTerrain);
        if (ImGui::MenuItem("Cursor to Selection      ", nullptr, false, haveSel))
            run(Snap::CursorToSelection);
        ImGui::Unindent(inset);
        ImGui::Separator();
        ImGui::Indent(inset);
        if (ImGui::MenuItem("Selection to Cursor      ", nullptr, false, haveSel))
            run(Snap::SelectionToCursor);
        if (ImGui::MenuItem("Selection to Grid      ", nullptr, false, haveSel))
            run(Snap::SelectionToGrid);
        ImGui::Unindent(inset);
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(); // WindowPadding
}

void panel(EditorContext& ed, Cursor& c, const Panel& p) {
    if (!p.show) return;
    if (ImGui::Begin("3D Cursor", &p.show)) {
        auto run = [&](Snap op) { snap(op, ed, c, p.groundAt); };
        ImGui::Checkbox("Show cursor", &c.visible);
        ui::hint(c.visible
                     ? "New objects are placed on the cursor."
                     : "Hidden: new objects are placed in view.");
        ImGui::TextDisabled("Shift+Right-click in the viewport to place it.");
        ImGui::DragFloat3("Position", &c.pos.x, 0.05f, 0.0f, 0.0f, "%.2f");
        ImGui::SetNextItemWidth(140.0f);
        ImGui::DragFloat("Grid step", &c.grid, 0.05f, 0.01f, 100.0f, "%.2f m");
        ImGui::SetNextItemWidth(140.0f);
        ImGui::DragFloat("Rotate step", &c.snapAngle, 0.5f, 1.0f, 90.0f, "%.0f deg");
        ImGui::SetNextItemWidth(140.0f);
        ImGui::DragFloat("Scale step", &c.snapScale, 0.01f, 0.01f, 1.0f, "%.2f x");
        ui::hint("Hold Ctrl while dragging the gizmo: a move lands on the\n"
                 "grid, a turn and a scale go in these steps.");
        ImGui::TextDisabled("Shift+S in the viewport opens the snap menu.");

        // The drawn grid IS this step, on this cursor's plane -- so these
        // controls belong next to it rather than in a panel of their own.
        ui::sectionText("Grid");
        ImGui::Checkbox("Show grid", &p.showGrid);
        ImGui::BeginDisabled(!p.showGrid);
        ImGui::SetNextItemWidth(140.0f);
        ImGui::DragFloat("Fade out", &p.gridFade, 2.0f, 20.0f, 1000.0f, "%.0f m");
        ImGui::EndDisabled();
        ui::hint("One cell = the grid step above, a heavier line every ten.\n"
                 "It lies on the cursor's height, so moving the cursor up\n"
                 "moves the plane you are building on with it. The fade is\n"
                 "capped by the view distance -- it cannot reach past it.");

        const bool haveSel = ed.sel.valid();

        ui::sectionText("Snap cursor");
        if (ImGui::Button("To world origin")) run(Snap::CursorToOrigin);
        ImGui::SameLine();
        if (ImGui::Button("To grid"))         run(Snap::CursorToGrid);
        if (ImGui::Button("To terrain"))      run(Snap::CursorToTerrain);
        ImGui::SameLine();
        ImGui::BeginDisabled(!haveSel);
        if (ImGui::Button("To selection"))    run(Snap::CursorToSelection);
        ImGui::EndDisabled();

        ui::sectionText("Snap selection");
        ImGui::BeginDisabled(!haveSel);
        if (ImGui::Button("Selection to cursor")) run(Snap::SelectionToCursor);
        ImGui::SameLine();
        if (ImGui::Button("Selection to grid"))   run(Snap::SelectionToGrid);
        ImGui::EndDisabled();

        ui::sectionText("Create");
        if (ImGui::Button("Add object at cursor") && p.addAt) p.addAt(c.pos);
        ImGui::SameLine();
        ImGui::TextDisabled("(base rests on the cursor)");
    }
    ImGui::End();
}

} // namespace cursor3d
