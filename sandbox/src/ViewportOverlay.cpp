#include "ViewportOverlay.hpp"

#include <algorithm>
#include <cmath>

#include <glm/gtc/quaternion.hpp>
#include <imgui.h>

#include "Component.hpp"
#include "EditorContext.hpp"
#include "ObjectIcons.hpp"
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

namespace {

// Where an object's icon stands on screen this frame, and how it is drawn.
struct Placed {
    const Entity* e = nullptr;
    objicon::Kind kind = objicon::Kind::None;
    ImVec2 c;              // centre, screen pixels
    float  half  = 0.0f;   // half the plate's side (the plain cross: its reach)
    float  alpha = 1.0f;
    float  dist  = 0.0f;   // from the eye
    bool   near  = true;   // close enough for its name
};

float smooth(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

ImU32 withAlpha(ImU32 col, float k) {
    const int a = static_cast<int>(((col >> IM_COL32_A_SHIFT) & 0xFF) * k);
    return (col & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
}

// Every icon in view, farthest first: the order they are painted in, so of two
// that overlap the later one is on top.
std::vector<Placed> placeIcons(const std::vector<Entity>& entities, const ViewportFrame& view,
                               float scale) {
    std::vector<Placed> out;
    // Three text lines tall: a picture to read at a glance and a target to
    // hit without aiming, at the default scale.
    const float base  = ImGui::GetFontSize() * 1.5f * std::clamp(scale, 0.25f, 4.0f);
    const bool  ortho = view.orthoHalfH > 0.0f;
    for (const Entity& e : entities) {
        if (!e.activeInHierarchy) continue;   // switched off: not shown, not picked
        const objicon::Kind k = objicon::kindOf(e);
        if (k == objicon::Kind::None) continue;
        Placed p;
        if (!view.project(e.center, p.c)) continue;
        p.e    = &e;
        p.kind = k;
        p.dist = glm::length(e.center - view.cameraPos);
        // Nearly the same size anywhere, so a light 80 m off can still be found
        // and hit -- but a little smaller and fainter with distance, which is
        // what tells near from far where nothing else in the picture does. An
        // orthographic view has no near and far.
        const float shrink = ortho ? 0.0f : smooth(8.0f, 70.0f, p.dist);
        p.half  = base * (1.0f - 0.35f * shrink) * (k == objicon::Kind::Empty ? 0.6f : 1.0f);
        p.alpha = 1.0f - 0.45f * (ortho ? 0.0f : smooth(15.0f, 150.0f, p.dist));
        p.near  = ortho || p.dist < 30.0f;
        out.push_back(p);
    }
    std::sort(out.begin(), out.end(),
              [](const Placed& a, const Placed& b) { return a.dist > b.dist; });
    return out;
}

bool covers(const Placed& p, ImVec2 at) {
    return std::abs(at.x - p.c.x) <= p.half && std::abs(at.y - p.c.y) <= p.half;
}

// What a light's icon is filled with: the hue it shines in, at full brightness
// -- a dim warm light still reads as warm, not as a dark blot.
ImU32 shineOf(const Entity& e, float alpha) {
    glm::vec3 c(1.0f, 0.95f, 0.8f);
    if (const auto* l = e.components.get<LightComponent>()) c = glm::max(l->color, glm::vec3(0.0f));
    const float m = std::max(c.r, std::max(c.g, c.b));
    c = (m > 1e-3f) ? c / m : glm::vec3(0.35f);
    return IM_COL32(int(c.r * 255.0f), int(c.g * 255.0f), int(c.b * 255.0f), int(230.0f * alpha));
}

} // namespace

void icons(const EditorContext& ed, const ViewportFrame& view, float scale) {
    const std::vector<Placed> placed = placeIcons(ed.entities, view, scale);
    // The icon under the pointer: the last one painted there.
    int hot = -1;
    if (view.hovered)
        for (int i = static_cast<int>(placed.size()) - 1; i >= 0; --i)
            if (covers(placed[i], view.mousePos)) { hot = i; break; }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int   active = ed.sel.activeId();
    const float fh     = ImGui::GetFontSize();
    auto draw = [&](int i) {
        const Placed& p   = placed[i];
        const bool    sel = ed.sel.contains(p.e->id);
        const bool    lit = i == hot;
        const float   a   = (sel || lit) ? 1.0f : p.alpha;
        const ImU32   col = withAlpha(objicon::tint(p.kind), a);
        const ImU32   rim = p.e->id == active ? IM_COL32(255, 140, 0, 240)
                                              : IM_COL32(255, 170, 40, 190);
        if (p.kind == objicon::Kind::Empty) {
            objicon::glyph(dl, p.kind, p.c, p.half, sel ? rim : col, 0);
        } else {
            const ImVec2 lo(p.c.x - p.half, p.c.y - p.half), hi(p.c.x + p.half, p.c.y + p.half);
            const float  round = p.half * 0.38f;
            dl->AddRectFilled(lo, hi, lit ? IM_COL32(60, 64, 76, int(240.0f * a))
                                          : IM_COL32(22, 24, 30, int(205.0f * a)), round);
            if (sel) dl->AddRect(lo, hi, rim, round, 0, p.e->id == active ? 2.4f : 1.8f);
            else     dl->AddRect(lo, hi, withAlpha(col, 0.7f), round, 0, 1.2f);
            objicon::glyph(dl, p.kind, p.c, p.half * 0.64f, col, shineOf(*p.e, a));
            // The camera Play starts with carries a play mark in its corner.
            if (const auto* cam = p.e->components.get<CameraComponent>();
                cam && cam->activeOnStart) {
                const float  s = p.half * 0.42f;
                const ImVec2 m(hi.x - s * 0.15f, hi.y - s * 0.15f);
                dl->AddCircleFilled(m, s * 0.78f, IM_COL32(22, 24, 30, int(235.0f * a)));
                dl->AddTriangleFilled(ImVec2(m.x - s * 0.3f, m.y - s * 0.45f),
                                      ImVec2(m.x - s * 0.3f, m.y + s * 0.45f),
                                      ImVec2(m.x + s * 0.48f, m.y),
                                      IM_COL32(120, 220, 140, int(255.0f * a)));
            }
        }
        // Its name while it is near, and always once it is picked or pointed
        // at -- forty far-off labels are a texture, not information.
        if (!p.e->name.empty() && (p.near || sel || lit)) {
            const ImVec2 tp(p.c.x + p.half + 4.0f, p.c.y - fh * 0.5f);
            const char*  nm = p.e->name.c_str();
            dl->AddText(ImVec2(tp.x + 1.0f, tp.y + 1.0f), IM_COL32(0, 0, 0, int(170.0f * a)), nm);
            dl->AddText(tp, (sel || lit) ? IM_COL32(245, 245, 245, 255)
                                         : withAlpha(IM_COL32(222, 226, 234, 235), a), nm);
        }
    };
    for (int i = 0; i < static_cast<int>(placed.size()); ++i)
        if (i != hot) draw(i);
    // The one pointed at last, over its neighbours' plates and names: it is the
    // top one under the pointer either way, and its name is the one being read.
    if (hot >= 0) draw(hot);
}

std::vector<int> iconsAt(const std::vector<Entity>& entities, const ViewportFrame& view,
                         ImVec2 at, float scale) {
    const std::vector<Placed> placed = placeIcons(entities, view, scale);
    std::vector<int> ids;
    for (auto it = placed.rbegin(); it != placed.rend(); ++it)
        if (covers(*it, at)) ids.push_back(it->e->id);
    return ids;
}

} // namespace overlay
