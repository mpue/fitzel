#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include "BuildingGen.hpp"
#include "CityGen.hpp"
#include "HouseGen.hpp"
#include "SceneTypes.hpp"

// The town generator: a whole settlement from a handful of numbers -- a street
// grid with its crossings, blocks zoned from a high-rise core out to detached
// houses at the edge, parks left for the trees -- standing on the terrain,
// staying out of the rivers and bridging them where a street has to cross.
//
// --- What it reuses, and why it invents so little ----------------------------
// Everything a town is made of already exists in this editor, one generator per
// thing, and this module only decides WHERE each of them goes:
//   * streets are ordinary roads in the RoadSet (CitySystem lays them), so they
//     are graded into the terrain, meet in junction aprons, carry bridges and
//     collide exactly like a hand-drawn road -- and stay editable afterwards;
//   * towers and apartment blocks are BuildingGen's, lifted and merged the way
//     the roadside city does it (city::flatten / city::merge);
//   * terraced and detached houses are HouseGen's, walls, windows and roofs.
//
// --- Two layers: laid and derived --------------------------------------------
// The STREETS are laid once, on request ("Lay streets" in the panel), because
// laying them grades a corridor into the ground -- a build-sized job, and one
// the author should see happen, not one a slider nudge triggers. The BUILDINGS
// are derived: only the rule is saved, and every lot, tower and house is
// re-planned from it whenever the rule, the roads or the ground change (the
// roadside city's bargain -- a thousand houses cost a few hundred bytes in the
// scene file, no hierarchy rows and no undo snapshot).
//
// The derived layer measures instead of trusting the plan: a lot is dropped if
// its footprint reaches into ANY road (a hand-drawn main road through the town
// included), stands in water, or sits on ground too steep for it. That is what
// lets a town be dropped onto a landscape that already has things in it.
//
// Pure data -> data, like BuildingGen and CityGen: no ImGui, no GPU, no scene.
// CitySystem owns towns and meshes, CityPlanPanel is the editor.
namespace cityplan {

// What a block is built with, from the centre outwards.
enum class Zone {
    Towers,   // high-rise core (BuildingGen)
    Blocks,   // apartment blocks around the block's edge (BuildingGen slabs)
    Rows,     // terraced houses, party wall to party wall (HouseGen)
    Houses,   // detached family houses in gardens (HouseGen)
    Park,     // nothing built: the vegetation keeps it
    Count
};
const char* zoneName(Zone z);

// A town's starting character. Like every preset in this editor it only seeds
// the fields below -- all of them stay editable.
enum class Preset {
    Village,     // few crooked streets, houses and a green
    SmallTown,   // apartment core, terraces, family houses
    City,        // a high-rise centre with the lot around it
    Metropolis,  // big grid, towers far out
    Suburb,      // family houses and nothing else
    Count
};
const char* presetName(Preset p);

// Where a town stands and how its streets run -- everything the street grid is
// a function of. Kept apart from the rest of the rule because it exists twice:
// as the draft the panel edits, and as what the streets were actually laid with.
struct Grid {
    glm::vec2 center{0.0f};         // world XZ
    float     sizeX    = 520.0f;    // metres, along the town's own X
    float     sizeZ    = 440.0f;    // metres, along the town's own Z
    float     rotation = 0.0f;      // degrees about +Y
    float blockX      = 95.0f;      // street spacing along X (metres, centre to centre)
    float blockZ      = 75.0f;      // ...and along Z
    float organic     = 0.15f;      // 0 = chessboard .. 1 = crooked old town
    unsigned seed     = 11;         // the crooks (only with organic > 0)
    float streetWidth = 7.0f;
    float avenueWidth = 12.0f;
    int   avenueEvery = 3;          // every Nth line through the centre is an avenue (0 = none)
    float stub        = 30.0f;      // metres each street runs on past the town edge
    bool  bridges     = true;       // carry a street over water (else break it there)
    float maxBridge   = 180.0f;     // longer crossings are broken instead
    bool operator==(const Grid& o) const;
    bool operator!=(const Grid& o) const { return !(*this == o); }
};

// One town: where it stands, how its streets run and what it builds.
struct Rule {
    // Stable identity. The streets carry it (RoadSystem::cityId), which is how
    // re-laying them finds exactly the roads to replace. Handed out by
    // CitySystem, never reused within a scene.
    int         id      = 0;
    std::string name    = "Town";
    bool        enabled = true;
    unsigned    seed    = 11;       // lots, zones and buildings

