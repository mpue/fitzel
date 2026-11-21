// geocheck: the world generator's real places, without a window.
//
//   geocheck                         the parts that need no network: tile maths,
//                                    the cache's tile format, multipolygon rings,
//                                    clipping, the elevation mosaic, the measured
//                                    ground's seams, and the planner's decisions
//                                    on a made-up village and a made-up railway
//   geocheck --live LAT LON SIZE     a real square through fitzelgeo (started if
//                                    not running): streamed, planned, summarised;
//                                    a second load must come from the cache
//     [--port P] [--map out.ppm]     ...and the plan drawn
//     [--rails out.ppm]              ...and its railways on the graded ground
//   GEOCHECK_RAILDEBUG=1             lists the railways planned (offline), the
//                                    tracks round the worst spot (--live)
//
// Exit 0 when every check holds.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#include <fitzel/world/Terrain.hpp>

#include "CivicGen.hpp"
#include "GeoClient.hpp"
#include "GeoPlan.hpp"
#include "GeoTiles.hpp"
#include "HouseGen.hpp"
#include "WorldGen.hpp"

namespace {

int g_fail = 0;
void check(bool ok, const char* what) {
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// --- A made-up village, as Overpass would answer it ------------------------------------

struct Fake {
    std::vector<geo::Feature> fs;
    geo::Frame fr = geo::frameAt({9.5, 51.3});
    std::uint64_t next = 1000;
    std::vector<geo::LonLat> ring(std::vector<glm::vec2> pts) {
        std::vector<geo::LonLat> r;
        for (glm::vec2 p : pts) r.push_back(fr.toLonLat(p));
        return r;
    }
    void building(glm::vec2 c, float w, float d, std::vector<std::pair<std::string, std::string>> tags) {
        geo::Feature f;
        f.id = geo::featureId(1, next++);
        f.geom = geo::Feature::Geom::Area;
        f.tags = std::move(tags);
        f.rings.push_back(ring({c + glm::vec2(-w, -d) * 0.5f, c + glm::vec2(w, -d) * 0.5f,
                                c + glm::vec2(w, d) * 0.5f, c + glm::vec2(-w, d) * 0.5f}));
        f.hole.push_back(0);
        fs.push_back(std::move(f));
    }
    void road(std::vector<glm::vec2> pts, std::vector<std::pair<std::string, std::string>> tags) {
        geo::Feature f;
        f.id = geo::featureId(1, next++);
        f.geom = geo::Feature::Geom::Line;
        f.tags = std::move(tags);
        f.rings.push_back(ring(std::move(pts)));
        f.hole.push_back(0);
        fs.push_back(std::move(f));
    }
    void area(std::vector<glm::vec2> pts, std::vector<std::pair<std::string, std::string>> tags) {
        geo::Feature f;
        f.id = geo::featureId(1, next++);
        f.geom = geo::Feature::Geom::Area;
        f.tags = std::move(tags);
        f.rings.push_back(ring(std::move(pts)));
        f.hole.push_back(0);
        fs.push_back(std::move(f));
    }
};

void railChecks(const worldgen::Plan& pl, const char* out);

const geoplan::Building* nearest(const geoplan::Real& R, glm::vec2 p) {
    const geoplan::Building* b = nullptr;
    for (const geoplan::Building& x : R.buildings)
        if (!b || glm::length(x.centre - p) < glm::length(b->centre - p)) b = &x;
    return b;
}

int offline() {
    std::printf("tiles\n");
    {
        const geo::LonLat k{9.4979, 51.3157};
        const double tx = geo::tileX(k.lon, 15), ty = geo::tileY(k.lat, 15);
        check(std::fabs(geo::lonOf(tx, 15) - k.lon) < 1e-9 && std::fabs(geo::latOf(ty, 15) - k.lat) < 1e-9,
              "lon/lat -> tile -> lon/lat round trip");
        const geo::TileId t{15, static_cast<int>(tx), static_cast<int>(ty)};
        check(geo::tileBounds(t).contains(k), "the tile holds the point");
        const geo::TileId m = geo::metaOf(t);
        bool inMeta = false;
        for (const geo::TileId& c : geo::childrenAt(m, 15)) inMeta |= c == t;
        check(inMeta && m.z == geo::kMetaZoom, "a tile is one of its metatile's children");
        const geo::BBox sq = geo::squareAround(k, 2000.0);
        const double w = (sq.e - sq.w) * geo::metresPerDegLon(k.lat), h = (sq.n - sq.s) * geo::metresPerDegLat(k.lat);
        check(std::fabs(w - 2000.0) < 1.0 && std::fabs(h - 2000.0) < 1.0, "a 2 km square is 2 km square");
        const geo::Frame f = geo::frameAt(k);
        const glm::vec2 p = f.toLocal({k.lon + 0.01, k.lat + 0.01});
        check(p.x > 600.0f && p.x < 800.0f && p.y < -1000.0f && p.y > -1200.0f, "north is -Z, east is +X");
    }
    std::printf("format\n");
    {
        const std::string over = R"({"elements":[
          {"type":"node","id":7,"lat":51.3,"lon":9.5,"tags":{"natural":"tree","genus":"Tilia"}},
          {"type":"way","id":8,"tags":{"building":"house","building:levels":"2","fixme":"x"},
           "geometry":[{"lat":51.30,"lon":9.50},{"lat":51.30,"lon":9.5002},{"lat":51.3002,"lon":9.5002},{"lat":51.3002,"lon":9.50},{"lat":51.30,"lon":9.50}]},
          {"type":"way","id":9,"tags":{"highway":"residential","name":"Lindenweg"},
           "geometry":[{"lat":51.299,"lon":9.499},{"lat":51.301,"lon":9.501}]},
          {"type":"relation","id":10,"tags":{"type":"multipolygon","landuse":"forest","leaf_type":"needleleaved"},
           "members":[
             {"type":"way","role":"outer","geometry":[{"lat":51.31,"lon":9.51},{"lat":51.31,"lon":9.52}]},
             {"type":"way","role":"outer","geometry":[{"lat":51.32,"lon":9.52},{"lat":51.31,"lon":9.52}]},
             {"type":"way","role":"outer","geometry":[{"lat":51.32,"lon":9.52},{"lat":51.32,"lon":9.51},{"lat":51.31,"lon":9.51}]}]}
        ]})";
        std::vector<geo::Feature> fs;
        check(geo::fromOverpass(over, fs) && fs.size() == 4, "Overpass answer read");
        bool tagsKept = false, fixmeGone = true, ring = false;
        for (const geo::Feature& f : fs) {
            if (f.is("building", "house")) {
                tagsKept = f.is("building:levels", "2");
                fixmeGone = !f.has("fixme");
                check(f.geom == geo::Feature::Geom::Area && f.rings[0].size() == 4, "a closed building way is an area");
            }
            if (f.is("landuse", "forest")) ring = f.rings.size() == 1 && f.rings[0].size() == 4;
        }
        check(tagsKept && fixmeGone, "the tags the generator reads, and no others");
        check(ring, "three member ways joined into one ring");
        const geo::TileId t{15, static_cast<int>(geo::tileX(9.5001, 15)), static_cast<int>(geo::tileY(51.3001, 15))};
        const std::string text = geo::writeTile(geo::forTile(fs, t), t);
        std::vector<geo::Feature> back;
        check(geo::parseTile(text, back) && !back.empty(), "tile written and read back");
        bool same = false;
        for (const geo::Feature& f : back)
            if (f.is("building", "house"))
                same = std::fabs(f.rings[0][1].lon - 9.5002) < 1e-7 && f.id == geo::featureId(1, 8);
        check(same, "...with its coordinates to 1e-7 degrees and its id");
        // A forest over four tiles, clipped: the pieces add up to the whole.
        std::vector<geo::Feature> big(1);
        big[0].id = 1; big[0].geom = geo::Feature::Geom::Area; big[0].tags = {{"landuse", "forest"}};
        const geo::TileId c{15, 17000, 11000};
        const geo::BBox b0 = geo::tileBounds(c), b1 = geo::tileBounds({15, 17001, 11001});
        const double mx = 0.5 * (b0.w + b0.e), my = 0.5 * (b0.s + b0.n), Mx = 0.5 * (b1.w + b1.e), My = 0.5 * (b1.s + b1.n);
        big[0].rings.push_back({{mx, my}, {Mx, my}, {Mx, My}, {mx, My}});
        big[0].hole.push_back(0);
        double whole = std::fabs(geo::ringAreaDeg(big[0].rings[0])), parts = 0.0;
        for (const geo::TileId& q : {c, geo::TileId{15, 17001, 11000}, geo::TileId{15, 17000, 11001}, geo::TileId{15, 17001, 11001}})
            for (const geo::Feature& f : geo::forTile(big, q)) parts += std::fabs(geo::ringAreaDeg(f.rings[0]));
        check(std::fabs(parts - whole) < whole * 1e-6, "an area clipped to four tiles adds up to itself");
    }
    std::printf("elevation\n");
    {
        std::vector<geo::DemTile> tiles;
        for (int dx = 0; dx < 2; ++dx) {
            geo::DemTile t;
            t.id = {14, 8600 + dx, 5400};
            t.w = t.h = 256;
            t.m.resize(256 * 256);
            for (int y = 0; y < 256; ++y)
                for (int x = 0; x < 256; ++x) t.m[static_cast<std::size_t>(y) * 256 + x] = static_cast<float>(dx * 256 + x);
            tiles.push_back(std::move(t));
        }
        const geo::DemMosaic m(tiles);
        float a = 0, b = 0;
        const double lat = geo::latOf(5400.5, 14);
        const bool okA = m.sample({geo::lonOf(8601.0 - 0.5 / 256.0, 14), lat}, a);
        const bool okB = m.sample({geo::lonOf(8601.0 + 0.5 / 256.0, 14), lat}, b);
        check(okA && okB && std::fabs(a - 255.0f) < 0.01f && std::fabs(b - 256.0f) < 0.01f, "sampled across a tile border");
        check(std::fabs(geo::terrarium(128, 0, 0) - 0.0f) < 1e-3f, "Terrarium 128/0/0 is sea level");
    }
    std::printf("measured ground\n");
    {
        auto hf = std::make_shared<fitzel::TerrainHeightfield>();
        hf->id = "geocheck";
        auto fill = [](fitzel::TerrainHeightfield::Level& L, float side, float cell, float base) {
            L.cell = cell;
            L.nx = L.nz = static_cast<int>(side / cell) + 1;
            L.x0 = L.z0 = -0.5f * side;
            L.h.assign(static_cast<std::size_t>(L.nx) * L.nz, base);
            for (int j = 0; j < L.nz; ++j)
                for (int i = 0; i < L.nx; ++i)
                    L.h[static_cast<std::size_t>(j) * L.nx + i] = base + 0.02f * (L.x0 + cell * i);
        };
        fill(hf->fine, 2000.0f, 4.0f, 100.0f);
        fill(hf->coarse, 20000.0f, 40.0f, 100.0f);
        fitzel::TerrainSettings ts;
        ts.heightfield = hf;
        float worst = 0.0f;
        for (float x = -12000.0f; x < 12000.0f; x += 1.0f) {
            const float a = fitzel::terrainShapeHeight(ts, x, 37.0f), b = fitzel::terrainShapeHeight(ts, x + 1.0f, 37.0f);
            worst = std::max(worst, std::fabs(b - a));
        }
        check(worst < 1.5f, "no step anywhere from the square out past the coarse grid");
        check(std::fabs(fitzel::terrainShapeHeight(ts, 300.0f, 0.0f) - 106.0f) < 0.01f, "inside the square: the measurement exactly");
        fitzel::registerHeightfield(hf);
        check(fitzel::findHeightfield("geocheck") == hf, "found again by its id");
    }
    std::printf("deciding\n");
    {
        Fake v;
        // A village street with farmhouses, a church on it, a terrace in the
        // town over the road, a garage, an industrial hall, a block of flats.
        v.road({{-600, 0}, {0, 2}, {600, 0}}, {{"highway", "secondary"}, {"name", "Hauptstraße"}});
        v.road({{0, 2}, {5, -300}}, {{"highway", "residential"}, {"name", "Kirchweg"}});
        v.road({{-200, 2}, {-200, 200}}, {{"highway", "residential"}, {"name", "Am Hang"}});
        v.road({{-200, 200}, {-200, 380}}, {{"highway", "residential"}, {"name", "Am Hang"}});
        for (int i = 0; i < 40; ++i) v.building({-560.0f + i * 28.0f, 18.0f}, 11.0f, 9.0f, {{"building", "yes"}});
        for (int i = 0; i < 20; ++i) v.building({-560.0f + i * 28.0f, -20.0f}, 12.0f, 10.0f, {{"building", "house"}});
        v.building({10, -40}, 14, 30, {{"building", "church"}, {"amenity", "place_of_worship"}});
        // Houses close along the Kirchweg: the lane is as narrow as they leave it.
        for (int i = 0; i < 10; ++i) v.building({7.0f, -75.0f - i * 22.0f}, 8.0f, 10.0f, {{"building", "house"}});
        v.building({-560, 40}, 3, 6, {{"building", "garage"}});
        v.building({300, 120}, 60, 30, {{"building", "industrial"}});
        v.building({150, 70}, 30, 14, {{"building", "apartments"}, {"building:levels", "5"}});
        // A row: twelve houses wall to wall.
        for (int i = 0; i < 12; ++i) v.building({-186.0f, 30.0f + i * 7.0f}, 10.0f, 7.0f, {{"building", "house"}});
        v.area({{-700, -700}, {700, -700}, {700, -400}, {-700, -400}}, {{"landuse", "forest"}, {"leaf_type", "needleleaved"}});
        v.area({{-700, 400}, {700, 400}, {700, 700}, {-700, 700}}, {{"landuse", "farmland"}});
        geo::Feature tree;
        tree.id = geo::featureId(0, 5);
        tree.geom = geo::Feature::Geom::Point;
        tree.tags = {{"natural", "tree"}, {"genus", "Quercus"}};
        tree.rings.push_back(v.ring({{50, 300}}));
        tree.hole.push_back(0);
        v.fs.push_back(tree);

        auto area = std::make_shared<geoplan::AreaData>();
        area->center = v.fr.origin;
        area->size = 1500.0f;
        area->features = v.fs;
        geo::DemTile flat;
        const double tx = geo::tileX(v.fr.origin.lon, geoplan::kDemZoom), ty = geo::tileY(v.fr.origin.lat, geoplan::kDemZoom);
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                flat.id = {geoplan::kDemZoom, static_cast<int>(tx) + dx, static_cast<int>(ty) + dy};
                flat.w = flat.h = 256;
                flat.m.assign(256 * 256, 210.0f);
                area->dem.push_back(flat);
            }
        worldgen::Params p;
        p.source = worldgen::Source::RealPlace;
        p.area = area;
        p.size = 1500.0f;
        const worldgen::Plan pl = worldgen::plan(p, fitzel::TerrainSettings{}, -1000.0f,
            [](const fitzel::TerrainSettings& s, float x, float z) { return fitzel::terrainShapeHeight(s, x, z); });
        check(pl.real != nullptr, "a real plan came out");
        if (!pl.real) return 1;
        const geoplan::Real& R = *pl.real;
        const geoplan::Building* church = nearest(R, {10, -40});
        check(church && church->build == geoplan::Build::Civic && church->civic == static_cast<int>(civic::Kind::Church),
              "the church is CivicGen's church");
        const geoplan::Building* garage = nearest(R, {-560, 40});
        check(garage && garage->build == geoplan::Build::Skip, "the garage is left out");
        const geoplan::Building* hall = nearest(R, {300, 120});
        check(hall && hall->build == geoplan::Build::Hall, "the industrial building is a hall");
        const geoplan::Building* flats = nearest(R, {150, 70});
        check(flats && flats->build == geoplan::Build::Block && flats->floors == 5, "the flats are a five-storey block");
        const geoplan::Building* row = nearest(R, {-186, 30 + 5 * 7});
        check(row && row->attached, "a house in the row shares its walls");
        int houses = 0;
        for (const geoplan::Building& b : R.buildings) houses += b.build == geoplan::Build::House;
        check(houses >= 55, "the village's houses are houses");
        const geoplan::Building* farm = nearest(R, {-560 + 3 * 28, 18});
        check(farm && farm->build == geoplan::Build::House, "a village house is built as a house");
        std::printf("    (%s: %s)\n", farm ? housegen::presetName(static_cast<housegen::Preset>(farm->preset)) : "-",
                    farm ? farm->why.c_str() : "");
        bool joined = false;
        for (const geoplan::Road& r : R.roads)
            if (r.name == "Am Hang") joined = joined || (glm::length(r.pts.front() - glm::vec2(-200, 2)) < 1.0f &&
                                                         glm::length(r.pts.back() - glm::vec2(-200, 380)) < 1.0f) ||
                                                        (glm::length(r.pts.back() - glm::vec2(-200, 2)) < 1.0f &&
                                                         glm::length(r.pts.front() - glm::vec2(-200, 380)) < 1.0f);
        check(joined, "two ways of one street joined into one road");
        float hauptW = 0.0f, kirchW = 0.0f;
        for (const geoplan::Road& r : R.roads) {
            if (r.name == "Hauptstraße") hauptW = r.width;
            if (r.name == "Kirchweg") kirchW = r.width;
        }
        check(std::fabs(hauptW - 7.5f) < 0.01f, "a secondary road with room is as wide as its class");
        check(kirchW > 3.4f && kirchW < 5.4f, "a lane between houses as wide as they leave room for");
        check(R.eco && R.eco->at({0, -550}).r > 0.9f && R.eco->at({0, 550}).r < 0.05f, "the wood in the mask, the field out of it");
        check(R.summary.needleShare > 0.9f, "the wood's leaf type read");
        bool oak = false;
        for (const geoplan::Tree& t : R.trees) oak |= t.species == "Oak";
        check(oak, "the mapped oak is an oak");
        check(!pl.terrain.heightfield || std::fabs(fitzel::terrainShapeHeight(pl.terrain, 0, 0) - 2.0f) < 0.2f,
              "the ground is the elevation, 2 m over the square's lowest point");
        int lots = 0, civics = 0;
        for (const geoplan::Town& t : R.towns) {
            lots += static_cast<int>(t.lots.size());
            for (const cityplan::Lot& l : t.lots) civics += l.civic >= 0;
        }
        check(lots >= 60 && civics >= 2, "the plots are on the towns");
        check(R.region == worldgen::Region::Lowland, "flat ground reads as lowland");
        // The house types a real plot asks for lay out (as the town builds them).
        int bad = 0, all = 0;
        cityplan::Rule rule;
        for (const geoplan::Town& t : R.towns)
            for (const cityplan::Lot& l : t.lots) {
                if (!(l.kind & cityplan::kRealHouse)) continue;
                const housegen::Params hp = cityplan::housePrototype(rule, l.zone, l.kind);
                const housegen::Plan hpl = housegen::layout(hp);
                ++all;
                if (hpl.warnings > 0) {
                    ++bad;
                    if (bad <= 3)
                        std::printf("    %s %.1f x %.1f, %d storeys: %s\n", housegen::presetName(static_cast<housegen::Preset>(l.kind & 15)),
                                    hp.width, hp.depth, hp.storeys, hpl.notes.empty() ? "?" : hpl.notes.front().c_str());
                }
            }
        check(all > 0 && bad * 10 <= all, "the real houses' types lay out at their sizes (9 in 10 without a fault)");
    }
    std::printf("railways\n");
    {
        Fake v;
        // A double-track line east-west along z = 100 and 104, over a road on
        // one bridge for both, under a road bridge, over a short underpass, into
        // a hill and through it in a tunnel; an abandoned line beside it, a
        // platform at the second track.
        auto rail = [&](std::vector<glm::vec2> pts, std::vector<std::pair<std::string, std::string>> tags) {
            v.road(std::move(pts), std::move(tags));
        };
        const std::pair<std::string, std::string> main{"railway", "rail"}, name{"name", "Teststrecke"};
        rail({{-700, 100}, {-320, 100}}, {main, name});
        rail({{-320, 100}, {-280, 100}}, {main, name, {"bridge", "yes"}});
        rail({{-280, 100}, {500, 100}}, {main, name});
        rail({{500, 100}, {600, 100}}, {main, name, {"tunnel", "yes"}});
        rail({{600, 100}, {700, 100}}, {main, name});
        rail({{-700, 104}, {-320, 104}}, {main, name});
        rail({{-320, 104}, {-280, 104}}, {main, name, {"bridge", "yes"}});
        rail({{-280, 104}, {0, 104}}, {main, name});
        rail({{-700, 300}, {700, 300}}, {{"railway", "abandoned"}});
        v.road({{-300, -200}, {-300, 400}}, {{"highway", "residential"}, {"name", "Unterf\xC3\xBChrung"}});
        v.road({{100, -200}, {100, 60}}, {{"highway", "tertiary"}, {"name", "Br\xC3\xBC" "ckenstra\xC3\x9F" "e"}});
        v.road({{100, 60}, {100, 140}}, {{"highway", "tertiary"}, {"name", "Br\xC3\xBC" "ckenstra\xC3\x9F" "e"}, {"bridge", "yes"}});
        v.road({{100, 140}, {100, 400}}, {{"highway", "tertiary"}, {"name", "Br\xC3\xBC" "ckenstra\xC3\x9F" "e"}});
        v.road({{-100, -200}, {-100, 80}}, {{"highway", "residential"}, {"name", "Durchlass"}});
        v.road({{-100, 80}, {-100, 125}}, {{"highway", "residential"}, {"name", "Durchlass"}, {"tunnel", "yes"}});
        v.road({{-100, 125}, {-100, 400}}, {{"highway", "residential"}, {"name", "Durchlass"}});
        v.area({{-600, 107}, {-450, 107}, {-450, 113}, {-600, 113}}, {{"railway", "platform"}});
        // Land falling 1 % to the east, and a 30 m hill the tunnel goes through.
        auto area = std::make_shared<geoplan::AreaData>();
        area->center = v.fr.origin;
        area->size = 1500.0f;
        area->features = v.fs;
        const double tx = geo::tileX(v.fr.origin.lon, geoplan::kDemZoom), ty = geo::tileY(v.fr.origin.lat, geoplan::kDemZoom);
        auto land = [](glm::vec2 w) {
            const glm::vec2 d = w - glm::vec2(550.0f, 100.0f);
            return 210.0f - 0.01f * w.x + 30.0f * std::exp(-glm::dot(d, d) / (2.0f * 70.0f * 70.0f));
        };
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                geo::DemTile t;
                t.id = {geoplan::kDemZoom, static_cast<int>(tx) + dx, static_cast<int>(ty) + dy};
                t.w = t.h = 256;
                t.m.resize(256 * 256);
                for (int py = 0; py < 256; ++py)
                    for (int px = 0; px < 256; ++px) {
                        const double gx = t.id.x + (px + 0.5) / 256.0, gy = t.id.y + (py + 0.5) / 256.0;
                        t.m[static_cast<std::size_t>(py) * 256 + px] =
                            land(v.fr.toLocal({geo::lonOf(gx, geoplan::kDemZoom), geo::latOf(gy, geoplan::kDemZoom)}));
                    }
                area->dem.push_back(std::move(t));
            }
        worldgen::Params p;
        p.source = worldgen::Source::RealPlace;
        p.area = area;
        p.size = 1500.0f;
        const worldgen::Plan pl = worldgen::plan(p, fitzel::TerrainSettings{}, -1000.0f,
            [](const fitzel::TerrainSettings& s, float x, float z) { return fitzel::terrainShapeHeight(s, x, z); });
        if (!pl.real) { check(false, "a real plan came out"); return 1; }
        const geoplan::Real& R = *pl.real;
        const float off = pl.terrain.heightfield ? pl.terrain.heightfield->elevationOffset : 0.0f;
        auto ground = [&](float x, float z) { return fitzel::terrainShapeHeight(pl.terrain, x, z); };
        // The main line's level at x (on the piece that reaches it, z ~ 100).
        auto levelAt = [&](float x, float z) {
            for (const geoplan::Rail& r : R.rails)
                for (std::size_t k = 1; k < r.pts.size(); ++k) {
                    const glm::vec2 a = r.pts[k - 1], b = r.pts[k];
                    if ((a.x - x) * (b.x - x) > 0.0f || std::fabs(a.y - z) > 1.0f) continue;
                    return glm::mix(r.level[k - 1], r.level[k], (x - a.x) / std::max(b.x - a.x, 1e-3f));
                }
            return -1e9f;
        };
        bool abandoned = false, inTunnel = false;
        for (const geoplan::Rail& r : R.rails)
            for (const glm::vec2& q : r.pts) {
                abandoned |= std::fabs(q.y - 300.0f) < 5.0f;
                inTunnel |= q.x > 510.0f && q.x < 590.0f;
            }
        check(!abandoned, "an abandoned line is not laid");
        check(!inTunnel && R.summary.railTunnels == 1, "the tunnel is left out, the track ends at its portals");
        const geoplan::RailBridge* both = nullptr;
        for (const geoplan::RailBridge& b : R.railBridges)
            if (b.tracks == 2 && glm::length(0.5f * (b.pts.front() + b.pts.back()) - glm::vec2(-300, 102)) < 15.0f) both = &b;
        check(both && std::fabs(both->spacing - 4.0f) < 0.5f, "the two tracks cross the road on one deck");
        const float deck = levelAt(-300.0f, 100.0f), under = ground(-300.0f, 30.0f + 70.0f);
        std::printf("    deck over the road %.1f m over it (track %.1f m over the land)\n", deck - under,
                    deck - (land({-300, 100}) - off));
        check(deck - under > 6.4f && deck - (land({-300, 100}) - off) < 3.6f,
              "the road dips under the track's bridge for its headroom, the track climbs 3.2 m at most");
        bool decked = false, kept = false, synth = false;
        for (const geoplan::Road& r : R.roads) {
            if (r.name == "Br\xC3\xBC" "ckenstra\xC3\x9F" "e") decked |= !r.decks.empty();
            if (r.name == "Durchlass") kept |= std::min(r.pts.front().y, r.pts.back().y) < 0.0f && std::max(r.pts.front().y, r.pts.back().y) > 200.0f;
        }
        for (const geoplan::RailBridge& b : R.railBridges)
            synth |= glm::length(0.5f * (b.pts.front() + b.pts.back()) - glm::vec2(-100, 100)) < 15.0f;
        check(decked, "a road bridge over the line is a deck");
        check(kept && synth, "the road in a short tunnel under the line stays, the track bridges it");
        const float roadDeck = std::min(land({100, 60}), land({100, 140})) - off;
        std::printf("    under the road bridge: %.1f m below its deck\n", roadDeck - levelAt(100.0f, 100.0f));
        check(roadDeck - levelAt(100.0f, 100.0f) >= 8.7f, "the line passes under the road bridge, wires and all");
        check(R.summary.deepestCut > 3.0f && levelAt(480.0f, 100.0f) < land({480, 100}) - off - 2.0f,
              "the line cuts into the hill before the tunnel");
        check(R.platforms.size() == 1 && R.platforms[0].width > 4.5f && R.platforms[0].width < 7.5f, "the platform beside the track");
        int streetDecks = 0;
        for (const geoplan::Town& t : R.towns)
            for (const cityplan::Street& st : t.streets)
                if (!st.decks.empty()) {
                    ++streetDecks;
                    const std::vector<cityplan::StreetRun> runs = cityplan::streetRuns(cityplan::Grid{}, st, nullptr);
                    check(!runs.empty() && !runs.front().bridges.empty(), "the street lays its bridge as a deck");
                }
        check(streetDecks == 1, "the bridge goes with its street");
        if (std::getenv("GEOCHECK_RAILDEBUG")) {
            for (const geoplan::Rail& r : R.rails) {
                std::printf("    rail %s pts %zu bridges %zu:", r.name.c_str(), r.pts.size(), r.bridges.size());
                for (std::size_t k = 0; k < r.pts.size(); ++k) std::printf(" (%.0f,%.0f %.1f)", r.pts[k].x, r.pts[k].y, r.level[k] + off);
                std::printf("\n");
            }
            for (const geoplan::RailBridge& b : R.railBridges) std::printf("    bridge %.0f,%.0f..%.0f,%.0f tracks %d\n", b.pts.front().x, b.pts.front().y, b.pts.back().x, b.pts.back().y, b.tracks);
            for (const geoplan::Road& r : R.roads) { std::printf("    road %s:", r.name.c_str()); for (auto q : r.pts) std::printf(" (%.0f,%.0f)", q.x, q.y); std::printf(" decks %zu\n", r.decks.size()); }
        }
        railChecks(pl, nullptr);
    }
    return g_fail;
}

