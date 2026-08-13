#pragma once

#include <cmath>
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
#include "RoadLoop.hpp"
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
    //
    // The answer follows the CROSS-FALL: on a banked stretch the carriageway at
    // the query point is `lateral * tan(bank)` off the centreline, and returning
    // the centreline height there would float a craft over the low edge and bury
    // it in the high one.
    // `maxY` is a ceiling that only REJECTS: surfaces above it are skipped, and
    // the NEAREST of what is left wins. That is what makes an underpass work --
    // where a road crosses itself, the flyover's deck is out of reach overhead
    // and the carriageway being driven answers instead.
    //
    // It must not select by height, and that is not a stylistic point. The hover
    // craft that asks this passes its OWN height as the ceiling, so an answer
    // chosen by height feeds back into the next question: a sample two metres
    // uphill slips in and out as the craft bobs, the ground moves by the
    // gradient, and the hover spring chases it. That version shipped once and
    // made gliders judder on every slope.
    bool surfaceHeightAt(const glm::vec2& xz, float halfWidth, float& outY,
                         float maxY = 1.0e9f) const;

    // The raised edge resolved into the two numbers everything downstream wants:
    // how far out it reaches across the section, and how high it climbs. Derived
    // in one place so the mesh, the height query, the bridge decks and the graded
    // corridor cannot disagree about where the road stops.
    float edgeReach() const {
        return edgeWidth * std::cos(glm::radians(glm::clamp(edgeAngle, 0.0f, 90.0f)));
    }
    float edgeRise() const {
        return edgeWidth * std::sin(glm::radians(glm::clamp(edgeAngle, 0.0f, 90.0f)));
    }
    // Half the full drivable section: carriageway plus the lip beside it.
    float surfaceHalf() const { return width * 0.5f + edgeReach(); }

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

    // Swap the wetness map: a greyscale image saying where the water stands
    // (black dry, white fully wet). Any image will do -- it is read as a mask,
    // not as colour -- so it is picked from the same wide list as the glow map.
    // Pass "" for none, which puts the surface back to an even sheen.
    void setWetMap(const std::string& file);
    // Push the wetness map + its tiling onto the surface and bridge materials.
    // Called every frame for the same reason applyEmission is: the tiling is
    // derived from width/texTile/wetTile, all of which the panel edits live.
    void applyWetness();

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
        std::vector<float>          banks;
        std::vector<BridgeSpec>     bridges;
        std::vector<roadloop::Spec> loops;
        bool                        closed = false;
        std::vector<roadside::Line> sideObjects;
        std::vector<city::Biome>    biomes;
    };
    Shape shape() const {
        return {roadPts, ptLift, ptBank, bridges, loops, closed, sideLines, biomes};
    }
    void  setShape(const Shape& s) {
        roadPts = s.points;
        ptLift  = s.lifts;
        ptLift.resize(roadPts.size(), 0.0f);
        ptBank  = s.banks;
        ptBank.resize(roadPts.size(), 0.0f);
        bridges = s.bridges;
        loops   = s.loops;
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
    void  insertPoint(int at, glm::vec2 p, float lift = 0.0f, float bank = 0.0f);
    void  erasePoint(int at);
    void  clearPoints();
    // Height offset of point `i`, 0 if it has none (out of range, or a scene
    // saved before heights existed and only partly filled in).
    float liftOf(int i) const;
    void  setLift(int i, float lift);
    // Cross-fall of point `i` in degrees, 0 if it has none. Positive rolls the
    // road's RIGHT edge down (the way a track banks into a right-hand corner).
    float bankOf(int i) const;
    void  setBank(int i, float deg);

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
    // The loops, as carriageway. Drawn with the road's OWN surface material --
    // a loop is road, not structure -- but as its own mesh, because its geometry
    // cannot live in a ribbon that has one height per ground position.
    const fitzel::Mesh& loopMesh()       const { return m_loopMesh; }
    bool                hasLoops()       const { return m_loopVerts > 0; }
    // The built loops, for the sim: a craft rides these frames rather than the
    // flat centreline while it is on one (see RaceSim).
    const std::vector<roadloop::Loop>& loopGeometry() const { return m_loops; }

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
    // Cross-fall per centreline sample, in degrees, parallel to centerlineY().
    // Anything that stands OFF the centre of a banked road needs it: the
    // carriageway there is `lateral * tan(bank)` below the centreline, and a
    // craft that ignores it floats over the low edge and cuts into the high one.
    const std::vector<float>&         centerlineBank() const { return m_centerlineBank; }

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
    // Per-point cross-fall (banking) in degrees, one entry per control point,
    // same length as roadPts always. 0 (the default, and what an older scene
    // loads as) is a level carriageway, so nothing changes for existing tracks.
    // Positive rolls the RIGHT edge down, which is the way a track banks into a
    // right-hand corner. Ramped between points and lightly smoothed exactly like
    // ptLift, so a bank eases in rather than snapping on at one sample.
    //
    // This tilts the ribbon's CROSS-SECTION; it does not roll the road over. The
    // corridor grading follows the tilt (see build), so a banked stretch cuts its
    // own bed instead of having the terrain poke through its low edge.
    std::vector<float>       ptBank;
    bool                     closed    = false; // loop the spline back to the start
    bool                     enabled   = true;
    float                    width     = 5.0f;
    float                    texTile   = 8.0f; // world metres per texture tile
    float                    fadeWidth = 0.0f; // metres of edge alpha-fade (0 = off)
    // Raised edges: a lip that runs along both sides of the carriageway, so a
    // craft drifting wide is turned back down instead of sailing off the track.
    // Part of the road's own section rather than a side object, because a wall
    // you can drive up is surface, not scenery: it collides, the hover query
    // reads its height, and it banks with the road.
    //
    // `edgeWidth` is the band's width MEASURED ALONG THE SLOPE, so the angle can
    // go all the way to 90 (a vertical wall that high) without the geometry
    // blowing up on the way there. 0 = no lip, which is what every road saved
    // before this loads as.
    float                    edgeWidth = 0.0f;  // metres up the slope (0 = none)
    float                    edgeAngle = 35.0f; // degrees from the carriageway
    float                    rainRings = 1.0f; // drop-impact ring strength (0 = off)
    // The road's OWN wetness, independent of the weather: a permanently slick,
    // reflective carriageway under a clear sky, which is a look a track is
    // authored to have rather than something the rain happens to do to it. It
    // sets a FLOOR under the weather's value (the two take the higher of the
    // pair), so rain can still add to a dry-looking road and can never wash a
    // deliberately wet one back to matte. 0 (what every road saved before this
    // loads as) leaves the surface entirely at the weather's mercy, as before.
    //
    // One number, not two: in the shader wetness darkens the tarmac and sharpens
    // its highlight together (see lit.frag's rainWet), which is what makes it
    // read as wet rather than as varnished, and splitting the pair would mean
    // splitting that.
    float                    wetness   = 0.0f; // 0 = dry (weather only) .. 1 = soaked
    // Where that wetness actually stands. A road is never evenly wet -- water
    // pools where the camber lets it -- and an evenly mirrored carriageway reads
    // as plastic no matter how good the reflection is. `wetMap` names a greyscale
    // image (black dry, white wet), `wetTile` how many metres of road one tile of
    // it covers, and `wetVar` how much of it to believe. wetVar 0 (the default,
    // and what every road saved before this loads as) is the even sheen as before.
    std::string              wetMap;            // display name, "" = none
    float                    wetTile   = 26.0f; // metres per repeat, both ways
    float                    wetVar    = 0.0f;  // 0 = even .. 1 = fully map-driven
    // How much a wet carriageway mirrors its surroundings through the scene
    // probe. Its own knob because it is the expensive half of the look: above 0
    // the frame pays for a cubemap capture whenever the road is wet (rain
    // included), so this is the switch to reach for when the frame rate matters
    // more than the puddles.
    float                    wetReflect = 0.45f; // 0 = sun sheen only .. 1 = mirror
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

    // Vertical loops (see RoadLoop.hpp). Saved as rules naming two control
    // points; the geometry is derived on every build, like a bridge deck.
    std::vector<roadloop::Spec> loops;

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
        std::vector<float>            bank;   // cross-fall per sample (degrees)
        std::vector<roadbridge::Span> spans;  // sample runs carried by a deck
        std::vector<roadloop::Loop>   loops;  // vertical loops on this road
    };
    Layout layout() const;

    // Per-sample height offset from ptLift: ramped between the control points
    // (whose sample indices are `ptSample`, as sampleCenterlineXZ hands them
    // back) and lightly smoothed. Empty when no point has a height set, which
    // is the caller's cue to skip the addition entirely. Shared by layout() and
    // previewGeometry() so the preview shows what Build will produce.
    // Ramp one per-control-point value (heights, cross-fall) out to every sample:
    // linear between the points, then lightly smoothed to round off the kink each
    // point leaves. Returns empty when every value is 0, which is the caller's cue
    // to skip the work entirely. Shared by ptLift and ptBank so the two cannot
    // drift into interpolating differently.
    std::vector<float> pointRamp(const std::vector<float>& perPoint,
                                 const std::vector<int>& ptSample,
                                 std::size_t samples) const;

    // Densely sample the Catmull-Rom centreline through roadPts (world XZ).
    // Sample density follows span length, so control point i is *not* at a fixed
    // sample stride: pass `ptSample` to get the sample index of each point (one
    // entry per control point, plus the closing one).
    std::vector<glm::vec2> sampleCenterlineXZ(
        std::vector<int>* ptSample = nullptr) const;
    // Loft the ribbon mesh + collider + veg centreline from the sampled centre and
    // its per-sample surface heights (already lifted onto the graded profile).
    void loft(const std::vector<glm::vec2>& center, const std::vector<float>& height,
              const std::vector<float>& bank);
    // Build the deck mesh for `layout`'s spans and merge it into the collider.
    // Must run after loft(), which owns (and clears) the collider arrays.
    void buildBridges(const Layout& layout);
    // Loft the loop carriageways and merge them into the collider. Runs after
    // loft() and buildBridges(), which own (and clear) the collider arrays.
    void buildLoops(const Layout& layout);
    // Drop every mesh, collider and centreline (a road of fewer than 2 points).
    void clearGeometry();

    fitzel::AssetDatabase&   m_assetDb;
    fitzel::TerrainStreamer& m_streamer;
    std::string              m_texDir;
    std::vector<std::string> m_texPaths;  // full paths, parallel to texFiles
    std::vector<std::string> m_normPaths; // full paths, parallel to normFiles
    std::vector<std::string> m_emisPaths; // full paths, parallel to emisFiles

    // A display name from any of the scanned lists -> its full path on disk,
    // falling back to the content dir for a name that is no longer there.
    std::string resolveTexPath(const std::string& name) const;

    std::shared_ptr<fitzel::Texture> m_tex;     // kept alive while the material binds it
    std::shared_ptr<fitzel::Texture> m_normTex;
    std::shared_ptr<fitzel::Texture> m_emisTex;
    std::shared_ptr<fitzel::Texture> m_wetTex;
    fitzel::Material         m_mat;
    fitzel::Mesh             m_mesh;
    int                      m_verts = 0;
    std::shared_ptr<fitzel::Texture> m_bridgeTex;
    fitzel::Material         m_bridgeMat;
    fitzel::Mesh             m_bridgeMesh;
    int                      m_bridgeVerts = 0;
    fitzel::Mesh             m_loopMesh;
    int                      m_loopVerts = 0;
    std::vector<roadloop::Loop> m_loops;
    std::vector<glm::vec3>       m_collVerts;   // road + bridge geometry for physics
    std::vector<std::uint32_t>   m_collIndices;
    std::vector<glm::vec2>       m_centerline;  // sampled centre (for veg masking)
    std::vector<float>           m_centerlineY; // road surface height per sample (deck over bridges)
    std::vector<float>           m_centerlineBank; // cross-fall per sample (degrees)
    std::vector<SideBatch>       m_sideBatches; // derived side-object instances
    city::District               m_city;        // derived roadside buildings
    std::vector<fitzel::Mesh>    m_cityMeshes;  // one per m_city.batches entry
};