    // --- Streets ---------------------------------------------------------------
    // `grid` is the draft the panel edits; `laid` is what the town's roads were
    // laid from ("Lay streets" copies one to the other). The BUILDINGS follow
    // `laid` once there is one -- otherwise moving the town without re-laying
    // would plan houses on streets that are not there, beside streets that are.
    // Before the first lay they follow the draft, as a preview.
    Grid grid;
    Grid laid;
    bool hasLaid = false;
    const Grid& built() const { return hasLaid ? laid : grid; }
    // The draft differs from what is on the ground: the panel's re-lay nag.
    bool streetsStale() const { return !hasLaid || laid != grid; }

    // --- Zoning --------------------------------------------------------------
    // Rings as fractions of the town's half-size (0 = the centre, 1 = the edge,
    // measured on the ellipse the town's extent spans). A block is zoned by its
    // centre, nudged by `zoneNoise` so the rings are not drawn with a compass.
    float towerRing  = 0.20f;       // Towers inside this
    float blockRing  = 0.50f;       // then apartment Blocks
    float rowRing    = 0.70f;       // then terraced Rows; Houses beyond
    float zoneNoise  = 0.12f;
    float parkChance = 0.08f;       // chance any block is left as a park
    bool  centrePark = true;        // the block at the centre is a square

    // --- Buildings -----------------------------------------------------------
    float sidewalk        = 3.0f;   // kerb to the building line (metres)
    int   towerFloorsMin  = 12;
    int   towerFloorsMax  = 45;
    int   blockFloorsMin  = 3;
    int   blockFloorsMax  = 6;
    float houseLot        = 20.0f;  // frontage of a detached house's plot (metres)
    float houseSetback    = 5.0f;   // front garden (metres)
    float maxSlope        = 0.22f;  // drop per metre over a footprint's diagonal
    float fill            = 0.94f;  // chance a lot is built at all (holes = plots)
    int   budget          = 1500;   // hard cap on buildings (a typo costs a street, not the session)

    // --- Look ----------------------------------------------------------------
    // Towers and apartment blocks each take a BuildingGen palette slot ("Building
    // A".."H"); give them slots the roadside city is not using and the two stay
    // independent. Houses take the "City House ..." material set.
    int       towerPalette = 5;
    int       blockPalette = 6;
    glm::vec3 glassTint{0.10f, 0.13f, 0.17f};
    glm::vec3 towerBase{0.20f, 0.20f, 0.21f};
    glm::vec3 blockColor{0.62f, 0.57f, 0.49f};    // plaster
    glm::vec3 blockBase{0.30f, 0.29f, 0.28f};
    glm::vec3 facadeColor{0.93f, 0.91f, 0.86f};   // houses
    glm::vec3 roofColor{0.56f, 0.23f, 0.15f};
    float     windowLit = 0.45f;
    float     weathering = 0.45f;
    bool      collider  = true;

