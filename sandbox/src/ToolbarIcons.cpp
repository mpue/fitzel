#include "ToolbarIcons.hpp"

#include <cmath>

namespace icon {

// --- The toolbar strip's icons ---------------------------------------------
// Every button in the strip paints its own picture into the window's draw list.
// No icon font: nothing extra to ship, nothing to fall back to when a glyph is
// missing, and a 26 px symbol built from a handful of lines stays sharp where a
// scaled bitmap would not. It is all pure painting -- draw list, centre, radius,
// colour -- which is why it lives out here, and why the strip itself is left
// holding only what a click does.

ImU32 on() { return ImGui::GetColorU32(ImGuiCol_CheckMark); }

// The primitive shapes, drawn as themselves.
void shape(ImDrawList* dl, EntityType t, ImVec2 c, float r, ImU32 col) {
    switch (t) {
        case EntityType::Box:
            dl->AddRect({c.x - r, c.y - r}, {c.x + r, c.y + r}, col, 0.0f, 0, 2.0f);
            break;
        case EntityType::Ramp:
            dl->AddTriangle({c.x - r, c.y + r}, {c.x + r, c.y + r},
                            {c.x + r, c.y - r}, col, 2.0f);
            break;
        case EntityType::Cylinder:
            dl->AddRect({c.x - r * 0.7f, c.y - r}, {c.x + r * 0.7f, c.y + r},
                        col, 4.0f, 0, 2.0f);
            dl->AddLine({c.x - r * 0.7f, c.y - r}, {c.x + r * 0.7f, c.y - r}, col, 2.0f);
            break;
        case EntityType::Sphere:
            dl->AddCircle(c, r, col, 0, 2.0f);
            break;
        case EntityType::Plane: {
            // A quad seen at a shallow angle: the flat thing it is, told apart
            // from the Box beside it by being flat rather than by a label.
            const ImVec2 p[4] = {{c.x - r, c.y + r * 0.45f}, {c.x - r * 0.45f, c.y - r * 0.45f},
                                 {c.x + r, c.y - r * 0.45f}, {c.x + r * 0.45f, c.y + r * 0.45f}};
            dl->AddPolyline(p, 4, col, ImDrawFlags_Closed, 2.0f);
            break;
        }
        case EntityType::Light:
            dl->AddCircleFilled(c, r * 0.45f, col);
            for (int a = 0; a < 8; ++a) {
                const float  ang = a * 0.7853982f;
                const ImVec2 d(std::cos(ang), std::sin(ang));
                dl->AddLine({c.x + d.x * r * 0.7f, c.y + d.y * r * 0.7f},
                            {c.x + d.x * r, c.y + d.y * r}, col, 1.5f);
            }
            break;
        case EntityType::Empty:  // small dashed cross = transform node
            dl->AddLine({c.x - r, c.y}, {c.x + r, c.y}, col, 1.5f);
            dl->AddLine({c.x, c.y - r}, {c.x, c.y + r}, col, 1.5f);
            dl->AddCircle(c, r * 0.4f, col, 0, 1.5f);
            break;
        default: break;
    }
}

// A mouse arrow for Select; the same arrow with a plus next to it for Create.
void pointer(ImDrawList* dl, bool create, ImVec2 c, float r, ImU32 col) {
    const ImVec2 a(c.x - r * (create ? 0.9f : 0.45f), c.y - r);
    dl->AddTriangleFilled(a, {a.x, a.y + r * 1.7f},
                          {a.x + r * 1.15f, a.y + r * 1.15f}, col);
    if (create) {
        const ImVec2 q(c.x + r * 0.6f, c.y - r * 0.3f);
        dl->AddLine({q.x - r * 0.5f, q.y}, {q.x + r * 0.5f, q.y}, col, 2.0f);
        dl->AddLine({q.x, q.y - r * 0.5f}, {q.x, q.y + r * 0.5f}, col, 2.0f);
    }
}

// A little horizon with a hill on it.
void terrain(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    dl->AddLine({c.x - r, c.y + r * 0.6f}, {c.x + r, c.y + r * 0.6f}, col, 1.5f);
    dl->AddTriangle({c.x - r * 0.8f, c.y + r * 0.6f}, {c.x, c.y - r * 0.7f},
                    {c.x + r * 0.8f, c.y + r * 0.6f}, col, 1.8f);
}

// The three gizmo operations: a 4-way arrow, a circular arrow, a diagonal
// between a filled and an open handle.
void gizmo(ImDrawList* dl, ImGuizmo::OPERATION op, ImVec2 c, float r, ImU32 col) {
    const float a = r * 0.44f; // arrowhead, in step with the icon's size
    if (op == ImGuizmo::TRANSLATE) {
        dl->AddLine({c.x - r, c.y}, {c.x + r, c.y}, col, 1.6f);
        dl->AddLine({c.x, c.y - r}, {c.x, c.y + r}, col, 1.6f);
        dl->AddTriangleFilled({c.x + r, c.y}, {c.x + r - a, c.y - a}, {c.x + r - a, c.y + a}, col);
        dl->AddTriangleFilled({c.x - r, c.y}, {c.x - r + a, c.y - a}, {c.x - r + a, c.y + a}, col);
        dl->AddTriangleFilled({c.x, c.y - r}, {c.x - a, c.y - r + a}, {c.x + a, c.y - r + a}, col);
        dl->AddTriangleFilled({c.x, c.y + r}, {c.x - a, c.y + r - a}, {c.x + a, c.y + r - a}, col);
    } else if (op == ImGuizmo::ROTATE) {
        dl->PathArcTo(c, r, 0.6f, 5.4f, 20);
        dl->PathStroke(col, 0, 1.8f);
        const ImVec2 e(c.x + std::cos(5.4f) * r, c.y + std::sin(5.4f) * r);
        const ImVec2 tg(-std::sin(5.4f), std::cos(5.4f));
        const ImVec2 no(std::cos(5.4f), std::sin(5.4f));
        dl->AddTriangleFilled({e.x + tg.x * a, e.y + tg.y * a},
                              {e.x - no.x * a * 0.7f, e.y - no.y * a * 0.7f},
                              {e.x + no.x * a * 0.7f, e.y + no.y * a * 0.7f}, col);
    } else {
        dl->AddLine({c.x - r * 0.7f, c.y + r * 0.7f}, {c.x + r * 0.7f, c.y - r * 0.7f}, col, 1.8f);
        const float h = r * 0.375f; // handle half-size
        dl->AddRectFilled({c.x + r * 0.7f - h, c.y - r * 0.7f - h},
                          {c.x + r * 0.7f + h, c.y - r * 0.7f + h}, col);
        dl->AddRect({c.x - r * 0.7f - h, c.y + r * 0.7f - h},
                    {c.x - r * 0.7f + h, c.y + r * 0.7f + h}, col, 0.0f, 0, 1.5f);
    }
}

// Object box with its own tilted axis = local frame; globe with meridian and
// equator = world frame.
void gizmoSpace(ImDrawList* dl, bool local, ImVec2 c, float r, ImU32 col) {
    if (local) {
        dl->AddRect({c.x - r * 0.7f, c.y - r * 0.55f},
                    {c.x + r * 0.35f, c.y + r * 0.7f}, col, 0.0f, 0, 1.6f);
        dl->AddLine({c.x + r * 0.35f, c.y - r * 0.55f}, {c.x + r, c.y - r}, col, 1.6f);
    } else {
        dl->AddCircle(c, r, col, 0, 1.6f);
        dl->AddLine({c.x - r, c.y}, {c.x + r, c.y}, col, 1.2f);
        dl->AddLine({c.x, c.y - r}, {c.x, c.y + r}, col, 1.2f);
        dl->AddBezierQuadratic({c.x, c.y - r}, {c.x - r * 0.9f, c.y},
                               {c.x, c.y + r}, col, 1.1f);
        dl->AddBezierQuadratic({c.x, c.y - r}, {c.x + r * 0.9f, c.y},
                               {c.x, c.y + r}, col, 1.1f);
    }
}

// Two edges converging into the distance plus a dashed centre line: a road,
// readable at 26 px without an icon font.
void road(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    dl->AddLine({c.x - r, c.y + r}, {c.x - r * 0.35f, c.y - r}, col, 1.8f);
    dl->AddLine({c.x + r, c.y + r}, {c.x + r * 0.35f, c.y - r}, col, 1.8f);
    dl->AddLine({c.x, c.y + r * 0.9f}, {c.x, c.y + r * 0.2f}, col, 1.4f);
    dl->AddLine({c.x, c.y - r * 0.2f}, {c.x, c.y - r * 0.8f}, col, 1.4f);
}

// The viewport shading ladder: a wire cube, then the same ball with as much of
// the material as each mode keeps -- nothing, the scene's light, the paintwork.
// One shape across three of the four, because what changes between them is not
// the object.
void shade(ImDrawList* dl, int mode, ImVec2 c, float r, ImU32 col) {
    // The toolbar's own background, for the pattern that has to be cut OUT of a
    // filled ball rather than drawn on top of it -- these icons have one colour
    // to draw with, and a checker needs two.
    const ImU32 kInk = ImGui::GetColorU32(ImGuiCol_WindowBg);
    if (mode == 3) { // wireframe: a cube with its far edges left in
        const float a = r * 0.78f, o = r * 0.42f;
        dl->AddRect({c.x - a, c.y - a + o}, {c.x + a - o, c.y + a}, col, 0.0f, 0, 1.5f);
        dl->AddRect({c.x - a + o, c.y - a}, {c.x + a, c.y + a - o}, col, 0.0f, 0, 1.1f);
        dl->AddLine({c.x - a, c.y - a + o}, {c.x - a + o, c.y - a}, col, 1.1f);
        dl->AddLine({c.x + a - o, c.y + a}, {c.x + a, c.y + a - o}, col, 1.1f);
        return;
    }
    if (mode == 4) {                       // pathtraced: a ray bouncing off it
        dl->AddCircleFilled({c.x + r * 0.25f, c.y + r * 0.3f}, r * 0.55f, col);
        dl->AddLine({c.x - r, c.y - r}, {c.x - r * 0.1f, c.y - r * 0.15f}, col, 1.4f);
        dl->AddLine({c.x - r * 0.1f, c.y - r * 0.15f}, {c.x + r * 0.35f, c.y - r}, col, 1.4f);
        dl->AddLine({c.x + r * 0.35f, c.y - r}, {c.x + r, c.y - r * 0.35f}, col, 1.4f);
        return;
    }
    dl->AddCircleFilled(c, r * 0.85f, col);
    if (mode == 2) {                       // solid lit: the scene's sun on it
        for (int i = 0; i < 5; ++i) {
            const float ang = 3.4f + i * 0.30f;
            const ImVec2 d(std::cos(ang), std::sin(ang));
            dl->AddLine({c.x + d.x * r * 1.15f, c.y + d.y * r * 1.15f},
                        {c.x + d.x * r * 1.55f, c.y + d.y * r * 1.55f}, col, 1.3f);
        }
    } else if (mode == 0) {                // textured: a pattern, cut in
        const float q = r * 0.42f;
        dl->AddRectFilled({c.x - q, c.y - q}, {c.x, c.y}, kInk);
        dl->AddRectFilled({c.x, c.y}, {c.x + q, c.y + q}, kInk);
    }
}

bool button(const char* id, ImVec2 size, const char* tip, bool disabled,
            ImVec2& center, bool active) {
    ImGui::PushID(id);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (active) {
        ImVec4 wash = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
        wash.w = 0.22f;
        ImGui::PushStyleColor(ImGuiCol_Button, wash);
        wash.w = 0.32f;
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, wash);
    }
    ImGui::BeginDisabled(disabled);
    const bool clicked = ImGui::Button("##b", size);
    ImGui::EndDisabled();
    if (active) ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    center = ImVec2(p0.x + size.x * 0.5f, p0.y + size.y * 0.5f);
    ImGui::PopID();
    ImGui::SameLine();
    return clicked;
}

} // namespace icon
