#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/graphics/Material.hpp>
#include <fitzel/graphics/Mesh.hpp>
#include <fitzel/graphics/Texture.hpp>

namespace fitzel {
class Shader;
class TerrainStreamer;
class AssetDatabase;
}

// A road is a ribbon mesh lofted along a Catmull-Rom spline through control
// points, draped on the terrain and textured with asphalt. Owns its GPU mesh +
// material, the CPU geometry for the Play-mode physics collider, and the sampled
// centreline the vegetation uses to keep plants off the road. The control-point
// editor (viewport handles + panel) stays in main and drives roadPts directly.
class RoadSystem {
public:
    RoadSystem(fitzel::Shader& lit, fitzel::AssetDatabase& assetDb,
               fitzel::TerrainStreamer& streamer, const std::string& texDir);

    // Rebuild the mesh, collider and centreline from roadPts (drapes on terrain).
    void rebuild();
    void rebuildIfDirty() { if (dirty) rebuild(); }
    bool dirty = false; // set when roadPts/width/tex change; cleared by rebuild()

    // Swap the surface texture (by file name under the texture dir).
    void setSurface(const std::string& file);

    // --- Accessors for the renderer / physics / vegetation -------------------
    const fitzel::Mesh& mesh() const { return m_mesh; }
    fitzel::Material&   material()   { return m_mat; }
    int                 verts() const { return m_verts; }
    const std::vector<glm::vec3>&     collVerts()   const { return m_collVerts; }
    const std::vector<std::uint32_t>& collIndices() const { return m_collIndices; }
    const std::vector<glm::vec2>&     centerline()  const { return m_centerline; }

    // --- Editor state the viewport handles + panel drive directly ------------
    std::vector<glm::vec2>   roadPts;          // control points (world x,z)
    bool                     enabled   = true;
    float                    width     = 5.0f;
    float                    texTile   = 8.0f; // world metres per texture tile
    bool                     vegDirty  = false; // road changed -> re-clear plants
    std::vector<std::string> texFiles;         // selectable diffuse textures
    int                      texSel = 0;

private:
    fitzel::AssetDatabase&   m_assetDb;
    fitzel::TerrainStreamer& m_streamer;
    std::string              m_texDir;

    std::shared_ptr<fitzel::Texture> m_tex; // kept alive while the material binds it
    fitzel::Material         m_mat;
    fitzel::Mesh             m_mesh;
    int                      m_verts = 0;
    std::vector<glm::vec3>       m_collVerts;   // road geometry for physics
    std::vector<std::uint32_t>   m_collIndices;
    std::vector<glm::vec2>       m_centerline;  // sampled centre (for veg masking)
};
