#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <fitzel/graphics/Material.hpp>
#include <fitzel/graphics/Mesh.hpp>
#include <fitzel/graphics/Texture.hpp>

#include "RoadBridge.hpp"

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
//
// A gap the road should fly over rather than be banked across is marked by the
// user: `bridges` names pairs of control points to carry on a deck, and Build
// leaves the terrain between them alone (see RoadBridge.hpp).
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

    // Swap the surface texture (by file name; resolved against the scanned
    // texture dirs, see refreshTextures).
    void setSurface(const std::string& file);
    // Swap the surface's normal map, which is what gives the asphalt its grain
    // under a low sun. Pass "" for none. The mesh needs no tangents: the lit shader
    // builds a frame from screen-space derivatives (see applyNormalMap).
    void setNormal(const std::string& file);

    // --- Scene persistence ---------------------------------------------------
    // The road's whole scene state: control points, build params, surface and
    // bridges. Not the meshes or the graded corridor -- those are re-derived on
    // load (the corridor rides along in the scene's terrain edits), exactly as
    // Build derives them, so there is one code path that decides what a road looks
    // like rather than two that can disagree.
    //
    // Runtime, not editor: the player loads scenes too.
    void save(nlohmann::json& j) const;
    void load(const nlohmann::json& j);

    // Rebuild the selectable surface + normal-map lists from the built-in content
    // texture dir plus the open project folder (`projectDir`, scanned recursively;
    // pass "" for none). Call it whenever the project changes so project-local road
    // textures appear in the pickers. Both selections are preserved by file name.
    void refreshTextures(const std::string& projectDir);

    // The normal map that goes with `file` by naming convention -- the same stem
    // with the colour token swapped for a normal one (asphalt_02_diff_4k.jpg ->
    // asphalt_02_nor_gl_4k.png). "" when the pack ships none. Used to follow the
    // surface picker, so choosing asphalt brings asphalt's grain with it instead of
    // making you find the matching file yourself.
    std::string normalFor(const std::string& file) const;

    // --- Accessors for the renderer / physics / vegetation -------------------
    const fitzel::Mesh& mesh() const { return m_mesh; }
    fitzel::Material&   material()   { return m_mat; }
    int                 verts() const { return m_verts; }
    bool                built() const { return m_verts > 0; }
    // The bridge decks, as one concrete mesh (drawn separately from the asphalt
    // ribbon). Their collision is already merged into collVerts/collIndices.
    const fitzel::Mesh& bridgeMesh()     const { return m_bridgeMesh; }
    fitzel::Material&   bridgeMaterial()       { return m_bridgeMat; }
    bool                hasBridges()     const { return m_bridgeVerts > 0; }
    const std::vector<glm::vec3>&     collVerts()   const { return m_collVerts; }
    const std::vector<std::uint32_t>& collIndices() const { return m_collIndices; }
    const std::vector<glm::vec2>&     centerline()  const { return m_centerline; }

    // --- Editor state the viewport handles + panel drive directly ------------
    std::vector<glm::vec2>   roadPts;          // control points (world x,z)
    bool                     closed    = false; // loop the spline back to the start
    bool                     enabled   = true;
    float                    width     = 5.0f;
    float                    texTile   = 8.0f; // world metres per texture tile
    float                    fadeWidth = 0.0f; // metres of edge alpha-fade (0 = off)
    float                    rainRings = 1.0f; // drop-impact ring strength (0 = off)
    float                    grade     = 0.55f; // 0..1 longitudinal smoothing (flatter road)
    float                    shoulder  = 3.0f; // metres of terrain blend beyond the edge
    bool                     needsBuild = false; // roadPts/width/grade changed since Build
    bool                     vegDirty  = false; // road changed -> re-clear plants
    std::vector<std::string> texFiles;         // selectable diffuse textures (display names)
    int                      texSel = 0;
    std::vector<std::string> normFiles;        // selectable normal maps (display names)
    int                      normSel = -1;     // -1 = none (flat, lit by geometry alone)

    // A stretch of road the user has asked to be carried on a bridge, named by the
    // two control points at its ends (indices into roadPts, either order). Points
    // move and vanish under the editor, so these are validated on every build --
    // and main fixes them up when a point is deleted.
    struct BridgeSpec {
        int a, b;
    };
    std::vector<BridgeSpec> bridges;
    roadbridge::Params      bridgeStyle; // deck look, shared by all of them

private:
    // The sampled centreline plus everything derived from the *base* (procedural)
    // terrain under it. Measuring against the base rather than the current ground
    // is what makes a rebuild idempotent: the corridor a previous Build graded in
    // is ignored, so the road resolves to the same profile -- and the same bridge
    // spans -- however many times it is built.
    struct Layout {
        std::vector<glm::vec2>        center;
        std::vector<float>            prof;   // smoothed road surface height
        std::vector<float>            ground; // bare terrain under each sample
        std::vector<float>            gradeW; // 1 = grade ground to road, 0 = bridged
        std::vector<roadbridge::Span> spans;  // sample runs carried by a deck
    };
    Layout layout() const;

    // Densely sample the Catmull-Rom centreline through roadPts (world XZ).
    std::vector<glm::vec2> sampleCenterlineXZ() const;
    // Loft the ribbon mesh + collider + veg centreline from the sampled centre and
    // its per-sample surface heights (already lifted onto the graded profile).
    void loft(const std::vector<glm::vec2>& center, const std::vector<float>& height);
    // Build the deck mesh for `layout`'s spans and merge it into the collider.
    // Must run after loft(), which owns (and clears) the collider arrays.
    void buildBridges(const Layout& layout);
    // Drop every mesh, collider and centreline (a road of fewer than 2 points).
    void clearGeometry();

    fitzel::AssetDatabase&   m_assetDb;
    fitzel::TerrainStreamer& m_streamer;
    std::string              m_texDir;
    std::vector<std::string> m_texPaths;  // full paths, parallel to texFiles
    std::vector<std::string> m_normPaths; // full paths, parallel to normFiles

    std::shared_ptr<fitzel::Texture> m_tex;     // kept alive while the material binds it
    std::shared_ptr<fitzel::Texture> m_normTex;
    fitzel::Material         m_mat;
    fitzel::Mesh             m_mesh;
    int                      m_verts = 0;
    std::shared_ptr<fitzel::Texture> m_bridgeTex;
    fitzel::Material         m_bridgeMat;
    fitzel::Mesh             m_bridgeMesh;
    int                      m_bridgeVerts = 0;
    std::vector<glm::vec3>       m_collVerts;   // road + bridge geometry for physics
    std::vector<std::uint32_t>   m_collIndices;
    std::vector<glm::vec2>       m_centerline;  // sampled centre (for veg masking)
};
