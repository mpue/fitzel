#include "ViewportPick.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "EditorContext.hpp"
#include "SandboxMath.hpp" // rayAABB

namespace viewpick {

std::vector<int> rayPick(const EditorContext& ed, const ViewportFrame& view) {
    glm::vec3 ro, rd;
    view.mouseRay(ro, rd);
    std::vector<std::pair<float, int>> hits;
    for (const Entity& e : ed.entities) {
        if (!e.activeInHierarchy) continue; // not shown, not pickable
        const float t = rayAABB(ro, rd, e.center - e.half, e.center + e.half);
        if (t >= 0.0f) hits.emplace_back(t, e.id);
    }
    std::sort(hits.begin(), hits.end());
    std::vector<int> ids;
    ids.reserve(hits.size());
    for (const auto& h : hits) ids.push_back(h.second);
    return ids;
}

void click(EditorContext& ed, const ViewportFrame& view, Picker& p, const Host& h) {
    const ImGuiIO& io = ImGui::GetIO();
    const bool selMod = io.KeyCtrl; // Ctrl = modify-selection gesture

    // Ctrl+left: start a selection gesture (a click toggles one; a drag draws
    // an additive box).
    if (h.canPick && selMod && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        p.boxSelecting = true;
        p.boxStart     = io.MousePos;
    }
    if (p.boxSelecting) {
        const ImVec2 cur = io.MousePos;
        const ImVec2 a(std::min(p.boxStart.x, cur.x), std::min(p.boxStart.y, cur.y));
        const ImVec2 b2(std::max(p.boxStart.x, cur.x), std::max(p.boxStart.y, cur.y));
        ImDrawList* bdl = ImGui::GetWindowDrawList();
        bdl->AddRectFilled(a, b2, IM_COL32(255, 160, 0, 40));
        bdl->AddRect(a, b2, IM_COL32(255, 160, 0, 180));
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            p.boxSelecting = false;
            const float dragPx = std::max(std::abs(cur.x - p.boxStart.x),
                                          std::abs(cur.y - p.boxStart.y));
            if (dragPx < 4.0f) { // no drag -> toggle the object clicked
                const std::vector<int> ids = rayPick(ed, view);
                if (!ids.empty()) ed.sel.toggle(ids[0]);
            } else {             // box -> add every centre inside the rect
                std::vector<int> inBox;
                for (const Entity& e : ed.entities) {
                    if (!e.activeInHierarchy || e.type == EntityType::Sun) continue;
                    ImVec2 sc;
                    if (!view.project(e.center, sc)) continue;
                    if (sc.x >= a.x && sc.x <= b2.x && sc.y >= a.y && sc.y <= b2.y)
                        inBox.push_back(e.id);
                }
                ed.sel.addMany(inBox);
            }
        }
        return;
    }

    // Plain left-click (no Ctrl): select or place.
    if (!h.canPick || selMod || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
    // ...except while modelling, where a click that lands on the selected mesh
    // picks one of its FACES. It only takes the click when it actually hits
    // that mesh, so clicking anything else still selects objects as usual --
    // and clicking the object you already have selected was a no-op anyway,
    // which is the click this borrows.
    if (h.meshClick && h.meshClick()) return;
    // A click that missed every face lets go of the one that was selected --
    // which is also how the gizmo is handed back to the whole object.
    ed.meshFaceSel = -1;
    const std::vector<int> ids = rayPick(ed, view);
    if (!ids.empty()) {
        // Same overlapping stack as last click -> advance to the next
        // candidate; a new stack -> start at the nearest.
        if (ids == p.pickStack)
            p.pickIdx = (p.pickIdx + 1) % static_cast<int>(ids.size());
        else { p.pickStack = ids; p.pickIdx = 0; }
        ed.sel.select(ids[p.pickIdx]);
    } else if (h.placeMode) {
        glm::vec3 at; // Create mode: empty ground -> drop a new block
        if (view.pickTerrain && view.pickTerrain(view.mouseNdc, view.viewProj, at) && h.place)
            h.place(at);
    } else {
        ed.sel.clear(); // empty click clears it
        p.pickStack.clear(); p.pickIdx = -1;
    }
}

} // namespace viewpick
