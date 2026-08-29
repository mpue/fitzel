#include "SceneSubmit.hpp"

#include <cstdint>
#include <string>

#include <glm/gtc/matrix_transform.hpp>

#include <fitzel/graphics/Material.hpp>
#include <fitzel/graphics/Mesh.hpp>
#include <fitzel/graphics/Shader.hpp>
#include <fitzel/render/Renderer.hpp>

#include "Component.hpp"

namespace scenesubmit {

using fitzel::AssetId;
using fitzel::Material;
using fitzel::Mesh;

void submit(const Context& c, Scratch& scratch) {
    scratch.clear();
    scratch.gpuMats.reserve(c.materials.size());
    for (const MaterialDef& md : c.materials) {
        Material& m = scratch.gpuMats.emplace_back(c.lit);
        m.set("uWaterLevel", -1.0e4f)
         .set("uWetness", c.roadWetness)
         .set("uReflectivity", md.reflectivity)
         .set("uRoughness", md.roughness)
         .set("uGlass", md.glass ? 1 : 0);
        if (md.tex)
            m.set("uColorMode", 2).setTexture("uTexture", *md.tex, 0)
             .set("uTint", md.tint); // always written (shared program)
        else
            m.set("uColorMode", 0).set("uAlbedo", md.albedo);
        if (md.normalTex)
            m.setTexture("uNormalMap", *md.normalTex, 1).set("uHasNormalMap", 1);
        else
            m.set("uHasNormalMap", 0);
        // Cutout ("transparency map"): let the shader discard masked
        // texels. Blend routes through the transparent queue at submit.
        if (md.alphaMode == AlphaMode::Cutout)
            m.set("uAlphaCutout", 1).set("uAlphaCutoff", md.alphaCutoff);
        else
            m.set("uAlphaCutout", 0);
        // Emission (self-illumination): colour * strength, optionally
        // masked by an _Illum map (unit 3 -- free for object materials).
        m.set("uEmission", md.emission)
         .set("uEmissionStrength", md.emissionStrength);
        if (md.emissionTex)
            m.setTexture("uEmissionMap", *md.emissionTex, 3).set("uHasEmissionMap", 1);
        else
            m.set("uHasEmissionMap", 0);
        // Procedural window grid (generated buildings): hashed lit windows
        // on the vertical faces, world-space so the rows line up across a
        // stack's setbacks. Written either way -- the shader program is
        // shared, so a material that leaves it alone inherits the last
        // building's facade.
        if (md.windowGrid)
            m.set("uWindowGrid", 1)
             .set("uWindowCell", md.windowCell)
             .set("uWindowLit", md.windowLit)
             .set("uWindowSeed", md.windowSeed)
             .set("uWindowColor", md.windowColor)
             .set("uWindowGlow", md.windowGlow);
        else
            m.set("uWindowGrid", 0);
        // Only a painted mesh's own copy of this material turns vPaint
        // into layer weights (below). Written on every material, or a
        // shared program hands the last painted object's flag to the next
        // thing drawn with the same shader.
        m.set("uMeshPaint", 0);
    }
    // Per-entity copies for the modelled meshes somebody has painted: the
    // layer textures and the flag are per OBJECT, while scratch.gpuMats is shared
    // by every entity using that material. Reserved up front so the
    // references handed to submit() stay valid as it fills.
    scratch.paintMats.reserve(c.entities.size());
    scratch.lightMats.reserve(c.entities.size());
    for (const Entity& b : c.entities) {
        if (!b.activeInHierarchy) continue;         // deactivated: hidden
        if (b.type == EntityType::Sun) continue;   // directional, no geometry
        if (b.type == EntityType::Empty) continue;  // grouping node, no geometry
        // Player-start markers are authoring aids -- hidden while playing.
        if (c.playMode && b.components.get<PlayerStartComponent>()) continue;
        // Light markers (the glowing cube) are authoring aids too: hide them
        // while playing so headlights etc. don't show a box -- the light
        // itself still shines (collected further below).
        if (c.playMode && b.type == EntityType::Light) continue;
        if (b.type == EntityType::Model) {
            // Imported model: draw every primitive with its baked material.
            // Centre the model's AABB at b.center (so it matches the pick
            // box), then translate/scale into the world.
            const auto* mdl = b.components.get<ModelComponent>();
            LoadedModel* lm = mdl ? c.models.byId(mdl->modelId) : nullptr;
            if (!lm) continue;
            // Derive the scale from the entity's AABB half-extents so the
            // model fills center +/- half exactly: this makes the Scale
            // gizmo (which writes half) and the pick box work like a
            // primitive, in addition to the inspector's Scale slider.
            const glm::vec3 sz = glm::max(lm->size(), glm::vec3(1e-4f));
            const glm::mat4 mm =
                c.composeModel(b.center, b.rotation, (b.half * 2.0f) / sz) *
                glm::translate(glm::mat4(1.0f), -lm->center());
            for (std::size_t i = 0; i < lm->meshes.size(); ++i) {
                const int mi = c.document.materialIndex(lm->primMaterialId[i]);
                c.renderer.submit(lm->meshes[i], scratch.gpuMats[mi], mm, true,
                                isMirror(c.materials[mi]),
                                c.materials[mi].opacity,
                                c.materials[mi].alphaMode == AlphaMode::Blend);
            }
            continue;
        }
        // Modelled geometry replaces the primitive this entity would
        // otherwise draw. Scaled to fill center +/- half exactly, the same
        // way an imported model is: the mesh is kept centred on its own
        // bounds after every edit, so that factor is 1 until someone drags
        // the Scale gizmo -- and then the shape scales with the box, which
        // is what dragging it is asking for.
        if (const auto* meshC = b.components.get<MeshComponent>()) {
            glm::vec3 mn, mx;
            meshC->mesh.bounds(mn, mx);
            const glm::vec3 sz = glm::max(mx - mn, glm::vec3(1e-4f));
            const glm::mat4 mm =
                c.composeModel(b.center, b.rotation, (b.half * 2.0f) / sz);
            const auto* mc = b.components.get<MaterialComponent>();
            const int   mi = c.document.materialIndex(mc ? mc->material : AssetId{});
            // Painted? Then this object needs a material of its own: its
            // four paint slots, bound on units its own maps do not use
            // (0/1/3), plus the flag that tells the shader vPaint means
            // slot weights here. It cannot go on scratch.gpuMats[mi] -- that one
            // is shared by every entity wearing the same material, and
            // the slots are this object's alone.
            const Material* useMat = &scratch.gpuMats[mi];
            if (meshC->mesh.painted()) {
                Material pm = scratch.gpuMats[mi];
                int filled = 0;
                for (int k = 0; k < static_cast<int>(meshC->paintSlots.size());
                     ++k) {
                    const MeshPaintSlot& sl = meshC->paintSlots[k];
                    const MaterialDef*   smd = nullptr;
                    // By GUID, not through materialIndex(): that answers 0
                    // -- a real material -- for anything it does not know,
                    // and an empty slot has to stay empty.
                    if (sl.material.valid())
                        for (const MaterialDef& cand : c.materials)
                            if (cand.assetId == sl.material) { smd = &cand; break; }
                    const std::string ix = std::to_string(k);
                    if (smd && smd->tex) {
                        pm.setTexture("uPaintTex[" + ix + "]", *smd->tex,
                                      8 + static_cast<std::uint32_t>(k))
                          .set("uPaintScale[" + ix + "]", sl.scale)
                          .set("uPaintHas[" + ix + "]", 1);
                        ++filled;
                    } else {
                        pm.set("uPaintHas[" + ix + "]", 0);
                    }
                }
                // Nothing to paint with -> draw it as the plain material
                // rather than as a painted one with every slot switched
                // off, which is the same picture through more work.
                if (filled > 0) {
                    pm.set("uMeshPaint", 1);
                    scratch.paintMats.push_back(std::move(pm));
                    useMat = &scratch.paintMats.back();
                }
            }
            c.renderer.submit(c.meshCache.mesh(b.id, meshC->revision, meshC->mesh),
                            *useMat, mm, true,
                            isMirror(c.materials[mi]),
                            c.materials[mi].opacity,
                            c.materials[mi].alphaMode == AlphaMode::Blend);
            continue;
        }

        const Mesh& mesh = (b.type == EntityType::Ramp)     ? c.ramp
                         : (b.type == EntityType::Cylinder) ? c.cylinder
                         : (b.type == EntityType::Sphere)   ? c.sphere
                                                            : c.box;
        const glm::mat4 m = c.composeModel(b.center, b.rotation, b.half * 2.0f);
        if (b.type == EntityType::Light) {
            // Light markers glow (emissive-ish). A marker sits on its own
            // light position, so it must NOT cast into that light's shadow
            // cube (it would wrap the light in a caster and go dark).
            const auto* lc = b.components.get<LightComponent>();
            const glm::vec3 lcol = lc ? lc->color : glm::vec3(1.0f);
            Material& mat = scratch.lightMats.emplace_back(c.lit);
            mat.set("uColorMode", 0).set("uWaterLevel", -1.0e4f)
               .set("uWetness", 0.0f) // markers glow, never wet
               .set("uAlbedo", lcol * 1.5f).set("uReflectivity", 0.0f);
            c.renderer.submit(mesh, mat, m, /*castsPointShadow=*/false);
        } else {
            // Assigned library material; MIRROR-like solids are excluded
            // from the env probe so they don't reflect their own interior
            // (see isMirror -- glossy surfaces stay in, which is what puts
            // the city in a wet road's reflection).
            const auto* mc = b.components.get<MaterialComponent>();
            const int mi = c.document.materialIndex(mc ? mc->material : AssetId{});
            c.renderer.submit(mesh, scratch.gpuMats[mi], m, true,
                            isMirror(c.materials[mi]),
                            c.materials[mi].opacity,
                            c.materials[mi].alphaMode == AlphaMode::Blend);
        }
    }
}

} // namespace scenesubmit
