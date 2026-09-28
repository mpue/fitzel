#include "GroundBrush.hpp"

#include <cmath>

#include "EditorContext.hpp"

namespace groundbrush {

Aim aim(const ViewportFrame& view, bool eraseToggle) {
    Aim at;
    at.onGround = view.hovered && view.pickTerrain &&
                  view.pickTerrain(view.mouseNdc, view.viewProj, at.center);
    at.erasing  = eraseToggle || ImGui::GetIO().KeyAlt;
    return at;
}

void drag(const Aim& at, glm::vec2& last, float spacing,
          const std::function<void(glm::vec2)>& put,
          const std::function<void(glm::vec2)>& rub) {
    // A fresh press starts a stroke; forget the last stamp point.
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) last = glm::vec2(1e9f);
    if (!at.onGround || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
    const glm::vec2 xz(at.center.x, at.center.z);
    if (at.erasing) {
        rub(xz);
    } else if (glm::length(xz - last) > spacing) {
        put(xz);
        last = xz;
    }
}

void ring(const ViewportFrame& view, const Aim& at, float radius, ImU32 paintCol,
          ImU32 eraseCol, int segments) {
    if (!at.onGround) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = at.erasing ? eraseCol : paintCol;
    ImVec2 prev; bool have = false;
    for (int i = 0; i <= segments; ++i) {
        const float a  = static_cast<float>(i) / segments * 6.2831853f;
        const float wx = at.center.x + std::cos(a) * radius;
        const float wz = at.center.z + std::sin(a) * radius;
        const float wy = (view.groundAt ? view.groundAt(wx, wz) : at.center.y) + 0.05f;
        ImVec2 sp;
        if (!view.toScreen(glm::vec3(wx, wy, wz), sp)) { have = false; continue; }
        if (have) dl->AddLine(prev, sp, col, 2.0f);
        prev = sp; have = true;
    }
}

} // namespace groundbrush