// The railways a real plan laid: on the ground graded for them, no steeper
// than their lines climb -- and, with `out`, a picture of them: the graded
// ground shaded from the north-west, a metre a pixel, the tracks dark, their
// bridges magenta, the platforms white, the streets pale.
void railChecks(const worldgen::Plan& pl, const char* out) {
    const geoplan::Real& R = *pl.real;
    if (R.rails.empty()) { std::printf("  no railways\n"); return; }
    float worstOff = 0.0f, worstGrade = 0.0f;
    glm::vec2 worstAt(0.0f);
    int worstRail = -1;
    glm::vec2 lo(1e9f), hi(-1e9f);
    for (const geoplan::Rail& r : R.rails) {
        for (const glm::vec2& q : r.pts) { lo = glm::min(lo, q); hi = glm::max(hi, q); }
        if (r.cls == geoplan::RailClass::Tram) continue;
        auto onDeck = [&](std::size_t k) {
            for (const auto& [a, b] : r.bridges) if (static_cast<int>(k) > a && static_cast<int>(k) < b) return true;
            return false;
        };
        for (std::size_t k = 0; k < r.pts.size(); ++k) {
            if (!onDeck(k)) {
                const float off = std::fabs(fitzel::terrainShapeHeight(pl.terrain, r.pts[k].x, r.pts[k].y) - r.level[k]);
                if (off > worstOff) { worstOff = off; worstAt = r.pts[k]; worstRail = static_cast<int>(&r - R.rails.data()); }
            }
            if (k > 0) {
                const float d = glm::length(r.pts[k] - r.pts[k - 1]);
                if (d > 1.0f) worstGrade = std::max(worstGrade, std::fabs(r.level[k] - r.level[k - 1]) / d);
            }
        }
    }
    std::printf("  railways: %zu tracks, %zu bridges (%d double), %zu platforms; track off its formation by at most "
                "%.2f m, steepest %.1f %%\n", R.rails.size(), R.railBridges.size(),
                static_cast<int>(std::count_if(R.railBridges.begin(), R.railBridges.end(), [](const geoplan::RailBridge& b) { return b.tracks == 2; })),
                R.platforms.size(), worstOff, worstGrade * 100.0f);
    if (worstRail >= 0) {
        std::printf("    most off at %.0f, %.0f (a %s)\n", worstAt.x, worstAt.y,
                    geoplan::railClassName(R.rails[static_cast<std::size_t>(worstRail)].cls));
        if (std::getenv("GEOCHECK_RAILDEBUG"))
            for (const geoplan::Rail& r : R.rails)
                for (std::size_t k = 0; k < r.pts.size(); ++k)
                    if (glm::length(r.pts[k] - worstAt) < 25.0f)
                        std::printf("      rail %d %s pt %zu/%zu at %.1f,%.1f level %.2f ground %.2f bridges %zu%s\n",
                                    static_cast<int>(&r - R.rails.data()), geoplan::railClassName(r.cls), k, r.pts.size(),
                                    r.pts[k].x, r.pts[k].y, r.level[k],
                                    fitzel::terrainShapeHeight(pl.terrain, r.pts[k].x, r.pts[k].y), r.bridges.size(),
                                    r.bridges.empty() ? "" : (" first " + std::to_string(r.bridges[0].first) + ".." + std::to_string(r.bridges[0].second)).c_str());
    }
    check(worstOff < 0.25f, "every track lies on the ground graded for it");
    check(worstGrade < 0.041f, "no line climbs steeper than it may");
    if (!out) return;
    lo -= glm::vec2(80.0f);
    hi += glm::vec2(80.0f);
    const int w = static_cast<int>(hi.x - lo.x), h = static_cast<int>(hi.y - lo.y);
    if (w <= 0 || h <= 0 || w * h > 40000000) return;
    std::vector<unsigned char> img(static_cast<std::size_t>(w) * h * 3);
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) {
            const float x = lo.x + static_cast<float>(i), z = lo.y + static_cast<float>(j);
            const float c = fitzel::terrainShapeHeight(pl.terrain, x, z);
            const float nw = fitzel::terrainShapeHeight(pl.terrain, x - 1.0f, z - 1.0f);
            const float v = glm::clamp(0.62f + (nw - c) * 0.9f, 0.05f, 1.0f);
            unsigned char* p = &img[(static_cast<std::size_t>(j) * w + i) * 3];
            p[0] = static_cast<unsigned char>(v * 205.0f); p[1] = static_cast<unsigned char>(v * 200.0f); p[2] = static_cast<unsigned char>(v * 185.0f);
        }
    auto line = [&](glm::vec2 a, glm::vec2 b, float half, unsigned char r, unsigned char g, unsigned char bl) {
        const float len = glm::length(b - a);
        for (float t = 0.0f; t <= len; t += 0.5f) {
            const glm::vec2 q = glm::mix(a, b, len > 0.0f ? t / len : 0.0f) - lo;
            for (int dj = -static_cast<int>(half); dj <= static_cast<int>(half); ++dj)
                for (int di = -static_cast<int>(half); di <= static_cast<int>(half); ++di) {
                    const int i = static_cast<int>(q.x) + di, j = static_cast<int>(q.y) + dj;
                    if (i < 0 || j < 0 || i >= w || j >= h) continue;
                    unsigned char* p = &img[(static_cast<std::size_t>(j) * w + i) * 3];
                    p[0] = r; p[1] = g; p[2] = bl;
                }
        }
    };
    for (const geoplan::Road& r : R.roads)
        for (std::size_t k = 1; k < r.pts.size(); ++k) line(r.pts[k - 1], r.pts[k], 0.0f, 250, 240, 200);
    for (const geoplan::Platform& pf : R.platforms)
        for (std::size_t k = 1; k < pf.pts.size(); ++k) line(pf.pts[k - 1], pf.pts[k], 0.5f * pf.width, 255, 255, 255);
    for (const geoplan::Rail& r : R.rails) {
        for (std::size_t k = 1; k < r.pts.size(); ++k) line(r.pts[k - 1], r.pts[k], 0.0f, 120, 20, 20);
        for (const auto& [a, b] : r.bridges)
            for (int k = a + 1; k <= b; ++k) line(r.pts[static_cast<std::size_t>(k - 1)], r.pts[static_cast<std::size_t>(k)], 1.0f, 230, 60, 220);
    }
    std::ofstream f(out, std::ios::binary);
    f << "P6\n" << w << " " << h << "\n255\n";
    f.write(reinterpret_cast<const char*>(img.data()), static_cast<std::streamsize>(img.size()));
    std::printf("  railways: %s (%d x %d m, from %.0f, %.0f)\n", out, w, h, lo.x, lo.y);
}

