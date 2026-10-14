#include "ModelMode.hpp"

#include <algorithm>
#include <memory>

#include <ImGuizmo.h>

#include "Command.hpp"
#include "Component.hpp"
#include "EditMesh.hpp"
#include "EditorContext.hpp"
#include "SceneGraph.hpp"

namespace modelmode {

namespace {

// The editable mesh a solid primitive turns into, at its real size.
EditMesh primitiveMesh(const Entity& e) {
    switch (e.type) {
        case EntityType::Ramp:     return EditMesh::ramp(e.half);
        case EntityType::Cylinder: return EditMesh::cylinder(e.half);
        case EntityType::Sphere:   return EditMesh::sphere(e.half);
        case EntityType::Plane:    return EditMesh::plane(e.half);
        default:                   return EditMesh::box(e.half);
    }
}

bool joinable(const Entity& e) {
    return e.components.get<MeshComponent>() != nullptr || isSolidPrimitive(e.type);
}

fitzel::AssetId objectMaterial(const Entity& e) {
    const auto* m = e.components.get<MaterialComponent>();
    return m ? m->material : fitzel::AssetId{};
}

// The join as one undo step: the active object takes the joined mesh, the
// others go (their children onto the nearest surviving ancestor, as Delete).
class JoinCmd : public Command {
public:
    JoinCmd(const Document& d, Entity before, Entity after, const std::vector<int>& gone)
        : m_mod(std::move(before), std::move(after)), m_del(d, gone) {}
    void redo(Document& d) override { m_mod.redo(d); m_del.redo(d); }
    void undo(Document& d) override { m_del.undo(d); m_mod.undo(d); }
    const char* name() const override { return "Join"; }

private:
    ModifyEntityCmd   m_mod;
    DeleteEntitiesCmd m_del;
};

// The selected objects that can go into the active one.
std::vector<int> joinSources(const EditorContext& ed, int* skipped) {
    std::vector<int> out;
    if (skipped) *skipped = 0;
    if (!ed.sel.valid()) return out;
    const Entity& target = ed.entities[ed.sel.index()];
    for (int id : ed.sel.ids()) {
        if (id == target.id) continue;
        const Entity* o = nullptr;
        for (const Entity& e : ed.entities)
            if (e.id == id) { o = &e; break; }
        if (!o) continue;
        const std::vector<int> below = scenegraph::subtree(ed.entities, id);
        const bool ancestor = std::find(below.begin(), below.end(), target.id) != below.end();
        if (!joinable(*o) || ancestor) {
            if (skipped) ++*skipped;
            continue;
        }
        out.push_back(id);
    }
    return out;
}

} // namespace

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
    mc->mesh = primitiveMesh(e);
    mc->touch();
    e.components.items.push_back(std::move(mc));
    ed.meshFaceSel = -1;
    ed.history.pushApplied(std::make_unique<ModifyEntityCmd>(before, e));
}

bool canJoin(const EditorContext& ed) {
    return ed.sel.valid() && joinable(ed.entities[ed.sel.index()]) && !joinSources(ed, nullptr).empty();
}

void joinSelected(EditorContext& ed) {
    if (!ed.sel.valid()) return;
    const Entity& target = ed.entities[ed.sel.index()];
    if (!joinable(target)) { ed.status = "Join: the active object is neither a mesh nor a solid"; return; }
    int skipped = 0;
    const std::vector<int> gone = joinSources(ed, &skipped);
    if (gone.empty()) { ed.status = "Join: select one more mesh or solid besides the active object"; return; }

    // Built on a copy: the command puts it in place (and takes it out on undo).
    Entity after = target;
    MeshComponent* mc = after.components.get<MeshComponent>();
    if (!mc) {
        auto fresh = std::make_unique<MeshComponent>();
        fresh->mesh = primitiveMesh(after);
        mc = fresh.get();
        after.components.items.push_back(std::move(fresh));
    }
    const glm::vec3 scale   = editmesh::fitScale(mc->mesh, after.half);
    const glm::mat4 toLocal = glm::inverse(scenegraph::parentWorld(ed.entities, after) *
                                           meshModelOf(after, *mc));
    const fitzel::AssetId targetMat = objectMaterial(after);
    for (int id : gone) {
        const Entity* o = nullptr;
        for (const Entity& e : ed.entities)
            if (e.id == id) { o = &e; break; }
        if (!o) continue;
        MeshComponent src;
        if (const MeshComponent* om = o->components.get<MeshComponent>()) src.mesh = om->mesh;
        else                                                              src.mesh = primitiveMesh(*o);
        const glm::mat4 world = scenegraph::parentWorld(ed.entities, *o) * meshModelOf(*o, src);
        const fitzel::AssetId om = objectMaterial(*o);
        editmesh::appendMesh(mc->mesh, src.mesh, toLocal * world,
                             (om.valid() && om != targetMat) ? om : fitzel::AssetId{});
    }
    normalizeMeshEntity(ed.entities, after, *mc, scale);

    const int keepId = after.id;
    ed.history.push(std::make_unique<JoinCmd>(ed.document, target, std::move(after), gone),
                    ed.document);
    ed.sel.select(keepId);
    ed.meshFaceSel = -1;
    ed.status = "Joined " + std::to_string(gone.size() + 1) + " objects into one mesh";
    if (skipped > 0)
        ed.status += " (" + std::to_string(skipped) + " left out: not a mesh or solid, or a parent of the active one)";
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
