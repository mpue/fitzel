#include "ModelLibrary.hpp"

#include <cstdio>
#include <filesystem>

#include <glm/glm.hpp>

#include <fitzel/Fitzel.hpp>
#include <fitzel/asset/AssetDatabase.hpp>

using namespace fitzel;

LoadedModel* ModelLibrary::byId(int id) {
    for (auto& lm : models_) if (lm->id == id) return lm.get();
    return nullptr;
}

int ModelLibrary::import(const std::string& path, AssetDatabase& assetDb,
                         std::vector<MaterialDef>& materials) {
    for (auto& lm : models_) if (lm->path == path) return lm->id;
    // Resolve through the asset database: assigns the file a GUID and caches the
    // decoded CPU mesh so re-imports (e.g. across scene loads) reuse the parse
    // instead of re-reading the glTF from disk.
    std::shared_ptr<ModelData> mdPtr = assetDb.loadModelData(path);
    if (!mdPtr || mdPtr->empty()) {
        std::fprintf(stderr, "Model import failed: %s\n", path.c_str());
        return -1;
    }
    return buildFromData(std::filesystem::path(path).stem().string(), path,
                         assetDb.idForPath(path), *mdPtr,
                         mdPtr->animated() ? mdPtr : nullptr, assetDb, materials);
}

const std::vector<ModelNode>& ModelLibrary::nodes(const std::string& path, bool flipV) {
    const std::string key = path + (flipV ? "#f" : "#n");
    auto it = nodeCache_.find(key);
    if (it == nodeCache_.end())
        it = nodeCache_.emplace(key, loadModelNodes(path, flipV)).first;
    return it->second;
}

int ModelLibrary::importNode(const std::string& path, int nodeIndex, bool flipV,
                             AssetDatabase& assetDb, std::vector<MaterialDef>& materials) {
    const std::string key = path + "#" + std::to_string(nodeIndex) + (flipV ? "f" : "n");
    for (auto& lm : models_) if (lm->path == key) return lm->id;
    const std::vector<ModelNode>& ns = nodes(path, flipV);
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(ns.size())) return -1;
    return buildFromData(ns[nodeIndex].name, key, assetDb.idForPath(path),
                         ns[nodeIndex].data, nullptr, assetDb, materials);
}

int ModelLibrary::buildFromData(const std::string& name, const std::string& path,
                                AssetId assetId, const ModelData& md,
                                std::shared_ptr<ModelData> keepAnim,
                                AssetDatabase& assetDb,
                                std::vector<MaterialDef>& materials) {
    (void)assetDb;
    auto lm = std::make_unique<LoadedModel>();
    lm->id      = counter_++;
    lm->assetId = assetId;
    lm->path    = path;
    lm->name    = name;
    lm->meshes.reserve(md.primitives.size());
    lm->primMaterialId.reserve(md.primitives.size());
    glm::vec3 lo(1e30f), hi(-1e30f);
    int primIdx = 0;
    for (const ModelPrimitive& p : md.primitives) {
        std::vector<Vertex> verts;
        verts.reserve(p.vertexCount());
        for (std::size_t i = 0; i + 7 < p.vertices.size(); i += 8) {
            const glm::vec3 pos(p.vertices[i], p.vertices[i + 1], p.vertices[i + 2]);
            lo = glm::min(lo, pos); hi = glm::max(hi, pos);
            lm->hullPoints.push_back(pos); // for the physics convex hull
            verts.push_back({pos,
                {p.vertices[i + 3], p.vertices[i + 4], p.vertices[i + 5]},
                {p.vertices[i + 6], p.vertices[i + 7]}});
        }
        lm->meshes.push_back(Mesh::create(verts));
        // Register this glTF material as a library material so it shows up
        // (and is editable) in the Materials panel.
        MaterialDef def;
        def.assetId   = AssetId::generate(); // ephemeral (recreated on import)
        def.fromModel = true;
        def.name      = lm->name + ":" + (p.materialName.empty()
                            ? std::to_string(primIdx) : p.materialName);
        def.albedo    = glm::vec3(p.baseColor[0], p.baseColor[1], p.baseColor[2]);
        def.opacity   = p.baseColor[3]; // glTF/Collada base-colour alpha
        if (!p.texPixels.empty())
            def.tex = std::make_shared<Texture>(Texture::fromPixels(
                p.texPixels.data(), p.texWidth, p.texHeight, 4));
        // Import a MASK/BLEND material whose colour map carries an alpha channel
        // as Cutout so its "transparency map" reads through by default (switch to
        // Blend in the Materials panel for soft/glassy translucency).
        if (p.alphaCutout && !p.texPixels.empty())
            def.alphaMode = AlphaMode::Cutout;
        if (!p.normalPixels.empty())
            def.normalTex = std::make_shared<Texture>(Texture::fromPixels(
                p.normalPixels.data(), p.normalWidth, p.normalHeight, 4));
        // Emission (_Illum) map: glow white through the map's lit texels. Strength
        // > 1 pushes the lit texels past the bloom threshold so the glow blooms
        // (spills into the surroundings) instead of only self-illuminating.
        if (!p.emissionPixels.empty()) {
            def.emissionTex = std::make_shared<Texture>(Texture::fromPixels(
                p.emissionPixels.data(), p.emissionWidth, p.emissionHeight, 4));
            def.emission = glm::vec3(1.0f);
            def.emissionStrength = 3.0f;
        }
        const AssetId matId = def.assetId;
        materials.push_back(std::move(def));
        lm->primMaterialId.push_back(matId);
        ++primIdx;
    }
    if (md.primitives.empty()) { lo = hi = glm::vec3(0.0f); }
    lm->boundsMin = lo; lm->boundsMax = hi;
    // Cap the physics hull cloud so convex-hull build stays cheap on big models;
    // keep the AABB corners so it still spans the full extent.
    if (lm->hullPoints.size() > 2048) {
        const std::size_t stride = lm->hullPoints.size() / 2048 + 1;
        std::vector<glm::vec3> reduced;
        reduced.reserve(2048 + 2);
        for (std::size_t i = 0; i < lm->hullPoints.size(); i += stride)
            reduced.push_back(lm->hullPoints[i]);
        reduced.push_back(lo); reduced.push_back(hi);
        lm->hullPoints = std::move(reduced);
    }
    // Keep the CPU model data for skinned characters so the editor can animate
    // them (CPU skinning re-uploads the meshes each frame).
    if (keepAnim && keepAnim->animated()) { lm->animated = true; lm->animData = keepAnim; }
    const int id = lm->id;
    models_.push_back(std::move(lm));
    return id;
}
