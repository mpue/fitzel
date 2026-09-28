#include "SceneDrop.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <fitzel/asset/AssetDatabase.hpp>

#include "Component.hpp"
#include "EditorContext.hpp"
#include "ModelLibrary.hpp"
#include "SandboxMath.hpp"

using namespace fitzel;

namespace scenedrop {

void dropOnScene(EditorContext& ed, const ViewportFrame& view, const AssetId& gid) {
    const AssetType at = ed.assetDb.typeForId(gid);
    // Is it one of the scene's materials? Asked of the library
    // rather than of the asset database, because a material
    // dragged out of the Materials panel has a GUID and may
    // have no .fmat on disk yet -- and it is still the thing
    // the drop is about.
    int dropMat = -1;
    for (int k = 0; k < static_cast<int>(ed.materials.size()); ++k)
        if (ed.materials[k].assetId == gid) { dropMat = k; break; }
    const glm::mat4& vp = view.viewProj;
    if (at == AssetType::Model) {
        glm::vec3 hit;
        if (view.pickTerrain(view.mouseNdc, vp, hit)) {
            const std::string mp = ed.assetDb.pathForId(gid).string();
            if (ed.isStructuredModel(mp)) ed.addModelHierarchy(hit, mp);
            else {
                const int id = ed.models.import(mp, ed.assetDb, ed.materials);
                if (id >= 0) ed.addModelEntity(hit, id);
            }
        }
    } else if (at == AssetType::Material || dropMat >= 0) {
        // A material dropped ON A FACE dresses that face
        // alone; dropped anywhere else on an object it
        // becomes the object's. This is the drag half of the
        // Modeling panel's picker, and it exists BESIDE it
        // rather than instead of it: aiming at a face is a
        // gesture some days do not have, and the panel's
        // combo is the same operation without one.
        const int mi = dropMat;
        glm::vec3 ro, rd;
        view.mouseRay(ro, rd);
        if (mi < 0) {
            ed.status = "That material isn't in this scene's "
                           "library -- open the project it "
                           "belongs to first.";
        } else {
        // The face under the cursor, over every modelled mesh
        // in the scene: dressing a face should not first
        // require selecting the object it belongs to.
        int   faceEnt = -1, faceHit = -1;
        float faceT   = 1e30f;
        for (int i = 0; i < static_cast<int>(ed.entities.size()); ++i) {
            const MeshComponent* emc =
                ed.entities[i].components.get<MeshComponent>();
            if (!emc) continue;
            for (int f = 0;
                 f < static_cast<int>(emc->mesh.faces.size()); ++f) {
                const std::vector<glm::vec3> w =
                    meshFaceWorld(ed.entities[i], *emc, f);
                // The same fan the GPU mesh is built from, so
                // what is dropped on is exactly what is drawn.
                for (std::size_t k = 1; k + 1 < w.size(); ++k) {
                    const float t =
                        rayTriangle(ro, rd, w[0], w[k], w[k + 1]);
                    if (t >= 0.0f && t < faceT) {
                        faceT = t; faceHit = f; faceEnt = i;
                    }
                }
            }
        }
        int hit = faceEnt;
        if (hit < 0) {
            // No face: the nearest solid takes it whole.
            float bestT = 1e30f;
            for (int i = 0; i < static_cast<int>(ed.entities.size()); ++i) {
                if (!isSolidPrimitive(ed.entities[i].type)) continue;
                const float d = rayAABB(ro, rd, ed.entities[i].center - ed.entities[i].half,
                                                ed.entities[i].center + ed.entities[i].half);
                if (d >= 0.0f && d < bestT) { bestT = d; hit = i; }
            }
        }
        if (hit >= 0) {
            const std::vector<int> ids{ed.entities[hit].id};
            auto before = ed.snapshot(ids);
            Entity& e = ed.entities[hit];
            if (faceEnt == hit && faceHit >= 0) {
                MeshComponent* emc = e.components.get<MeshComponent>();
                emc->mesh.setFaceMaterial(faceHit, gid);
                emc->touch();   // the GPU copy is split by material
                ed.meshFaceOwner = e.id;
                ed.meshFaceSel   = faceHit;
                ed.status  = "Material on one face.";
            } else if (auto* emc = e.components.get<MaterialComponent>()) {
                emc->material = gid;
            } else {
                auto nc = std::make_unique<MaterialComponent>();
                nc->material = gid;
                e.components.items.push_back(std::move(nc));
            }
            ed.sel.selectIndex(hit);
            ed.matSel    = mi;
            auto cmd = std::make_unique<ModifyEntitiesCmd>(
                before, ed.snapshot(ids));
            if (!cmd->trivial()) ed.history.pushApplied(std::move(cmd));
        }
        }
    } else if (at == AssetType::Texture) {
        // Pick the solid under the drop point.
        glm::vec3 ro, rd;
        view.mouseRay(ro, rd);
        int hit = -1; float bestT = 1e30f;
        for (int i = 0; i < static_cast<int>(ed.entities.size()); ++i) {
            if (!isSolidPrimitive(ed.entities[i].type)) continue;
            const float d = rayAABB(ro, rd, ed.entities[i].center - ed.entities[i].half,
                                            ed.entities[i].center + ed.entities[i].half);
            if (d >= 0.0f && d < bestT) { bestT = d; hit = i; }
        }
        if (hit >= 0) {
            // A new material that samples the dropped texture.
            MaterialDef nm;
            nm.assetId = AssetId::generate();
            const AssetDatabase::Entry* te = ed.assetDb.entry(gid);
            nm.name  = te ? std::filesystem::path(te->relPath).stem().string()
                          : "Textured";
            nm.texId = gid;
            nm.tex   = ed.assetDb.loadTexture(gid);
            ed.materials.push_back(nm);
            ed.matSel = static_cast<int>(ed.materials.size()) - 1;
            // Assign it to the object's MaterialComponent (undoable).
            const std::vector<int> ids{ed.entities[hit].id};
            auto before = ed.snapshot(ids);
            Entity& e = ed.entities[hit];
            if (auto* mc = e.components.get<MaterialComponent>())
                mc->material = nm.assetId;
            else {
                auto c = std::make_unique<MaterialComponent>();
                c->material = nm.assetId;
                e.components.items.push_back(std::move(c));
            }
            ed.sel.selectIndex(hit);
            auto cmd = std::make_unique<ModifyEntitiesCmd>(
                before, ed.snapshot(ids));
            if (!cmd->trivial()) ed.history.pushApplied(std::move(cmd));
        }
    }
}

} // namespace scenedrop
