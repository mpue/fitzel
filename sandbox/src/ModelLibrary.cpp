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
    const ModelData& md = *mdPtr;
    auto lm = std::make_unique<LoadedModel>();
    lm->id      = counter_++;
    lm->assetId = assetDb.idForPath(path); // stable GUID for save/load
    lm->path    = path;
    lm->name    = std::filesystem::path(path).stem().string();
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
        if (!p.normalPixels.empty())
            def.normalTex = std::make_shared<Texture>(Texture::fromPixels(
                p.normalPixels.data(), p.normalWidth, p.normalHeight, 4));
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
    if (md.animated()) { lm->animated = true; lm->animData = mdPtr; }
    const int id = lm->id;
    models_.push_back(std::move(lm));
    return id;
}