int live(double lat, double lon, float size, int port, const char* mapOut, const char* railsOut) {
    geoclient::Client client;
    client.setServer("127.0.0.1", port);
    for (int pass = 0; pass < 2; ++pass) {
        std::printf("%s load of %.4f, %.4f (%.0f m)\n", pass ? "second" : "first", lat, lon, size);
        geoclient::AreaLoad load;
        load.start({lon, lat}, size);
        const double t0 = now();
        double lastSay = 0.0;
        while (!load.done()) {
            load.poll(client);
            if (now() - lastSay > 5.0) { std::printf("    %s\n", load.status().c_str()); lastSay = now(); }
            if (now() - t0 > 900.0) { std::printf("    gave up\n"); return 1; }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        const double took = now() - t0;
        std::printf("    %s (%.2f s)\n", load.status().c_str(), took);
        if (!load.area()) { check(false, "something came"); return 1; }
        if (pass == 1) check(took < 10.0, "the second load streams from the cache");
        worldgen::Params p;
        p.source = worldgen::Source::RealPlace;
        p.area = load.area();
        p.size = size;
        const double tp = now();
        const worldgen::Plan pl = worldgen::plan(p, fitzel::TerrainSettings{}, -1000.0f,
            [](const fitzel::TerrainSettings& s, float x, float z) { return fitzel::terrainShapeHeight(s, x, z); });
        std::printf("    planned in %.2f s\n", now() - tp);
        if (!pl.real) { check(false, "a plan"); return 1; }
        const geoplan::Summary& S = pl.real->summary;
        if (pass == 0) {
            std::printf("  region: %s (%s)\n", worldgen::regionName(S.region), S.regionWhy.c_str());
            std::printf("  ground: %.0f..%.0f m, %d rivers/brooks, %d lakes\n", S.lowest, S.highest, S.rivers, S.lakes);
            std::printf("  %d buildings in %d places:", S.buildings, S.places);
            for (const geoplan::Place& pc : pl.real->places)
                if (pc.kind != geoplan::Settlement::Farmstead && pc.buildings >= 12)
                    std::printf(" %s (%s, %d)", pc.name.c_str(), geoplan::settlementName(pc.kind), pc.buildings);
            std::printf("\n  built as:");
            for (int b = 0; b < static_cast<int>(geoplan::Build::Count); ++b)
                if (S.builds[static_cast<std::size_t>(b)])
                    std::printf(" %s %d,", geoplan::buildName(static_cast<geoplan::Build>(b)), S.builds[static_cast<std::size_t>(b)]);
            std::printf("\n  houses:");
            for (int k = 0; k < static_cast<int>(housegen::Preset::Count); ++k)
                if (S.housePresets[static_cast<std::size_t>(k)])
                    std::printf(" %s %d,", housegen::presetName(static_cast<housegen::Preset>(k)), S.housePresets[static_cast<std::size_t>(k)]);
            std::printf("\n  uses:");
            for (int u = 0; u < static_cast<int>(geoplan::Use::Count); ++u)
                if (S.uses[static_cast<std::size_t>(u)])
                    std::printf(" %s %d,", geoplan::useName(static_cast<geoplan::Use>(u)), S.uses[static_cast<std::size_t>(u)]);
            std::printf("\n  roads:");
            for (int c = 0; c < static_cast<int>(geoplan::RoadClass::Count); ++c)
                if (S.roads[static_cast<std::size_t>(c)])
                    std::printf(" %s %d (%.1f km),", geoplan::roadClassName(static_cast<geoplan::RoadClass>(c)),
                                S.roads[static_cast<std::size_t>(c)], S.roadKm[static_cast<std::size_t>(c)]);
            std::printf("\n  land: forest %.0f %%, fields %.0f %%, built %.0f %%; needle share %.2f; %d single trees\n",
                        S.forestShare * 100.0f, S.fieldShare * 100.0f, S.builtShare * 100.0f, S.needleShare, S.trees);
            std::printf("  towns:");
            for (const geoplan::Town& t : pl.real->towns)
                std::printf(" %s [%zu streets, %zu plots]", t.name.c_str(), t.streets.size(), t.lots.size());
            std::printf("\n");
            for (const std::string& n : S.notes) std::printf("  note: %s\n", n.c_str());
            check(S.buildings > 0 || S.roads[6] > 0 || pl.real->roads.size() > 0, "a populated square");
            check(pl.terrain.heightfield != nullptr, "measured ground");
            if (mapOut) {
                int w = 0;
                const std::vector<unsigned char> img = worldgen::image(pl, 2, w);
                std::ofstream f(mapOut, std::ios::binary);
                f << "P6\n" << w << " " << w << "\n255\n";
                for (std::size_t i = 0; i + 3 < img.size(); i += 4) f.write(reinterpret_cast<const char*>(&img[i]), 3);
                std::printf("  map: %s\n", mapOut);
            }
            railChecks(pl, railsOut);
        }
    }
    return g_fail;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 5 && std::strcmp(argv[1], "--live") == 0) {
        int port = geo::kDefaultPort;
        const char* map = nullptr;
        const char* rails = nullptr;
        for (int i = 5; i < argc; ++i) {
            if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = std::atoi(argv[++i]);
            else if (std::strcmp(argv[i], "--map") == 0 && i + 1 < argc) map = argv[++i];
            else if (std::strcmp(argv[i], "--rails") == 0 && i + 1 < argc) rails = argv[++i];
        }
        const int r = live(std::atof(argv[2]), std::atof(argv[3]), static_cast<float>(std::atof(argv[4])), port, map, rails);
        std::printf("%s\n", r == 0 ? "geocheck --live: all good" : "geocheck --live: FAILED");
        return r == 0 ? 0 : 1;
    }
    const int r = offline();
    std::printf("%s\n", r == 0 ? "geocheck: all good" : "geocheck: FAILED");
    return r == 0 ? 0 : 1;
}
