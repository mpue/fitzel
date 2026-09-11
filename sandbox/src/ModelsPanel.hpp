#pragma once

#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "ModelLibrary.hpp"
#include "SceneTypes.hpp" // MaterialDef

namespace fitzel {
class AssetDatabase;
}

// The editor's "Models" panel: what glTF/GLB (and FBX/DAE) files the project's
// models/ folder holds, and a button that puts one into the scene where new
// objects go (the host's spawnPoint: the 3D cursor, or in view).
//
// A structured file (FBX, DAE) comes in as a HIERARCHY -- one entity per
// mesh-bearing node, under a shared root -- and everything else as a single
// Model entity. Which of the two happens is the host's decision, not this
// panel's; it only says which file and where.
namespace modelsui {

struct PanelState {
    bool& show;

    std::string  modelDir;   // the folder listed
    std::string& modelFile;  // the selected filename, remembered between frames

    ModelLibrary&             models;
    fitzel::AssetDatabase&    assetDb;
    std::vector<MaterialDef>& materials; // an import registers its materials here

    // Where an imported model lands -- the same answer every other "add" gets.
    std::function<glm::vec3()>                                spawnPoint;

    std::function<bool(const std::string&)>                   isStructured;
    std::function<void(const glm::vec3&, const std::string&)> addHierarchy;
    std::function<void(const glm::vec3&, int)>                addEntity;
};

void drawPanel(const PanelState& s);

} // namespace modelsui
