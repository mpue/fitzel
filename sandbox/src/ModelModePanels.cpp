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

void uvPanel(EditorContext& ed, bool& show) {
    if (!show) return;
    MeshComponent* mc  = keepFaceSelection(ed);
    const bool haveSel = ed.sel.valid();
    // The material the OBJECT wears: the panel draws the texture the face is
    // actually seen through, and a face wearing none of its own is seen
    // through this one.
    fitzel::AssetId objMat;
    if (haveSel)
        if (const auto* mcp = ed.entities[ed.sel.index()].components.get<MaterialComponent>())
            objMat = mcp->material;
    uvui::drawPanel({
        show, mc, ed.meshFaceSel, ed.materials, objMat, haveSel,
        haveSel && !mc && isSolidPrimitive(ed.entities[ed.sel.index()].type),
        [&ed] { convertToMesh(ed); },
        [&ed](const std::function<int(MeshComponent&)>& op, const char* label) {
            applyEdit(ed, op, label);
        },
        mc ? static_cast<int>(mc->mesh.faces.size()) : 0,
    });
}

} // namespace modelmode
