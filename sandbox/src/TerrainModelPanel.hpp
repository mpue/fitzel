#pragma once

#include <functional>
#include <string>

#include <glm/glm.hpp>

#include "TerrainModel.hpp"

class CommandStack;
class Document;
class Selection;

// The editor's "Terrain model" panel: a square of the scene's terrain (round
// the 3D cursor or where the camera looks) or a heightmap image, as a relief or
// a coloured contour model, drawn as a map while it is set up and placed as an
// entity subtree in front of the camera. The root keeps the parameters, so
// "Edit selected" re-opens a placed model and "Update placed" builds it again
// from the terrain as it is now -- after the world generator ran again, after
// sculpting. File > Export Selection as 3MF prints it, a part a terrace.
//
// The Add menu's "Terrain model" lands here too. Owns its state and its
// actions, so main only constructs it and calls panel().
namespace terrainui {

class TerrainModelTool {
public:
    struct Deps {
        Document&     document;
        CommandStack& history;
        Selection&    sel;
        int&          entityCounter;
        std::function<glm::vec3(float)> spawnPoint;   // where a new object lands; far ahead: where the camera looks
        std::string&  status;                          // the editor's footer line
        std::function<bool()> hasTerrain;              // does the scene have ground
        terrainmodel::Heights ground;                  // its height at a world (x, z)
        std::function<float()> waterLevel;             // the scene's water (-1000 and below: none)
        std::function<bool(glm::vec3&)> cursor;        // the 3D cursor, when it is shown
    };
    explicit TerrainModelTool(Deps d);

    void panel(bool& show);

    // A model with the panel's settings standing at `at` (world). Selected, and
    // the panel edits it.
    void placeAt(const glm::vec3& at, int parentId = -1);
    // A new model's centre: the 3D cursor if shown, else where the camera looks;
    // and the scene's water level (the Add menu, before placing).
    void aimAtView();
    void setParams(const terrainmodel::Params& p);
    void updateWith(const terrainmodel::Params& p);
    const terrainmodel::Params& params() const { return m_cfg; }

private:
    void place();
    void rebuild();
    void editSelected();
    int  selectedId() const;
    void refresh(bool force = false);
    terrainmodel::Heights heights() const;   // the scene's, or null without terrain

    Deps                 m_d;
    terrainmodel::Params m_cfg;
    int  m_liveId  = -1;
    bool m_auto    = true;
    bool m_pending = false;
    bool m_raise   = false;
    char m_image[1024] = {};

    terrainmodel::Params m_shown;
    bool                 m_haveModel = false;
    terrainmodel::Model  m_model;
};

} // namespace terrainui
