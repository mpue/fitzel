#include "SceneSubmit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include <fitzel/graphics/Material.hpp>
#include <fitzel/graphics/Mesh.hpp>
#include <fitzel/graphics/Shader.hpp>
#include <fitzel/graphics/Texture.hpp>
#include <fitzel/render/Renderer.hpp>

#include "Component.hpp"
#include "Modifiers.hpp"

namespace scenesubmit {

using fitzel::AssetId;
using fitzel::Material;
using fitzel::Mesh;

namespace {
// Does this material have to go through the transparent pass?
//
// Blend says so outright. Glass has to as well, and used not to: a pane at full
// opacity landed in the OPAQUE queue and came out a solid wall, so the only way
// to see through glass was to also turn its opacity down -- two settings for one
// idea, and the one named "Glass" was the one that did nothing. It matters twice
// over now, because the copy of the scene a refraction reads is taken between
// the two passes: a surface drawn in the opaque queue has nothing behind it yet.
bool isBlended(const MaterialDef& md) {
    return md.alphaMode == AlphaMode::Blend || md.glass;
}

// Which channel of an opacity map holds the coverage: a grey image carries it
// in every channel (R is read), but some packs ship a WHITE image with the mask
// in its alpha -- read R there and nothing would ever be transparent.
int coverageChannel(const fitzel::ImagePixels& img) {
    if (img.channels < 4) return 0;
    int rLo = 255, rHi = 0, aLo = 255, aHi = 0;
    const std::size_t texels = img.pixels.size() / 4;
    for (std::size_t i = 0; i < texels; i += 7) {
        const int r = img.pixels[i * 4], a = img.pixels[i * 4 + 3];
        rLo = std::min(rLo, r); rHi = std::max(rHi, r);
        aLo = std::min(aLo, a); aHi = std::max(aHi, a);
    }
    return (rHi - rLo < 8 && aHi - aLo >= 8) ? 3 : 0;
}

// The texture a material's base colour is drawn from, with its opacity map (if
// it has one) standing in for the alpha channel. Both images are read back off
// the GPU rather than off disk: a model's maps have no file, and the ones that
// do are already the right way up in the texture -- the same orientation the
// other map was loaded in, which is what makes them line up texel for texel.
// The coverage is resampled to the base's size, so the two need not match.
// Rebuilt only when either input changes (MaterialDef::opacityFold).
const fitzel::Texture* baseColour(const MaterialDef& md) {
    if (!md.opacityTex || !md.opacityTex->isValid()) return md.tex.get();
    // A playing video rewrites its texture every frame, a camera's picture as
    // well; a folded copy would stop it on the frame it was taken from.
    if (md.videoId.valid() || !md.cameraName.empty()) return md.tex.get();
    MaterialDef::OpacityFold& f = md.opacityFold;
    if (f.merged && f.opacity.lock() == md.opacityTex && f.hadBase == (md.tex != nullptr) &&
        f.base.lock() == md.tex)
        return f.merged.get();

    const fitzel::ImagePixels op = md.opacityTex->readback();
    if (!op.valid()) return md.tex.get();
    fitzel::ImagePixels base;
    if (md.tex) {
        base = md.tex->readback();
        if (!base.valid()) return md.tex.get();
    }
    const int w  = base.valid() ? base.width  : op.width;
    const int h  = base.valid() ? base.height : op.height;
    const int bc = base.valid() ? base.channels : 0;
    const int oc = op.channels;
    const int ch = coverageChannel(op);
    auto cov = [&](int x, int y) {
        x = std::clamp(x, 0, op.width - 1);
        y = std::clamp(y, 0, op.height - 1);
        return static_cast<float>(
            op.pixels[(static_cast<std::size_t>(y) * op.width + x) * oc + std::min(ch, oc - 1)]);
    };
    std::vector<unsigned char> px(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        // Bilinear, texel centre to texel centre.
        const float sy = (static_cast<float>(y) + 0.5f) * op.height / h - 0.5f;
        const int   y0 = static_cast<int>(std::floor(sy));
        const float fy = sy - static_cast<float>(y0);
        for (int x = 0; x < w; ++x) {
            const float sx = (static_cast<float>(x) + 0.5f) * op.width / w - 0.5f;
            const int   x0 = static_cast<int>(std::floor(sx));
            const float fx = sx - static_cast<float>(x0);
            const float a = (cov(x0, y0) * (1.0f - fx) + cov(x0 + 1, y0) * fx) * (1.0f - fy) +
                            (cov(x0, y0 + 1) * (1.0f - fx) + cov(x0 + 1, y0 + 1) * fx) * fy;
            unsigned char* d = &px[(static_cast<std::size_t>(y) * w + x) * 4];
            if (bc > 0) {
                const unsigned char* b = &base.pixels[(static_cast<std::size_t>(y) * w + x) * bc];
                d[0] = b[0];
                d[1] = b[bc > 1 ? 1 : 0];
                d[2] = b[bc > 2 ? 2 : 0];
            } else {
                d[0] = d[1] = d[2] = 255;   // untextured: white, tinted by the albedo
            }
            d[3] = static_cast<unsigned char>(std::clamp(a + 0.5f, 0.0f, 255.0f));
        }
    }
    f.merged  = std::make_shared<fitzel::Texture>(fitzel::Texture::fromPixels(px.data(), w, h, 4));
    f.base    = md.tex;
    f.opacity = md.opacityTex;
    f.hadBase = md.tex != nullptr;
    return f.merged->isValid() ? f.merged.get() : md.tex.get();
}
} // namespace

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
        // Only written for glass. uGlass is reset to 0 by the renderer's
        // baseline on every draw, so a material that never turns it on never
        // reads these -- and the shader's cheapest uniform is the one nobody
        // uploads.
        if (md.glass)
            m.set("uIor", md.ior).set("uGlassThickness", md.thickness);
        // An untextured material with an opacity map is drawn from a white
        // texture carrying the map, tinted by its albedo -- the same colour it
        // had without one.
        if (const fitzel::Texture* base = baseColour(md))
            m.set("uColorMode", 2).setTexture("uTexture", *base, 0)
             .set("uTint", md.tex ? md.tint : md.albedo); // always written (shared program)
        else
            m.set("uColorMode", 0).set("uAlbedo", md.albedo);
        // Which way up the map was stored decides green's sign (lit.frag,
        // applyNormalMap): a model's own maps are top row first, the library's
        // bottom row first. Written with the map every time -- the program is
        // shared, and a sign left over from the last material is someone else's.
        if (md.normalTex)
            m.setTexture("uNormalMap", *md.normalTex, 1).set("uHasNormalMap", 1)
             .set("uNormalTopDown", md.normalTex->bottomUp() ? 0 : 1);
        else
            m.set("uHasNormalMap", 0);
        // Metallic-roughness-occlusion (imported models). Unit 4, which like
        // the emission map's unit 3 is a terrain layer's only on terrain draws.
        if (md.ormTex)
            m.setTexture("uOrmMap", *md.ormTex, 4).set("uHasOrmMap", 1)
             .set("uOrmMean", md.ormMean);
        else
            m.set("uHasOrmMap", 0);
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
    // Per-drawn-piece copies for the modelled meshes somebody has painted: the
    // layer textures and the flag are per OBJECT, while scratch.gpuMats is shared
    // by every entity using that material. A deque holds them, so a mesh dressed
    // in several materials can add one per piece and the references already
    // handed to submit() stay where they are (see Scratch).
    scratch.lightMats.reserve(c.entities.size());
    for (const Entity& b : c.entities) {
        if (!b.activeInHierarchy) continue;         // deactivated: hidden
        if (b.type == EntityType::Sun) continue;   // directional, no geometry
        if (c.vanished && c.vanished(b.id)) continue;   // a pane shot to pieces
        // A decal draws no box: what it shows is its image cut from whatever is
        // in its box (Decals.hpp), already in world space, in its material.
        if (b.components.get<DecalComponent>()) {
            if (const fitzel::Mesh* dm = c.decalMesh ? c.decalMesh(b.id) : nullptr) {
                const auto* mc = b.components.get<MaterialComponent>();
                const int mi = c.document.materialIndex(mc ? mc->material : AssetId{});
                c.renderer.submit(*dm, scratch.gpuMats[mi], glm::mat4(1.0f), /*castsPointShadow=*/false,
                                  /*reflective=*/false, c.materials[mi].opacity, isBlended(c.materials[mi]));
            }
            continue;
        }
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
                // Its glass shot out: this copy draws what is left of it.
                const fitzel::Mesh* mesh = &lm->meshes[i];
                // A second figure of the same model: posed in its own copy.
                if (const fitzel::Mesh* own = c.skinnedOf ? c.skinnedOf(b.id, i) : nullptr)
                    mesh = own;
                if (const fitzel::Mesh* left = c.leftOf ? c.leftOf(b.id, i) : nullptr) {
                    if (left->vertexCount() == 0) continue;
                    mesh = left;
                }
                c.renderer.submit(*mesh, scratch.gpuMats[mi], mm, true,
                                isMirror(c.materials[mi]),
                                c.materials[mi].opacity,
                                isBlended(c.materials[mi]));
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
            // What the object shows: its mesh, or what its modifier stack makes
            // of it -- in the mesh's own space, so the transform is the same.
            const modifiers::Shown shownMesh = modifiers::shown(b, *meshC);
            const glm::mat4 mm = c.composeModel(
                b.center, b.rotation, editmesh::fitScale(*shownMesh.mesh, b.half));
            const auto* mc = b.components.get<MaterialComponent>();
            const int   own = c.document.materialIndex(mc ? mc->material : AssetId{});
            // Painted? Then this object needs a material of its own: its
            // four paint slots, bound on units its own maps do not use
            // (0/1/3), plus the flag that tells the shader vPaint means
            // slot weights here. It cannot go on scratch.gpuMats[mi] -- that one
            // is shared by every entity wearing the same material, and
            // the slots are this object's alone.
            const bool painted = meshC->mesh.painted();
            auto withPaint = [&](int mi) -> const Material* {
                if (!painted) return &scratch.gpuMats[mi];
                Material pm = scratch.gpuMats[mi];
                int filled = 0;
#ifdef __EMSCRIPTEN__
                // The browser reads the four slots from one texture array, bound
                // where the terrain's layer array goes (lit.frag, FITZEL_WEB).
                // Built once per combination of slot textures and kept.
                std::vector<const fitzel::Texture*> slotTex(4, nullptr);
#endif
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
#ifdef __EMSCRIPTEN__
                        if (k < 4) slotTex[static_cast<std::size_t>(k)] = smd->tex.get();
#else
                        pm.setTexture("uPaintTex[" + ix + "]", *smd->tex,
                                      8 + static_cast<std::uint32_t>(k));
#endif
                        pm.set("uPaintScale[" + ix + "]", sl.scale)
                          .set("uPaintHas[" + ix + "]", 1);
                        ++filled;
                    } else {
                        pm.set("uPaintHas[" + ix + "]", 0);
                    }
                }
                // Nothing to paint with -> draw it as the plain material
                // rather than as a painted one with every slot switched
                // off, which is the same picture through more work.
                if (filled == 0) return &scratch.gpuMats[mi];
#ifdef __EMSCRIPTEN__
                {
                    static std::map<std::vector<const fitzel::Texture*>, fitzel::Texture> arrays;
                    auto it = arrays.find(slotTex);
                    if (it == arrays.end()) {
                        if (arrays.size() > 64) arrays.clear();   // stale combinations
                        int size = 16;
                        for (const fitzel::Texture* t : slotTex)
                            if (t) size = std::max({size, t->width(), t->height()});
                        const unsigned char grey[4] = {128, 128, 128, 255};
                        it = arrays.emplace(slotTex, fitzel::Texture::arrayOf(
                                                         slotTex, std::min(size, 1024), grey))
                                 .first;
                    }
                    pm.setTexture("uLayerArr", it->second, 8);
                }
#endif
                pm.set("uMeshPaint", 1);
                scratch.paintMats.push_back(std::move(pm));
                return &scratch.paintMats.back();
            };
            // One draw per material the mesh wears: a face dressed in the
            // Modeling panel is its own piece of geometry, and a mesh nobody has
            // dressed is the one piece it always was (see editmesh::buildGroups).
            for (const EditMeshCache::Sub& sub :
                     c.meshCache.submeshes(b.id, shownMesh.revision, *shownMesh.mesh)) {
                // A face's material by GUID -- not through materialIndex(), which
                // answers 0 for one it does not know. A material deleted from the
                // library since the face was dressed hands the face back to the
                // object's own, which is the state it can actually be drawn in.
                int mi = own;
                if (sub.material.valid())
                    for (std::size_t k = 0; k < c.materials.size(); ++k)
                        if (c.materials[k].assetId == sub.material) {
                            mi = static_cast<int>(k);
                            break;
                        }
                c.renderer.submit(sub.mesh, *withPaint(mi), mm, true,
                                isMirror(c.materials[mi]),
                                c.materials[mi].opacity,
                                isBlended(c.materials[mi]));
            }
            continue;
        }

        const Mesh& mesh = (b.type == EntityType::Ramp)     ? c.ramp
                         : (b.type == EntityType::Cylinder) ? c.cylinder
                         : (b.type == EntityType::Sphere)   ? c.sphere
                         : (b.type == EntityType::Plane)    ? c.plane
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
                            isBlended(c.materials[mi]));
        }
    }
}

} // namespace scenesubmit
