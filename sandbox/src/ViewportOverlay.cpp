#include "ViewportOverlay.hpp"

#include <cmath>

#include <glm/gtc/quaternion.hpp>
#include <imgui.h>

#include "Component.hpp"
#include "EditorContext.hpp"
#include "SceneGraph.hpp"

namespace overlay {

void cursorMark(const ViewportFrame& view, const glm::vec3& at) {
    ImVec2 c;
    if (!view.project(at, c)) return;
    ImDrawList* cdl = ImGui::GetWindowDrawList();
    const float R = 10.0f;
    const ImU32 red = IM_COL32(232, 66, 66, 255);
    const ImU32 wht = IM_COL32(245, 245, 245, 255);
    for (int s = 0; s < 8; ++s) {
        const float a0 = s * 0.7853982f, a1 = (s + 1) * 0.7853982f;
        cdl->PathArcTo(c, R, a0, a1, 8);
        cdl->PathStroke((s & 1) ? wht : red, 0, 2.2f);
    }
    const ImU32 k = IM_COL32(20, 20, 20, 220);
    cdl->AddLine({c.x - R - 5, c.y}, {c.x - R + 1, c.y}, k, 1.4f);
    cdl->AddLine({c.x + R - 1, c.y}, {c.x + R + 5, c.y}, k, 1.4f);
    cdl->AddLine({c.x, c.y - R - 5}, {c.x, c.y - R + 1}, k, 1.4f);
    cdl->AddLine({c.x, c.y + R - 1}, {c.x, c.y + R + 5}, k, 1.4f);
    cdl->AddCircleFilled(c, 1.6f, k);
}

namespace {

// A component's gizmo, drawn through the viewport's projection. Generic -- the
// viewport only supplies the projection, so a new component brings its gizmo
// with no change here.
struct VpGizmo : GizmoDraw {
    ImDrawList*          dl = nullptr;
    const ViewportFrame* view = nullptr;
    static ImU32 toCol(const glm::vec4& c) {
        return IM_COL32(int(c.r * 255.0f), int(c.g * 255.0f),
                        int(c.b * 255.0f), int(c.a * 255.0f));
    }
    void line(const glm::vec3& a, const glm::vec3& b, const glm::vec4& c) override {
        ImVec2 pa, pb;
        if (view->project(a, pa) && view->project(b, pb))
            dl->AddLine(pa, pb, toCol(c), 2.0f);
    }
    void circle(const glm::vec3& ctr, float rad,
                const glm::vec3& axis, const glm::vec4& c) override {
        const glm::vec3 n = glm::normalize(axis);
        const glm::vec3 up = (std::abs(n.y) < 0.99f)
            ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
        const glm::vec3 u = glm::normalize(glm::cross(n, up));
        const glm::vec3 v = glm::cross(n, u);
        const int SEG = 40;
        ImVec2 prev; bool have = false;
        for (int i = 0; i <= SEG; ++i) {
            const float a = 6.2831853f * i / SEG;
            ImVec2 s2;
            if (!view->project(ctr + (u * std::cos(a) + v * std::sin(a)) * rad, s2)) {
                have = false; continue;
            }
            if (have) dl->AddLine(prev, s2, toCol(c), 1.5f);
            prev = s2; have = true;
        }
    }
};

} // namespace

void selection(const EditorContext& ed, const ViewportFrame& view) {
    if (!ed.sel.valid()) return;
    const Entity& b = ed.entities[ed.sel.index()];
    if (b.type == EntityType::Sun) return;

    // Oriented wireframe highlight. One projector, reused for the active object
    // (bright) and any other selected objects (dim), so a multi-selection shows
    // every picked box.
    auto wireBox = [&](const Entity& e, ImU32 col, float thick) {
        view.wireBox(scenegraph::compose(e.center, e.rotation, glm::vec3(1.0f)),
                     -e.half, e.half, col, thick);
    };
    // Other selected objects first (dim) so the active box (bright) draws on top.
    for (int sid : ed.sel.multi()) {
        if (sid == b.id) continue;
        if (const Entity* se = ed.document.find(sid))
            wireBox(*se, IM_COL32(255, 170, 40, 150), 1.4f);
    }
    wireBox(b, IM_COL32(255, 140, 0, 230), 1.8f);

    // Component gizmos: each component of the selected entity draws its own
    // world-space overlay (a radius, a path).
    VpGizmo gz;
    gz.dl   = ImGui::GetWindowDrawList();
    gz.view = &view;
    if (b.parent >= 0)
        if (const Entity* pe = ed.document.find(b.parent)) {
            gz.parentCenter = pe->center;
            gz.parentHalf   = pe->half;
            gz.hasParent    = true;
        }
    // A multishot camera's "parent" for gizmo purposes is what it SHOOTS, which
    // is deliberately not what it hangs from (see CameraComponent::shotTarget).
    // Same question -- which object is this camera about -- so it goes down the
    // same channel rather than growing a second one.
    if (const auto* mcam = b.components.get<CameraComponent>();
        mcam && mcam->mode == CameraComponent::Multishot && mcam->shotTarget >= 0)
        if (const Entity* se = ed.document.find(mcam->shotTarget)) {
            gz.parentCenter = se->center;
            gz.parentHalf   = se->half;
            gz.hasParent    = true;
        }
    for (const auto& comp : b.components.items)
        comp->onGizmo(gz, b.center, glm::quat(glm::radians(b.rotation)));
}

void empties(const std::vector<Entity>& entities, const ViewportFrame& view) {
    ImDrawList* odl = ImGui::GetWindowDrawList();
    for (const Entity& e : entities) {
        if (e.type != EntityType::Empty) continue;
        if (!e.activeInHierarchy) continue;   // hidden group node
        ImVec2 sc;
        if (!view.project(e.center, sc)) continue;
        const float r = 7.0f;
        const ImU32 col = IM_COL32(170, 175, 185, 220);
        odl->AddLine({sc.x - r, sc.y}, {sc.x + r, sc.y}, col, 1.5f);
        odl->AddLine({sc.x, sc.y - r}, {sc.x, sc.y + r}, col, 1.5f);
        odl->AddCircle(sc, r * 0.45f, col, 0, 1.5f);
        if (!e.name.empty())
            odl->AddText({sc.x + r + 3.0f, sc.y - 7.0f}, col, e.name.c_str());
    }
}

} // namespace overlay
