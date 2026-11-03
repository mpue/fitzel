// The Modeling and UV panels of the modelling mode (ModelMode.hpp), apart from
// the rest of it: the Modeling panel's "duplicate along a path" reads the
// scene's splines, which the headless editorcheck does not link.
#include "ModelMode.hpp"

#include "Component.hpp"
#include "EditorContext.hpp"
#include "ModelingPanel.hpp"
#include "UvPanel.hpp"

namespace modelmode {

void modelingPanel(EditorContext& ed, Session& s, const PanelHost& p) {
    if (!p.show) return;
    MeshComponent* mc  = keepFaceSelection(ed);
    const bool haveSel = ed.sel.valid();
    // validate() does for the picked corners and edges what keepFaceSelection
    // did for the face.
    modeltools::validate(s.sel, haveSel ? ed.entities[ed.sel.index()].id : -1,
                         mc ? &mc->mesh : nullptr, &ed.meshFaceSel);
    ImGui::BeginDisabled(modelkeys::busy());
    modelui::drawPanel({
        p.show, mc, ed.meshFaceSel, s.sel, ed.materials, haveSel,
        haveSel && !mc && isSolidPrimitive(ed.entities[ed.sel.index()].type),
        mc ? meshModelOf(ed.entities[ed.sel.index()], *mc) : glm::mat4(1.0f),
        p.viewMin, p.viewMax,
        [&ed] { convertToMesh(ed); },
        [&ed](const std::function<int(MeshComponent&)>& op, const char* label) {
            applyEdit(ed, op, label);
        },
        // "Edit" on a face's material: the surface itself is a material, and
        // the place to change one is the Materials panel. Reads only, so it is
        // safe from inside the panel.
        [&](fitzel::AssetId id) {
            if (!id.valid()) return;
            ed.matSel       = ed.document.materialIndex(id);
            p.showMaterials = true;
        },
        mc ? static_cast<int>(mc->mesh.faces.size()) : 0,
        mc ? static_cast<int>(mc->mesh.verts.size()) : 0,
        p.cursor, p.splines,
    });
    ImGui::EndDisabled();
}

void uvPanel(EditorContext& ed, Session& ms, bool& show, const std::function<void(bool)>& undo) {
    if (!show) {
        uvui::drawPanel({show, nullptr, ed.meshFaceSel, -1, {}, ed.materials});
        return;
    }
    MeshComponent* mc  = keepFaceSelection(ed);
    const bool haveSel = ed.sel.valid();
    // The material the OBJECT wears: the panel draws the texture the faces are
    // actually seen through, and a face wearing none of its own is seen
    // through this one.
    fitzel::AssetId objMat;
    if (haveSel)
        if (const auto* mcp = ed.entities[ed.sel.index()].components.get<MaterialComponent>())
            objMat = mcp->material;
    // The faces picked in the viewport are the ones the UV editor shows.
    std::vector<int> picked;
    if (mc) {
        modeltools::validate(ms.sel, ed.entities[ed.sel.index()].id, &mc->mesh, &ed.meshFaceSel);
        if (ms.sel.mode == modeltools::Mode::Face) picked = modeltools::selectedFaces(ms.sel, mc->mesh, ed.meshFaceSel);
    }
    uvui::PanelState st{show, mc, ed.meshFaceSel, haveSel ? ed.entities[ed.sel.index()].id : -1, picked,
                        ed.materials};
    st.objectMaterial = objMat;
    st.haveSelection  = haveSel;
    st.canConvert     = haveSel && !mc && isSolidPrimitive(ed.entities[ed.sel.index()].type);
    st.convert        = [&ed] { convertToMesh(ed); };
    st.edit           = [&ed](const std::function<int(MeshComponent&)>& op, const char* label) { applyEdit(ed, op, label); };
    st.live = [&ed, &ms](uvui::Live ph, const std::function<void(EditMesh&)>& op, const char* label) {
        static const modelkeys::Live map[4] = {modelkeys::Live::Begin, modelkeys::Live::Set,
                                               modelkeys::Live::Commit, modelkeys::Live::Cancel};
        live(ed, ms, map[static_cast<int>(ph)], op, label);
    };
    st.meshNow   = [&ed] { return selectedMesh(ed); };
    st.undo      = undo;
    st.faceCount = mc ? static_cast<int>(mc->mesh.faces.size()) : 0;
    uvui::drawPanel(st);
}

} // namespace modelmode
