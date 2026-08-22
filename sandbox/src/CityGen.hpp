#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/graphics/Mesh.hpp>   // fitzel::MeshData (CPU-side, no GPU)

#include "BuildingGen.hpp"
#include "SceneTypes.hpp"

// Roadside city biomes: whole street canyons generated along the road.
//
// A biome is a RULE for a stretch of road -- "from 200 m to 900 m, both sides,
// towers 30..60 floors hard against the kerb, neon, skyways" -- not a pile of
// placed objects. The buildings are DERIVED from the biome list plus the road's
// current centreline, exactly the way the graded corridor and the guard rails
// are (see RoadSide.hpp): only the rules are saved, the city itself is rebuilt
// whenever the road is. Move a control point and the whole skyline follows;
// change one number and a kilometre of frontage re-plans itself. That is the only
// way to author a canyon without dragging three hundred towers into place, which
// is what this editor exists to avoid.
//
// The massing is NOT reinvented here: a lot picks a style and a size, and the
// existing tower generator (BuildingGen.hpp) builds it. This module only decides
// WHERE a building stands and WITH WHICH parameters -- parcelling, height
// distribution, style mix, cross streets, and the pieces that belong to the
// street rather than to any one building (skyways, projecting signs).
//
// Pure data -> data, like BuildingGen and RoadSide: no ImGui, no GPU, no scene.
// The caller draws the pieces (main submits them to the renderer) and, in Play,
// gives the flagged ones a static collider. The panel lives in CityPanel.hpp.
namespace city {

// A biome's starting character. Like buildings::Style this only presets the
// fields below -- every one of them stays editable afterwards.
enum class Preset {
    Downtown,    // wide plots, mixed heights, generous setback
    Canyon,      // the money shot: tall and deep, hard against both kerbs
    Industrial,  // low slabs, big footprints, few crowns
    Slums,       // dense, small, low, ragged frontage
    Megablock,   // arcology walls sliced by cross streets
    Outskirts,   // sparse needles standing well back
    Count
};
const char* presetName(Preset p);

// Which side(s) of the road a biome builds on. Same numbering as roadside::Side
// so the two panels read alike.
enum Side { Left = 0, Right = 1, Both = 2 };

constexpr int kStyleCount = static_cast<int>(buildings::Style::Count);

// One stretch of road with one character. Biomes are matched by arc length from
// the road's start, in order: the FIRST enabled biome whose range contains a
// station owns it, so overlapping ranges are resolved by list order rather than
// by silently blending two rulesets into something neither describes.
struct Biome {
    bool        enabled = true;
    std::string name    = "Downtown";

    // --- Where it applies ----------------------------------------------------
    // Metres of arc length from the road's first control point. `to <= from`
    // means "to the end of the road", which is what a single whole-road biome
    // wants and what a freshly added one defaults to.
    float from  = 0.0f;
    float to    = 0.0f;
    // Metres at each end over which the biome fades out: buildings thin out and
    // lose height towards the boundary, so a district ends in a ragged edge
    // instead of a wall that stops mid-block.
    float blend = 60.0f;

    // --- Parcelling ----------------------------------------------------------
    int   side        = Side::Both;
    float setback     = 8.0f;   // metres from the road EDGE to the facade
    float setbackVar  = 3.0f;   // random spread of the above (0 = a dead-straight wall)
    float frontage    = 28.0f;  // average parcel width ALONG the road
    float frontageVar = 10.0f;
    float gap         = 5.0f;   // metres of daylight between neighbours (0 = party wall)
    float depth       = 26.0f;  // building depth AWAY from the road
    float depthVar    = 8.0f;
    float fill        = 0.92f;  // 0..1 chance a parcel is built at all (holes = plots)
    float blockLength = 0.0f;   // metres between cross streets (0 = one endless block)
    float blockGap    = 16.0f;  // width of a cross street

    // --- Ground --------------------------------------------------------------
    // A tower needs somewhere flat-ish to stand. A lot whose footprint corners
    // differ by more than `maxSlope` (metres of drop per metre of run) is
    // skipped rather than left floating or buried, and a lot that would sit more
    // than `maxDrop` below the carriageway is pulled up to it -- that is what
    // keeps a canyon's ground floors addressing the street across a dip.
    float maxSlope = 0.30f;
    float maxDrop  = 10.0f;

    // --- Height distribution -------------------------------------------------
    int   floorsMin    = 10;
    int   floorsMax    = 40;
    float floorHeight  = 3.6f;
    // Metres: the wavelength of the tall/short swell along the road. Heights are
    // smooth noise at this scale plus per-lot break-up, so a skyline has peaks
    // and troughs instead of the salt-and-pepper a pure per-lot random gives.
    float skylineScale = 220.0f;
    float skylineBias  = 0.5f;  // 0 = mostly at floorsMin, 1 = mostly at floorsMax
    float twist        = 0.0f;  // max total yaw over a tower's height (degrees)

