#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <fitzel/world/Terrain.hpp>   // TerrainSettings -- a plain struct, no GL

#include "CityPlan.hpp"               // cityplan::Preset
#include "HouseGen.hpp"               // housegen::Preset
#include "RiverGen.hpp"               // rivergen::Preset
#include "SunPosition.hpp"            // sunpos::Place

// The world generator: one seed and one region, and every other generator in the
// editor asked for its part of the SAME place.
//
// --- Why one more generator ---------------------------------------------------
// Each generator here is good at its own thing and blind to the others. The
// terrain does not know where the river wants to run, the town generator puts a
// town wherever the panel's centre is -- on a slope, in the river -- and the roads
// go where they are drawn. Run them one after the other by hand and the result is
// a pile of correct parts: a village on a ridge with its streets on stilts, a
// river that climbs, a road that ends in a field. What makes a landscape read as
// one place is that every part was decided KNOWING the others, and the order that
// knowledge flows in is fixed by the land itself:
//
//   1. the ground                      (everything stands on it)
//   2. where water drains              (the valleys the ground already has)
//   3. where people settle             (flat, dry, near the water, not in it)
//   4. how they get from one to another(the cheapest way over that ground,
//                                       a bridge wherever it meets the water)
//   5. what grows and what is bare     (around all of that, never on it)
//   6. what lies about                 (rock where it is steep or high)
//
// This module is that order, and it does NOT re-implement a single one of the
// generators it orchestrates. It decides WHERE and WITH WHICH PRESET -- a town's
// centre, size and grid rotation, a river's control points, a road's control
// points and bridge spans -- and the apply step in main hands those to the
// generators that already exist, in the order above.
//
// --- One region, not eight panels ---------------------------------------------
// Coherence is mostly a matter of every part agreeing on what kind of place this
// is: an alpine valley has a mountain stream, a village of chalets, steep roads
// with hairpins, conifers up to a tree line and scree above it; a coastal plain
// has a wide river into the sea, a small town at its mouth, brick houses, and
// almost no bare rock. A Region is that agreement written down ONCE (see
// Character): every stage reads its part of the same row, so no stage can pick a
// look the others contradict. Every number in it stays editable.
//
// --- Pure, and why that is not tidiness ---------------------------------------
// Same bargain as LevelGen.hpp: this takes a seed, parameters and a function that
// answers how high the BASE ground is, and returns a DESCRIPTION. No GL, no
// Document, no RoadSystem. A harness (worldcheck) can therefore ask the questions
// that matter and no screenshot answers -- does every river run downhill to an
// outlet, does a town stand on ground that can be levelled, is every town reached
// by a road, does any road climb steeper than its limit, does a road cross a river
// anywhere but on a bridge -- over hundreds of seeds, with no window open.
//
// --- Deterministic ------------------------------------------------------------
// The same Params and the same sampler give the same Plan. Nothing here reads a
// global, a clock or a random_device.
namespace worldgen {

// The kind of place. Saved as an index: append only.
enum class Region {
    Uplands,     // Mittelgebirge: rolling wooded hills, a river in the valley
    Alpine,      // an alpine valley under real mountains
    Lowland,     // flat farmland, a wide meandering river
    Coast,       // a coastal plain running into the sea
    Fjord,       // steep land falling into sea inlets
    Canyon,      // dry plateaus cut by a canyon
    Island,      // a bounded island in open sea
    Count
};
[[nodiscard]] const char* regionName(Region r);
[[nodiscard]] const char* regionBlurb(Region r);

// Everything a region decides, in one row. The panel shows it as "Character" and
// every field is editable; picking a region only seeds it (like every preset in
// this editor).
struct Character {
    // --- Land: the terrain generator's own knobs -----------------------------
    // At the scale of a landscape, not of a scene. The terrain panel's presets
    // are tuned for a few hundred metres -- hills eighty metres apart and twenty
    // high -- and over three kilometres that is a washboard: no valley long
    // enough to drain a river, no field flat enough for a village. These put
    // hills half a kilometre apart and the big swings several kilometres apart.
    float heightScale = 70.0f, ridgeScale = 60.0f, continentAmp = 1.6f;
    float biomeFreq = 0.00035f, terrace = 0.0f, warpStrength = 120.0f;
    float warpFrequency = 0.0012f;
    float frequency = 0.0016f, valleyDepth = 25.0f, peakSharpness = 0.9f;
    float reliefGain = 1.0f;
    int   octaves = 6;
    // The mountain backdrop around the planned area (Terrain.hpp): 0 = none.
    float backdrop = 0.0f;
    // A bounded landmass: islandRadius as a FRACTION of the planned area's half
    // size, so an island always fits the map it is planned on (0 = none).
    float island = 0.0f;

