#include "ModelMode.hpp"

#include <memory>

#include <ImGuizmo.h>

#include "Command.hpp"
#include "Component.hpp"
#include "EditMesh.hpp"
#include "EditorContext.hpp"

namespace modelmode {

MeshComponent* selectedMesh(EditorContext& ed) {
    if (!ed.sel.valid()) return nullptr;
    return ed.entities[ed.sel.index()].components.get<MeshComponent>();
}

// Built at the object's real dimensions, so a metre in the modelling panel is a
// metre in the world rather than a fraction of a unit cube. Nothing else about
// the object changes: same transform, same material, and the same type, so it
// keeps the collider of the shape it started as (a ramp stays a slope to walk
// and hover up), fitted to its bounds.
void convertToMesh(EditorContext& ed) {
    if (!ed.sel.valid()) return;
    Entity& e = ed.entities[ed.sel.index()];
    if (!isSolidPrimitive(e.type) || e.components.get<MeshComponent>()) return;
    const Entity before = e;
    auto mc = std::make_unique<MeshComponent>();
    switch (e.type) {
        case EntityType::Ramp:     mc->mesh = EditMesh::ramp(e.half);     break;
        case EntityType::Cylinder: mc->mesh = EditMesh::cylinder(e.half); break;
        case EntityType::Sphere:   mc->mesh = EditMesh::sphere(e.half);   break;
        case EntityType::Plane:    mc->mesh = EditMesh::plane(e.half);    break;
        default:                   mc->mesh = EditMesh::box(e.half);      break;
    }
    mc->touch();
    e.components.items.push_back(std::move(mc));
    ed.meshFaceSel = -1;
    ed.history.pushApplied(std::make_unique<ModifyEntityCmd>(before, e));
}

void applyEdit(EditorContext& ed, const std::function<int(MeshComponent&)>& op,
               const char* label) {
    if (!ed.sel.valid()) return;
    Entity& e = ed.entities[ed.sel.index()];
    MeshComponent* mc = e.components.get<MeshComponent>();
    if (!mc || !op) return;
    const Entity    before = e;
    // The scale the entity applies to its mesh (1 unless someone has dragged
    // the Scale gizmo): read before the edit and re-applied after, or
    // re-deriving the half-extents from raw bounds would quietly undo it.
    const glm::vec3 scale  = editmesh::fitScale(mc->mesh, e.half);
    const glm::mat4 model  = meshModelOf(e, *mc);
    const EditMesh  beforeMesh = mc->mesh;
    ed.meshFaceSel = op(*mc);
    // Before the re-centre: the world positions it computes with `model` are
    // the ones the re-centred mesh keeps, and the edges that changed glow for a
    // moment where they now are.
    modeltools::flash(beforeMesh, mc->mesh, model, label);
    normalizeMeshEntity(ed.entities, e, *mc, scale);
    auto cmd = std::make_unique<ModifyEntityCmd>(before, e);
    if (!cmd->trivial()) ed.history.pushApplied(std::move(cmd));
}

void live(EditorContext& ed, Session& s, modelkeys::Live ph,
          const std::function<void(EditMesh&)>& op, const char* label) {
    if (!ed.sel.valid()) return;
    Entity& e = ed.entities[ed.sel.index()];
    if (ph == modelkeys::Live::Begin) {
        s.liveBefore = e;
        if (const MeshComponent* mc = e.components.get<MeshComponent>())
            s.liveScale = editmesh::fitScale(mc->mesh, e.half);
    } else if (ph == modelkeys::Live::Set || ph == modelkeys::Live::Cancel) {
        e = s.liveBefore;
        MeshComponent* mc = e.components.get<MeshComponent>();
        if (ph == modelkeys::Live::Set && mc && op) {
            op(mc->mesh);
            normalizeMeshEntity(ed.entities, e, *mc, s.liveScale);
        }
    } else {
        if (const MeshComponent* mc = e.components.get<MeshComponent>())
            if (const MeshComponent* b0 = s.liveBefore.components.get<MeshComponent>())
                modeltools::flash(b0->mesh, mc->mesh, meshModelOf(s.liveBefore, *b0), label);
        auto cmd = std::make_unique<ModifyEntityCmd>(s.liveBefore, e);
        if (!cmd->trivial()) ed.history.pushApplied(std::move(cmd));
    }
}

void viewport(EditorContext& ed, const ViewportFrame& view, Session& s, const ViewportHost& h) {
    if (!selectedMesh(ed)) return;
    modeltools::View mv;
    mv.vp   = view.viewProj;
    mv.min  = view.origin;
    mv.size = ImVec2(view.w, view.h);
    modelkeys::Host kh;
    auto keyHost = [&] {   // fresh each call: a modal edit replaces the mesh
        kh.mesh      = selectedMesh(ed);
        if (kh.mesh) mv.model = meshModelOf(ed.entities[ed.sel.index()], *kh.mesh);
        kh.sel       = &s.sel;
        kh.faceSel   = &ed.meshFaceSel;
        kh.view      = mv;
        kh.hovered   = view.hovered;
        kh.keysFree  = !ImGui::GetIO().WantTextInput;
        kh.gizmoOver = h.gizmoOut && (ImGuizmo::IsOver() || ImGuizmo::IsUsing());
        kh.edit      = [&ed](const std::function<int(MeshComponent&)>& op, const char* label) {
            applyEdit(ed, op, label);
        };
        kh.live      = [&ed, &s](modelkeys::Live ph, const std::function<void(EditMesh&)>& op,
                                 const char* label) { live(ed, s, ph, op, label); };
        kh.frame     = h.frame;
        return kh;
    };
    modelkeys::update(keyHost());
    if (const MeshComponent* mc = keyHost().mesh) {
        modeltools::Hit hov;
        const bool hovering =
            view.hovered && !ImGuizmo::IsUsing() && !h.faceDragged &&
            !modelkeys::busy() && !ImGui::IsMouseDown(ImGuiMouseButton_Right);
        if (hovering)
            hov = modeltools::pick(mc->mesh, mv, ImGui::GetIO().MousePos, s.sel.mode);
        modeltools::drawOverlay(ImGui::GetWindowDrawList(), mc->mesh, mv, s.sel, ed.meshFaceSel,
                                hovering ? &hov : nullptr);
        modelkeys::drawHud(ImGui::GetWindowDrawList(), kh);
    }
}

bool click(EditorContext& ed, const ViewportFrame& view, Session& s) {
    const MeshComponent* mc = selectedMesh(ed);
    if (!mc) return false;
    const ImGuiIO& io = ImGui::GetIO();
    modeltools::View mv;
    mv.model = meshModelOf(ed.entities[ed.sel.index()], *mc);
    mv.vp    = view.viewProj;
    mv.min   = view.origin;
    mv.size  = ImVec2(view.w, view.h);
    const modeltools::Hit hit = modeltools::pick(mc->mesh, mv, io.MousePos, s.sel.mode);
    return modeltools::click(s.sel, ed.meshFaceSel, hit, s.sel.additive || io.KeyShift);
}

MeshComponent* keepFaceSelection(EditorContext& ed) {
    MeshComponent* mc = selectedMesh(ed);
    const int selId = ed.sel.valid() ? ed.entities[ed.sel.index()].id : -1;
    if (selId != ed.meshFaceOwner) { ed.meshFaceOwner = selId; ed.meshFaceSel = -1; }
    if (!mc || ed.meshFaceSel >= static_cast<int>(mc->mesh.faces.size())) ed.meshFaceSel = -1;
    return mc;
}

} // namespace modelmode
