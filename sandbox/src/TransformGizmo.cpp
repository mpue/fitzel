#include "TransformGizmo.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <fitzel/scene/Camera.hpp>

#include "Command.hpp"
#include "Component.hpp"
#include "EditMesh.hpp"
#include "EditorContext.hpp"
#include "ModelingTools.hpp"
#include "SceneGraph.hpp"

namespace gizmo {

namespace {

// With a face selected in Modeling, the gizmo drives THAT face rather than the
// object: the same Move/Rotate/Scale handles (Q/W/E), the same drag, applied to
// four corners instead of a transform. The panel's numbered buttons stay --
// typing 0.4 m and dragging to about 0.4 m are different tools, and which one is
// right depends on the day and on the hand.
void faceDrag(EditorContext& ed, Drag& d, const Settings& s, Entity& b, MeshComponent& mc,
              const std::vector<int>& verts, const glm::mat4& view, const glm::mat4& proj,
              bool snapHeld, const float* snapStep) {
    const glm::mat4 M = meshModelOf(b, mc);
    // The gizmo sits at the face's centre, oriented like the object. Handed over
    // fresh each frame; what comes back is a DELTA, which is the only form that
    // can be baked into geometry -- an absolute matrix would be re-applied on top
    // of itself every frame and a scale drag would run away exponentially.
    glm::vec3 pivot(0.0f);   // the centre of what it drives
    for (int vi : verts) pivot += mc.mesh.verts[vi];
    pivot /= static_cast<float>(verts.size());
    const glm::mat4 F = glm::translate(glm::mat4(1.0f), pivot);
    glm::mat4 world = M * F;
    // A face snaps in steps from where it started: its corners, not its centre,
    // are what would have to meet the grid.
    float delta[16];
    ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), s.op, s.mode,
                         glm::value_ptr(world), delta, snapHeld ? snapStep : nullptr);
    const bool using3d = ImGuizmo::IsUsing();
    if (using3d && !d.faceActive) {
        d.faceActive     = true;
        d.faceBefore     = b;   // one undo step per drag
        d.faceScale      = editmesh::fitScale(mc.mesh, b.half);
        d.faceAccScale   = glm::vec3(1.0f);
        d.faceStartPivot = glm::vec3(M * glm::vec4(pivot, 1.0f));
    }
    if (using3d) {
        const glm::mat4 D = glm::make_mat4(delta);
        glm::mat4 L(1.0f);
        if (s.op == ImGuizmo::SCALE) {
            // ImGuizmo reports the scale delta on different terms from the other
            // two: measured from the START of the drag, and in the gizmo's own
            // frame -- while move and rotate report the step since the last
            // frame, in world space. Applying it as if it were a step multiplies
            // the face by the whole drag again every frame, which runs away
            // exponentially. Divide out what has already been applied to get the
            // actual step.
            const glm::vec3 acc(D[0][0], D[1][1], D[2][2]);
            const glm::vec3 step = acc / glm::max(d.faceAccScale, glm::vec3(1e-6f));
            d.faceAccScale = acc;
            L = F * glm::scale(glm::mat4(1.0f), step) * glm::inverse(F);
        } else {
            // World-space step, conjugated into the mesh's own space:
            // p' = M^-1 * D * M * p.
            L = glm::inverse(M) * D * M;
        }
        editmesh::transformVerts(mc.mesh, verts, L);
        // Square the object's bounds with the new shape NOW, not when the drag
        // ends. The mesh is drawn at half/bounds, so leaving `half` behind while
        // the geometry grows shrinks that factor by exactly as much as the mesh
        // grew: the shape would sit there apparently unmoved while its local size
        // ran off, and let go of it at the end to reveal a body stretched to the
        // horizon.
        normalizeMeshEntity(ed.entities, b, mc, d.faceScale);

        // How far, next to the pointer: the drag says in numbers what it is
        // doing while it does it.
        char rd[96];
        if (s.op == ImGuizmo::TRANSLATE) {
            const glm::mat4 M2 = meshModelOf(b, mc);
            glm::vec3 p2(0.0f);
            for (int vi : verts) p2 += mc.mesh.verts[vi];
            p2 /= static_cast<float>(verts.size());
            const glm::vec3 dv = glm::vec3(M2 * glm::vec4(p2, 1.0f)) - d.faceStartPivot;
            std::snprintf(rd, sizeof rd, "%.2f m   (%+.2f, %+.2f, %+.2f)",
                          glm::length(dv), dv.x, dv.y, dv.z);
        } else if (s.op == ImGuizmo::SCALE) {
            std::snprintf(rd, sizeof rd, "x %.2f  %.2f  %.2f",
                          d.faceAccScale.x, d.faceAccScale.y, d.faceAccScale.z);
        } else {
            std::snprintf(rd, sizeof rd, "rotating %d corner%s",
                          static_cast<int>(verts.size()), verts.size() == 1 ? "" : "s");
        }
        const ImVec2 mp = ImGui::GetIO().MousePos;
        modeltools::readout(ImGui::GetWindowDrawList(), ImVec2(mp.x + 18.0f, mp.y + 18.0f), rd);
    } else if (d.faceActive) {
        // Drag finished: bank the whole of it as one undoable step. The bounds
        // are already square with the shape -- that happens on every frame above.
        d.faceActive = false;
        auto cmd = std::make_unique<ModifyEntityCmd>(d.faceBefore, b);
        if (!cmd->trivial()) ed.history.pushApplied(std::move(cmd));
    }
}

} // namespace