    // --- Water -----------------------------------------------------------------
    // The sea. Its level is set relative to the land that came out, as the
    // fraction of the planned area that is to be under water -- an absolute
    // height means nothing across seeds whose land sits a hundred metres apart.
    float seaShare = 0.0f;        // 0 = no sea
    int   rivers   = 1;           // main rivers, at most; fewer if the land drains fewer
    int   brooks   = 2;           // tributaries
    rivergen::Preset river = rivergen::Preset::River;
    rivergen::Preset brook = rivergen::Preset::Brook;

    // --- Settlement --------------------------------------------------------------
    int   towns    = 1;           // the main town(s)
    int   villages = 2;
    cityplan::Preset town    = cityplan::Preset::SmallTown;
    cityplan::Preset village = cityplan::Preset::Village;
    int   farms    = 6;           // lone farmsteads along the country roads
    housegen::Preset farmHouse = housegen::Preset::CountryHouse;
    // How much a site may rise across its own radius before it is not a site.
    // Alpine villages stand on slopes the lowland would never build on.
    float siteSlope = 0.08f;      // mean gradient over the site, rise over run
    // How big a village is against its preset: a fjord or an alpine valley has
    // hamlets on the few level shelves it offers, not villages of 300 m.
    float villageScale = 1.0f;

    // --- Roads -------------------------------------------------------------------
    float roadWidth = 7.0f;
    float maxGrade  = 0.08f;      // rise over run a country road should keep to
    float extraRoads = 0.3f;      // 0 = a tree (every place reached once) .. 1 = loops
    int   exits     = 2;          // roads that run on off the edge of the map

    // --- Planting and stone ---------------------------------------------------------
    float forest    = 0.6f;       // 0 = open land .. 1 = dense forest
    // What the forest is made of, and how much of each: the tree generator's
    // ready-made species (treegen::presets() by name), grown afresh into the
    // project's trees/ folder -- or, with `file` set, a tree someone made in the
    // Tree generator and saved there, planted as it is. A row whose file has
    // gone falls back to its preset; one with neither is skipped.
    struct Tree {
        std::string preset;
        float       share = 1.0f;
        std::string file;          // a tree in trees/, by file name ("" = grow the preset)
        bool operator==(const Tree&) const = default;
    };
    std::vector<Tree> trees{{"Beech", 0.5f}, {"Spruce", 0.3f}, {"Oak", 0.2f}};
    float treeLine  = 0.0f;       // metres above the lowest ground; 0 = none
    float rocks     = 0.3f;       // 0 = none .. 1 = a lot of bare rock
    float rockSlope = 0.55f;      // gradient above which ground counts as crag

    // --- What it is built of --------------------------------------------------------
    // One stone and one roof for the whole region: the rock lying about is the
    // rock the houses would be built from, and a slate country has slate roofs.
    glm::vec3 facade{0.92f, 0.90f, 0.85f};
    glm::vec3 roof{0.56f, 0.23f, 0.15f};
    int       rockType = 0;       // proc::rock::rockLabels(): Granite, Sandstone, Limestone, Basalt, Slate

    // --- Where on Earth ------------------------------------------------------------
    // A place the region could be, for the sun: an alpine valley at Innsbruck's
    // latitude stands in the sun from mid-morning in June, and its mountains
    // shade it at the hours and in the season they would there.
    sunpos::Place place;
};

[[nodiscard]] Character character(Region r);

// The stages, each one switchable. A stage that is off keeps what the scene
// already has -- turning "Terrain" off plans towns and roads onto the ground as it
// stands, which is how one re-plans the roads of a landscape someone sculpted.
struct Stages {
    bool terrain  = true;
    bool rivers   = true;
    bool towns    = true;
    bool roads    = true;
    bool farms    = true;
    bool forest   = true;
    bool rocks    = true;
};

struct Params {
    unsigned  seed   = 1;
    Region    region = Region::Uplands;
    Character look   = character(Region::Uplands);
    Stages    stages;
    // The square that is planned, in world XZ. The terrain goes on forever
    // outside it; rivers and roads run off its edge as if the world did.
    glm::vec2 center{0.0f};
    float     size = 3000.0f;     // metres per side
    // The clock time the world is shown at (the place's own, summer time
    // included). Generate sets the scene's place, date and time to these.
    float     hour = 11.0f;
};

// --- What comes out --------------------------------------------------------------

// The analysis grid, kept because the panel draws the plan on it and the harness
// measures against it. Row-major, x fastest; cell (i, j) is centred on
// origin + (i, j) * cell.
struct Grid {
    int       n = 0;
    float     cell = 16.0f;
    glm::vec2 origin{0.0f};
    std::vector<float>         height;  // base ground (m)
    std::vector<float>         slope;   // gradient magnitude, rise over run
    std::vector<float>         flow;    // drained area, m^2
    std::vector<std::uint8_t>  use;     // use::Kind, below