    bool operator==(const Rule& o) const;
    bool operator!=(const Rule& o) const { return !(*this == o); }
};

// The defaults a Preset seeds (identity, place and size are left to the caller).
void applyPreset(Rule& r, Preset p);

void save(nlohmann::json& j, const Rule& r);
void load(const nlohmann::json& j, Rule& r);

// --- The plan ------------------------------------------------------------------

// One street, as the town lays it: a polyline through its grid nodes, run on
// past the edge by `stub`. Straight between nodes -- the road it becomes is a
// spline THROUGH the same nodes, so the two meet at every crossing exactly.
struct Street {
    std::vector<glm::vec2> pts;
    float width  = 7.0f;
    bool  avenue = false;
    bool  alongZ = false;  // runs along the town's Z (a line of constant X)
    int   line   = 0;      // index among the lines of its direction
};

// One block: the four grid nodes around it (CCW seen from above) and the width
// of the street on each side, side k running from corner k to k+1.
struct Block {
    glm::vec2 corner[4];
    float     streetHalf[4] = {3.5f, 3.5f, 3.5f, 3.5f};
    Zone      zone = Zone::Houses;
    float     ring = 0.0f;     // 0 = centre .. 1 = edge (before the noise)
    int       ix = 0, iz = 0;
};

struct Layout {
    int nx = 0, nz = 0;                   // blocks along X and Z
    std::vector<glm::vec2> nodes;         // (nx+1) x (nz+1), row-major in z
    std::vector<Street>    streets;
    std::vector<Block>     blocks;
    glm::vec2 axisX{1.0f, 0.0f}, axisZ{0.0f, 1.0f};   // the town's own axes, world
};

// The grid, its streets and its zoned blocks, for grid `g` (by default the one
// the buildings follow, Rule::built()). Deterministic.
Layout layout(const Rule& r, const Grid& g);
inline Layout layout(const Rule& r) { return layout(r, r.built()); }

// One plot and what goes on it: the footprint is `width` along the street by
// `depth` away from it, centred at `pos`; `front` points from the building to
// the street it addresses.
struct Lot {
    glm::vec2 pos{0.0f};
    glm::vec2 front{0.0f, -1.0f};
    float     width = 10.0f, depth = 10.0f;
    Zone      zone  = Zone::Houses;
    int       block = 0;
    int       kind  = 0;           // Rows/Houses: which house type
    std::uint32_t hash = 0;
};

// A street as it is actually laid: water decides. A crossing short enough is
// bridged -- a control point on each bank and the pair in `bridges`, which is
// exactly RoadSystem::BridgeSpec -- and a longer one (or any, with bridges off)
// breaks the street into two runs that end at the banks. A street that starts or
// ends in the water is cut back to the shore.
struct StreetRun {
    std::vector<glm::vec2>           pts;
    std::vector<std::pair<int, int>> bridges;   // control-point index pairs
    float width  = 7.0f;
    bool  avenue = false;
};
std::vector<StreetRun> streetRuns(const Grid& g, const Street& s,
                                  const std::function<bool(float, float)>& isWater);

// Lots around every block's edge, per its zone -- before the ground, the water
// and the roads have had their say (derive() asks them).
std::vector<Lot> lots(const Rule& r, const Layout& lay);

// The house types the town builds, as HouseGen parameters. `terraced` picks the
// row type (index 0 of the rows), else the detached mix. Colours from the rule.
housegen::Params housePrototype(const Rule& r, Zone z, int kind);
int              houseKinds(Zone z);

// --- Deriving the town ----------------------------------------------------------

// A road as the derive step sees it: its sampled centreline and half-width.
struct RoadLine {
    std::vector<glm::vec2> pts;
    float half = 3.5f;
};

struct Context {
    std::function<float(float, float)> groundAt;   // terrain height
    std::function<bool(float, float)>   isWater;   // river, lake or sea at (x,z)
    std::vector<RoadLine>               roads;     // EVERY road in the scene
};

// What derive() produced, for the panel's "why is my block empty" line.
struct Stats {
    int lots = 0, built = 0;
    int skippedRoad = 0, skippedWater = 0, skippedSlope = 0, skippedEmpty = 0;
    int towers = 0, blocks = 0, rows = 0, houses = 0, parks = 0;
    bool budgetHit = false;
};

// One built thing, for the vegetation's clearings and for picking.
struct Placed {
    glm::vec2 pos{0.0f};
    float     radius = 5.0f;
    Zone      zone = Zone::Houses;
};

struct Town {
    city::District      district;   // merged batches + colliders
    std::vector<Placed> placed;
    Stats               stats;
    void clear() { district.clear(); placed.clear(); stats = Stats{}; }
};

// The palettes a town draws with, find-or-created in the project library and
// re-tinted from the rule (see buildings::ensurePalette, housegen::ensurePalette).
struct Palettes {
    buildings::Palette towers, blocks;
    housegen::Palette  houses;
};
Palettes ensurePalettes(std::vector<MaterialDef>& materials, const Rule& r);

// Plan and build the town. Deterministic: the same rule, ground and roads give
// the same town.
Town derive(const Rule& r, const Palettes& pal, const Context& ctx);

} // namespace cityplan