    // --- Style mix -----------------------------------------------------------
    // Relative weights over buildings::Style. All-zero falls back to Skyscraper,
    // so a biome can never end up unable to pick anything.
    float styleMix[kStyleCount] = {1, 0, 0, 0, 0, 0};

    // --- Street furniture ----------------------------------------------------
    // Overhead pieces that belong to the canyon rather than to any one building,
    // and are the cheapest way to make a street read as science fiction: a skyway
    // is one deck plus two stubs, a sign is two emissive slabs.
    float skywayEvery  = 0.0f;   // metres between skyways (0 = none)
    float skywayHeight = 40.0f;  // metres above the carriageway
    float skywayWidth  = 6.0f;
    float signChance   = 0.35f;  // 0..1 chance a building carries a projecting sign

    // --- Look ----------------------------------------------------------------
    // Generated buildings share ONE material set per `palette` slot, so a whole
    // city costs four materials per slot rather than four per tower (see
    // buildings::ensurePalette). Give two biomes different slots and they can be
    // recoloured independently; give them the same slot and they stay in step.
    int       palette        = 0;                      // 0..7 -> "Building A".."H"
    glm::vec3 glassTint{0.07f, 0.10f, 0.15f};
    glm::vec3 frameTint{0.14f, 0.15f, 0.18f};
    glm::vec3 accentColor{0.20f, 0.85f, 1.00f};
    glm::vec3 baseTint{0.30f, 0.31f, 0.34f};
    float     accentStrength = 3.0f;
    bool      neon           = true;
    bool      collider       = true;  // solid in Play (drive into it)

    // --- Wear ----------------------------------------------------------------
    // How neglected the district is. `weathering` acts on the shared palette, so
    // it is necessarily district-wide (kills the mirror finish, soils the
    // colours, and roughens the massing). The other three vary BUILDING TO
    // BUILDING, which is what a run-down street actually looks like -- uniform
    // grime just reads as a dark district, whereas a glass tower between two
    // concrete hulks with dead signage reads as one that stopped being
    // maintained.
    float weathering = 0.60f;   // 0 = new curtain wall .. 1 = derelict concrete
    float grime      = 0.35f;   // chance a mass is bare concrete, not glazing
    int   clutter    = 6;       // rooftop tanks/vents/ducts per building
    float clutterVar = 0.6f;    // 0 = every roof equally littered, 1 = wildly uneven
    float deadNeon   = 0.30f;   // fraction of buildings whose signage is unlit
    // Lit windows on the glazing (drawn procedurally by the lit shader, see
    // buildings::Params). A district-wide property, and one of the strongest
    // pieces of storytelling the generator has: the same massing reads as a
    // working downtown at 0.6 and as an evacuated one at 0.05.
    float windowLit  = 0.55f;   // fraction of windows lit (0 = a dark district)
    glm::vec3 windowColor{1.00f, 0.83f, 0.58f};  // interior light colour (sRGB)

    unsigned  seed = 7;  // reroll for a different city, same rules

    // Value equality, so the road's undo snapshot can tell a real edit from a
    // no-op (see RoadShapeCmd::trivial), the same reason roadside::Line has one.
    bool operator==(const Biome& o) const;
    bool operator!=(const Biome& o) const { return !(*this == o); }
};

// The defaults a Preset seeds.
Biome preset(Preset p);

// One drawable part of the finished city. Deliberately flat and material-only:
// derived geometry never becomes an Entity, so it costs no hierarchy row, no
// undo snapshot and no line in the scene file -- the same trade roadside
// instances make, and the only reason a four-hundred-tower canyon is affordable.
// Every part BuildingGen emits is a primitive rotated about +Y alone, so a single
// yaw is the whole transform.
struct Piece {
    glm::vec3       center{0.0f};   // world
    glm::vec3       half{1.0f};
    float           yaw = 0.0f;     // degrees about +Y
    EntityType      type = EntityType::Box;
    fitzel::AssetId material;
    bool            collide = false;
};

// A run of the city merged into ONE mesh: every piece within a stretch of road
// that shares a material, welded into a single buffer of world-space geometry.
//
// This is what makes a district affordable, and it is not a micro-optimisation.
// The renderer replays its queue about a dozen times a frame (four shadow
// cascades, six env-probe faces, the water mirror, the main pass) and re-uploads
// roughly twenty-five uniforms per mesh per pass. Twenty thousand loose pieces is
// therefore millions of GL calls a frame, and no amount of culling detail parts
// fixes that -- the cost is per DRAW, not per triangle. Merged, the same city is
// a few hundred draws, and the triangles it saved were never the problem.
//
// Chunked along the road rather than welded into one giant mesh so that frustum
// and distance culling still have something to bite on.
struct Batch {
    fitzel::AssetId  material;
    glm::vec3        lo{0.0f}, hi{0.0f};   // world AABB (culling)
    // Geometry in WORLD space (so it draws with an identity model matrix).
    // Emptied by the caller once it has been uploaded to the GPU -- keeping a
    // second CPU copy of a district's vertices costs tens of megabytes for
    // nothing.
    fitzel::MeshData data;
};

// The generated city.
struct District {
    // What it takes to find a building again and rebuild it as real entities on
    // demand (see bake()). Deliberately NOT how it is drawn -- the geometry lives
    // in `batches`, merged; this is the metadata that survives that merge.
    struct Building {
        glm::vec3         center{0.0f};         // bounding sphere
        float             radius = 0.0f;
        glm::vec3         ground{0.0f};         // where the building stands
        float             yaw    = 0.0f;        // degrees about +Y
        int               biome  = 0;
        float             station = 0.0f;       // metres along the road
        buildings::Params params;               // what produced it
    };