    [[nodiscard]] glm::vec2 at(int i, int j) const {
        return origin + glm::vec2(float(i), float(j)) * cell;
    }
    [[nodiscard]] int index(int i, int j) const { return j * n + i; }
    // Bilinear height at a world point (clamped to the grid).
    [[nodiscard]] float heightAt(glm::vec2 p) const;
};

// What a grid cell has been given to, in the order the stages claim it: a
// later stage never takes a cell an earlier one holds (lower value wins).
namespace use {
enum Kind : std::uint8_t { Free = 0, Sea, River, Town, Road, Farm, Crag };
}

// The deepest a generated river may cut into the ground to keep falling (the
// river style's maxCut; the presets say 8). A planned course needs at most about
// half of it, measured on the plan's grid -- the rest is for what the grid does
// not see.
inline constexpr float kMaxCut = 16.0f;

struct River {
    std::vector<glm::vec2> points;   // control points, SOURCE first
    float  maxCut = kMaxCut;         // what the apply step sets on the style
    rivergen::Preset       preset = rivergen::Preset::River;
    float  length = 0.0f;            // m, in plan
    float  drop   = 0.0f;            // m of ground fall, source to mouth
    bool   brook  = false;
    int    joins  = -1;              // the river a brook runs into (-1 = an outlet)
};

struct Town {
    std::string      name;
    cityplan::Preset preset = cityplan::Preset::Village;
    glm::vec2        center{0.0f};
    float            sizeX = 300.0f, sizeZ = 260.0f;  // the street grid's extent
    float            rotation = 0.0f;                 // degrees about +Y
    float            ground   = 0.0f;  // the level the site is graded to (m)
    float            relief   = 0.0f;  // max - min of the base ground over the site
    bool             village  = false;
    unsigned         seed     = 11;  // lots, houses and the crooks of its streets
    // Where roads from outside meet the town: the middle of a block on each of
    // its four edge streets, in world XZ, in the town's own +X, -X, +Z, -Z order,
    // and the way out of town from each. A country road ends ON the edge street,
    // arriving square to it -- which RoadJunction reads as a T. Ending it on a
    // street's stub instead would be end-to-end, which is neither a T nor an X,
    // and the two ribbons would simply overlap.
    std::vector<glm::vec2> gates;
    std::vector<glm::vec2> gateDir;
    [[nodiscard]] float radius() const { return 0.5f * std::max(sizeX, sizeZ); }
};

// A stretch named by the two control points at its ends, as RoadSystem's
// BridgeSpec does.
struct Span { int a = 0, b = 0; };

struct Road {
    std::string            name;
    std::vector<glm::vec2> points;   // control points, world XZ
    std::vector<Span>      bridges;  // where it crosses a river
    float  width     = 7.0f;
    float  length    = 0.0f;
    float  maxGrade  = 0.0f;         // steepest stretch on the GROUND it was routed over
    int    from = -1, to = -1;       // town indices; -1 = off the edge of the map
};

struct Farm {
    glm::vec2 pos{0.0f};
    float     yaw = 0.0f;            // degrees; the gable faces the road
    float     ground = 0.0f;
    housegen::Preset preset = housegen::Preset::CountryHouse;
    unsigned  seed = 1;
};

struct Rock {
    glm::vec3 pos{0.0f};
    float     size = 2.0f;           // metres across
    float     yaw  = 0.0f;
    unsigned  seed = 1;
};

struct Report {
    int   rivers = 0, brooks = 0, towns = 0, villages = 0, roads = 0, bridges = 0;
    int   farms = 0, rocks = 0;
    float roadKm = 0.0f, riverKm = 0.0f;
    float seaLevel = 0.0f;           // absolute, as set; meaningless without sea
    float lowest = 0.0f, highest = 0.0f;
    float steepestRoad = 0.0f;       // over all roads, on the routed ground
    int   unreached = 0;             // towns no road reaches
    int   dropped   = 0;             // ...left out of the plan for it
    bool  ok = true;
    std::string why;                 // the first thing that did not come out as asked
};

// The ground around the plan at a coarser step, reaching well past its edge: the
// shade on the map has to include what casts it from outside -- the mountains
// round an alpine valley stand beyond the planned square.
struct Horizon {
    int       n = 0;
    float     cell = 50.0f;
    glm::vec2 origin{0.0f};
    float     top = 0.0f;              // the highest sample
    std::vector<float> height;
    [[nodiscard]] float heightAt(glm::vec2 p) const;
    [[nodiscard]] bool  covers(glm::vec2 p) const;
};

struct Plan {
    fitzel::TerrainSettings terrain;
    bool   sea = false;
    float  seaLevel = 0.0f;
    float  treeLine = 0.0f;          // absolute height (m); 0 = none
    Grid   grid;
    Horizon horizon;
    std::vector<River> rivers;
    std::vector<Town>  towns;
    std::vector<Road>  roads;
    std::vector<Farm>  farms;
    std::vector<Rock>  rocks;
    Report report;
};

// The landscape these params ask for (seed, region and size folded in).
//
// Separate from plan() for the same chicken-and-egg as levelgen::terrainFor: the
// plan is made AGAINST a landscape, so the caller needs the settings, and a
// sampler for them, first. With stages.terrain off this returns `current`
// untouched -- the plan is made on the ground the scene already has.
[[nodiscard]] fitzel::TerrainSettings terrainFor(const Params& p,
                                                 const fitzel::TerrainSettings& current);

// `groundAt(settings, x, z)` must answer the BASE height for exactly the settings
// passed (no sculpt layered on): every generator that grades or carves measures
// against the base so that a rebuild is idempotent, and a plan measured against
// edited ground would move a little every time it was regenerated.
using GroundFn = std::function<float(const fitzel::TerrainSettings&, float, float)>;

//
// `currentWater` is the scene's sea level, used as it is when the terrain stage
// is off (<= -500 = no sea, as everywhere in the editor).
[[nodiscard]] Plan plan(const Params& p, const fitzel::TerrainSettings& current,
                        float currentWater, const GroundFn& groundAt);

// The steepest gradient along a polyline over the plan's ground, measured over
// `window` metres (a bump between two neighbouring samples is not a slope).
// Shared by the planner, the report and the harness so all three mean the same.
[[nodiscard]] float steepest(const Grid& g, const std::vector<glm::vec2>& line,
                             float window = 30.0f);
// ...and of a road: over a bridge span the deck runs on a straight chord between
// its two ends, so the valley under it is not part of the road's profile.
// `where`, if given, is told where that stretch begins.
[[nodiscard]] float steepest(const Grid& g, const Road& road, float window = 30.0f,
                             glm::vec2* where = nullptr);

// German-sounding place names, one per seed and index. Shared with the harness.
[[nodiscard]] std::string placeName(unsigned seed, int index);

// The plan's ground as a picture, `px` pixels per grid cell, RGBA, row 0 at the
// grid's -Z edge: hill-shaded, the sea blue, towns tinted, bare crag grey. The
// lines on top (rivers, roads, places) are the caller's to draw at its own
// resolution -- the panel draws them crisp over a scaled image.
[[nodiscard]] std::vector<unsigned char> image(const Plan& plan, int px, int& width,
                                               const std::vector<std::uint8_t>* shade = nullptr);

// Which cells of the plan's grid lie in the shade of the land with the sun
// towards `sunDir` (1 = shade, 0 = sun; all shade with the sun down): the ground
// marched towards the sun until it rises above the line of sight -- on the
// plan's grid nearby, on the horizon's past it.
[[nodiscard]] std::vector<std::uint8_t> shade(const Plan& plan, glm::vec3 sunDir);

// The recipe, for the scene file: what made this world, so the panel can show it
// and a later Generate can start from it. Missing keys keep their defaults.
void save(nlohmann::json& j, const Params& p);
void load(const nlohmann::json& j, Params& p);

} // namespace worldgen
