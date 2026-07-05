#pragma once

#include <memory>
#include <string>
#include <vector>

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

    // Look up a loaded model by id (nullptr if unknown).
    LoadedModel* byId(int id);

    void        clear() { models_.clear(); }
    std::size_t size() const { return models_.size(); }

private:
    std::vector<std::unique_ptr<LoadedModel>> models_;
    int counter_ = 0;
};