    std::vector<Batch>    batches;
    // Only the pieces the generator flagged SOLID, kept for Play's static
    // colliders. The rest are geometry and nothing else, and are dropped once
    // they have been merged into `batches`.
    std::vector<Piece>    colliders;
    std::vector<Building> buildings;

    // What the panel reports, so "why is my street empty" has an answer that
    // isn't guesswork: parcels rejected for slope, for overlap on a tight inside
    // curve, for standing in the carriageway, and whether the budget cut the run
    // short.
    int  placed       = 0;
    int  skippedSlope = 0;
    int  skippedTight = 0;
    // Lots dropped because the finished footprint could not be cleared of the
    // carriageway -- a bend tighter than the frontage is wide, or a road that
    // comes back past its own district. Counted apart from skippedTight because
    // it means something different to the user: skippedTight is "the plot folded
    // through itself", this is "the road was in the way".
    int  skippedRoad  = 0;
    bool budgetHit    = false;
    int  parts        = 0;   // pieces that went into the merge (before it)
    int  verts        = 0;   // vertices across every batch

    void clear() { *this = District{}; }
    bool empty() const { return buildings.empty(); }
};

// Find-or-create the shared material set each biome's `palette` slot names, and
// re-apply that biome's colours to it. Returns one palette per biome, parallel to
// `biomes`. The materials persist with the PROJECT (as .fmat files) like any
// other; the district only stores their GUIDs.
std::vector<buildings::Palette> ensurePalettes(std::vector<MaterialDef>& materials,
                                               const std::vector<Biome>& biomes);

// Plan and build the city along `center` (the road's sampled centreline, world
// XZ) with `centerY` its carriageway height per sample (the deck top over a
// bridge, so a canyon crossing a gorge addresses the deck rather than the river).
// `halfWidth` is the road's half-width; `groundAt(x,z)` samples the terrain.
//
// `maxBuildings` is a hard budget: generation stops there and sets budgetHit, so
// a mis-typed frontage of 0.5 m costs a short street instead of the session.
//
// Deterministic: the same rules, road and seed give the same city, so rebuilding
// after a road tweak does not reshuffle a skyline the user has been looking at.
District generate(const std::vector<glm::vec2>& center,
                  const std::vector<float>& centerY, float halfWidth,
                  const std::vector<Biome>& biomes,
                  const std::vector<buildings::Palette>& palettes,
                  const std::function<float(float, float)>& groundAt,
                  int maxBuildings);

// Turn one derived building back into a real, editable entity subtree -- the
// escape hatch from "derived" to "authored". Returns a parent-before-child list
// whose front() is an Empty root, exactly what AddEntitiesCmd and prefab::
// fromSubtree want, so a tower the user wants to open up, redress or save as a
// prefab can be lifted out of the district without the district itself becoming
// three thousand hierarchy rows. Ids are minted from `entityCounter`.
//
// Nothing marks the district as "this one is baked": the derived copy is still
// generated. The caller is expected to be doing this to take a building AWAY
// (bake it, then exclude the stretch with a biome boundary or a hole), or to
// harvest a prefab. Keeping a per-lot suppression list would mean saving derived
// state, which is the property this whole module is built on not having.
std::vector<Entity> bake(const District& d, int buildingIndex,
                         const buildings::Palette& pal, int& entityCounter);

} // namespace city
