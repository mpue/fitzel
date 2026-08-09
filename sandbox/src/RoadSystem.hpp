#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <fitzel/graphics/Material.hpp>
#include <fitzel/graphics/Mesh.hpp>
#include <fitzel/graphics/Texture.hpp>

#include "CityGen.hpp"
#include "RoadBridge.hpp"
#include "RoadSide.hpp"

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
        // Sample index of each control point in the polylines above (plus a
        // closing entry for the last point / the loop seam). Lets the editor
        // colour the stretch between two control points -- which is how a bridge
        // is named -- without knowing how the spline was sampled.
        std::vector<int> ptSample;
    };
    Preview previewGeometry() const;

    // Road surface height at world XZ, if within `halfWidth` of the centreline.
    // Over a bridged (elevated) stretch this is the deck top, not the terrain far
    // below -- so a hover craft can use it as ground and not sink through the
    // bridge. Cheap: projects onto the cached sampled centreline. Returns false
    // when off the road or the road isn't built.
    bool surfaceHeightAt(const glm::vec2& xz, float halfWidth, float& outY) const;

    // Swap the surface texture (by file name; resolved against the scanned
    // texture dirs, see refreshTextures).
    void setSurface(const std::string& file);
    // Swap the emission (glow) map: the image whose lit texels make the road
    // glow -- centre lines, edge strips, Wipeout-style light channels. Pass "" for
    // none, which leaves the whole carriageway glowing flat in `emission` (or not
    // at all while `emissionStrength` is 0). The map is sampled across the road's
    // OWN width (see applyEmission), not with the asphalt's tiling, so a stripe
    // painted down the middle of the image stays down the middle of the road.
    void setEmission(const std::string& file);
    // Push the glow settings (colour, strength, map, UV scale) onto the surface
    // material. The UV scale depends on `width`/`texTile`/`emissionTile`, which
    // the editor changes freely, so the renderer calls this each frame rather
    // than trying to catch every edit.
    void applyEmission();

    // Swap the surface's normal map, which is what gives the asphalt its grain
    // under a low sun. Pass "" for none. The mesh needs no tangents: the lit shader
    // builds a frame from screen-space derivatives (see applyNormalMap).
    void setNormal(const std::string& file);

    // A stretch of road the user has asked to be carried on a bridge, named by the
    // two control points at its ends (indices into roadPts, either order). Points
    // move and vanish under the editor, so these are validated on every build --
    // and main fixes them up when a point is deleted.
    struct BridgeSpec {
        int a, b;
    };

    // Everything an editor edit can change about the road's *shape*: the state
    // an undo step has to put back. Small enough (a few hundred points) to
    // snapshot whole rather than describing deltas, the same trade ModifyEntityCmd
    // makes for entities. Surface/width/tunables are deliberately not in here --
    // they are not reachable through the point handles.
    struct Shape {
        std::vector<glm::vec2>      points;
        std::vector<float>          lifts;
        std::vector<BridgeSpec>     bridges;
        bool                        closed = false;
        std::vector<roadside::Line> sideObjects;
        std::vector<city::Biome>    biomes;
    };
    Shape shape() const { return {roadPts, ptLift, bridges, closed, sideLines, biomes}; }
    void  setShape(const Shape& s) {
        roadPts = s.points;
        ptLift  = s.lifts;
        ptLift.resize(roadPts.size(), 0.0f);
        bridges = s.bridges;
        closed  = s.closed;
        sideLines = s.sideObjects;
        biomes    = s.biomes;
        needsBuild = true;
        rebuildSideObjects(); // reflect a side-object undo/redo immediately
        rebuildCity();        // ...and a biome one
    }

    // --- Control point edits -------------------------------------------------
    // The only operations that change how many control points there are: they
    // keep roadPts and ptLift in lockstep. Bridge indices are the caller's
    // business (main fixes those up, see removeRoadPoint/insertRoadPoint) --
    // these do not touch them.
    void  insertPoint(int at, glm::vec2 p, float lift = 0.0f);
    void  erasePoint(int at);
    void  clearPoints();
    // Height offset of point `i`, 0 if it has none (out of range, or a scene
    // saved before heights existed and only partly filled in).
    float liftOf(int i) const;
    void  setLift(int i, float lift);

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

    // Side objects (guard rails, curbs, posts), derived from `sideLines` and the
    // current centreline -- regenerated by every mesh rebuild, not stored per
    // instance. Grouped by the model each line places, so the caller resolves the
    // model once and draws/collides the batch. main renders these (a model at each
    // transform) and gives each instance a static box collider in Play.
    struct SideBatch {
        std::string           model;      // asset ref to resolve (name/path/GUID)
        float                 scale = 1.0f;// carried from the line (uniform)
        bool                  knockable = false; // dynamic body in Play (flies off)
        float                 mass  = 8.0f;      // kg when knockable
        std::vector<roadside::Instance> instances;
    };
    const std::vector<SideBatch>& sideBatches() const { return m_sideBatches; }
    // Recompute only the side-object instances from the current centreline (cheap;
    // no terrain regrade, no ribbon reloft). A no-op until the road has been built
    // once -- side objects follow a committed centreline. Call after editing a line.
    void rebuildSideObjects();

    // --- Roadside city (see CityGen.hpp) -------------------------------------
    // Same deal as the side objects, one level up: `biomes` are the saved rules,
    // the district is derived from them and the current centreline and is never
    // written to the scene file. Regenerated by every mesh rebuild and by an
    // undo/redo, so the skyline follows the road the way the guard rails do.
    const city::District& district() const { return m_city; }
    // The uploaded meshes, parallel to district().batches -- the caller draws
    // batch i with cityMesh(i) and an IDENTITY model matrix (the geometry is
    // already in world space). The CPU copy in the batch is released after upload,
    // so this is the only place the district's vertices live.
    const fitzel::Mesh& cityMesh(std::size_t i) const { return m_cityMeshes[i]; }
    // Re-derive the district. Needs the shared building materials, which live in
    // the project's material library -- something a road has no business knowing
    // about, so the owner injects `cityPalettes` once at startup and this stays a
    // no-op until it does (the player wires it exactly as the editor does).
    void rebuildCity();
    std::function<std::vector<buildings::Palette>(const std::vector<city::Biome>&)>
        cityPalettes;
    const std::vector<glm::vec3>&     collVerts()   const { return m_collVerts; }
    const std::vector<std::uint32_t>& collIndices() const { return m_collIndices; }
    const std::vector<glm::vec2>&     centerline()  const { return m_centerline; }
    // Road surface height per centreline sample -- the DECK top over a bridged
    // stretch, not the ground far below. Parallel to centerline() (same length
    // once the road is built), so anything already walking the centreline by arc
    // length gets the drivable height for free. Empty before the first build.
    const std::vector<float>&         centerlineY() const { return m_centerlineY; }

    // --- Editor state the viewport handles + panel drive directly ------------
    std::vector<glm::vec2>   roadPts;          // control points (world x,z)
    // Per-point height offset in metres above the graded ground, one entry per
    // control point. 0 (the default, and what an older scene loads as) means
    // "follow the terrain", which is how the road behaved before heights
    // existed. Positive lifts the road onto an embankment, negative cuts it in;
    // either way the corridor grading pulls the ground along, so a raised
    // stretch is a dam rather than a floating ribbon -- use a bridge to span.
    //
    // Same length as roadPts, always. insert/erase/clearPoints below are the
    // only places that resize either, so the two cannot drift apart.
    std::vector<float>       ptLift;
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
    // Glow. `emissionStrength` 0 (the default, and what an older scene loads as)
    // means the road doesn't glow at all, so nothing changes for existing tracks.
    // With a map, only its lit texels glow; without one the whole surface does.
    // Emission is added after lighting, so a glowing road stays bright at night
    // and reads through fog -- the point of the whole feature.
    std::vector<std::string> emisFiles;        // selectable glow maps (display names)
    int                      emisSel = -1;     // -1 = none (glow the whole surface)
    glm::vec3                emission{1.0f};   // glow colour (multiplies the map)
    float                    emissionStrength = 0.0f; // 0 = off; >1 for a hard glow
    float                    emissionTile = 40.0f;    // metres per repeat ALONG the road

    std::vector<BridgeSpec> bridges;
    roadbridge::Params      bridgeStyle; // deck look, shared by all of them

    // Side-object placement rules (see RoadSide.hpp). Saved with the road; the
    // instances they produce are derived, not saved.
    std::vector<roadside::Line> sideLines;

    // Roadside city rules (see CityGen.hpp). Saved with the road; the buildings
    // they produce are derived, not saved.
    std::vector<city::Biome> biomes;
    bool  cityEnabled = true;
    // Hard ceiling on how many buildings a district may contain. A budget rather
    // than a warning: a frontage typo of 0.5 m would otherwise try to fill a
    // kilometre of road with two thousand towers before anyone could stop it.
    int   cityBudget  = 400;
    // Metres beyond which a building is not even submitted. The renderer culls by
    // frustum already; this is the cheap distance pass in front of it, and the one
    // knob that trades skyline depth for frame time.
    float cityRange   = 1200.0f;

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

    // Per-sample height offset from ptLift: ramped between the control points
    // (whose sample indices are `ptSample`, as sampleCenterlineXZ hands them
    // back) and lightly smoothed. Empty when no point has a height set, which
    // is the caller's cue to skip the addition entirely. Shared by layout() and
    // previewGeometry() so the preview shows what Build will produce.
    std::vector<float> liftRamp(const std::vector<int>& ptSample,
                                std::size_t samples) const;

    // Densely sample the Catmull-Rom centreline through roadPts (world XZ).
    // Sample density follows span length, so control point i is *not* at a fixed
    // sample stride: pass `ptSample` to get the sample index of each point (one
    // entry per control point, plus the closing one).
    std::vector<glm::vec2> sampleCenterlineXZ(
        std::vector<int>* ptSample = nullptr) const;
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
    std::vector<std::string> m_emisPaths; // full paths, parallel to emisFiles

    std::shared_ptr<fitzel::Texture> m_tex;     // kept alive while the material binds it
    std::shared_ptr<fitzel::Texture> m_normTex;
    std::shared_ptr<fitzel::Texture> m_emisTex;
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
    std::vector<float>           m_centerlineY; // road surface height per sample (deck over bridges)
    std::vector<SideBatch>       m_sideBatches; // derived side-object instances
    city::District               m_city;        // derived roadside buildings
    std::vector<fitzel::Mesh>    m_cityMeshes;  // one per m_city.batches entry
};
