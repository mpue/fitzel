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
struct TerrainEditField;
}

// A road is a ribbon mesh lofted along a Catmull-Rom spline through control
// points. Editing is now two-phase: while the user drags control points only a
// *preview* of the smoothed spline is shown (see previewGeometry); nothing is
// committed. Pressing Build embeds the road into the terrain -- it grades a
// smoothed longitudinal profile into the terrain deformation field so the ground
// under the road is flush and gently sloped, then lofts the render mesh + physics
// collider on that graded profile, so the finished road is cleanly drivable. The
// control-point editor (viewport handles + panel) stays in main and drives
// roadPts directly.
class RoadSystem {
public:
    RoadSystem(fitzel::Shader& lit, fitzel::AssetDatabase& assetDb,
               fitzel::TerrainStreamer& streamer, const std::string& texDir);

    // Commit the road: grade a smoothed corridor into `edit` (the terrain
    // deformation field) relative to the *base* terrain -- so re-building the
    // same road is idempotent -- then loft the render mesh + collider on that
    // profile. Fills [outMin,outMax] with the world-space rectangle whose terrain
    // changed (already padded for the chunk rebuild) and returns true when the
    // terrain was modified. Clears everything and returns false for < 2 points.
    bool build(fitzel::TerrainEditField& edit, glm::vec2& outMin, glm::vec2& outMax);

    // Rebuild only the render mesh + collider from the *current* terrain heights,
    // without touching the terrain. Used after a scene load, where the graded
    // corridor is already baked into the restored terrain edits.
    void rebuildMesh();

    // Editor preview: the smoothed spline draped on the current terrain, as three
    // world-space polylines (centre + left/right edges). Empty for < 2 points.
    struct Preview {
        std::vector<glm::vec3> center, left, right;
    };
    Preview previewGeometry() const;

    // Swap the surface texture (by file name under the texture dir).
    void setSurface(const std::string& file);

    // --- Accessors for the renderer / physics / vegetation -------------------
    const fitzel::Mesh& mesh() const { return m_mesh; }
    fitzel::Material&   material()   { return m_mat; }
    int                 verts() const { return m_verts; }
    bool                built() const { return m_verts > 0; }
    const std::vector<glm::vec3>&     collVerts()   const { return m_collVerts; }
    const std::vector<std::uint32_t>& collIndices() const { return m_collIndices; }
    const std::vector<glm::vec2>&     centerline()  const { return m_centerline; }

    // --- Editor state the viewport handles + panel drive directly ------------
    std::vector<glm::vec2>   roadPts;          // control points (world x,z)
    bool                     closed    = false; // loop the spline back to the start
    bool                     enabled   = true;
    float                    width     = 5.0f;
    float                    texTile   = 8.0f; // world metres per texture tile
    float                    grade     = 0.55f; // 0..1 longitudinal smoothing (flatter road)
    float                    shoulder  = 3.0f; // metres of terrain blend beyond the edge
    bool                     needsBuild = false; // roadPts/width/grade changed since Build
    bool                     vegDirty  = false; // road changed -> re-clear plants
    std::vector<std::string> texFiles;         // selectable diffuse textures
    int                      texSel = 0;

private:
    // Densely sample the Catmull-Rom centreline through roadPts (world XZ).
    std::vector<glm::vec2> sampleCenterlineXZ() const;
    // Loft the ribbon mesh + collider + veg centreline from the sampled centre and
    // its per-sample surface heights (already lifted onto the graded profile).
    void loft(const std::vector<glm::vec2>& center, const std::vector<float>& height);

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
