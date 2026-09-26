#include "CityPlan.hpp"

#include <algorithm>
#include <deque>
#include <cmath>
#include <iterator>
#include <map>
#include <string>
#include <memory>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "Component.hpp"
#include "EditMesh.hpp"

namespace cityplan {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// --- Deterministic noise ----------------------------------------------------
// Hashed from indices, never drawn from a running generator -- the same rule as
// CityGen: a block keeps its houses when the block next to it changes.
std::uint32_t hashU(std::uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
std::uint32_t hash3(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    return hashU(a ^ hashU(b ^ hashU(c)));
}
float unit(std::uint32_t h) { return static_cast<float>(h & 0xffffffU) / 16777215.0f; }
float sym(std::uint32_t h)  { return unit(h) * 2.0f - 1.0f; }

float cross2(glm::vec2 a, glm::vec2 b) { return a.x * b.y - a.y * b.x; }

// Where the lines p + t*d and q + s*e meet; false when they are (nearly) parallel.
bool intersect(glm::vec2 p, glm::vec2 d, glm::vec2 q, glm::vec2 e, glm::vec2& out) {
    const float den = cross2(d, e);
    if (std::abs(den) < 1e-6f) return false;
    const float t = cross2(q - p, e) / den;
    out = p + d * t;
    return true;
}

// Inside a convex polygon, whichever way it is wound, with `tol` metres of give.
bool insideConvex(const std::vector<glm::vec2>& poly, glm::vec2 p, float tol) {
    float sign = 0.0f;
    const std::size_t n = poly.size();
    for (std::size_t i = 0; i < n; ++i) {
        const glm::vec2 a = poly[i], b = poly[(i + 1) % n];
        const glm::vec2 e = b - a;
        const float len = glm::length(e);
        if (len < 1e-5f) continue;
        const float c = cross2(e, p - a) / len;   // signed distance, metres
        if (sign == 0.0f) {
            // The polygon's own winding, from its centroid.
            glm::vec2 cen(0.0f);
            for (const glm::vec2& v : poly) cen += v;
            cen /= static_cast<float>(n);
            sign = (cross2(e, cen - a) >= 0.0f) ? 1.0f : -1.0f;
        }
        if (c * sign < -tol) return false;
    }
    return true;
}

// An oriented footprint: centre, the unit axis along the street, half-sizes.
struct Obb {
    glm::vec2 c{0.0f}, u{1.0f, 0.0f};
    float hu = 1.0f, hv = 1.0f;
    glm::vec2 v() const { return {-u.y, u.x}; }
    void corners(glm::vec2 out[4]) const {
        const glm::vec2 a = u * hu, b = v() * hv;
        out[0] = c - a - b; out[1] = c + a - b; out[2] = c + a + b; out[3] = c - a + b;
    }
};

// Separating-axis test between two footprints, `shrink` metres of tolerance so
// that neighbours sharing a party wall do not count as overlapping.
bool overlaps(const Obb& A, const Obb& B, float shrink) {
    const glm::vec2 axes[4] = {A.u, A.v(), B.u, B.v()};
    const glm::vec2 d = B.c - A.c;
    for (const glm::vec2& ax : axes) {
        const float ra = A.hu * std::abs(glm::dot(A.u, ax)) + A.hv * std::abs(glm::dot(A.v(), ax));
        const float rb = B.hu * std::abs(glm::dot(B.u, ax)) + B.hv * std::abs(glm::dot(B.v(), ax));
        if (std::abs(glm::dot(d, ax)) >= ra + rb - shrink) return false;
    }
    return true;
}

// --- Roads as something a footprint can be measured against ------------------
// A uniform grid over every road segment, so a thousand lots times nine samples
// do not each walk every segment of every street.
struct SegGrid {
    struct Seg { glm::vec2 a, b; float half; int road; };
    std::vector<Seg> segs;
    float cell = 40.0f;
    std::unordered_map<std::int64_t, std::vector<int>> cells;

    static std::int64_t key(int x, int z) {
        return (static_cast<std::int64_t>(x) << 32) ^ static_cast<std::uint32_t>(z);
    }
    void build(const std::vector<RoadLine>& roads) {
        for (std::size_t ri = 0; ri < roads.size(); ++ri) {
            const RoadLine& r = roads[ri];
            for (std::size_t i = 0; i + 1 < r.pts.size(); ++i) {
                const glm::vec2 a0 = r.pts[i], b0 = r.pts[i + 1];
                // Long segments go in as cell-sized pieces, so the box each one
                // is filed under stays a few cells rather than a whole diagonal.
                const float len = glm::length(b0 - a0);
                const int   n   = std::max(1, static_cast<int>(std::ceil(len / cell)));
                if (n > 4096) continue;   // not a road: a break marker or garbage
                for (int k = 0; k < n; ++k) {
                    const glm::vec2 a = glm::mix(a0, b0, static_cast<float>(k) / n);
                    const glm::vec2 b = glm::mix(a0, b0, static_cast<float>(k + 1) / n);
                    const int id = static_cast<int>(segs.size());
                    segs.push_back({a, b, r.half, static_cast<int>(ri)});
                    const glm::vec2 lo = glm::min(a, b) - r.half - 1.0f;
                    const glm::vec2 hi = glm::max(a, b) + r.half + 1.0f;
                    for (int x = static_cast<int>(std::floor(lo.x / cell));
                         x <= static_cast<int>(std::floor(hi.x / cell)); ++x)
                        for (int z = static_cast<int>(std::floor(lo.y / cell));
                             z <= static_cast<int>(std::floor(hi.y / cell)); ++z)
                            cells[key(x, z)].push_back(id);
                }
            }
        }
    }
    // The road whose carriageway `p` is on (or within a metre of) and that runs
    // along `d` -- the street a sign at a crossing is naming. -1 when none.
    int nearestParallel(glm::vec2 p, glm::vec2 d) const {
        int   best = -1;
        float bestD = 1e9f;
        const auto it = cells.find(key(static_cast<int>(std::floor(p.x / cell)),
                                       static_cast<int>(std::floor(p.y / cell))));
        if (it == cells.end()) return best;
        for (int id : it->second) {
            const Seg& s = segs[static_cast<std::size_t>(id)];
            const glm::vec2 ab = s.b - s.a;
            const float l2 = glm::dot(ab, ab);
            if (l2 < 1e-8f) continue;
            if (std::abs(glm::dot(ab / std::sqrt(l2), d)) < 0.85f) continue;
            const float t = glm::clamp(glm::dot(p - s.a, ab) / l2, 0.0f, 1.0f);
            const float dist = glm::length(p - (s.a + ab * t));
            if (dist <= s.half + 1.0f && dist < bestD) { bestD = dist; best = s.road; }
        }
        return best;
    }
    // The road running along `d` nearest to `p` (within 12 m): the closest point
    // on its centreline, its heading there and its half-width. The grid's
    // streets are straight between nodes, the roads laid from them are splines
    // through the same nodes -- this is how a kerb finds the real edge.
    bool snap(glm::vec2 p, glm::vec2 d, glm::vec2& at, glm::vec2& dir, float& half) const {
        float bestD = 12.0f;
        bool  found = false;
        const auto it = cells.find(key(static_cast<int>(std::floor(p.x / cell)),
                                       static_cast<int>(std::floor(p.y / cell))));
        if (it == cells.end()) return false;
        for (int id : it->second) {
            const Seg& s = segs[static_cast<std::size_t>(id)];
            const glm::vec2 ab = s.b - s.a;
            const float l2 = glm::dot(ab, ab);
            if (l2 < 1e-8f) continue;
            const glm::vec2 u = ab / std::sqrt(l2);
            if (std::abs(glm::dot(u, d)) < 0.85f) continue;
            const float t = glm::clamp(glm::dot(p - s.a, ab) / l2, 0.0f, 1.0f);
            const glm::vec2 q = s.a + ab * t;
            const float dist = glm::length(p - q);
            if (dist < bestD) { bestD = dist; at = q; dir = u; half = s.half; found = true; }
        }
        return found;
    }
    // How far `p` is from the nearest carriageway EDGE (negative = on it).
    float clearance(glm::vec2 p) const {
        float best = 1e9f;
        const auto it = cells.find(key(static_cast<int>(std::floor(p.x / cell)),
                                       static_cast<int>(std::floor(p.y / cell))));
        if (it == cells.end()) return best;
        for (int id : it->second) {
            const Seg& s = segs[static_cast<std::size_t>(id)];
            const glm::vec2 ab = s.b - s.a;
            const float l2 = glm::dot(ab, ab);
            const float t  = l2 > 1e-8f ? glm::clamp(glm::dot(p - s.a, ab) / l2, 0.0f, 1.0f) : 0.0f;
            best = std::min(best, glm::length(p - (s.a + ab * t)) - s.half);
        }
        return best;
    }
};

// Which chunk a point belongs to, for the batch merge: frustum and distance
// culling need something to bite on, so a town is welded in squares. 250 m, not
// the roadside city's 160: a town's batches are per chunk AND per material, and
// the colour variants multiplied the materials -- at 160 m the City preset went
// from 320 to 650 draws, and the cost of a draw is paid a dozen times a frame.
int chunkOf(glm::vec2 p) {
    const int cx = static_cast<int>(std::floor(p.x / 250.0f));
    const int cz = static_cast<int>(std::floor(p.y / 250.0f));
    return cx * 8192 + cz;
}

// Degrees about +Y that turn a building's local -Z (its street face, in both
// BuildingGen and HouseGen) onto `front`. With the engine's Euler-Y convention
// local (0,0,-1) lands on (-sin y, -cos y).
float yawFacing(glm::vec2 front) {
    return glm::degrees(std::atan2(-front.x, -front.y));
}

} // namespace

// --- Names ----------------------------------------------------------------------

const char* zoneName(Zone z) {
    switch (z) {
        case Zone::Towers: return "Towers";
        case Zone::Blocks: return "Apartment blocks";
        case Zone::Rows:   return "Terraced houses";
        case Zone::Houses: return "Family houses";
        case Zone::Industry: return "Industry";
        case Zone::Park:   return "Park";
        default:           return "?";
    }
}

const char* presetName(Preset p) {
    switch (p) {
        case Preset::Village:    return "Village";
        case Preset::SmallTown:  return "Small town";
        case Preset::City:       return "City";
        case Preset::Metropolis: return "Metropolis";
        case Preset::Suburb:     return "Suburb";
        default:                 return "?";
    }
}

void applyPreset(Rule& r, Preset p) {
    // Only what makes the character; place, size and seed stay the author's.
    switch (p) {
        case Preset::Village:
            r.grid.sizeX = 300.0f; r.grid.sizeZ = 240.0f;
            r.grid.blockX = 100.0f; r.grid.blockZ = 80.0f; r.grid.organic = 0.55f;
            r.grid.streetWidth = 6.0f; r.grid.avenueWidth = 7.5f; r.grid.avenueEvery = 0;
            r.towerRing = 0.0f; r.blockRing = 0.0f; r.rowRing = 0.25f;
            r.parkChance = 0.10f; r.centrePark = true;
            r.houseLot = 24.0f; r.houseSetback = 6.0f; r.fill = 0.85f;
            r.churches = 1; r.policeStations = 0; r.fireStations = 1; r.hospitals = 0;
            r.industryShare = 0.0f;
            break;
        case Preset::Count:
        case Preset::SmallTown:
            r.grid.sizeX = 520.0f; r.grid.sizeZ = 440.0f;
            r.grid.blockX = 95.0f; r.grid.blockZ = 75.0f; r.grid.organic = 0.18f;
            r.grid.streetWidth = 7.0f; r.grid.avenueWidth = 11.0f; r.grid.avenueEvery = 3;
            r.towerRing = 0.0f; r.blockRing = 0.35f; r.rowRing = 0.60f;
            r.blockFloorsMin = 3; r.blockFloorsMax = 5;
            r.parkChance = 0.08f; r.centrePark = true;
            r.houseLot = 20.0f; r.houseSetback = 5.0f; r.fill = 0.94f;
            r.churches = 2; r.policeStations = 1; r.fireStations = 1; r.hospitals = 1;
            r.industryShare = 0.08f;
            break;
        case Preset::City:
            r.grid.sizeX = 900.0f; r.grid.sizeZ = 760.0f;
            r.grid.blockX = 110.0f; r.grid.blockZ = 85.0f; r.grid.organic = 0.06f;
            r.grid.streetWidth = 8.0f; r.grid.avenueWidth = 14.0f; r.grid.avenueEvery = 3;
            r.towerRing = 0.25f; r.blockRing = 0.60f; r.rowRing = 0.78f;
            r.towerFloorsMin = 10; r.towerFloorsMax = 40;
            r.blockFloorsMin = 4; r.blockFloorsMax = 7;
            r.parkChance = 0.07f; r.centrePark = false;
            r.houseLot = 18.0f; r.houseSetback = 4.0f; r.fill = 0.96f;
            r.churches = 3; r.policeStations = 2; r.fireStations = 2; r.hospitals = 1;
            r.industryShare = 0.12f;
            break;
        case Preset::Metropolis:
            r.grid.sizeX = 1500.0f; r.grid.sizeZ = 1200.0f;
            r.grid.blockX = 120.0f; r.grid.blockZ = 90.0f; r.grid.organic = 0.0f;
            r.grid.streetWidth = 9.0f; r.grid.avenueWidth = 16.0f; r.grid.avenueEvery = 4;
            // Towers and apartment blocks only. A metropolis has no front
            // gardens -- and a house costs thirty times a block's vertices, so
            // two thousand of them would be a quarter-gigabyte town.
            r.towerRing = 0.55f; r.blockRing = 2.0f; r.rowRing = 2.0f;
            r.towerFloorsMin = 18; r.towerFloorsMax = 70;
            r.blockFloorsMin = 5; r.blockFloorsMax = 9;
            r.parkChance = 0.05f; r.centrePark = false;
            r.houseLot = 18.0f; r.houseSetback = 4.0f; r.fill = 0.97f;
            r.churches = 4; r.policeStations = 3; r.fireStations = 3; r.hospitals = 2;
            r.industryShare = 0.15f;
            r.budget = 3000;
            break;
        case Preset::Suburb:
            r.grid.sizeX = 480.0f; r.grid.sizeZ = 360.0f;
            r.grid.blockX = 90.0f; r.grid.blockZ = 60.0f; r.grid.organic = 0.30f;
            r.grid.streetWidth = 6.0f; r.grid.avenueWidth = 9.0f; r.grid.avenueEvery = 0;
            r.towerRing = 0.0f; r.blockRing = 0.0f; r.rowRing = 0.0f;
            r.parkChance = 0.06f; r.centrePark = true;
            r.houseLot = 20.0f; r.houseSetback = 6.0f; r.fill = 0.92f;
            r.churches = 1; r.policeStations = 0; r.fireStations = 1; r.hospitals = 0;
            r.industryShare = 0.0f;
            break;
    }
}

// --- Persistence ------------------------------------------------------------------

namespace {
nlohmann::json v3(const glm::vec3& v) { return nlohmann::json::array({v.x, v.y, v.z}); }
glm::vec3 rv3(const nlohmann::json& j, const char* k, glm::vec3 d) {
    if (!j.contains(k) || !j[k].is_array() || j[k].size() != 3) return d;
    return {j[k][0].get<float>(), j[k][1].get<float>(), j[k][2].get<float>()};
}
} // namespace

namespace {

nlohmann::json gridJson(const Grid& g) {
    return {
        {"center", {g.center.x, g.center.y}}, {"sizeX", g.sizeX}, {"sizeZ", g.sizeZ},
        {"rotation", g.rotation}, {"blockX", g.blockX}, {"blockZ", g.blockZ},
        {"organic", g.organic}, {"seed", g.seed},
        {"streetWidth", g.streetWidth}, {"avenueWidth", g.avenueWidth},
        {"avenueEvery", g.avenueEvery}, {"stub", g.stub}, {"bridges", g.bridges},
        {"maxBridge", g.maxBridge}, {"names", g.names},
    };
}

Grid jsonGrid(const nlohmann::json& j) {
    const Grid d;
    Grid g;
    if (!j.is_object()) return g;
    if (j.contains("center") && j["center"].is_array() && j["center"].size() == 2)
        g.center = {j["center"][0].get<float>(), j["center"][1].get<float>()};
    g.sizeX       = j.value("sizeX", d.sizeX);
    g.sizeZ       = j.value("sizeZ", d.sizeZ);
    g.rotation    = j.value("rotation", d.rotation);
    g.blockX      = j.value("blockX", d.blockX);
    g.blockZ      = j.value("blockZ", d.blockZ);
    g.organic     = j.value("organic", d.organic);
    g.seed        = j.value("seed", d.seed);
    g.streetWidth = j.value("streetWidth", d.streetWidth);
    g.avenueWidth = j.value("avenueWidth", d.avenueWidth);
    g.avenueEvery = j.value("avenueEvery", d.avenueEvery);
    g.stub        = j.value("stub", d.stub);
    g.names       = j.value("names", d.names);
    g.bridges     = j.value("bridges", d.bridges);
    g.maxBridge   = j.value("maxBridge", d.maxBridge);
    return g;
}

} // namespace

bool Grid::operator==(const Grid& o) const { return gridJson(*this) == gridJson(o); }

void save(nlohmann::json& j, const Rule& r) {
    j = {
        {"id", r.id}, {"name", r.name}, {"enabled", r.enabled}, {"seed", r.seed},
        {"grid", gridJson(r.grid)},
        {"towerRing", r.towerRing}, {"blockRing", r.blockRing}, {"rowRing", r.rowRing},
        {"zoneNoise", r.zoneNoise}, {"parkChance", r.parkChance},
        {"centrePark", r.centrePark},
        {"industryShare", r.industryShare}, {"industryAngle", r.industryAngle},
        {"churches", r.churches}, {"policeStations", r.policeStations},
        {"fireStations", r.fireStations}, {"hospitals", r.hospitals},
        {"sidewalk", r.sidewalk},
        {"towerFloorsMin", r.towerFloorsMin}, {"towerFloorsMax", r.towerFloorsMax},
        {"blockFloorsMin", r.blockFloorsMin}, {"blockFloorsMax", r.blockFloorsMax},
        {"houseLot", r.houseLot}, {"houseSetback", r.houseSetback},
        {"maxSlope", r.maxSlope}, {"fill", r.fill}, {"budget", r.budget},
        {"towerPalette", r.towerPalette}, {"blockPalette", r.blockPalette},
        {"glassTint", v3(r.glassTint)}, {"towerBase", v3(r.towerBase)},
        {"blockColor", v3(r.blockColor)}, {"blockBase", v3(r.blockBase)},
        {"facadeColor", v3(r.facadeColor)}, {"roofColor", v3(r.roofColor)},
        {"windowLit", r.windowLit}, {"weathering", r.weathering},
        {"colourVariety", r.colourVariety}, {"signs", r.signs},
        {"signStyle", r.signStyle}, {"pavements", r.pavements},
        {"trafficLights", r.trafficLights}, {"busStopEvery", r.busStopEvery},
        {"collider", r.collider},
    };
    // Only once streets exist: a town that was never laid has no "laid".
    if (r.hasLaid) j["laid"] = gridJson(r.laid);
}

void load(const nlohmann::json& j, Rule& r) {
    const Rule d;   // every field defaults to a fresh town's
    r.id       = j.value("id", d.id);
    r.name     = j.value("name", d.name);
    r.enabled  = j.value("enabled", d.enabled);
    r.seed     = j.value("seed", d.seed);
    r.grid     = j.contains("grid") ? jsonGrid(j["grid"]) : d.grid;
    r.hasLaid  = j.contains("laid") && j["laid"].is_object();
    r.laid     = r.hasLaid ? jsonGrid(j["laid"]) : d.laid;
    r.towerRing  = j.value("towerRing", d.towerRing);
    r.blockRing  = j.value("blockRing", d.blockRing);
    r.rowRing    = j.value("rowRing", d.rowRing);
    r.zoneNoise  = j.value("zoneNoise", d.zoneNoise);
    r.parkChance = j.value("parkChance", d.parkChance);
    r.centrePark = j.value("centrePark", d.centrePark);
    r.industryShare  = j.value("industryShare", d.industryShare);
    r.industryAngle  = j.value("industryAngle", d.industryAngle);
    r.churches       = j.value("churches", d.churches);
    r.policeStations = j.value("policeStations", d.policeStations);
    r.fireStations   = j.value("fireStations", d.fireStations);
    r.hospitals      = j.value("hospitals", d.hospitals);
    r.sidewalk   = j.value("sidewalk", d.sidewalk);
    r.towerFloorsMin = j.value("towerFloorsMin", d.towerFloorsMin);
    r.towerFloorsMax = j.value("towerFloorsMax", d.towerFloorsMax);
    r.blockFloorsMin = j.value("blockFloorsMin", d.blockFloorsMin);
    r.blockFloorsMax = j.value("blockFloorsMax", d.blockFloorsMax);
    r.houseLot     = j.value("houseLot", d.houseLot);
    r.houseSetback = j.value("houseSetback", d.houseSetback);
    r.maxSlope     = j.value("maxSlope", d.maxSlope);
    r.fill         = j.value("fill", d.fill);
    r.budget       = j.value("budget", d.budget);
    r.towerPalette = j.value("towerPalette", d.towerPalette);
    r.blockPalette = j.value("blockPalette", d.blockPalette);
    r.glassTint   = rv3(j, "glassTint", d.glassTint);
    r.towerBase   = rv3(j, "towerBase", d.towerBase);
    r.blockColor  = rv3(j, "blockColor", d.blockColor);
    r.blockBase   = rv3(j, "blockBase", d.blockBase);
    r.facadeColor = rv3(j, "facadeColor", d.facadeColor);
    r.roofColor   = rv3(j, "roofColor", d.roofColor);
    r.windowLit   = j.value("windowLit", d.windowLit);
    r.colourVariety = j.value("colourVariety", d.colourVariety);
    r.signs         = j.value("signs", d.signs);
    r.signStyle     = j.value("signStyle", d.signStyle);
    r.pavements     = j.value("pavements", d.pavements);
    r.trafficLights = j.value("trafficLights", d.trafficLights);
    r.busStopEvery  = j.value("busStopEvery", d.busStopEvery);
    r.weathering  = j.value("weathering", d.weathering);
    r.collider    = j.value("collider", d.collider);
}

bool Rule::operator==(const Rule& o) const {
    // Through the JSON on purpose: one list of fields (save) instead of a second
    // one here that the next new field is forgotten in.
    nlohmann::json a, b;
    save(a, *this);
    save(b, o);
    return a == b;
}

// --- The grid ---------------------------------------------------------------------

Layout layout(const Rule& r, const Grid& g) {
    Layout L;
    const float sx = std::max(g.sizeX, 40.0f), sz = std::max(g.sizeZ, 40.0f);
    L.nx = glm::clamp(static_cast<int>(std::lround(sx / std::max(g.blockX, 25.0f))), 1, 40);
    L.nz = glm::clamp(static_cast<int>(std::lround(sz / std::max(g.blockZ, 25.0f))), 1, 40);
    const float cx = sx / L.nx, cz = sz / L.nz;   // cell size

    const float a = glm::radians(g.rotation);
    L.axisX = {std::cos(a), std::sin(a)};
    L.axisZ = {-std::sin(a), std::cos(a)};
    auto world = [&](float u, float v) { return g.center + L.axisX * u + L.axisZ * v; };

    // --- Nodes ------------------------------------------------------------------
    // "Organic" is two things at once, because each alone looks wrong: a slow
    // warp bends whole streets (an old town follows the lie of the land, not a
    // ruler), and a per-node jitter keeps the blocks from all being the same
    // bent shape. Both are bounded well inside a cell, so no street can cross its
    // neighbour and no block can turn itself inside out.
    const float org = glm::clamp(g.organic, 0.0f, 1.0f);
    const float ph1 = unit(hashU(g.seed ^ 0x51edU)) * 2.0f * kPi;
    const float ph2 = unit(hashU(g.seed ^ 0x2badU)) * 2.0f * kPi;
    L.nodes.resize(static_cast<std::size_t>((L.nx + 1) * (L.nz + 1)));
    for (int j = 0; j <= L.nz; ++j)
        for (int i = 0; i <= L.nx; ++i) {
            float u = -0.5f * sx + i * cx;
            float v = -0.5f * sz + j * cz;
            const float wu = org * 0.30f * cx * std::sin(v / sz * 2.6f + ph1);
            const float wv = org * 0.30f * cz * std::sin(u / sx * 2.6f + ph2);
            const std::uint32_t h = hash3(g.seed, static_cast<std::uint32_t>(i),
                                          static_cast<std::uint32_t>(j));
            u += wu + org * 0.16f * cx * sym(hashU(h ^ 0x11U));
            v += wv + org * 0.16f * cz * sym(hashU(h ^ 0x22U));
            L.nodes[static_cast<std::size_t>(j * (L.nx + 1) + i)] = world(u, v);
        }
    auto node = [&](int i, int j) {
        return L.nodes[static_cast<std::size_t>(j * (L.nx + 1) + i)];
    };

    // --- Streets ------------------------------------------------------------------
    // Every line runs on past the edge. That is not decoration: two streets that
    // both END at the same corner are neither an X nor a T, and the junction
    // finder (RoadJunction.hpp) would leave their ribbons overlapping there. With
    // a stub the corner is an ordinary crossing -- and the town has roads out.
    const float stub = std::max(g.stub, std::max(g.avenueWidth, g.streetWidth) * 1.5f);
    auto isAvenue = [&](int line, int count) {
        if (g.avenueEvery <= 0) return false;
        const int mid = count / 2;
        return ((line - mid) % g.avenueEvery) == 0;
    };
    std::vector<float> widthAlongZ(static_cast<std::size_t>(L.nx + 1));
    std::vector<float> widthAlongX(static_cast<std::size_t>(L.nz + 1));
    for (int i = 0; i <= L.nx; ++i) {
        Street s;
        s.alongZ = true;
        s.line   = i;
        s.avenue = isAvenue(i, L.nx);
        s.width  = s.avenue ? g.avenueWidth : g.streetWidth;
        const glm::vec2 d0 = glm::normalize(node(i, 0) - node(i, 1));
        const glm::vec2 d1 = glm::normalize(node(i, L.nz) - node(i, L.nz - 1));
        s.pts.push_back(node(i, 0) + d0 * stub);
        for (int j = 0; j <= L.nz; ++j) s.pts.push_back(node(i, j));
        s.pts.push_back(node(i, L.nz) + d1 * stub);
        widthAlongZ[static_cast<std::size_t>(i)] = s.width;
        L.streets.push_back(std::move(s));
    }
    for (int j = 0; j <= L.nz; ++j) {
        Street s;
        s.alongZ = false;
        s.line   = j;
        s.avenue = isAvenue(j, L.nz);
        s.width  = s.avenue ? g.avenueWidth : g.streetWidth;
        const glm::vec2 d0 = glm::normalize(node(0, j) - node(1, j));
        const glm::vec2 d1 = glm::normalize(node(L.nx, j) - node(L.nx - 1, j));
        s.pts.push_back(node(0, j) + d0 * stub);
        for (int i = 0; i <= L.nx; ++i) s.pts.push_back(node(i, j));
        s.pts.push_back(node(L.nx, j) + d1 * stub);
        widthAlongX[static_cast<std::size_t>(j)] = s.width;
        L.streets.push_back(std::move(s));
    }

    const std::vector<std::string> names = streetNames(g, L.streets);
    for (std::size_t k = 0; k < L.streets.size(); ++k) L.streets[k].name = names[k];

    // --- Blocks and their zones -------------------------------------------------
    int   centreBlock = -1;
    float centreRing  = 1e9f;
    for (int j = 0; j < L.nz; ++j)
        for (int i = 0; i < L.nx; ++i) {
            Block b;
            b.ix = i; b.iz = j;
            b.corner[0] = node(i, j);
            b.corner[1] = node(i + 1, j);
            b.corner[2] = node(i + 1, j + 1);
            b.corner[3] = node(i, j + 1);
            // Side k runs corner k -> k+1: 0 and 2 lie on lines along X, 1 and 3
            // on lines along Z.
            b.streetHalf[0] = widthAlongX[static_cast<std::size_t>(j)] * 0.5f;
            b.streetHalf[1] = widthAlongZ[static_cast<std::size_t>(i + 1)] * 0.5f;
            b.streetHalf[2] = widthAlongX[static_cast<std::size_t>(j + 1)] * 0.5f;
            b.streetHalf[3] = widthAlongZ[static_cast<std::size_t>(i)] * 0.5f;
            // The ring from the UNWARPED cell centre, on the ellipse the town's
            // extent spans -- so a long town is zoned along its length.
            const float u = -0.5f * sx + (i + 0.5f) * cx;
            const float v = -0.5f * sz + (j + 0.5f) * cz;
            b.local = {u, v};
            b.ring = std::sqrt((u / (0.5f * sx)) * (u / (0.5f * sx)) +
                               (v / (0.5f * sz)) * (v / (0.5f * sz)));
            const std::uint32_t h = hash3(r.seed ^ 0x7a11U, static_cast<std::uint32_t>(i),
                                          static_cast<std::uint32_t>(j));
            const float rn = b.ring + r.zoneNoise * sym(hashU(h));
            if      (rn < r.towerRing) b.zone = Zone::Towers;
            else if (rn < r.blockRing) b.zone = Zone::Blocks;
            else if (rn < r.rowRing)   b.zone = Zone::Rows;
            else                       b.zone = Zone::Houses;
            if (unit(hashU(h ^ 0x9a2cU)) < r.parkChance) b.zone = Zone::Park;
            if (b.ring < centreRing) { centreRing = b.ring; centreBlock = static_cast<int>(L.blocks.size()); }
            L.blocks.push_back(b);
        }
    // A square at the heart of it -- only where there are blocks around it to
    // make it one, or a hamlet of four blocks loses a quarter of itself to grass.
    if (r.centrePark && centreBlock >= 0 && L.nx * L.nz >= 6)
        L.blocks[static_cast<std::size_t>(centreBlock)].zone = Zone::Park;

    // --- Industry: a district on one side of the edge ---------------------------
    // Ranked by how far a block lies towards `industryAngle`, outer half only, so
    // the estate comes out as one coherent quarter rather than a speckle -- and
    // the number of blocks is the share, not a threshold that empties or floods
    // the edge depending on the grid.
    {
        const float a = glm::radians(r.industryAngle);
        const glm::vec2 dir(std::cos(a), std::sin(a));
        std::vector<std::pair<float, int>> cand;
        for (int i = 0; i < static_cast<int>(L.blocks.size()); ++i) {
            const Block& b = L.blocks[static_cast<std::size_t>(i)];
            if (b.zone == Zone::Park || b.ring < 0.45f) continue;
            const glm::vec2 lp(b.local.x / (0.5f * sx), b.local.y / (0.5f * sz));
            const float along = glm::dot(glm::normalize(lp), dir);
            if (along < 0.2f) continue;
            cand.push_back({along + 0.25f * b.ring, i});
        }
        std::sort(cand.begin(), cand.end(), [](const auto& x, const auto& y) {
            return x.first > y.first || (x.first == y.first && x.second < y.second);
        });
        const int want = static_cast<int>(std::lround(
            glm::clamp(r.industryShare, 0.0f, 1.0f) * static_cast<float>(L.blocks.size())));
        for (int k = 0; k < want && k < static_cast<int>(cand.size()); ++k)
            L.blocks[static_cast<std::size_t>(cand[static_cast<std::size_t>(k)].second)].zone =
                Zone::Industry;
    }

    // --- Public buildings: one whole block each ----------------------------------
    // Each kind has a ring it belongs in (the church at the heart, the hospital
    // and the fire station further out), and every pick keeps its distance from
    // the ones before it, so two police stations do not end up side by side.
    {
        std::vector<int> taken;
        const float cellMin = std::min(cx, cz);
        auto pick = [&](float targetRing, std::uint32_t salt) {
            int   best  = -1;
            float bestS = 1e9f;
            for (int i = 0; i < static_cast<int>(L.blocks.size()); ++i) {
                const Block& b = L.blocks[static_cast<std::size_t>(i)];
                if (b.zone == Zone::Park || b.zone == Zone::Industry || b.civic >= 0) continue;
                float s = std::abs(b.ring - targetRing);
                for (int t : taken) {
                    const float d = glm::length(b.local - L.blocks[static_cast<std::size_t>(t)].local)
                                    / cellMin;
                    if (d < 2.5f) s += 0.6f * (2.5f - d);
                }
                s += 0.03f * unit(hash3(r.seed ^ salt, static_cast<std::uint32_t>(i), 0x5eedU));
                if (s < bestS) { bestS = s; best = i; }
            }
            return best;
        };
        auto place = [&](civic::Kind k, int count, float ring0, float ringStep) {
            for (int n = 0; n < count; ++n) {
                const int i = pick(ring0 + ringStep * static_cast<float>(n),
                                   0x1000U * (static_cast<std::uint32_t>(k) + 1U) +
                                       static_cast<std::uint32_t>(n));
                if (i < 0) return;
                L.blocks[static_cast<std::size_t>(i)].civic = static_cast<int>(k);
                taken.push_back(i);
            }
        };
        place(civic::Kind::Church,      std::max(r.churches, 0),       0.0f,  0.45f);
        place(civic::Kind::Hospital,    std::max(r.hospitals, 0),      0.45f, 0.25f);
        place(civic::Kind::Police,      std::max(r.policeStations, 0), 0.25f, 0.35f);
        place(civic::Kind::FireStation, std::max(r.fireStations, 0),   0.55f, 0.25f);
    }
    return L;
}

// --- Streets over water -------------------------------------------------------------

std::vector<StreetRun> streetRuns(const Grid& g, const Street& s,
                                  const std::function<bool(float, float)>& isWater) {
    std::vector<StreetRun> out;
    if (s.pts.size() < 2) return out;
    // Arc length at every control point.
    std::vector<float> arc(s.pts.size(), 0.0f);
    for (std::size_t i = 1; i < s.pts.size(); ++i)
        arc[i] = arc[i - 1] + glm::length(s.pts[i] - s.pts[i - 1]);
    const float total = arc.back();
    auto at = [&](float q) {
        q = glm::clamp(q, 0.0f, total);
        std::size_t i = 1;
        while (i + 1 < s.pts.size() && arc[i] < q) ++i;
        const float seg = std::max(arc[i] - arc[i - 1], 1e-6f);
        return glm::mix(s.pts[i - 1], s.pts[i], (q - arc[i - 1]) / seg);
    };

    // Wet stretches, sampled every 1.5 m -- fine enough for a brook.
    struct Span { float a, b; };
    std::vector<Span> wet;
    if (isWater) {
        const float step = 1.5f;
        bool  in = false;
        float a  = 0.0f;
        for (float q = 0.0f; q <= total + 1e-3f; q += step) {
            const glm::vec2 p = at(q);
            const bool w = isWater(p.x, p.y);
            if (w && !in) { in = true; a = q; }
            if (!w && in) { in = false; wet.push_back({a, q}); }
        }
        if (in) wet.push_back({a, total});
    }

    // Past this much bank on either side of the water, the bridge stands on
    // ground: where a deck begins, and where a broken street stops short.
    constexpr float kBank = 6.0f;
    // Split into kept runs (between breaks), noting the bridges inside each.
    std::vector<Span> keep;
    std::vector<Span> spans;   // bridged stretches
    float from = 0.0f;
    for (const Span& w : wet) {
        const bool touchesEnd = w.a <= 0.5f || w.b >= total - 0.5f;
        const float len = w.b - w.a;
        if (!touchesEnd && g.bridges && len + 2.0f * kBank <= g.maxBridge) {
            spans.push_back({w.a - kBank, w.b + kBank});
            continue;
        }
        keep.push_back({from, w.a - kBank});
        from = w.b + kBank;
    }
    keep.push_back({from, total});

    for (const Span& k : keep) {
        if (k.b - k.a < 12.0f) continue;
        StreetRun run;
        run.width  = s.width;
        run.avenue = s.avenue;
        // The stations to lay a control point at: the run's ends, every grid
        // node inside it, and both ends of every bridge inside it.
        std::vector<float> st{k.a, k.b};
        for (std::size_t i = 0; i < arc.size(); ++i)
            if (arc[i] > k.a + 1.0f && arc[i] < k.b - 1.0f) st.push_back(arc[i]);
        for (const Span& b : spans)
            if (b.a > k.a && b.b < k.b) { st.push_back(b.a); st.push_back(b.b); }
        std::sort(st.begin(), st.end());
        // A node within a couple of metres of a bridge end becomes that end,
        // rather than leaving a stub of a control point beside it.
        std::vector<float> merged;
        for (float q : st)
            if (merged.empty() || q - merged.back() > 2.5f) merged.push_back(q);
            else {
                bool isEnd = false;
                for (const Span& b : spans) isEnd |= (q == b.a || q == b.b);
                if (isEnd) merged.back() = q;
            }
        for (float q : merged) run.pts.push_back(at(q));
        for (const Span& b : spans) {
            if (!(b.a > k.a && b.b < k.b)) continue;
            int ia = -1, ib = -1;
            for (std::size_t i = 0; i < merged.size(); ++i) {
                if (std::abs(merged[i] - b.a) < 2.6f && ia < 0) ia = static_cast<int>(i);
                if (std::abs(merged[i] - b.b) < 2.6f) ib = static_cast<int>(i);
            }
            if (ia >= 0 && ib > ia) run.bridges.push_back({ia, ib});
        }
        if (run.pts.size() >= 2) out.push_back(std::move(run));
    }
    return out;
}

// --- Street names --------------------------------------------------------------------
// German names, the kind every town has: trees and flowers, the church and the
// market, poets and composers, the lie of the land. Drawn from a pool shuffled by
// the street seed, so a town keeps its names while its buildings are rerolled,
// and never twice in one town. The avenue through the middle is the Hauptstraße.
// UTF-8 written as escapes (split where the next letter is a hex digit, or the
// compiler would read it into the escape).
namespace {

const char* const kNamePrefix[] = {
    "Linden", "Eichen", "Birken", "Kastanien", "Ahorn", "Buchen", "Erlen", "Tannen",
    "Ulmen", "Weiden", "Rosen", "Tulpen", "Nelken", "Flieder", "Bahnhof", "Kirch",
    "Markt", "Schul", "Post", "Rathaus", "M\xC3\xBC" "hlen", "Brunnen", "Garten", "Wiesen",
    "Feld", "Wald", "Berg", "Tal", "Bach", "See", "Schloss", "Burg", "Goethe", "Schiller",
    "Beethoven", "Mozart", "Luther", "Kant", "Heine", "D\xC3\xBC" "rer", "Gutenberg",
    "Humboldt", "Kepler", "Brahms", "Bismarck", "Sonnen", "Stern", "Lerchen", "Amsel",
    "Finken", "Falken", "Hirsch", "Fuchs", "Dahlien", "Veilchen", "Holunder", "Hasel",
    "Kiefern", "Fichten", "Obst", "Weinberg", "Hopfen", "Korn", "Hafer", "Sand", "Stein",
    "Kreuz", "Friedens", "Freiheits", "Werk", "Hafen", "Gewerbe", "Handwerker", "Blumen",
    "Schwalben", "Meisen", "Mittel", "Neben", "Quer", "Anger", "Hof", "Wein", "Heide",
    "Moor", "Br\xC3\xBC" "cken", "M\xC3\xBC" "nster", "Kloster", "Zoll", "Turm", "Tor",
};
const char* const kStreet = "stra\xC3\x9F" "e";

} // namespace

std::vector<std::string> streetNames(const Grid& g, const std::vector<Street>& streets) {
    std::vector<std::string> out(streets.size());
    if (g.names == 1) {
        // Real names: a seeded draw from Frankfurt's directory, no repeats.
        const std::vector<std::string>& pool = streetsign::frankfurtNames();
        const auto n = static_cast<std::uint32_t>(pool.size());
        std::vector<std::uint32_t> taken;
        for (std::size_t i = 0; i < streets.size(); ++i) {
            std::uint32_t k = hash3(g.seed ^ 0xf7a1U, static_cast<std::uint32_t>(i), 7U) % n;
            while (std::find(taken.begin(), taken.end(), k) != taken.end()) k = (k + 1) % n;
            taken.push_back(k);
            out[i] = pool[k];
        }
        return out;
    }
    constexpr int kPool = static_cast<int>(sizeof(kNamePrefix) / sizeof(kNamePrefix[0]));
    std::vector<int> order(kPool);
    for (int i = 0; i < kPool; ++i) order[static_cast<std::size_t>(i)] = i;
    for (int i = kPool - 1; i > 0; --i) {                       // seeded shuffle
        const int j = static_cast<int>(hash3(g.seed ^ 0x57ee7U, static_cast<std::uint32_t>(i), 3U) %
                                       static_cast<std::uint32_t>(i + 1));
        std::swap(order[static_cast<std::size_t>(i)], order[static_cast<std::size_t>(j)]);
    }
    // The main street: the avenue nearest the middle of its direction, or the
    // middle line when the town has no avenues.
    int main = -1, bestOff = 1 << 30;
    for (int i = 0; i < static_cast<int>(streets.size()); ++i) {
        const Street& s = streets[static_cast<std::size_t>(i)];
        if (!s.alongZ) continue;
        int count = 0;
        for (const Street& t : streets) count += t.alongZ ? 1 : 0;
        const int off = std::abs(s.line - count / 2) * 2 + (s.avenue ? 0 : 1);
        if (off < bestOff) { bestOff = off; main = i; }
    }
    int next = 0;
    for (int i = 0; i < static_cast<int>(streets.size()); ++i) {
        if (i == main) { out[static_cast<std::size_t>(i)] = std::string("Haupt") + kStreet; continue; }
        const Street& s = streets[static_cast<std::size_t>(i)];
        const std::string pre = kNamePrefix[order[static_cast<std::size_t>(next++ % kPool)]];
        const float pick = unit(hash3(g.seed ^ 0x4a3eU, static_cast<std::uint32_t>(i), 5U));
        const char* suffix = s.avenue ? (pick < 0.5f ? "allee" : kStreet)
                           : pick < 0.55f ? kStreet
                           : pick < 0.80f ? "weg"
                           : pick < 0.90f ? "gasse"
                                          : "ring";
        out[static_cast<std::size_t>(i)] = pre + suffix;
    }
    return out;
}

// --- House types -------------------------------------------------------------------

int houseKinds(Zone z) { return z == Zone::Rows ? 1 : 5; }

housegen::Params housePrototype(const Rule& r, Zone z, int kind) {
    using housegen::Preset;
    housegen::Params p;
    if (z == Zone::Rows) {
        housegen::applyPreset(p, Preset::Townhouse);
        // Party wall to party wall: a gable overhang would poke into the
        // neighbour's roof and fight it for the same pixels.
        p.gableOverhang = 0.0f;
    } else {
        static const Preset kMix[5] = {Preset::Classic, Preset::TownVilla, Preset::Bungalow,
                                       Preset::CountryHouse, Preset::SemiDetached};
        housegen::applyPreset(p, kMix[glm::clamp(kind, 0, 4)]);
    }
    p.facadeColor = r.facadeColor;
    p.roofColor   = r.roofColor;
    p.collider    = false;   // one box per house instead, see derive()
    return p;
}

// --- Lots -------------------------------------------------------------------------

namespace {

// The building line of a block: each side pulled in by its own street's
// half-width plus the pavement, the corners where neighbouring pulled-in sides
// meet. False when there is no block left inside (streets wider than it).
bool buildingLine(const Rule& r, const Block& B, std::vector<glm::vec2>& inset,
                  glm::vec2& icen) {
    glm::vec2 cen(0.0f);
    for (const glm::vec2& c : B.corner) cen += c;
    cen *= 0.25f;
    glm::vec2 lp[4], ld[4];
    for (int k = 0; k < 4; ++k) {
        const glm::vec2 a = B.corner[k], b = B.corner[(k + 1) % 4];
        const glm::vec2 d = glm::normalize(b - a);
        glm::vec2 n(-d.y, d.x);
        if (glm::dot(n, cen - a) < 0.0f) n = -n;
        lp[k] = a + n * (B.streetHalf[k] + std::max(r.sidewalk, 0.0f));
        ld[k] = d;
    }
    inset.assign(4, glm::vec2(0.0f));
    bool ok = true;
    for (int k = 0; k < 4; ++k)
        ok &= intersect(lp[(k + 3) % 4], ld[(k + 3) % 4], lp[k], ld[k], inset[k]);
    if (!ok) return false;
    // Folded through itself? Then the pulled-in outline winds the other way
    // round, and nothing fits.
    float area = 0.0f, area0 = 0.0f;
    for (int k = 0; k < 4; ++k) {
        area  += cross2(inset[k], inset[(k + 1) % 4]);
        area0 += cross2(B.corner[k], B.corner[(k + 1) % 4]);
    }
    if (area * area0 <= 0.0f || std::abs(area) * 0.5f < 150.0f) return false;
    icen = glm::vec2(0.0f);
    for (const glm::vec2& c : inset) icen += c;
    icen *= 0.25f;
    return true;
}

} // namespace


std::vector<Lot> lots(const Rule& r, const Layout& lay) {
    std::vector<Lot> out;
    // House sizes per kind, looked up once rather than per lot.
    const housegen::Params rowP = housePrototype(r, Zone::Rows, 0);
    std::vector<glm::vec2> houseWD;
    for (int k = 0; k < houseKinds(Zone::Houses); ++k) {
        const housegen::Params hp = housePrototype(r, Zone::Houses, k);
        houseWD.push_back({hp.width, hp.depth});
    }

    for (std::size_t bi = 0; bi < lay.blocks.size(); ++bi) {
        const Block& B = lay.blocks[bi];
        if (B.zone == Zone::Park || B.zone == Zone::Industry || B.civic >= 0) continue;

        std::vector<glm::vec2> inset;
        glm::vec2 icen(0.0f);
        if (!buildingLine(r, B, inset, icen)) continue;

        // How deep a lot may reach before it meets the one from the far side: half
        // the block's narrower inside span, less a little light well.
        float span = 1e9f;
        for (int k = 0; k < 4; ++k) {
            const glm::vec2 a = inset[k], d = glm::normalize(inset[(k + 1) % 4] - a);
            const glm::vec2 n(-d.y, d.x);
            for (int m = 0; m < 4; ++m)
                if (m != k && m != (k + 1) % 4)
                    span = std::min(span, std::abs(glm::dot(inset[m] - a, n)));
        }
        const float halfSpan = std::max(span * 0.5f - 1.0f, 4.0f);

        // Longest sides first, so they get the corners.
        int order[4] = {0, 1, 2, 3};
        std::sort(order, order + 4, [&](int x, int y) {
            return glm::length(inset[(x + 1) % 4] - inset[x]) >
                   glm::length(inset[(y + 1) % 4] - inset[y]);
        });

        std::vector<Obb> taken;
        int lotIndex = 0;
        for (int oi = 0; oi < 4; ++oi) {
            const int k = order[oi];
            const glm::vec2 A = inset[k], Bp = inset[(k + 1) % 4];
            const float len = glm::length(Bp - A);
            if (len < 4.0f) continue;
            const glm::vec2 dir = (Bp - A) / len;
            glm::vec2 n(-dir.y, dir.x);
            if (glm::dot(n, icen - A) < 0.0f) n = -n;

            float s = 0.0f;
            int guard = 0;
            while (s < len && guard++ < 400) {
                const std::uint32_t h =
                    hash3(r.seed ^ 0x10757U, static_cast<std::uint32_t>(bi),
                          static_cast<std::uint32_t>(lotIndex * 7 + k));
                Lot lot;
                lot.zone  = B.zone;
                lot.block = static_cast<int>(bi);
                lot.hash  = h;
                // Plot along the street, building width and depth, how far the
                // building stands back, the gap to the next plot, and how deep
                // the plot claims (garden included) -- per zone.
                float plot = 10.0f, bw = 10.0f, bd = 10.0f, back = 0.0f, gap = 0.0f,
                      claim = 10.0f;
                bool  stretch = false;   // the last plot on a side takes up the slack
                switch (B.zone) {
                    case Zone::Towers:
                        plot  = 26.0f + 14.0f * unit(hashU(h ^ 0x1U));
                        bd    = std::min(glm::mix(24.0f, 36.0f, unit(hashU(h ^ 0x2U))), halfSpan);
                        bw    = plot;
                        gap   = 6.0f;
                        claim = bd;
                        break;
                    case Zone::Blocks:
                        plot  = 16.0f + 12.0f * unit(hashU(h ^ 0x1U));
                        bd    = std::min(13.0f, halfSpan);
                        bw    = plot;
                        claim = bd;
                        stretch = true;
                        break;
                    case Zone::Rows:
                        bw    = rowP.width;
                        plot  = rowP.width;
                        bd    = rowP.depth;
                        back  = 3.0f;
                        claim = std::min(back + bd + 6.0f, halfSpan);
                        break;
                    case Zone::Houses:
                    default: {
                        lot.kind = static_cast<int>(hashU(h ^ 0x3U) % houseWD.size());
                        const glm::vec2 wd = houseWD[static_cast<std::size_t>(lot.kind)];
                        bw    = wd.x;
                        bd    = wd.y;
                        plot  = std::max(r.houseLot + 3.0f * sym(hashU(h ^ 0x1U)), bw + 4.0f);
                        back  = std::max(r.houseSetback, 1.0f);
                        claim = std::min(back + bd + 9.0f, halfSpan);
                        break;
                    }
                }
                if (back + bd > halfSpan + 0.5f) break;   // the block is too shallow
                if (s + plot > len) {
                    // What is left of the side: a slab can shorten to it, anything
                    // else simply stops here.
                    const float rest = len - s;
                    if (!stretch || rest < 10.0f) break;
                    plot = bw = rest;
                }
                Obb claimBox;
                claimBox.u  = dir;
                claimBox.hu = plot * 0.5f;
                claimBox.hv = claim * 0.5f;
                claimBox.c  = A + dir * (s + plot * 0.5f) + n * (claim * 0.5f);
                glm::vec2 cs[4];
                claimBox.corners(cs);
                bool fits = true;
                for (const glm::vec2& c : cs) fits &= insideConvex(inset, c, 0.05f);
                for (const Obb& t : taken)
                    if (fits && overlaps(claimBox, t, 0.05f)) fits = false;
                if (!fits) { s += 2.0f; continue; }   // slide on, try again

                taken.push_back(claimBox);
                ++lotIndex;
                lot.width = bw;
                lot.depth = bd;
                lot.front = -n;
                lot.pos   = A + dir * (s + plot * 0.5f) + n * (back + bd * 0.5f);
                out.push_back(lot);
                s += plot + gap;
            }
        }
    }
    return out;
}

// --- Palettes -------------------------------------------------------------------

namespace {

// The shared look of a zone's BuildingGen towers (colours only -- the massing is
// per lot, below).
buildings::Params towerLook(const Rule& r) {
    buildings::Params p;
    p.palette     = glm::clamp(r.towerPalette, 0, 7);
    p.glassTint   = r.glassTint;
    p.baseTint    = r.towerBase;
    p.windowLit   = r.windowLit;
    p.weathering  = r.weathering;
    return p;
}
buildings::Params blockLook(const Rule& r) {
    buildings::Params p;
    p.palette     = glm::clamp(r.blockPalette, 0, 7);
    // The "glass" of an apartment block is its rendered wall: the windows are
    // drawn on it by the lit shader (see MaterialDef::windowGrid), so the facade
    // colour is plaster, not a curtain wall.
    p.glassTint   = r.blockColor;
    p.baseTint    = r.blockBase;
    p.frameTint   = r.blockBase * 0.8f;
    p.windowLit   = r.windowLit;
    p.weathering  = std::max(r.weathering, 0.7f);
    return p;
}

} // namespace

namespace {

// A copy of material `src` under another name and colour -- everything else
// (roughness, the window grid on a block facade) carried over. Find-or-create by
// name like every palette here, so the copy keeps its GUID across rebuilds.
fitzel::AssetId variantOf(std::vector<MaterialDef>& mats, const fitzel::AssetId& src,
                          const std::string& name, glm::vec3 albedo) {
    MaterialDef copy;
    bool found = false;
    for (const MaterialDef& m : mats)
        if (m.assetId == src) { copy = m; found = true; break; }
    for (MaterialDef& m : mats) {
        if (m.name != name) continue;
        const fitzel::AssetId keep = m.assetId;
        if (found) m = copy;
        m.name    = name;
        m.assetId = keep.valid() ? keep : fitzel::AssetId::generate();
        m.albedo  = albedo;
        return m.assetId;
    }
    MaterialDef md = found ? copy : MaterialDef{};
    md.name    = name;
    md.assetId = fitzel::AssetId::generate();
    md.albedo  = albedo;
    mats.push_back(md);
    return md.assetId;
}

// Plaster, roof and block colours a German street actually has: mostly light
// and warm, a few pastels, the odd brick. Chosen per building; the rule's own
// colour is always the first of each list.
const glm::vec3 kHouseWalls[] = {
    {0.93f, 0.88f, 0.76f}, {0.95f, 0.87f, 0.62f}, {0.86f, 0.71f, 0.47f},
    {0.90f, 0.69f, 0.58f}, {0.92f, 0.81f, 0.78f}, {0.79f, 0.79f, 0.77f},
    {0.73f, 0.79f, 0.67f}, {0.73f, 0.80f, 0.86f}, {0.62f, 0.31f, 0.23f},
};
const glm::vec3 kRoofs[] = {
    {0.30f, 0.20f, 0.15f}, {0.21f, 0.22f, 0.24f}, {0.66f, 0.33f, 0.18f},
};
const glm::vec3 kBlockWalls[] = {
    {0.80f, 0.74f, 0.62f}, {0.70f, 0.52f, 0.42f}, {0.62f, 0.66f, 0.70f},
    {0.83f, 0.80f, 0.70f}, {0.58f, 0.62f, 0.52f}, {0.76f, 0.62f, 0.50f},
};

} // namespace

Palettes ensurePalettes(std::vector<MaterialDef>& materials, const Rule& r) {
    Palettes p;
    p.towers = buildings::ensurePalette(materials, towerLook(r));
    p.blocks = buildings::ensurePalette(materials, blockLook(r));
    p.houses = housegen::ensurePalette(materials, housePrototype(r, Zone::Houses, 0),
                                       "City House");
    p.civic = civic::ensurePalette(materials, r.windowLit);
    p.signs = streetsign::ensurePalette(materials, streetsign::presetStyle(r.signStyle));
    // The town builds its houses as shells (see makeHouse), so its glass must not
    // be looked through: a dark, glossy, opaque pane reads as a window from the
    // street and hides the empty inside.
    for (MaterialDef& m : materials)
        if (m.assetId == p.houses.glass) {
            m.albedo       = {0.10f, 0.12f, 0.14f};
            m.reflectivity = 0.30f;
            m.roughness    = 0.06f;
            m.glass        = false;
            m.opacity      = 1.0f;
        }
    // The colour variants: copies of the facade and roof materials (and the
    // block facade, window grid and all), picked per building in derive().
    p.houseWalls = {p.houses.facade};
    for (std::size_t k = 0; k < std::size(kHouseWalls); ++k)
        p.houseWalls.push_back(variantOf(materials, p.houses.facade,
                                         "City House Facade " + std::to_string(k + 2), kHouseWalls[k]));
    p.houseRoofs = {p.houses.roof};
    for (std::size_t k = 0; k < std::size(kRoofs); ++k)
        p.houseRoofs.push_back(variantOf(materials, p.houses.roof,
                                         "City House Roof " + std::to_string(k + 2), kRoofs[k]));
    p.blockWalls = {p.blocks.glass};
    for (std::size_t k = 0; k < std::size(kBlockWalls); ++k)
        p.blockWalls.push_back(variantOf(materials, p.blocks.glass,
                                         "City Block Facade " + std::to_string(k + 2), kBlockWalls[k]));
    return p;
}

// --- Deriving ---------------------------------------------------------------------

namespace {

// One house type, generated once per derive and stamped onto every lot that
// asked for it: HouseGen lays a house out room by room, which is far too much
// work to repeat four hundred times for four hundred copies of the same plan.
struct HouseProto {
    std::vector<std::pair<fitzel::AssetId, fitzel::MeshData>> parts;   // local frame
    float width = 10.0f, depth = 10.0f, eaves = 6.0f;
};

HouseProto makeHouse(const housegen::Params& p, const housegen::Palette& pal) {
    HouseProto hp;
    hp.width = p.width;
    hp.depth = p.depth;
    hp.eaves = p.plinth + p.storeys * p.storeyHeight;
    int counter = 1;
    const std::vector<Entity> es = housegen::generate(p, pal, counter, glm::vec3(0.0f));

    // A town's house is its SHELL. HouseGen builds a house to walk through --
    // stair, doors, plastered partitions, floor boards -- and in a town all of
    // that sits behind glass nobody stands at: measured, the stair and the
    // interior are half of every house, and four hundred houses made the Small
    // Town preset a 110 MB mesh. So the inside is left out (the town's glass is
    // made opaque to match, see ensurePalettes) and what remains is welded.
    auto inside = [&](const fitzel::AssetId& m) {
        return m == pal.interior || m == pal.floor || m == pal.stair || m == pal.door;
    };

    // Weld exact duplicates: HouseGen's quads come out as six corners each, and
    // shared corners are a third of them.
    struct Key {
        glm::vec3 p, n; glm::vec2 uv;
        bool operator<(const Key& o) const {
            if (p.x != o.p.x) return p.x < o.p.x;
            if (p.y != o.p.y) return p.y < o.p.y;
            if (p.z != o.p.z) return p.z < o.p.z;
            if (n.x != o.n.x) return n.x < o.n.x;
            if (n.y != o.n.y) return n.y < o.n.y;
            if (n.z != o.n.z) return n.z < o.n.z;
            if (uv.x != o.uv.x) return uv.x < o.uv.x;
            return uv.y < o.uv.y;
        }
    };
    struct Acc {
        fitzel::MeshData md;
        std::map<Key, std::uint32_t> seen;
    };
    std::map<fitzel::AssetId, Acc> byMat;
    for (std::size_t i = 1; i < es.size(); ++i) {
        const Entity& e = es[i];
        const auto* mc = e.components.get<MeshComponent>();
        if (!mc) continue;
        if (e.name == "Doors" || e.name == "Stair") continue;
        fitzel::AssetId own;
        if (const auto* mat = e.components.get<MaterialComponent>()) own = mat->material;
        for (const editmesh::Group& g : editmesh::buildGroups(mc->mesh)) {
            const fitzel::AssetId m = g.material.valid() ? g.material : own;
            if (!m.valid() || inside(m)) continue;
            Acc& acc = byMat[m];
            auto put = [&](const fitzel::Vertex& src) {
                fitzel::Vertex v = src;
                v.position += e.localCenter;
                const Key k{v.position, v.normal, v.uv};
                auto it = acc.seen.find(k);
                if (it == acc.seen.end()) {
                    it = acc.seen.emplace(k, static_cast<std::uint32_t>(acc.md.vertices.size())).first;
                    acc.md.vertices.push_back(v);
                }
                acc.md.indices.push_back(it->second);
            };
            if (g.data.indices.empty())
                for (const fitzel::Vertex& v : g.data.vertices) put(v);
            else
                for (std::uint32_t idx : g.data.indices) put(g.data.vertices[idx]);
        }
    }
    for (auto& [m, acc] : byMat) hp.parts.emplace_back(m, std::move(acc.md));
    return hp;
}

} // namespace

Town derive(const Rule& r, const Palettes& pal, const Context& ctx) {
    Town out;
    const Layout lay = layout(r);
    const std::vector<Lot> plan = lots(r, lay);
    out.stats.lots = static_cast<int>(plan.size());
    for (const Block& b : lay.blocks)
        if (b.zone == Zone::Park) ++out.stats.parks;

    SegGrid roads;
    roads.build(ctx.roads);
    auto ground = [&](glm::vec2 p) { return ctx.groundAt ? ctx.groundAt(p.x, p.y) : 0.0f; };
    auto wet    = [&](glm::vec2 p) { return ctx.isWater && ctx.isWater(p.x, p.y); };

    // House types, made on first use.
    std::map<std::pair<int, int>, HouseProto> protos;
    auto proto = [&](Zone z, int kind) -> const HouseProto& {
        const auto key = std::make_pair(static_cast<int>(z), kind);
        auto it = protos.find(key);
        if (it == protos.end())
            it = protos.emplace(key, makeHouse(housePrototype(r, z, kind), pal.houses)).first;
        return it->second;
    };

    std::vector<city::Piece> pcs;
    std::vector<int>         pcChunk;
    std::vector<city::Extra> extras;
    const int budget = std::max(r.budget, 0);

    // --- Public buildings and industry: a whole block each ----------------------
    // The plot is the block's building line, squared up with its longest side:
    // the biggest rectangle of that orientation that fits, found by shrinking
    // until all four corners are inside. The street the building addresses is
    // the one along that longest side.
    std::deque<civic::Model> models;   // stable addresses: the extras point into them
    for (std::size_t bi = 0; bi < lay.blocks.size(); ++bi) {
        const Block& B = lay.blocks[bi];
        const bool estate = B.zone == Zone::Industry;
        if (!estate && B.civic < 0) continue;
        if (static_cast<int>(out.placed.size()) >= budget) { out.stats.budgetHit = true; break; }
        std::vector<glm::vec2> inset;
        glm::vec2 icen(0.0f);
        if (!buildingLine(r, B, inset, icen)) continue;
        int   longK = 0;
        float longL = 0.0f;
        for (int k = 0; k < 4; ++k) {
            const float l = glm::length(inset[(k + 1) % 4] - inset[k]);
            if (l > longL) { longL = l; longK = k; }
        }
        const glm::vec2 u = glm::normalize(inset[(longK + 1) % 4] - inset[longK]);
        glm::vec2 n(-u.y, u.x);                                  // into the block
        if (glm::dot(n, icen - inset[longK]) < 0.0f) n = -n;
        float span = 0.0f;
        for (const glm::vec2& c : inset) span = std::max(span, glm::dot(c - inset[longK], n));
        auto fits = [&](float w, float d) {
            Obb o;
            o.c = icen; o.u = u; o.hu = 0.5f * w; o.hv = 0.5f * d;
            glm::vec2 cs[4];
            o.corners(cs);
            for (const glm::vec2& c : cs)
                if (!insideConvex(inset, c, 0.02f)) return false;
            return true;
        };
        float lo = 0.0f, hi = 1.0f;
        for (int it = 0; it < 18; ++it) {
            const float m = 0.5f * (lo + hi);
            if (fits(longL * m, span * m)) lo = m; else hi = m;
        }
        // Then let the depth grow on its own: a long, shallow block gives up
        // width to its corners, not depth.
        float W = longL * lo, D = span * lo;
        for (int it = 0; it < 12 && D * 1.05f <= span && fits(W, D * 1.05f); ++it) D *= 1.05f;
        W *= 0.97f;
        D *= 0.97f;
        const glm::vec2 front = -n;   // towards the street on the longest side

        // The ground, measured over the plot like a lot's.
        const glm::vec2 side(-front.y, front.x);
        float gLo = 1e9f, gHi = -1e9f;
        bool inRoad = false, inWater = false;
        for (int a = -1; a <= 1; ++a)
            for (int c = -1; c <= 1; ++c) {
                const glm::vec2 p = icen + side * (static_cast<float>(a) * 0.5f * W) +
                                    front * (static_cast<float>(c) * 0.5f * D);
                const float g = ground(p);
                gLo = std::min(gLo, g);
                gHi = std::max(gHi, g);
                if (roads.clearance(p) < 0.8f) inRoad = true;
                if (wet(p)) inWater = true;
            }
        if (inRoad)  { ++out.stats.skippedRoad;  continue; }
        if (inWater) { ++out.stats.skippedWater; continue; }
        const float diag = std::sqrt(W * W + D * D);
        if ((gHi - gLo) / std::max(diag, 1.0f) > std::max(r.maxSlope, 0.01f) * 1.5f) {
            ++out.stats.skippedSlope;
            continue;
        }

        const civic::Kind kind = estate ? civic::Kind::Industry
                                        : static_cast<civic::Kind>(B.civic);
        const std::uint32_t h = hash3(r.seed ^ 0xc1c1cU, static_cast<std::uint32_t>(bi), 7U);
        models.push_back(civic::build(kind, W, D, h, pal.civic));
        const civic::Model& M = models.back();
        if (M.empty()) { ++out.stats.skippedEmpty; continue; }

        // Stood on the HIGH side like a house, the drop filled by a plinth under
        // every solid that meets the ground.
        const float yaw   = yawFacing(front);
        const float baseY = gHi;
        const int   chunk = chunkOf(icen);
        const float rr = glm::radians(yaw), cs = std::cos(rr), sn = std::sin(rr);
        auto toWorld = [&](glm::vec3 l) {
            return glm::vec3(icen.x + l.x * cs + l.z * sn, baseY + l.y,
                             icen.y - l.x * sn + l.z * cs);
        };
        for (const auto& [mat, md] : M.parts) {
            city::Extra x;
            x.mesh     = &md;
            x.at       = glm::vec3(icen.x, baseY, icen.y);
            x.yaw      = yaw;
            x.material = mat;
            x.chunk    = chunk;
            extras.push_back(x);
        }
        for (const city::Piece& s : M.solids) {
            city::Piece w = s;
            w.center  = toWorld(s.center);
            w.yaw     = yaw;
            w.collide = r.collider;
            if (w.collide) pcs.push_back(w);
            if (gHi - gLo > 0.05f && s.center.y - s.half.y < 0.01f) {
                city::Piece f;
                const float bottom = gLo - 0.4f;
                f.center   = glm::vec3(w.center.x, 0.5f * (bottom + baseY), w.center.z);
                f.half     = glm::vec3(s.half.x, 0.5f * (baseY - bottom), s.half.z);
                f.yaw      = yaw;
                f.material = pal.civic.concrete;
                pcs.push_back(f);
            }
        }
        pcChunk.resize(pcs.size(), chunk);

        switch (kind) {
            case civic::Kind::Church:      ++out.stats.churches; break;
            case civic::Kind::Police:      ++out.stats.police; break;
            case civic::Kind::FireStation: ++out.stats.fireStations; break;
            case civic::Kind::Hospital:    ++out.stats.hospitals; break;
            default:                       ++out.stats.industry; break;
        }
        Placed pl;
        pl.pos    = icen;
        pl.radius = 0.5f * diag + 1.5f;
        pl.zone   = B.zone;
        out.placed.push_back(pl);
        ++out.stats.built;
    }

    for (const Lot& lot : plan) {
        if (static_cast<int>(out.placed.size()) >= budget) { out.stats.budgetHit = true; break; }
        const std::uint32_t h = lot.hash;
        if (unit(hashU(h ^ 0x1dU)) > r.fill) { ++out.stats.skippedEmpty; continue; }

        // --- The footprint, measured ----------------------------------------------
        const glm::vec2 u(-lot.front.y, lot.front.x);   // along the street
        const glm::vec2 fw = lot.front;                  // towards it
        glm::vec2 smp[9];
        int ns = 0;
        for (int a = -1; a <= 1; ++a)
            for (int b = -1; b <= 1; ++b)
                smp[ns++] = lot.pos + u * (a * lot.width * 0.5f) + fw * (b * lot.depth * 0.5f);
        bool inRoad = false, inWater = false;
        for (int k = 0; k < ns; ++k) {
            if (roads.clearance(smp[k]) < 0.8f) inRoad = true;
            if (wet(smp[k])) inWater = true;
        }
        if (inRoad)  { ++out.stats.skippedRoad;  continue; }
        if (inWater) { ++out.stats.skippedWater; continue; }
        float gLo = 1e9f, gHi = -1e9f;
        for (int k = 0; k < ns; ++k) {
            const float g = ground(smp[k]);
            gLo = std::min(gLo, g);
            gHi = std::max(gHi, g);
        }
        const float diag = std::max(std::sqrt(lot.width * lot.width + lot.depth * lot.depth), 1.0f);
        if ((gHi - gLo) / diag > std::max(r.maxSlope, 0.01f)) { ++out.stats.skippedSlope; continue; }

        const float yaw   = yawFacing(lot.front);
        const int   chunk = chunkOf(lot.pos);

        if (lot.zone == Zone::Towers || lot.zone == Zone::Blocks) {
            // --- BuildingGen ----------------------------------------------------
            const bool tower = lot.zone == Zone::Towers;
            buildings::Params bp = tower ? towerLook(r) : blockLook(r);
            if (tower) {
                // Mostly the classic setback tower, some slabs, the odd needle.
                const float pick = unit(hashU(h ^ 0x5U));
                buildings::applyStyle(bp, pick < 0.62f ? buildings::Style::Skyscraper
                                        : pick < 0.90f ? buildings::Style::Slab
                                                       : buildings::Style::Needle);
                // Tallest at the heart, with enough per-lot scatter that the
                // skyline has peaks rather than a cone.
                const float ring = r.towerRing > 1e-3f
                    ? glm::clamp(lay.blocks[static_cast<std::size_t>(lot.block)].ring / r.towerRing, 0.0f, 1.0f)
                    : 1.0f;
                float t = glm::clamp(0.65f * (1.0f - ring) + 0.35f * unit(hashU(h ^ 0x6U)), 0.0f, 1.0f);
                t = t * t * (3.0f - 2.0f * t);
                const int fMin = std::max(1, std::min(r.towerFloorsMin, r.towerFloorsMax));
                const int fMax = std::max(fMin, std::max(r.towerFloorsMin, r.towerFloorsMax));
                bp.floors = fMin + static_cast<int>(std::lround((fMax - fMin) * t));
                // The podium is what meets the pavement, so it is the lot; the
                // shaft is divided down to stand on it (see CityGen's note).
                const float spread = bp.podiumFloors > 0 ? glm::clamp(bp.podiumSpread, 1.0f, 4.0f) : 1.0f;
                bp.width = lot.width / spread;
                bp.depth = lot.depth / spread;
                bp.twist = 0.0f;
                bp.neon  = false;
                bp.edgeStrips = false;
                bp.signs      = 0;
                bp.screens    = false;
                bp.roofSign   = false;
                bp.shopfronts = true;
                bp.clutter    = 3;
                ++out.stats.towers;
            } else {
                buildings::applyStyle(bp, buildings::Style::Slab);
                const int fMin = std::max(1, std::min(r.blockFloorsMin, r.blockFloorsMax));
                const int fMax = std::max(fMin, std::max(r.blockFloorsMin, r.blockFloorsMax));
                bp.floors       = fMin + static_cast<int>(hashU(h ^ 0x7U) % static_cast<std::uint32_t>(fMax - fMin + 1));
                bp.floorHeight  = 3.0f;
                bp.shape        = buildings::Footprint::Rect;
                bp.width        = lot.width;
                bp.depth        = lot.depth;
                bp.sections     = 1;
                bp.taper        = 1.0f;
                bp.jitter       = 0.0f;
                bp.twist        = 0.0f;
                bp.podiumFloors = 0;
                bp.bandEvery    = 0;
                bp.fins         = 0;
                bp.crown        = unit(hashU(h ^ 0x8U)) < 0.5f ? buildings::Crown::None
                                                                : buildings::Crown::Cap;
                bp.crownHeight  = 0.06f;
                bp.neon         = false;
                bp.edgeStrips   = false;
                bp.signs        = 0;
                bp.screens      = false;
                bp.roofSign     = false;
                bp.shopfronts   = lay.blocks[static_cast<std::size_t>(lot.block)].ring < r.blockRing * 0.6f;
                bp.clutter      = 2;
                bp.windowWidth  = 2.2f;
                ++out.stats.blocks;
            }
            bp.seed        = h | 1U;
            bp.sink        = std::max(0.4f, (gHi - gLo) * 0.6f);
            bp.beaconLight = false;
            bp.collider    = r.collider;
            int counter = 0;
            const std::vector<Entity> es = buildings::generate(
                bp, tower ? pal.towers : pal.blocks, counter, glm::vec3(0.0f));
            glm::vec3 lo(1e9f), hi(-1e9f);
            const std::size_t first = pcs.size();
            city::flatten(es, glm::vec3(lot.pos.x, gLo, lot.pos.y), yaw, pcs, lo, hi);
            // An apartment block in its own colour, window grid and all.
            if (!tower && pal.blockWalls.size() > 1 &&
                unit(hashU(h ^ 0xc0c0U)) < glm::clamp(r.colourVariety, 0.0f, 1.0f)) {
                const fitzel::AssetId wall = pal.blockWalls[1 + hashU(h ^ 0xc0c1U) %
                    static_cast<std::uint32_t>(pal.blockWalls.size() - 1)];
                for (std::size_t k = first; k < pcs.size(); ++k)
                    if (pcs[k].material == pal.blocks.glass) pcs[k].material = wall;
            }
        } else {
            // --- HouseGen -------------------------------------------------------
            // Stood on the HIGH corner, never buried: a front door below ground is
            // worse than a plinth that shows. The foundation under it fills the
            // drop down to the low corner.
            const HouseProto& hp = proto(lot.zone, lot.kind);
            const float baseY = gHi;
            // Each house its own plaster and, less often, its own roof -- the
            // colours are what tells one copy of a house type from the next.
            const float variety = glm::clamp(r.colourVariety, 0.0f, 1.0f);
            fitzel::AssetId wall = pal.houses.facade, roof = pal.houses.roof;
            if (pal.houseWalls.size() > 1 && unit(hashU(h ^ 0xc0c2U)) < variety)
                wall = pal.houseWalls[1 + hashU(h ^ 0xc0c3U) %
                                      static_cast<std::uint32_t>(pal.houseWalls.size() - 1)];
            if (pal.houseRoofs.size() > 1 && unit(hashU(h ^ 0xc0c4U)) < variety * 0.6f)
                roof = pal.houseRoofs[1 + hashU(h ^ 0xc0c5U) %
                                      static_cast<std::uint32_t>(pal.houseRoofs.size() - 1)];
            for (const auto& [mat, md] : hp.parts) {
                city::Extra x;
                x.mesh = &md;
                x.at   = glm::vec3(lot.pos.x, baseY, lot.pos.y);
                x.yaw  = yaw;
                x.material = mat == pal.houses.facade ? wall : mat == pal.houses.roof ? roof : mat;
                x.chunk    = chunk;
                extras.push_back(x);
            }
            if (gHi - gLo > 0.05f) {
                city::Piece f;
                const float bottom = gLo - 0.4f;
                f.center   = glm::vec3(lot.pos.x, 0.5f * (bottom + baseY), lot.pos.y);
                f.half     = glm::vec3(hp.width * 0.5f, 0.5f * (baseY - bottom), hp.depth * 0.5f);
                f.yaw      = yaw;
                f.material = pal.houses.plinth;
                pcs.push_back(f);
            }
            if (r.collider) {
                city::Piece c;   // one box to walk and drive into, not a mesh collider
                c.center  = glm::vec3(lot.pos.x, baseY + hp.eaves * 0.5f, lot.pos.y);
                c.half    = glm::vec3(hp.width * 0.5f, hp.eaves * 0.5f, hp.depth * 0.5f);
                c.yaw     = yaw;
                c.collide = true;   // no material: collides, draws nothing
                pcs.push_back(c);
            }
            if (lot.zone == Zone::Rows) ++out.stats.rows; else ++out.stats.houses;
        }
        pcChunk.resize(pcs.size(), chunk);

        Placed pl;
        pl.pos    = lot.pos;
        pl.radius = 0.5f * diag + 1.5f;
        pl.zone   = lot.zone;
        out.placed.push_back(pl);
        ++out.stats.built;
    }

    // --- Pavements: the sidewalk band of every block, paved, on a kerb ---------
    // Each block side gets a strip from the carriageway's edge to the building
    // line. The edge is the REAL road's (snapped to the laid spline, which bows
    // between the grid's nodes), the strip stops where a crossing's apron starts
    // -- the apron reaches `margin` past the far carriageway, so the corner is
    // cut on the same line the asphalt ends -- and the corner is closed by a fan
    // to where the two building lines meet. Dropped over water and where some
    // other road runs through the block.
    constexpr float kKerb = 0.12f;
    const bool paved = r.pavements && r.sidewalk > 0.3f;
    // The slabs ride on the highest ground within a stride: the band reaches past
    // the road's graded corridor onto natural ground, and a hump between two
    // samples would otherwise show through the paving. In a hollow the kerb and
    // the inner edge reach down to the real ground and close the gap.
    auto ride = [&](glm::vec2 p) {
        float g = -1e9f;
        for (int a = -1; a <= 1; ++a)
            for (int c = -1; c <= 1; ++c)
                g = std::max(g, ground(p + glm::vec2(a * 0.75f, c * 0.75f)));
        return g;
    };
    auto standY = [&](glm::vec2 p) { return paved ? ride(p) + kKerb : ground(p); };
    std::deque<fitzel::MeshData> paveMeshes;   // stable addresses for the extras
    if (paved) {
        const float w      = r.sidewalk;
        const float margin = 1.0f;   // RoadJunction::Params::margin, the roads' default
        const float minSin = std::sin(glm::radians(22.0f));   // ... and minAngleDeg
        auto addFace = [](fitzel::MeshData& md, std::vector<glm::vec3> p, glm::vec3 outward) {
            glm::vec3 n(0.0f);
            for (std::size_t i = 0; i < p.size(); ++i) {
                const glm::vec3& u = p[i];
                const glm::vec3& v = p[(i + 1) % p.size()];
                n += glm::vec3((u.y - v.y) * (u.z + v.z), (u.z - v.z) * (u.x + v.x),
                               (u.x - v.x) * (u.y + v.y));
            }
            if (glm::dot(n, n) < 1e-12f) return;
            if (glm::dot(n, outward) < 0.0f) { std::reverse(p.begin(), p.end()); n = -n; }
            n = glm::normalize(n);
            const auto base = static_cast<std::uint32_t>(md.vertices.size());
            const bool up = std::abs(n.y) > 0.5f;
            for (const glm::vec3& v : p) {
                fitzel::Vertex vx{};
                vx.position = v;
                vx.normal   = n;
                // Paving slabs: half a metre a tile on top, the kerb stones along.
                vx.uv = up ? glm::vec2(v.x, v.z) * 0.5f
                           : glm::vec2(std::abs(n.x) > std::abs(n.z) ? v.z : v.x, v.y) * 0.5f;
                md.vertices.push_back(vx);
            }
            for (std::uint32_t i = 1; i + 1 < p.size(); ++i) {
                md.indices.push_back(base);
                md.indices.push_back(base + i);
                md.indices.push_back(base + i + 1);
            }
        };
        auto top = [&](glm::vec2 p) { return glm::vec3(p.x, ride(p) + kKerb, p.y); };
        auto low = [&](glm::vec2 p) { return glm::vec3(p.x, ground(p) - 0.15f, p.y); };
        auto usable = [&](std::initializer_list<glm::vec2> pts) {
            glm::vec2 c(0.0f);
            for (const glm::vec2& p : pts) {
                // Every corner off every carriageway (the kerb's own sit ON its
                // road's edge, hence the hair of tolerance), and dry.
                if (wet(p) || roads.clearance(p) < -0.02f) return false;
                c += p;
            }
            c /= static_cast<float>(pts.size());
            return roads.clearance(c) > 0.2f;   // not on some other road
        };
        for (std::size_t bi = 0; bi < lay.blocks.size(); ++bi) {
            const Block& B = lay.blocks[bi];
            glm::vec2 cen(0.0f);
            for (const glm::vec2& c : B.corner) cen += c;
            cen *= 0.25f;
            struct Side {
                std::vector<glm::vec2> O, I;   // kerb and building-line points
                glm::vec2 d{0.0f}, n{0.0f};
            } sd[4];
            glm::vec2 dir[4];
            for (int k = 0; k < 4; ++k) {
                const glm::vec2 e = B.corner[(k + 1) % 4] - B.corner[k];
                dir[k] = glm::length(e) > 1e-4f ? glm::normalize(e) : glm::vec2(1.0f, 0.0f);
            }
            for (int k = 0; k < 4; ++k) {
                const glm::vec2 a0 = B.corner[k], a1 = B.corner[(k + 1) % 4];
                const float len = glm::length(a1 - a0);
                const glm::vec2 d = dir[k];
                glm::vec2 n(-d.y, d.x);
                if (glm::dot(n, cen - a0) < 0.0f) n = -n;
                // Where the strip may start: past the crossing street's far
                // edge. At a slanted corner the kerb (offset by this street's own
                // half) reaches back towards the other carriageway by h*cos, so
                // that goes on top of the apron's h/sin -- else an acute corner's
                // pavement runs over the other road for a metre.
                auto clear = [&](int other, glm::vec2 od) {
                    const float sn = std::max(std::abs(cross2(d, od)), minSin);
                    const float cs = std::abs(glm::dot(d, od));
                    return (B.streetHalf[other] + B.streetHalf[k] * cs) / sn + margin;
                };
                float s0 = clear((k + 3) % 4, dir[(k + 3) % 4]);
                float s1 = len - clear((k + 1) % 4, dir[(k + 1) % 4]);
                sd[k].d = d;
                sd[k].n = n;
                // The kerb at `t` along the side: on the real road's edge.
                auto kerbAt = [&](float t) {
                    const glm::vec2 c = a0 + d * t;
                    glm::vec2 at, rd;
                    float half = 0.0f;
                    if (!roads.snap(c, d, at, rd, half)) return c + n * B.streetHalf[k];
                    glm::vec2 rn(-rd.y, rd.x);
                    if (glm::dot(rn, n) < 0.0f) rn = -rn;
                    return at + rn * half;
                };
                // Then measured, like a lot: the ends move in until the kerb is
                // clear of every carriageway but its own. What the corner rule
                // above cannot know -- a street bending at the node, a stub
                // leaving the town at an angle -- this catches.
                auto blocked = [&](float t) {
                    return roads.clearance(kerbAt(t) + n * 0.05f) < 0.03f;
                };
                for (int g = 0; g < 40 && s1 - s0 > 1.0f && blocked(s0); ++g) s0 += 0.25f;
                for (int g = 0; g < 40 && s1 - s0 > 1.0f && blocked(s1); ++g) s1 -= 0.25f;
                if (s1 - s0 < 1.0f) continue;
                const int m = std::max(1, static_cast<int>(std::ceil((s1 - s0) / 2.0f)));
                for (int i = 0; i <= m; ++i) {
                    const glm::vec2 o = kerbAt(s0 + (s1 - s0) * static_cast<float>(i) / m);
                    sd[k].O.push_back(o);
                    sd[k].I.push_back(o + n * w);
                }
                // Sampled every 2 m to follow the ground and the road's bow; on
                // flat, straight stretches most of those samples say nothing.
                // Keep a sample only where leaving it out would move the kerb or
                // either edge's height by more than 2 cm (and every 15 m, so a
                // long slab still meets the terrain's own undulation).
                std::vector<glm::vec2>& O = sd[k].O;
                std::vector<glm::vec2>& I = sd[k].I;
                auto off = [&](const std::vector<glm::vec2>& L, std::size_t a, std::size_t b,
                               std::size_t i) {
                    const glm::vec2 ab = L[b] - L[a];
                    const float l2 = glm::dot(ab, ab);
                    const float t  = l2 > 1e-8f ? glm::dot(L[i] - L[a], ab) / l2 : 0.0f;
                    const glm::vec2 q = L[a] + ab * t;
                    const float gy = glm::mix(ride(L[a]), ride(L[b]), t);
                    return std::max(glm::length(L[i] - q), std::abs(ride(L[i]) - gy));
                };
                std::vector<std::size_t> keep{0};
                for (std::size_t j = 2; j < O.size(); ++j) {
                    const std::size_t a = keep.back();
                    bool ok = glm::length(O[j] - O[a]) <= 15.0f;
                    for (std::size_t i = a + 1; i < j && ok; ++i)
                        ok = off(O, a, j, i) < 0.02f && off(I, a, j, i) < 0.02f;
                    if (!ok) keep.push_back(j - 1);
                }
                if (O.size() > 1) keep.push_back(O.size() - 1);
                std::vector<glm::vec2> O2, I2;
                for (std::size_t i : keep) { O2.push_back(O[i]); I2.push_back(I[i]); }
                O = std::move(O2);
                I = std::move(I2);
            }
            // Slabs and kerb stones in one material: a second one would be a
            // second draw in every chunk of the town.
            paveMeshes.emplace_back();
            fitzel::MeshData& slab = paveMeshes.back();
            for (int k = 0; k < 4; ++k) {
                const Side& S = sd[k];
                for (std::size_t i = 0; i + 1 < S.O.size(); ++i) {
                    const glm::vec2 o0 = S.O[i], o1 = S.O[i + 1], i0 = S.I[i], i1 = S.I[i + 1];
                    if (!usable({o0, o1, i1, i0})) continue;
                    addFace(slab, {top(o0), top(o1), top(i1), top(i0)}, {0, 1, 0});
                    addFace(slab, {low(o0), low(o1), top(o1), top(o0)}, glm::vec3(-S.n.x, 0, -S.n.y));
                    addFace(slab, {low(i0), low(i1), top(i1), top(i0)}, glm::vec3(S.n.x, 0, S.n.y));
                    // Open ends where the strip breaks off (water, a foreign road).
                    if (i == 0 || !usable({S.O[i - 1], o0, i0, S.I[i - 1]}))
                        addFace(slab, {low(o0), low(i0), top(i0), top(o0)}, glm::vec3(-S.d.x, 0, -S.d.y));
                    if (i + 2 == S.O.size() || !usable({o1, S.O[i + 2], S.I[i + 2], i1}))
                        addFace(slab, {low(o1), low(i1), top(i1), top(o1)}, glm::vec3(S.d.x, 0, S.d.y));
                }
                // The corner at the start of side k, between side k-1's last
                // points and side k's first.
                const Side& P = sd[(k + 3) % 4];
                if (P.O.size() < 2 || S.O.size() < 2) continue;
                const glm::vec2 po = P.O.back(), pi = P.I.back(), so = S.O.front(), si = S.I.front();
                // Only onto strips that are there: a corner hung on a dropped
                // end quad would float beside the gap as a step.
                if (!usable({P.O[P.O.size() - 2], po, pi, P.I[P.I.size() - 2]}) ||
                    !usable({so, S.O[1], S.I[1], si}))
                    continue;
                glm::vec2 c;
                if (!intersect(pi, P.d, si, S.d, c)) continue;
                if (!usable({po, so, si, c, pi})) continue;
                addFace(slab, {top(c), top(pi), top(po)}, {0, 1, 0});
                addFace(slab, {top(c), top(po), top(so)}, {0, 1, 0});
                addFace(slab, {top(c), top(so), top(si)}, {0, 1, 0});
                const glm::vec2 ch = so - po;
                glm::vec2 cn(-ch.y, ch.x);
                if (glm::dot(cn, c - po) > 0.0f) cn = -cn;
                addFace(slab, {low(po), low(so), top(so), top(po)}, glm::vec3(cn.x, 0, cn.y));
                // The corner's inner edge, where the fan meets the plots.
                addFace(slab, {low(pi), low(c), top(c), top(pi)}, glm::vec3(P.n.x, 0, P.n.y));
                addFace(slab, {low(c), low(si), top(si), top(c)}, glm::vec3(S.n.x, 0, S.n.y));
            }
            const int chunk = chunkOf(cen);
            if (!slab.vertices.empty()) {
                city::Extra x;
                x.mesh     = &slab;
                x.material = pal.civic.pavement;
                x.chunk    = chunk;
                extras.push_back(x);
            }
        }
    }

    // --- At the crossings: street-name signs and traffic lights -----------------
    // The names are the ROADS' names where the roads are laid -- so renaming a
    // street in the Roads panel renames its signs -- and the town's own before
    // that (a preview has no roads to ask).
    std::vector<int> zLine(static_cast<std::size_t>(lay.nx + 1), -1);
    std::vector<int> xLine(static_cast<std::size_t>(lay.nz + 1), -1);
    for (int s = 0; s < static_cast<int>(lay.streets.size()); ++s) {
        const Street& st = lay.streets[static_cast<std::size_t>(s)];
        auto& line = st.alongZ ? zLine : xLine;
        if (st.line >= 0 && st.line < static_cast<int>(line.size()))
            line[static_cast<std::size_t>(st.line)] = s;
    }
    auto node = [&](int i, int j) {
        return lay.nodes[static_cast<std::size_t>(j * (lay.nx + 1) + i)];
    };
    auto nameOf = [&](int street, glm::vec2 p, glm::vec2 d) {
        const int road = roads.nearestParallel(p, d);
        if (road >= 0 && !ctx.roads[static_cast<std::size_t>(road)].name.empty())
            return ctx.roads[static_cast<std::size_t>(road)].name;
        return lay.streets[static_cast<std::size_t>(street)].name;
    };
    // A furniture model stood at `at` facing `front`, its solids colliding.
    // Merged in kilometre squares, not the buildings' 250 m: a light or a stop
    // is a few hundred vertices in half a dozen materials, and per 250 m chunk
    // those materials cost more draws than all the town's houses. Drawing a
    // kilometre of them when one is in view is the cheaper side of that trade.
    auto place = [&](const civic::Model& M, glm::vec2 at, glm::vec2 front, float radius) {
        const float yaw = yawFacing(front);
        const float y   = standY(at);
        const int chunk = (1 << 28) + static_cast<int>(std::floor(at.x / 1000.0f)) * 8192 +
                          static_cast<int>(std::floor(at.y / 1000.0f));
        const float rr = glm::radians(yaw), cs = std::cos(rr), sn = std::sin(rr);
        for (const auto& [mat, md] : M.parts) {
            city::Extra x;
            x.mesh     = &md;
            x.at       = glm::vec3(at.x, y, at.y);
            x.yaw      = yaw;
            x.material = mat;
            x.chunk    = chunk;
            extras.push_back(x);
        }
        if (r.collider)
            for (const city::Piece& s : M.solids) {
                city::Piece p = s;
                p.center = glm::vec3(at.x + s.center.x * cs + s.center.z * sn, y + s.center.y,
                                     at.y - s.center.x * sn + s.center.z * cs);
                p.yaw = yaw;
                pcs.push_back(p);
                pcChunk.push_back(chunk);
            }
        Placed pl;
        pl.pos    = at;
        pl.radius = radius;
        out.furniture.push_back(pl);
    };
    auto freeGround = [&](glm::vec2 p) { return roads.clearance(p) > 0.25f && !wet(p); };
    auto zoneAt = [&](int bx, int bz) {
        if (bx < 0 || bz < 0 || bx >= lay.nx || bz >= lay.nz) return Zone::Park;
        for (const Block& b : lay.blocks)
            if (b.ix == bx && b.iz == bz) return b.zone;
        return Zone::Park;
    };
    std::deque<std::vector<std::pair<fitzel::AssetId, fitzel::MeshData>>> signModels;
    streetsign::Params sign;
    sign.style = streetsign::presetStyle(r.signStyle);
    for (int j = 0; j <= lay.nz && lay.nx > 0; ++j)
        for (int i = 0; i <= lay.nx; ++i) {
            const int sz = zLine[static_cast<std::size_t>(i)];
            const int sx = xLine[static_cast<std::size_t>(j)];
            if (sz < 0 || sx < 0) continue;
            const glm::vec2 p  = node(i, j);
            const glm::vec2 dZ = glm::normalize(node(i, std::min(j + 1, lay.nz)) -
                                                node(i, std::max(j - 1, 0)));
            const glm::vec2 dX = glm::normalize(node(std::min(i + 1, lay.nx), j) -
                                                node(std::max(i - 1, 0), j));
            const Street& SZ = lay.streets[static_cast<std::size_t>(sz)];
            const Street& SX = lay.streets[static_cast<std::size_t>(sx)];
            const float hz = 0.5f * SZ.width;
            const float hx = 0.5f * SX.width;

            if (r.signs) {
                // The first corner that is pavement: not carriageway, not water.
                bool found = false;
                glm::vec2 post(0.0f);
                for (int q = 0; q < 4 && !found; ++q) {
                    const float a = (q & 1) ? -1.0f : 1.0f, b = (q & 2) ? -1.0f : 1.0f;
                    post = p + dX * (a * (hz + 1.3f)) + dZ * (b * (hx + 1.3f));
                    found = freeGround(post);
                }
                if (found) {
                    // Blade A along the X street, blade B turned onto the Z street;
                    // the whole post is then yawed so A lies along dX (Extra's yaw
                    // maps local +x to (cos, -sin)).
                    const float yawA = std::atan2(-dX.y, dX.x);
                    const float yawB = std::atan2(-dZ.y, dZ.x);
                    sign.textA  = nameOf(sx, p, dX);
                    sign.textB  = nameOf(sz, p, dZ);
                    sign.angleB = glm::degrees(yawB - yawA);
                    signModels.push_back(streetsign::meshes(streetsign::build(sign, pal.signs)));
                    const int chunk = chunkOf(post);
                    for (const auto& [mat, md] : signModels.back()) {
                        city::Extra x;
                        x.mesh     = &md;
                        x.at       = glm::vec3(post.x, standY(post), post.y);
                        x.yaw      = glm::degrees(yawA);
                        x.material = mat;
                        x.chunk    = chunk;
                        // The lettering lies on the plate: its shadow is the plate's.
                        x.castsShadow = mat != pal.signs.ink;
                        extras.push_back(x);
                    }
                    ++out.stats.signs;
                }
            }

            // Traffic lights where an avenue crosses, or where the blocks around
            // are the city's (towers, apartment blocks): one per approach, on the
            // right-hand kerb before the crossing, facing the traffic coming in.
            // The X street's lights on one phase, the Z street's on the other
            // (civic::signalPhase), switched per frame by CitySystem.
            if (r.trafficLights) {
                bool urban = SX.avenue || SZ.avenue;
                for (int bz = j - 1; bz <= j && !urban; ++bz)
                    for (int bx = i - 1; bx <= i && !urban; ++bx) {
                        const Zone z = zoneAt(bx, bz);
                        urban = z == Zone::Towers || z == Zone::Blocks;
                    }
                if (urban) {
                    int lit = 0;
                    const glm::vec2 dirs[4] = {dX, -dX, dZ, -dZ};
                    for (int q = 0; q < 4; ++q) {
                        const glm::vec2 d = dirs[q];         // the traffic's heading
                        const bool alongX = q < 2;
                        const float own = alongX ? hx : hz, other = alongX ? hz : hx;
                        const glm::vec2 right(-d.y, d.x);
                        const glm::vec2 at = p - d * (other + 1.8f) + right * (own + 0.5f);
                        if (!freeGround(at)) continue;
                        models.push_back(civic::trafficLight(alongX ? 0 : 1, pal.civic));
                        place(models.back(), at, -d, 1.0f);
                        ++lit;
                    }
                    if (lit) ++out.stats.lights;
                }
            }
        }

    // --- Bus stops ---------------------------------------------------------------
    // Walked along every street from node to node: after `busStopEvery` metres
    // the next block side gets a pair, one per direction on its right-hand kerb,
    // a few metres apart. Each named after the next street it comes to. A
    // shelter where the pavement is wide enough for one, the sign always.
    if (r.busStopEvery > 1.0f) {
        const bool shelter = r.sidewalk >= 2.4f;
        for (int s = 0; s < static_cast<int>(lay.streets.size()); ++s) {
            const Street& st = lay.streets[static_cast<std::size_t>(s)];
            const int count = st.alongZ ? lay.nz : lay.nx;
            float acc = 0.5f * r.busStopEvery;
            for (int m = 0; m < count; ++m) {
                const glm::vec2 a = st.alongZ ? node(st.line, m) : node(m, st.line);
                const glm::vec2 b = st.alongZ ? node(st.line, m + 1) : node(m + 1, st.line);
                const float len = glm::length(b - a);
                acc += len;
                if (acc < r.busStopEvery || len < 40.0f) continue;
                const glm::vec2 d = (b - a) / len, mid = 0.5f * (a + b);
                const float half = 0.5f * st.width;
                // The cross streets at either end, for the names.
                const int crossB = (st.alongZ ? xLine : zLine)[static_cast<std::size_t>(m + 1)];
                const int crossA = (st.alongZ ? xLine : zLine)[static_cast<std::size_t>(m)];
                const glm::vec2 perp(-d.y, d.x);
                bool any = false;
                for (int dirSign = 1; dirSign >= -1; dirSign -= 2) {
                    const glm::vec2 hd = d * static_cast<float>(dirSign);   // bus heading
                    const glm::vec2 right(-hd.y, hd.x);
                    const glm::vec2 base = mid - hd * 6.0f;                  // the pair staggered
                    const glm::vec2 signAt = base + hd * 3.0f + right * (half + 0.45f);
                    const glm::vec2 shelterAt =
                        base + right * (half + std::max(r.sidewalk - 0.9f, 1.2f));
                    if (!freeGround(signAt) || (shelter && !freeGround(shelterAt))) continue;
                    const int cross = dirSign > 0 ? crossB : crossA;
                    const glm::vec2 crossAt = dirSign > 0 ? b : a;
                    const std::string name =
                        cross >= 0 ? nameOf(cross, crossAt, perp) : nameOf(s, mid, d);
                    models.push_back(civic::busStopSign(name, pal.civic));
                    // The sign's faces point along the road, so both directions read it.
                    place(models.back(), signAt, hd, 0.8f);
                    if (shelter) {
                        models.push_back(civic::busShelter(pal.civic));
                        place(models.back(), shelterAt, -right, 2.6f);
                    }
                    ++out.stats.busStops;
                    any = true;
                }
                if (any) acc = 0.0f;
            }
        }
    }

    city::merge(pcs, pcChunk, out.district, &extras);
    out.district.placed = out.stats.built;
    return out;
}

} // namespace cityplan
