#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/world/Terrain.hpp>

#include "CityPlan.hpp"   // cityplan::Street, Lot, Zone, Preset (the types; nothing linked)
#include "Ecology.hpp"    // ecology::Mask
#include "GeoTiles.hpp"
#include "WorldGen.hpp"

// A real place, read the way a planner reads a map: what kind of land this is,
// where people live and how densely, what each building is -- and so which of
// the editor's generators stands for it. The world generator's plan() comes here
// for Source::RealPlace; what comes out is an ordinary worldgen::Plan (ground,
// rivers, the map) plus a Real: the towns as town rules with the real streets and
// the real footprints on them, the single trees, and the woods as an ecology mask.
//
// --- Deciding, not copying ------------------------------------------------------
// OpenStreetMap says "building=yes" for most buildings in most places, and nothing
// about their storeys. So every building is read in its CONTEXT before its tags
// are trusted with the last word:
//   * its footprint (area, the oriented rectangle round it: width, depth, ratio),
//   * its neighbourhood (how much of the ground within 60 m is built over, whether
//     it shares a wall with the next -- an old town, a terrace, a suburb, a farm),
//   * the land use it stands in (residential, farmyard, industrial, retail ...),
//   * the settlement it belongs to (a city, a town, a village, a hamlet: by how
//     many buildings stand together),
// and THEN mapped onto what the editor can build: one of HouseGen's types (a
// detached family house, a town villa, a bungalow, a farmhouse, a terraced house,
// a cottage), an apartment or office block (BuildingGen, with its real storeys),
// an industrial hall, or one of the public buildings (CivicGen: church, school,
// town hall, fire station ...). Garages, sheds and carports are left out: the
// generators have nothing that small, and a house in their place is worse than
// a gap.
//
// The region is read the same way -- relief, height above the sea, the coast --
// and with it the trees (the map's leaf types where it has them), the roofs (its
// roof colours) and the sun (the place's own latitude and longitude).
//
// Pure: data in, data out, no GL, no files, no network. geocheck runs it over a
// cached area and checks what it decided.
namespace geoplan {

// --- What was fetched --------------------------------------------------------------

struct AreaData {
    geo::LonLat center;
    float size = 2000.0f;                  // the square's side (m)
    std::vector<geo::Feature> features;    // lines and buildings once; area pieces all
    std::vector<geo::DemTile> dem;         // the square's ground
    std::vector<geo::DemTile> demCoarse;   // the land around it, for the horizon
    int tiles = 0, missing = 0;            // geometry tiles asked for / not had
};

// The tiles a square needs: geometry (zoom 15), its ground (zoom 14) and the
// ground around it (zoom 11).
struct Need {
    std::vector<geo::TileId> osm, dem, coarse;
};
Need tilesFor(geo::LonLat c, float size);
inline constexpr int kDemZoom = 14, kCoarseZoom = 11;
inline constexpr float kCoarseSize = 24000.0f;   // metres of ground around a square

// Keep one copy of each feature: lines come in every tile they cross. Areas are
// kept piece by piece (each piece is one tile's share of them).
void addTile(AreaData& a, std::vector<geo::Feature>&& tile);

// --- What a building is ----------------------------------------------------------------

enum class Use : std::uint8_t {
    House, Farmhouse, Terrace, SemiDetached, Bungalow, Villa, Cottage,
    Apartments, Commercial, Office, Retail, Industrial, Barn,
    Church, School, Kindergarten, Hospital, FireStation, Police, TownHall, Station,
    Supermarket, Hotel, Museum, Library, Theatre, Cinema, ParkingGarage, DepartmentStore,
    Garage, Shed, Other,
    Count
};
const char* useName(Use u);

// The kind of place a cluster of buildings is.
enum class Settlement : std::uint8_t { City, Town, Village, Hamlet, Farmstead, Count };
const char* settlementName(Settlement s);

// What a footprint was built as: which generator, which of its types.
enum class Build : std::uint8_t {
    Skip,          // garage, shed, carport: nothing that small to build
    House,         // HouseGen, `preset`
    Terraced,      // HouseGen's town house, party wall to party wall
    Block,         // BuildingGen slab: apartments, offices, shops
    Tower,         // BuildingGen tower (ten storeys and more)
    Hall,          // CivicGen's industrial hall
    Civic,         // CivicGen, `civic`
    Count
};
const char* buildName(Build b);

struct Building {
    std::uint64_t id = 0;
    std::vector<glm::vec2> poly;      // world XZ, the outer ring
    glm::vec2 centre{0.0f};
    float     area = 0.0f;            // m^2
    // The smallest rectangle round it: centre, unit axis along its longer side,
    // its length along that axis and its width across.
    glm::vec2 boxC{0.0f}, boxU{1.0f, 0.0f};
    float     boxL = 0.0f, boxW = 0.0f;
    int       levels = 0;             // from the map (0 = it does not say)
    float     height = 0.0f;          // from the map (0 = it does not say)
    std::string type, name;           // building=*, name=*
    Use       use = Use::Other;
    float     coverage = 0.0f;        // built share of the ground within 60 m
    bool      attached = false;       // shares a wall with another
    int       settlement = -1;        // index into Real::settlements (-1 = none)
    // The decision.
    Build     build = Build::Skip;
    int       preset = 0;             // housegen::Preset for House
    int       civic  = -1;            // civic::Kind for Civic / Hall
    int       floors = 0;             // what is built (storeys)
    std::string why;                  // the reason, for the panel's tooltip
};

struct Place {
    std::string name;
    glm::vec2   centre{0.0f};
    Settlement  kind = Settlement::Village;
    int         buildings = 0;
    float       radius = 0.0f;        // m, from the centre to the outermost building
    float       density = 0.0f;       // mean coverage of its buildings
    glm::vec2   lo{0.0f}, hi{0.0f};   // its buildings' bounds
};

// A road class, for width and paint. Saved (cityplan::Street::cls): append only.
enum class RoadClass : std::uint8_t {
    Motorway, Trunk, Primary, Secondary, Tertiary, Unclassified, Residential,
    LivingStreet, Pedestrian, Service, Track, Count
};
const char* roadClassName(RoadClass c);

struct Road {
    std::vector<glm::vec2> pts;
    RoadClass   cls = RoadClass::Residential;
    float       width = 6.0f;
    std::string name;
    bool        bridge = false;       // the map says so somewhere along it
    int         town = -1;            // which RealTown lays it
    bool        rural = false;        // outside every settlement
    // Its bridges over a railway (point index pairs): laid as decks, the track
    // passing under them in a cutting. Bridges over water the street finds
    // for itself; over another road it stays at grade (no road dips under).
    std::vector<std::pair<int, int>> decks;
};

// --- Railways ------------------------------------------------------------------------------
// The map draws every track of a line on its own (a double-track line is two
// ways), so each is laid as one: a track on a formation graded into the ground
// -- an embankment where the land falls away, a cutting where it rises, its
// level a smooth line no steeper than the line climbs -- over roads, rivers and
// other lines on bridges, where the map has one and where a road passes under
// it in a short tunnel; under the roads that bridge it, low enough to clear
// them. Tunnels are left out: the track ends at the portal.

// What is laid. Saved nowhere: the apply step turns each into a track preset.
enum class RailClass : std::uint8_t { Main, Light, Narrow, Siding, Disused, Tram, Count };
const char* railClassName(RailClass c);

struct Rail {
    std::vector<glm::vec2> pts;       // world XZ, the track's centre
    std::vector<float>     level;     // the formation at each point (the ground's heights)
    RailClass   cls = RailClass::Main;
    std::string name;
    // Stretches carried on a bridge deck (point index pairs). Each reaches a
    // few metres onto the formation at both ends, so the deck starts on ground
    // graded to its level; the track runs onto it there.
    std::vector<std::pair<int, int>> bridges;
};

// A deck carrying tracks: one where the map has one, or one for two tracks side
// by side where both of a line's tracks cross together.
struct RailBridge {
    std::vector<glm::vec2> pts;
    int   tracks  = 1;
    float spacing = 4.0f;             // between the two tracks' centres
    float width   = 5.6f;             // the deck, edge to edge
    std::string name;
};

// A platform beside the tracks: its middle line, as wide and as high above the
// formation as a German one (0.76 m over the rail head).
struct Platform {
    std::vector<glm::vec2> pts;
    float width = 3.0f;
    std::string name;
};

// A town to be laid: a cityplan rule with the map's streets and footprints (the
// apply step makes it a cityplan::Rule, Grid::pattern 2).
struct Town {
    std::string name;
    Settlement  kind = Settlement::Village;
    glm::vec2   centre{0.0f};
    float       sizeX = 100.0f, sizeZ = 100.0f;
    cityplan::Preset preset = cityplan::Preset::Village;   // its street furniture and look
    std::vector<cityplan::Street> streets;
    std::vector<cityplan::Lot>    lots;
    bool        land = false;         // the land between the places, not a place
};

struct Tree {
    glm::vec2   pos{0.0f};
    std::string species;              // a treegen preset name
    float       scale = 1.0f;
};

struct Summary {
    std::array<int, static_cast<int>(Use::Count)>   uses{};
    std::array<int, static_cast<int>(Build::Count)> builds{};
    std::array<int, 16> housePresets{};          // by housegen::Preset
    std::array<int, static_cast<int>(RoadClass::Count)> roads{};
    std::array<float, static_cast<int>(RoadClass::Count)> roadKm{};
    std::array<float, static_cast<int>(RailClass::Count)> railKm{};
    int   railBridges = 0, railTunnels = 0, platforms = 0;
    float deepestCut = 0.0f, highestBank = 0.0f;   // the formation below / above the land (m)
    int   buildings = 0, places = 0, trees = 0, rivers = 0, lakes = 0;
    float forestShare = 0.0f, fieldShare = 0.0f, builtShare = 0.0f, needleShare = 0.0f;
    float relief = 0.0f, lowest = 0.0f, highest = 0.0f, meanSlope = 0.0f;
    bool  coast = false;
    worldgen::Region region = worldgen::Region::Uplands;
    std::string regionWhy;
    std::vector<std::string> notes;              // what was left out, and why
};

struct Real {
    std::vector<Building> buildings;
    std::vector<Place>    places;
    std::vector<Road>     roads;
    std::vector<Rail>     rails;
    std::vector<RailBridge> railBridges;
    std::vector<Platform> platforms;
    std::vector<Town>     towns;
    std::vector<Tree>     trees;
    std::shared_ptr<const ecology::Mask> eco;
    // What the land itself suggests, for the panel to adopt into the params
    // (once, when an area is loaded -- after that the author's edits stand).
    worldgen::Region    region = worldgen::Region::Uplands;
    worldgen::Character look;
    Summary summary;
    geo::Frame frame;                 // lon/lat <-> world, for the map
    // The water level is a lake's (the biggest one at the bottom of the square)
    // rather than the sea's: its name, and how far the wind blows over it (m)
    // -- what keeps its waves a lake's.
    std::string lakeName;
    float       lakeFetch = 0.0f;
};

// The plan of a real place: everything worldgen::plan() makes for a generated one
// (ground, sea, rivers, the analysis grid, the horizon, the report) from the map,
// and `out.real`. `p.area` must be loaded; without it the report says so and the
// plan is empty. Rocks are left to the caller (worldgen's own crag rule).
void plan(const worldgen::Params& p, worldgen::Plan& out);

} // namespace geoplan
