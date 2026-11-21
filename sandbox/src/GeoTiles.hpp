#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

// Map data of real places, as the world generator's "Real place" and the local
// geometry cache (fitzelgeo) both see it. Pure: no sockets, no GL, no files --
// tile arithmetic, the projection into the editor's metres, and the one format
// the cache keeps geometry in.
//
// --- Where the data comes from ------------------------------------------------
// OpenStreetMap: the map pictures (tile.openstreetmap.org), the geometry
// (buildings, roads, water, land use, trees -- the Overpass API) and the place
// search (Nominatim). The ground: the Terrarium elevation tiles of the AWS open
// data terrain set. None of it is fetched by the editor itself: fitzelgeo
// fetches, keeps and serves it, so the editor speaks plain HTTP to one local
// address and a second look at a place costs nothing (and nobody's servers).
//
// --- Why tiles -----------------------------------------------------------------
// The geometry is cached per slippy-map tile at zoom kGeoZoom (about 600-800 m
// across in Central Europe), whatever area was asked for: two selections that
// overlap share the tiles they have in common, and the server streams an area as
// the list of its tiles, one request each, so the editor shows progress and can
// stop half way. One Overpass query fetches a whole metatile (kMetaZoom, 2 x 2
// geometry tiles, 1-1.5 km across) -- the API's own advice is few big queries
// rather than many small ones, and a city's metatile still answers in seconds.
//
// --- What a tile holds -------------------------------------------------------------
// Features with the tags the generator reads, their geometry in degrees (as
// integers of 1e-7 degrees -- OSM's own resolution, and a third of the text).
//   * Buildings and points (trees, place names) go to the ONE tile their centre
//     is in: never twice, nothing to merge.
//   * Lines (roads, waterways, railways) go whole to EVERY tile they touch: a road
//     must stay one road, and the reader keeps the first copy by id.
//   * Areas (land use, woods, water) are CLIPPED to each tile: a forest a hundred
//     kilometres across costs a tile only its own square, and the pieces of all
//     tiles rasterise into exactly the original area.
namespace geo {

inline constexpr int kGeoZoom  = 15;   // the cache's geometry tiles
inline constexpr int kMetaZoom = 14;   // one Overpass query each (2 x 2 geometry tiles)
inline constexpr int kDefaultPort = 8742;

struct LonLat { double lon = 0.0, lat = 0.0; };

struct BBox {
    double s = 0.0, w = 0.0, n = 0.0, e = 0.0;   // south, west, north, east (degrees)
    bool contains(LonLat p) const { return p.lat >= s && p.lat <= n && p.lon >= w && p.lon <= e; }
    bool overlaps(const BBox& o) const { return !(o.e < w || o.w > e || o.n < s || o.s > n); }
};

struct TileId {
    int z = 0, x = 0, y = 0;
    bool operator==(const TileId& o) const { return z == o.z && x == o.x && y == o.y; }
    bool operator<(const TileId& o) const {
        return z != o.z ? z < o.z : x != o.x ? x < o.x : y < o.y;
    }
};

// Web Mercator tile coordinates (fractional), and back.
double tileX(double lon, int z);
double tileY(double lat, int z);
double lonOf(double tx, int z);
double latOf(double ty, int z);
BBox   tileBounds(const TileId& t);
// Every tile at zoom z that overlaps `b`.
std::vector<TileId> tilesCovering(const BBox& b, int z);
// The metatile a geometry tile belongs to, and its geometry tiles.
TileId metaOf(const TileId& t);
std::vector<TileId> childrenAt(const TileId& t, int z);
// Metres per degree of longitude / latitude at a latitude (WGS84 ellipsoid).
double metresPerDegLon(double lat);
double metresPerDegLat(double lat);

// The square of `size` metres around a point, in degrees.
BBox squareAround(LonLat c, double size);

// --- The editor's frame ---------------------------------------------------------
// Metres east (x) and SOUTH (z) of an origin: the editor's world has north at -Z
// (as the house generator and the plan map do). A plain equirectangular frame:
// over the few kilometres a scene spans it is out by millimetres.
struct Frame {
    LonLat origin;
    double mx = 1.0, mz = 1.0;   // metres per degree of lon / lat at the origin
    glm::vec2 toLocal(LonLat p) const {
        return {static_cast<float>((p.lon - origin.lon) * mx),
                static_cast<float>(-(p.lat - origin.lat) * mz)};
    }
    LonLat toLonLat(glm::vec2 v) const {
        return {origin.lon + static_cast<double>(v.x) / mx, origin.lat - static_cast<double>(v.y) / mz};
    }
};
Frame frameAt(LonLat origin);

// --- Geometry ---------------------------------------------------------------------

struct Feature {
    enum class Geom : std::uint8_t { Point, Line, Area };
    // OSM type in the top two bits (0 node, 1 way, 2 relation), the id below.
    std::uint64_t id = 0;
    Geom geom = Geom::Point;
    std::vector<std::pair<std::string, std::string>> tags;   // the kept ones only
    // Point: one ring of one point. Line: one ring, the polyline. Area: outer
    // rings and holes (`hole[k]` = ring k is a hole), each closed implicitly.
    std::vector<std::vector<LonLat>> rings;
    std::vector<std::uint8_t>        hole;

