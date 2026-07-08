#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <fitzel/world/Model.hpp> // ModelNode

#include "SceneTypes.hpp"

namespace fitzel { class AssetDatabase; }

// Registry of imported glTF/GLB models uploaded to the GPU. Owns the LoadedModel
// list (behind unique_ptr so Materials/consumers can hold stable pointers) and
// mints the stable ids Model entities reference. Extracted from main(): the
// editor keeps one instance and threads it in where models are placed/drawn.
class ModelLibrary {
public:
    // Import a file into the registry and return its id (-1 on failure). Reuses
    // an already-loaded model with the same path. Each primitive's material is
    // registered into `materials` so it shows up (and is editable) in the
    // Materials panel; `assetDb` resolves the file's GUID and caches its mesh.
    int import(const std::string& path, fitzel::AssetDatabase& assetDb,
               std::vector<MaterialDef>& materials);

    // Structure-preserving import (FBX, DAE, ...): the model's mesh-bearing nodes
    // (cached per path+flipV). The caller makes one entity per node, positioned at
    // node.center, referencing importNode(path, i, flipV). `flipV` mirrors the V
    // texture coordinate (see loadModelNodes) -- part of the cache key so both
    // conventions can coexist for A/B comparison.
    const std::vector<fitzel::ModelNode>& nodes(const std::string& path, bool flipV = true);

    // Load a single node of a structured model into the registry and return its
    // id (-1 on failure). Reuses an already-loaded (path, nodeIndex, flipV).
    int importNode(const std::string& path, int nodeIndex, bool flipV,
                   fitzel::AssetDatabase& assetDb, std::vector<MaterialDef>& materials);

    // Look up a loaded model by id (nullptr if unknown).
    LoadedModel* byId(int id);

    void        clear() { models_.clear(); nodeCache_.clear(); }
    std::size_t size() const { return models_.size(); }

private:
    // Build a LoadedModel from CPU data, registering its materials. Returns id.
    int buildFromData(const std::string& name, const std::string& path,
                      fitzel::AssetId assetId, const fitzel::ModelData& md,
                      std::shared_ptr<fitzel::ModelData> keepAnim,
                      fitzel::AssetDatabase& assetDb, std::vector<MaterialDef>& materials);

    std::vector<std::unique_ptr<LoadedModel>> models_;
    std::unordered_map<std::string, std::vector<fitzel::ModelNode>> nodeCache_;
    int counter_ = 0;
};