void frame(EditorContext& ed, const ViewportFrame& view, Drag& d, const Settings& s) {
    const float     asp  = view.w / view.h;
    const glm::mat4 V    = ed.camera.viewMatrix();
    const glm::mat4 P    = ed.camera.projectionMatrix(asp);

    if (s.editMode) {
        ImGuizmo::SetOrthographic(ed.camera.orthographic());
        ImGuizmo::SetDrawlist();
        ImGuizmo::SetRect(view.origin.x, view.origin.y, view.w, view.h);
        // A finished gizmo drag becomes one undoable Transform step.
        if (d.active && !ImGuizmo::IsUsing()) {
            d.active = false;
            auto cmd = std::make_unique<ModifyEntitiesCmd>(d.before, ed.snapshot(d.ids));
            if (!cmd->trivial()) ed.history.pushApplied(std::move(cmd));
        }
    }
    if (!ed.sel.valid() || ed.entities[ed.sel.index()].type == EntityType::Sun) return;

    Entity& b = ed.entities[ed.sel.index()];
    const int selId = b.id;
    float t[3] = {b.center.x, b.center.y, b.center.z};
    float r[3] = {b.rotation.x, b.rotation.y, b.rotation.z};
    float sc[3] = {b.half.x * 2.0f, b.half.y * 2.0f, b.half.z * 2.0f};

    // Ctrl rasters the drag (see snapAngle). ImGuizmo's own snap counts steps
    // from where the drag began: right for a turn and a scale, and for a move
    // along the object's own axes, which have no grid to meet. A move along the
    // WORLD axes lands on the grid itself instead (rounded below), so what you
    // place lines up with the lattice drawn under it rather than keeping its old
    // offset.
    const bool snapHeld      = ImGui::GetIO().KeyCtrl;
    const bool snapToLattice = snapHeld && s.op == ImGuizmo::TRANSLATE &&
                               s.mode == ImGuizmo::WORLD;
    float snapStep[3] = {s.grid, s.grid, s.grid};
    if (s.op == ImGuizmo::ROTATE) snapStep[0] = s.snapAngle;
    if (s.op == ImGuizmo::SCALE)  snapStep[0] = s.snapScale;

    // --- Face gizmo ----------------------------------------------------------
    MeshComponent* faceMc =
        (s.faceMode && s.editMode) ? b.components.get<MeshComponent>() : nullptr;
    // What it drives: the selected face's corners, or in vertex and edge mode
    // the picked corners (both ends of each edge).
    const std::vector<int> faceVerts =
        (faceMc && s.modelSel) ? modeltools::activeVerts(*s.modelSel, faceMc->mesh, ed.meshFaceSel)
                               : std::vector<int>{};
    if (faceVerts.empty()) faceMc = nullptr;
    if (faceMc) {
        faceDrag(ed, d, s, b, *faceMc, faceVerts, V, P, snapHeld, snapStep);
        return;
    }
    if (!s.editMode || !s.objectFree) return;

    // --- Object gizmo --------------------------------------------------------
    float model[16];
    ImGuizmo::RecomposeMatrixFromComponents(t, r, sc, model);
    ImGuizmo::Manipulate(glm::value_ptr(V), glm::value_ptr(P), s.op, s.mode, model, nullptr,
                         (snapHeld && !snapToLattice) ? snapStep : nullptr);
    const bool using3d = ImGuizmo::IsUsing();
    if (using3d && !d.active) { // drag start: snapshot subtrees
        d.active = true;
        d.roots  = ed.sel.ids();
        d.ids.clear();
        for (int rid : d.roots)
            for (int id : scenegraph::subtree(ed.entities, rid))
                if (std::find(d.ids.begin(), d.ids.end(), id) == d.ids.end())
                    d.ids.push_back(id);
        d.before = ed.snapshot(d.ids);
        d.prevT  = glm::vec3(t[0], t[1], t[2]);
        d.prevR  = glm::vec3(r[0], r[1], r[2]);
        d.prevS  = glm::vec3(sc[0], sc[1], sc[2]);
        d.startT = d.prevT;
    }
    if (!using3d) return;

    ImGuizmo::DecomposeMatrixToComponents(model, t, r, sc);
    // Onto the grid -- but only the axes the drag actually moves. Pulling the X
    // arrow must not also drop the object's height onto a grid line. ImGuizmo
    // hands back the unsnapped target every frame (it measures from the ray, not
    // from what it got last time), so rounding it here holds.
    if (snapToLattice && s.grid > 0.0f)
        for (int k = 0; k < 3; ++k)
            if (std::abs(t[k] - d.startT[k]) > 1e-4f)
                t[k] = std::round(t[k] / s.grid) * s.grid;
    const glm::vec3 newT(t[0], t[1], t[2]);
    const glm::vec3 newR(r[0], r[1], r[2]);
    const glm::vec3 newS(sc[0], sc[1], sc[2]);
    b.half = glm::max(newS * 0.5f, glm::vec3(0.05f));
    // World-space edit -> local (children then follow via resolveHierarchy).
    const glm::mat4 pw = scenegraph::parentWorld(ed.entities, b);
    scenegraph::setWorld(b, newT, newR, b.parent >= 0 ? &pw : nullptr);
    // Multi-select: apply the active object's incremental delta to every other
    // selected root (each scales / rotates about its own centre; children follow
    // via resolveHierarchy).
    if (d.roots.size() > 1) {
        const glm::vec3 dT    = newT - d.prevT;
        const glm::vec3 dR    = newR - d.prevR;
        const glm::vec3 ratio = newS / glm::max(d.prevS, glm::vec3(1e-4f));
        for (int rid : d.roots) {
            if (rid == selId) continue;
            Entity* re = ed.document.find(rid);
            if (!re || re->type == EntityType::Sun) continue;
            re->half = glm::max(re->half * ratio, glm::vec3(0.05f));
            const glm::mat4 rpw = scenegraph::parentWorld(ed.entities, *re);
            scenegraph::setWorld(*re, re->center + dT, re->rotation + dR,
                                 re->parent >= 0 ? &rpw : nullptr);
        }
    }
    d.prevT = newT; d.prevR = newR; d.prevS = newS;
}

} // namespace gizmo