    const std::string* tag(const char* k) const;
    std::string tagOr(const char* k, const char* d = "") const;
    bool has(const char* k) const { return tag(k) != nullptr; }
    bool is(const char* k, const char* v) const;
};

std::uint64_t featureId(int osmType, std::uint64_t osmId);
std::string   featureIdText(std::uint64_t id);   // "w123456"

// Is this tag one the generator reads? (What the cache keeps.)
bool keepTag(const std::string& key);

// The Overpass query for everything the generator reads inside `b`.
std::string overpassQuery(const BBox& b, int timeoutSec = 180);

// Overpass JSON (`out geom`) -> features: multipolygon relations' member ways
// joined into rings, ways closed into areas where their tags make them areas.
bool fromOverpass(const std::string& json, std::vector<Feature>& out, std::string* err = nullptr);

// What of `all` belongs in tile `t` (see the header comment: points and
// buildings by their centre, lines whole, areas clipped).
std::vector<Feature> forTile(const std::vector<Feature>& all, const TileId& t);

// The cache's tile file, and back. parseTile appends to `out`.
std::string writeTile(const std::vector<Feature>& f, const TileId& t);
bool parseTile(const std::string& json, std::vector<Feature>& out, std::string* err = nullptr);

// The centre of a ring (area-weighted for a polygon, the vertex mean for a
// degenerate one) and a ring's area in square degrees (signed, CCW positive).
LonLat ringCentre(const std::vector<LonLat>& r);
double ringAreaDeg(const std::vector<LonLat>& r);

// --- Elevation ----------------------------------------------------------------------

// Terrarium: metres = R * 256 + G + B / 256 - 32768.
inline float terrarium(unsigned char r, unsigned char g, unsigned char b) {
    return static_cast<float>(r) * 256.0f + static_cast<float>(g) + static_cast<float>(b) / 256.0f - 32768.0f;
}

// One decoded elevation tile (row-major, x fastest, row 0 at the tile's north).
struct DemTile {
    TileId id;
    int w = 0, h = 0;
    std::vector<float> m;
};

// Height at a point from a set of tiles of one zoom, bilinear across the tile
// borders; false where no tile covers it (and its neighbours fill in at an
// edge).
class DemMosaic {
public:
    explicit DemMosaic(const std::vector<DemTile>& tiles);
    bool  empty() const { return m_zoom < 0; }
    int   zoom() const { return m_zoom; }
    bool  sample(LonLat p, float& out) const;
    // Metres between pixels at a latitude.
    double pixelMetres(double lat) const;

private:
    const float* pixel(long long px, long long py) const;   // global pixel, or null
    int m_zoom = -1;
    int m_x0 = 0, m_y0 = 0, m_nx = 0, m_ny = 0;
    std::vector<const DemTile*> m_grid;   // m_nx * m_ny, null = missing
};

// --- Search -------------------------------------------------------------------------

struct Place {
    std::string name;
    LonLat      at;
    BBox        box;
    std::string kind;   // "city", "village", ...
};
// Nominatim's jsonv2 answer -> places (fitzelgeo hands it on as it is).
bool parseSearch(const std::string& json, std::vector<Place>& out);

// %-encoding for a query string value.
std::string urlEncode(const std::string& s);
std::string urlDecode(const std::string& s);

} // namespace geo
