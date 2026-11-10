#include "CityPlan.hpp"

#include <algorithm>
#include <deque>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <string>
#include <memory>
#include <tuple>
#include <unordered_map>

#include <glm/gtc/matrix_transform.hpp>
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
    struct Seg { glm::vec2 a, b; float half; int road; float ya = 0.0f, yb = 0.0f; bool hasY = false; };
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
                const bool  hasY = r.y.size() == r.pts.size();
                const float y0 = hasY ? r.y[i] : 0.0f, y1 = hasY ? r.y[i + 1] : 0.0f;
                // Long segments go in as cell-sized pieces, so the box each one
                // is filed under stays a few cells rather than a whole diagonal.
                const float len = glm::length(b0 - a0);
                const int   n   = std::max(1, static_cast<int>(std::ceil(len / cell)));
                if (n > 4096) continue;   // not a road: a break marker or garbage
                for (int k = 0; k < n; ++k) {
                    const glm::vec2 a = glm::mix(a0, b0, static_cast<float>(k) / n);
                    const glm::vec2 b = glm::mix(a0, b0, static_cast<float>(k + 1) / n);
                    const int id = static_cast<int>(segs.size());
                    segs.push_back({a, b, r.half, static_cast<int>(ri),
                                    glm::mix(y0, y1, static_cast<float>(k) / n),
                                    glm::mix(y0, y1, static_cast<float>(k + 1) / n), hasY});
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
    // `road`, when given, is which of the roads that is.
    bool snap(glm::vec2 p, glm::vec2 d, glm::vec2& at, glm::vec2& dir, float& half,
              float* y = nullptr, bool* hasY = nullptr, int* road = nullptr) const {
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
            if (dist < bestD) {
                bestD = dist; at = q; dir = u; half = s.half; found = true;
                if (y) *y = glm::mix(s.ya, s.yb, t);
                if (hasY) *hasY = s.hasY;
                if (road) *road = s.road;
            }
        }
        return found;
    }
    // The road surface's height at the nearest road to `p` (within its cell),
    // if that road carries heights.
    bool surfaceNear(glm::vec2 p, float& y) const {
        float bestD = 1e9f;
        bool  found = false;
        const auto it = cells.find(key(static_cast<int>(std::floor(p.x / cell)),
                                       static_cast<int>(std::floor(p.y / cell))));
        if (it == cells.end()) return false;
        for (int id : it->second) {
            const Seg& s = segs[static_cast<std::size_t>(id)];
            if (!s.hasY) continue;
            const glm::vec2 ab = s.b - s.a;
            const float l2 = glm::dot(ab, ab);
            const float t  = l2 > 1e-8f ? glm::clamp(glm::dot(p - s.a, ab) / l2, 0.0f, 1.0f) : 0.0f;
            const float dist = glm::length(p - (s.a + ab * t));
            if (dist < bestD) { bestD = dist; y = glm::mix(s.ya, s.yb, t); found = true; }
        }
        return found;
    }
    // How far `p` is from the nearest carriageway EDGE (negative = on it) --
    // of any road but `skip`, the one a kerb stands on.
    float clearance(glm::vec2 p, int skip = -1) const {
        float best = 1e9f;
        const auto it = cells.find(key(static_cast<int>(std::floor(p.x / cell)),
                                       static_cast<int>(std::floor(p.y / cell))));
        if (it == cells.end()) return best;
        for (int id : it->second) {
            const Seg& s = segs[static_cast<std::size_t>(id)];
            if (s.road == skip) continue;
            const glm::vec2 ab = s.b - s.a;
            const float l2 = glm::dot(ab, ab);
            const float t  = l2 > 1e-8f ? glm::clamp(glm::dot(p - s.a, ab) / l2, 0.0f, 1.0f) : 0.0f;
            best = std::min(best, glm::length(p - (s.a + ab * t)) - s.half);
        }
        return best;
    }
};

// Which chunk a point belongs to, for the batch merge: frustum and distance
// culling need something to bite on, so a town is welded in squares. 500 m, not
// the roadside city's 160: a town's batches are per chunk AND per material, and
// the colour variants multiplied the materials -- at 160 m the City preset went
// from 320 to 650 draws, and the cost of a draw is paid a dozen times a frame
// (every cascade, the probe, the water). Measured on a 1.6 x 1.1 km town: 250 m
// made 874 draws, 500 m 368 and a frame a millisecond faster; 1000 m's 205 were
// no faster again, the coarser culling giving back what the draws saved.
int chunkOf(glm::vec2 p) {
    const int cx = static_cast<int>(std::floor(p.x / 500.0f));
    const int cz = static_cast<int>(std::floor(p.y / 500.0f));
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

std::array<CivicSlot, civic::kKinds> defaultCivic() {
    std::array<CivicSlot, civic::kKinds> a{};
    for (civic::Kind k : {civic::Kind::Church, civic::Kind::Police, civic::Kind::FireStation,
                          civic::Kind::Hospital, civic::Kind::TownHall, civic::Kind::School,
                          civic::Kind::Kindergarten, civic::Kind::Library,
                          civic::Kind::Supermarket})
        a[static_cast<std::size_t>(k)].count = 1;
    return a;
}

const char* civicKey(civic::Kind k) {
    switch (k) {
        case civic::Kind::Church:       return "church";
        case civic::Kind::Police:       return "police";
        case civic::Kind::FireStation:  return "fireStation";
        case civic::Kind::Hospital:     return "hospital";
        case civic::Kind::Industry:     return "industry";
        case civic::Kind::TownHall:     return "townHall";
        case civic::Kind::School:       return "school";
        case civic::Kind::Kindergarten: return "kindergarten";
        case civic::Kind::Library:      return "library";
        case civic::Kind::Museum:       return "museum";
        case civic::Kind::Theatre:      return "theatre";
        case civic::Kind::Pool:         return "pool";
        case civic::Kind::PetrolStation: return "petrolStation";
        case civic::Kind::PowerPlant:   return "powerPlant";
        case civic::Kind::Landfill:     return "landfill";
        case civic::Kind::Station:      return "station";
        case civic::Kind::Supermarket:  return "supermarket";
        case civic::Kind::Cinema:       return "cinema";
        case civic::Kind::DepartmentStore: return "departmentStore";
        case civic::Kind::ParkingGarage: return "parkingGarage";
        case civic::Kind::Hotel:        return "hotel";
        default:                        return "?";
    }
}

namespace {
// A preset's public buildings, then its cultural ones.
void civicCounts(Rule& r, int church, int police, int fire, int hospital,
                 std::initializer_list<int> culture) {
    using K = civic::Kind;
    r.civic[static_cast<std::size_t>(K::Church)].count      = church;
    r.civic[static_cast<std::size_t>(K::Police)].count      = police;
    r.civic[static_cast<std::size_t>(K::FireStation)].count = fire;
    r.civic[static_cast<std::size_t>(K::Hospital)].count    = hospital;
    const K order[] = {K::TownHall, K::School, K::Kindergarten, K::Library, K::Museum,
                       K::Theatre, K::Pool, K::PetrolStation, K::PowerPlant, K::Landfill,
                       K::Station, K::Supermarket, K::Cinema, K::DepartmentStore,
                       K::ParkingGarage, K::Hotel};
    std::size_t i = 0;
    for (int n : culture) r.civic[static_cast<std::size_t>(order[i++])].count = n;
}
} // namespace

void applyPreset(Rule& r, Preset p) {
    // Only what makes the character; place, size and seed stay the author's.
    switch (p) {
        case Preset::Village:
            r.grid.sizeX = 300.0f; r.grid.sizeZ = 240.0f;
            r.grid.blockX = 100.0f; r.grid.blockZ = 80.0f; r.grid.organic = 0.55f;
            // Lanes, not a grid (see villageLayout), as narrow as a village's are.
            r.grid.pattern = 1; r.grid.form = 0;
            r.grid.streetWidth = 4.5f; r.grid.avenueWidth = 6.5f; r.grid.avenueEvery = 0;
            r.lampEvery = 45.0f;
            r.towerRing = 0.0f; r.blockRing = 0.0f; r.rowRing = 0.25f;
            r.parkChance = 0.10f; r.centrePark = true;
            r.houseLot = 24.0f; r.houseSetback = 6.0f; r.fill = 0.85f;
            // One narrow pavement down the main street, not a town's three metres.
            r.sidewalk = 1.8f;
            // What a village of a few hundred has: the church, the fire station,
            // the Dorfgemeinschaftshaus and a kindergarten. The school, the
            // supermarket and the petrol station are in the next town.
            civicCounts(r, 1, 0, 1, 0, {1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
            r.industryShare = 0.0f;
            break;
        case Preset::Count:
        case Preset::SmallTown:
            r.grid.pattern = 0;
            r.grid.sizeX = 520.0f; r.grid.sizeZ = 440.0f;
            r.grid.blockX = 95.0f; r.grid.blockZ = 75.0f; r.grid.organic = 0.18f;
            r.grid.streetWidth = 7.0f; r.grid.avenueWidth = 11.0f; r.grid.avenueEvery = 3;
            r.towerRing = 0.0f; r.blockRing = 0.35f; r.rowRing = 0.60f;
            r.blockFloorsMin = 3; r.blockFloorsMax = 5;
            r.parkChance = 0.08f; r.centrePark = true;
            r.houseLot = 20.0f; r.houseSetback = 5.0f; r.fill = 0.94f; r.sidewalk = 3.0f;
            civicCounts(r, 2, 1, 1, 1, {1, 1, 1, 1, 0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 0, 1});
            r.industryShare = 0.08f;
            break;
        case Preset::City:
            r.grid.pattern = 0;
            r.grid.sizeX = 900.0f; r.grid.sizeZ = 760.0f;
            r.grid.blockX = 110.0f; r.grid.blockZ = 85.0f; r.grid.organic = 0.06f;
            r.grid.streetWidth = 8.0f; r.grid.avenueWidth = 14.0f; r.grid.avenueEvery = 3;
            r.towerRing = 0.25f; r.blockRing = 0.60f; r.rowRing = 0.78f;
            r.towerFloorsMin = 10; r.towerFloorsMax = 40;
            r.blockFloorsMin = 4; r.blockFloorsMax = 7;
            r.parkChance = 0.07f; r.centrePark = false;
            r.houseLot = 18.0f; r.houseSetback = 4.0f; r.fill = 0.96f; r.sidewalk = 3.0f;
            civicCounts(r, 3, 2, 2, 1, {1, 2, 2, 1, 1, 1, 1, 2, 1, 1, 1, 2, 1, 1, 1, 1});
            r.industryShare = 0.12f;
            break;
        case Preset::Metropolis:
            r.grid.pattern = 0;
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
            r.houseLot = 18.0f; r.houseSetback = 4.0f; r.fill = 0.97f; r.sidewalk = 3.0f;
            civicCounts(r, 4, 3, 3, 2, {1, 3, 3, 2, 2, 1, 2, 3, 1, 1, 1, 3, 2, 2, 2, 2});
            r.industryShare = 0.15f;
            r.budget = 3000;
            break;
        case Preset::Suburb:
            r.grid.pattern = 0;
            r.grid.sizeX = 480.0f; r.grid.sizeZ = 360.0f;
            r.grid.blockX = 90.0f; r.grid.blockZ = 60.0f; r.grid.organic = 0.30f;
            r.grid.streetWidth = 6.0f; r.grid.avenueWidth = 9.0f; r.grid.avenueEvery = 0;
            r.towerRing = 0.0f; r.blockRing = 0.0f; r.rowRing = 0.0f;
            r.parkChance = 0.06f; r.centrePark = true;
            r.houseLot = 20.0f; r.houseSetback = 6.0f; r.fill = 0.92f; r.sidewalk = 3.0f;
            civicCounts(r, 1, 0, 1, 0, {0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0});
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
        {"pattern", g.pattern}, {"form", g.form},
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
    // Absent in towns saved before villages: a grid, as they were laid.
    g.pattern     = j.value("pattern", d.pattern);
    g.form        = j.value("form", d.form);
    return g;
}

} // namespace

bool Grid::operator==(const Grid& o) const { return gridJson(*this) == gridJson(o); }

namespace {
nlohmann::json civicJson(const std::array<CivicSlot, civic::kKinds>& a) {
    nlohmann::json j = nlohmann::json::object();
    for (int k = 0; k < civic::kKinds; ++k) {
        const CivicSlot& c = a[static_cast<std::size_t>(k)];
        nlohmann::json e = {{"count", c.count}};
        if (c.useModel || !c.model.empty())
            e.update({{"useModel", c.useModel}, {"model", c.model}, {"fitPlot", c.fitPlot},
                      {"turn", c.turn}});
        j[civicKey(static_cast<civic::Kind>(k))] = e;
    }
    return j;
}
std::array<CivicSlot, civic::kKinds> jsonCivic(const nlohmann::json& j,
                                               const std::array<CivicSlot, civic::kKinds>& d) {
    std::array<CivicSlot, civic::kKinds> a{};   // a kind the file does not name: none
    for (int k = 0; k < civic::kKinds; ++k) {
        const char* key = civicKey(static_cast<civic::Kind>(k));
        if (!j.is_object() || !j.contains(key)) continue;
        const nlohmann::json& e = j[key];
        CivicSlot& c = a[static_cast<std::size_t>(k)];
        c.count    = e.value("count", d[static_cast<std::size_t>(k)].count);
        c.useModel = e.value("useModel", false);
        c.model    = e.value("model", std::string());
        c.fitPlot  = e.value("fitPlot", true);
        c.turn     = e.value("turn", 0);
    }
    return a;
}
} // namespace

void save(nlohmann::json& j, const Rule& r) {
    j = {
        {"id", r.id}, {"name", r.name}, {"enabled", r.enabled}, {"seed", r.seed},
        {"grid", gridJson(r.grid)},
        {"towerRing", r.towerRing}, {"blockRing", r.blockRing}, {"rowRing", r.rowRing},
        {"zoneNoise", r.zoneNoise}, {"parkChance", r.parkChance},
        {"centrePark", r.centrePark},
        {"industryShare", r.industryShare}, {"industryAngle", r.industryAngle},
        {"civic", civicJson(r.civic)},
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
        {"traffic", r.traffic}, {"busShare", r.busShare}, {"truckShare", r.truckShare},
        {"powerLines", r.powerLines}, {"powerLineLength", r.powerLineLength},
        {"vehiclePrefabs", [&] {
            nlohmann::json a = nlohmann::json::array();
            for (const VehiclePrefab& v : r.vehiclePrefabs)
                a.push_back({{"prefab", v.prefab}, {"kind", v.kind}, {"weight", v.weight},
                             {"forward", v.forward}});
            return a;
        }()},
        {"placeholderWeight", r.placeholderWeight},
        {"personPrefabs", [&] {
            nlohmann::json a = nlohmann::json::array();
            for (const PersonPrefab& p : r.personPrefabs)
                a.push_back({{"prefab", p.prefab}, {"weight", p.weight}, {"forward", p.forward}});
            return a;
        }()},
        {"personPlaceholderWeight", r.personPlaceholderWeight},
        {"lampPrefabs", [&] {
            nlohmann::json a = nlohmann::json::array();
            for (const LampPrefab& p : r.lampPrefabs)
                a.push_back({{"prefab", p.prefab}, {"weight", p.weight}, {"forward", p.forward}});
            return a;
        }()},
        {"lampEvery", r.lampEvery}, {"lampBothSides", r.lampBothSides},
        {"lampInset", r.lampInset},
        {"houseNumbers", r.houseNumbers}, {"trafficSigns", r.trafficSigns},
        {"shopSigns", r.shopSigns}, {"adverts", r.adverts}, {"binEvery", r.binEvery},
        {"manholes", r.manholes},
        {"people", r.people},
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
    if (j.contains("civic")) {
        r.civic = jsonCivic(j["civic"], d.civic);
    } else {
        // A town saved before the cultural buildings: its four counts, and none
        // of the new kinds -- loading it must not rebuild half its blocks.
        for (CivicSlot& c : r.civic) c = CivicSlot{};
        using K = civic::Kind;
        r.civic[static_cast<std::size_t>(K::Church)].count      = j.value("churches", 1);
        r.civic[static_cast<std::size_t>(K::Police)].count      = j.value("policeStations", 1);
        r.civic[static_cast<std::size_t>(K::FireStation)].count = j.value("fireStations", 1);
        r.civic[static_cast<std::size_t>(K::Hospital)].count    = j.value("hospitals", 1);
    }
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
    r.traffic       = j.value("traffic", d.traffic);
    r.powerLines    = j.value("powerLines", d.powerLines);
    r.vehiclePrefabs.clear();
    if (j.contains("vehiclePrefabs") && j["vehiclePrefabs"].is_array())
        for (const nlohmann::json& v : j["vehiclePrefabs"]) {
            VehiclePrefab p;
            p.prefab  = v.value("prefab", std::string());
            p.kind    = std::clamp(v.value("kind", 0), 0, 2);
            p.weight  = v.value("weight", 1.0f);
            p.forward = std::clamp(v.value("forward", 0), 0, 3);
            r.vehiclePrefabs.push_back(p);
        }
    r.placeholderWeight = j.value("placeholderWeight", d.placeholderWeight);
    r.personPrefabs.clear();
    if (j.contains("personPrefabs") && j["personPrefabs"].is_array())
        for (const nlohmann::json& v : j["personPrefabs"]) {
            PersonPrefab p;
            p.prefab  = v.value("prefab", std::string());
            p.weight  = v.value("weight", 1.0f);
            p.forward = std::clamp(v.value("forward", 0), 0, 3);
            r.personPrefabs.push_back(p);
        }
    r.personPlaceholderWeight = j.value("personPlaceholderWeight", d.personPlaceholderWeight);
    r.lampPrefabs.clear();
    if (j.contains("lampPrefabs") && j["lampPrefabs"].is_array())
        for (const nlohmann::json& v : j["lampPrefabs"]) {
            LampPrefab p;
            p.prefab  = v.value("prefab", std::string());
            p.weight  = v.value("weight", 1.0f);
            p.forward = std::clamp(v.value("forward", 0), 0, 3);
            r.lampPrefabs.push_back(p);
        }
    r.lampEvery     = j.value("lampEvery", d.lampEvery);
    r.lampBothSides = j.value("lampBothSides", d.lampBothSides);
    r.lampInset     = j.value("lampInset", d.lampInset);
    r.houseNumbers  = j.value("houseNumbers", d.houseNumbers);
    r.trafficSigns  = j.value("trafficSigns", d.trafficSigns);
    r.shopSigns     = j.value("shopSigns", d.shopSigns);
    r.adverts       = j.value("adverts", d.adverts);
    r.binEvery      = j.value("binEvery", d.binEvery);
    r.manholes      = j.value("manholes", d.manholes);
    r.powerLineLength = j.value("powerLineLength", d.powerLineLength);
    r.busShare      = j.value("busShare", d.busShare);
    r.truckShare    = j.value("truckShare", d.truckShare);
    r.people        = j.value("people", d.people);
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

int    houseKinds(Zone z);
Layout villageLayout(const Rule& r, const Grid& g);

Layout layout(const Rule& r, const Grid& g) {
    if (g.pattern == 1) return villageLayout(r, g);
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
        // In this order (earlier kinds get the better blocks), each from its
        // ring outwards. The four public ones keep the order they always had,
        // so a town saved before the cultural kinds plans the same blocks.
        auto n = [&](civic::Kind k) { return std::max(r.civic[static_cast<std::size_t>(k)].count, 0); };
        place(civic::Kind::TownHall,    n(civic::Kind::TownHall),    0.0f,  0.35f);
        place(civic::Kind::Church,      n(civic::Kind::Church),      0.0f,  0.45f);
        place(civic::Kind::Museum,      n(civic::Kind::Museum),      0.1f,  0.30f);
        place(civic::Kind::Theatre,     n(civic::Kind::Theatre),     0.15f, 0.30f);
        place(civic::Kind::Hospital,    n(civic::Kind::Hospital),    0.45f, 0.25f);
        place(civic::Kind::Police,      n(civic::Kind::Police),      0.25f, 0.35f);
        place(civic::Kind::FireStation, n(civic::Kind::FireStation), 0.55f, 0.25f);
        place(civic::Kind::Library,     n(civic::Kind::Library),     0.3f,  0.30f);
        place(civic::Kind::School,      n(civic::Kind::School),      0.45f, 0.25f);
        place(civic::Kind::Kindergarten, n(civic::Kind::Kindergarten), 0.6f, 0.20f);
        place(civic::Kind::Pool,        n(civic::Kind::Pool),        0.6f,  0.20f);
        place(civic::Kind::Station,     n(civic::Kind::Station),     0.3f,  0.30f);
        place(civic::Kind::PetrolStation, n(civic::Kind::PetrolStation), 0.55f, 0.20f);
        // The dirty and the big ones at the very edge.
        place(civic::Kind::PowerPlant,  n(civic::Kind::PowerPlant),  1.0f,  0.10f);
        place(civic::Kind::Landfill,    n(civic::Kind::Landfill),    1.0f,  0.10f);
        // Shopping and going out: the department store, the cinema and the
        // hotel in the centre, the multi-storey beside them, the supermarkets
        // where the houses are. Last, so the kinds above keep the blocks they
        // had before these existed.
        place(civic::Kind::DepartmentStore, n(civic::Kind::DepartmentStore), 0.1f, 0.25f);
        place(civic::Kind::Cinema,      n(civic::Kind::Cinema),      0.2f,  0.30f);
        place(civic::Kind::Hotel,       n(civic::Kind::Hotel),       0.2f,  0.30f);
        place(civic::Kind::ParkingGarage, n(civic::Kind::ParkingGarage), 0.25f, 0.30f);
        place(civic::Kind::Supermarket, n(civic::Kind::Supermarket), 0.55f, 0.25f);
    }
    return L;
}

// --- The village ---------------------------------------------------------------------
//
// What a village's streets look like, from the maps (OpenStreetMap: Marthalen, a
// Haufendorf; Weiperfelden in the Taunus; Goldenbow in Mecklenburg) and the
// forms the geography books name:
//
//  * One street carries it: the road through, bending with the land, and the
//    village strung along it. Nothing else is as long or as wide.
//  * Lanes leave it at odd angles -- forty degrees, seventy, hardly ever square
//    -- and bend as they go. Two leave together at the core (a fork, a Y), most
//    leave alone. Crossings of four are rare; T's are the rule.
//  * Half of them end: in a farmyard, at the last house, at the edge of the
//    fields. Some come back to another lane and close a loop no bigger than a
//    few gardens. A few run on out of the village as field tracks.
//  * The core is where they crowd -- the church, a widening, a green -- and the
//    houses are close there, right on the street. Outwards they thin, stand
//    back behind front gardens, and leave gaps: an orchard, a paddock.
//  * The houses follow the street they stand on, gable or eaves to it, and
//    never quite square: no two plots were measured out together.
//
// Three forms, as the books have them: the Haufendorf, all of the above round a
// core; the Straßendorf, one street with the farms lined up along it, gable on,
// and a few dead-end lanes; the Angerdorf, the street splitting round a long
// green with the church on it.
//
// Built in the village's own frame (x along the main street, z across), then
// turned into the world like the grid.
namespace {

struct VLane {
    std::vector<glm::vec2> pts;      // the village's frame
    float width   = 4.5f;
    int   parent  = -1;
    int   depth   = 0;
    std::uint8_t leaves = 0;
    std::string name;
};

struct VRng {
    std::uint32_t s;
    float next() { s = hashU(s + 0x9e3779b9U); return unit(s); }
    float range(float a, float b) { return a + (b - a) * next(); }
};

float polyLen(const std::vector<glm::vec2>& l) {
    float t = 0.0f;
    for (std::size_t i = 1; i < l.size(); ++i) t += glm::length(l[i] - l[i - 1]);
    return t;
}

// The point `s` metres along `l`, and the way it runs there.
glm::vec2 along(const std::vector<glm::vec2>& l, float s, glm::vec2* tan = nullptr) {
    float run = 0.0f;
    for (std::size_t i = 1; i < l.size(); ++i) {
        const float seg = glm::length(l[i] - l[i - 1]);
        if (run + seg >= s || i + 1 == l.size()) {
            const glm::vec2 d = seg > 1e-6f ? (l[i] - l[i - 1]) / seg : glm::vec2(1.0f, 0.0f);
            if (tan) *tan = d;
            return l[i - 1] + d * glm::clamp(s - run, 0.0f, seg);
        }
        run += seg;
    }
    if (tan) *tan = glm::vec2(1.0f, 0.0f);
    return l.empty() ? glm::vec2(0.0f) : l.front();
}

// Nearest point of `l` to `p`: its distance, where along, and the way it runs.
float nearestOn(const std::vector<glm::vec2>& l, glm::vec2 p, float* s = nullptr,
                glm::vec2* at = nullptr, glm::vec2* tan = nullptr) {
    float best = 1e30f, run = 0.0f;
    for (std::size_t i = 1; i < l.size(); ++i) {
        const glm::vec2 a = l[i - 1], ab = l[i] - a;
        const float seg = glm::length(ab);
        const float t = seg > 1e-6f ? glm::clamp(glm::dot(p - a, ab) / (seg * seg), 0.0f, 1.0f) : 0.0f;
        const glm::vec2 q = a + ab * t;
        const float d = glm::length(p - q);
        if (d < best) {
            best = d;
            if (s) *s = run + t * seg;
            if (at) *at = q;
            if (tan) *tan = seg > 1e-6f ? ab / seg : glm::vec2(1.0f, 0.0f);
        }
        run += seg;
    }
    return best;
}

float smooth01(float e0, float e1, float x) {
    const float t = glm::clamp((x - e0) / std::max(e1 - e0, 1e-6f), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Inside a polygon of any shape (even-odd).
bool insidePoly(const std::vector<glm::vec2>& poly, glm::vec2 p) {
    bool in = false;
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
        if (((poly[i].y > p.y) != (poly[j].y > p.y)) &&
            (p.x < (poly[j].x - poly[i].x) * (p.y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x))
            in = !in;
    return poly.size() > 2 && in;
}

glm::vec2 turnBy(glm::vec2 d, float a) {
    const float c = std::cos(a), s = std::sin(a);
    return {d.x * c - d.y * s, d.x * s + d.y * c};
}

// A village's street names: what lanes in a German village are called, by what
// they lead to or lie by -- not the town's poets and composers.
const char* const kLaneNames[] = {
    "Kirchgasse", "Brunnengasse", "Hintergasse", "Backhausgasse", "Pfarrgasse", "Schulgasse",
    "M\xC3\xBC" "hlweg", "Am Bach", "Lindenweg", "Gartenweg", "Im Winkel", "Am Rain",
    "Wiesenweg", "Hofgasse", "Am Graben", "Bergstra\xC3\x9F" "e", "Talweg", "Am Brunnen",
    "Scheunengasse", "Steingasse", "Am Hang", "Kirchberg", "Zehntgasse", "Kelterweg",
    "Am Weiher", "Lehmgrube", "Schmiedgasse", "Am Hirtenhaus", "Rathausgasse", "Sonnenhang",
};
const char* const kOutNames[] = {
    "Feldweg", "Zum Wald", "Am Hohlweg", "Zur M\xC3\xBC" "hle", "Viehtrift", "Flurweg",
};

} // namespace

void villageWalks(const Rule& r, const Layout& L, float from[2], float to[2]) {
    from[0] = from[1] = 1.0f;
    to[0] = to[1] = 0.0f;
    if (!L.organic || L.main < 0 || !r.pavements || r.sidewalk <= 0.3f) return;
    const Street& M = L.streets[static_cast<std::size_t>(L.main)];
    const float len   = polyLen(M.pts);
    const float coreR = 0.45f * 0.25f * (r.built().sizeX + r.built().sizeZ);
    float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f}, loC[2] = {1e9f, 1e9f},
          hiC[2] = {-1e9f, -1e9f};
    int   n[2] = {0, 0}, nC[2] = {0, 0};
    for (const Lot& lot : L.lots) {
        if (lot.street != L.main) continue;
        float t = 0.0f;
        glm::vec2 q, d;
        nearestOn(M.pts, lot.pos, &t, &q, &d);
        const int side = cross2(d, lot.pos - q) > 0.0f ? 0 : 1;
        lo[side] = std::min(lo[side], t);
        hi[side] = std::max(hi[side], t);
        ++n[side];
        if (glm::length(q - L.core) < coreR) {
            loC[side] = std::min(loC[side], t);
            hiC[side] = std::max(hiC[side], t);
            ++nC[side];
        }
    }
    const int a = n[0] >= n[1] ? 0 : 1, b = 1 - a;
    if (n[a] >= 2) {
        from[a] = std::max(lo[a] - 8.0f, 6.0f);
        to[a]   = std::min(hi[a] + 8.0f, len - 6.0f);
    }
    if (nC[b] >= 2) {
        from[b] = std::max(loC[b] - 8.0f, 6.0f);
        to[b]   = std::min(hiC[b] + 8.0f, len - 6.0f);
    }
}

const char* villageFormName(int form) {
    switch (form) {
        case 1: return "Haufendorf";
        case 2: return "Stra\xC3\x9F" "endorf";
        case 3: return "Angerdorf";
        default: return "By the seed";
    }
}

Layout villageLayout(const Rule& r, const Grid& g) {
    Layout L;
    L.organic = true;
    const float a = glm::radians(g.rotation);
    L.axisX = {std::cos(a), std::sin(a)};
    L.axisZ = {-std::sin(a), std::cos(a)};
    auto world = [&](glm::vec2 l) { return g.center + L.axisX * l.x + L.axisZ * l.y; };
    auto worldDir = [&](glm::vec2 d) { return L.axisX * d.x + L.axisZ * d.y; };

    const float hx = 0.5f * std::max(g.sizeX, 60.0f), hz = 0.5f * std::max(g.sizeZ, 50.0f);
    VRng rng{hashU(g.seed ^ 0xd0f5U)};
    int form = g.form;
    if (form < 1 || form > 3) {
        const float f = unit(hashU(g.seed ^ 0xf0e1U));
        form = f < 0.45f ? 1 : f < 0.8f ? 2 : 3;
    }
    // Inside the village: an ellipse with a wandering rim, so the edge of the
    // houses is not drawn with a compass either.
    const float rp1 = rng.range(0.0f, 6.283f), rp2 = rng.range(0.0f, 6.283f);
    auto rim = [&](glm::vec2 l) {
        const float ang = std::atan2(l.y / hz, l.x / hx);
        return 1.0f + 0.12f * std::sin(2.0f * ang + rp1) + 0.07f * std::sin(5.0f * ang + rp2);
    };
    auto inside = [&](glm::vec2 l, float k) {
        const float e = std::sqrt((l.x / hx) * (l.x / hx) + (l.y / hz) * (l.y / hz));
        return e < k * rim(l);
    };
    const float laneW  = std::max(g.streetWidth, 3.5f);
    const float mainW  = std::max(g.avenueWidth, laneW);
    std::vector<VLane> lanes;

    // --- The street through -----------------------------------------------------
    // Two slow waves of different length across the village, so it bends like a
    // road that went round things -- and runs on out at both ends, where the
    // country roads take it up.
    {
        VLane m;
        m.width  = mainW;
        m.leaves = 3;
        const float out = 30.0f;
        const float p1 = rng.range(0.0f, 6.283f), p2 = rng.range(0.0f, 6.283f);
        const float a1 = rng.range(0.10f, 0.22f) * hz, a2 = rng.range(0.03f, 0.08f) * hz;
        const float tilt = rng.range(-0.12f, 0.12f);
        for (float x = -hx - out; x <= hx + out + 0.1f; x += 10.0f)
            m.pts.push_back({x, a1 * std::sin(6.283f * x / (2.7f * hx) + p1) +
                                    a2 * std::sin(6.283f * x / (0.85f * hx) + p2) + tilt * x});
        m.name = unit(hashU(g.seed ^ 0x3a1U)) < 0.5f ? "Hauptstra\xC3\x9F" "e" : "Dorfstra\xC3\x9F" "e";
        lanes.push_back(std::move(m));
    }
    const float mainLen = polyLen(lanes[0].pts);
    const float coreS   = 0.5f * mainLen + rng.range(-0.15f, 0.15f) * hx;
    glm::vec2 coreTan;
    const glm::vec2 core = along(lanes[0].pts, coreS, &coreTan);

    // --- The green (Angerdorf) --------------------------------------------------
    // The street parts round a long lens of grass: one side carries on, the
    // other is "Am Anger". Both leave the line they shared at a good angle, so
    // the roads read them as junctions, and meet it again at the far tip.
    std::vector<glm::vec2> green;
    if (form == 3) {
        const float len = std::min(0.62f * 2.0f * hx, 190.0f);
        const float wid = std::clamp(0.30f * len, 34.0f, 60.0f);
        const float s0 = coreS - 0.5f * len, s1 = coreS + 0.5f * len;
        std::vector<glm::vec2> other;
        bool done = false;
        float run = 0.0f;
        std::vector<glm::vec2>& mp = lanes[0].pts;
        std::vector<glm::vec2> rebuilt;
        for (std::size_t i = 0; i < mp.size(); ++i) {
            if (i) run += glm::length(mp[i] - mp[i - 1]);
            if (run < s0 || run > s1) { rebuilt.push_back(mp[i]); continue; }
            if (!done) {
                // Re-sample the stretch, bulged both ways.
                const int n = static_cast<int>(len / 8.0f);
                for (int k = 0; k <= n; ++k) {
                    const float s = s0 + len * static_cast<float>(k) / static_cast<float>(n);
                    glm::vec2 t;
                    const glm::vec2 c = along(mp, s, &t);
                    const glm::vec2 nrm(-t.y, t.x);
                    // The street that carries on bends least; "Am Anger" takes
                    // the long way round.
                    const float b = wid * std::pow(std::sin(3.14159f * static_cast<float>(k) / static_cast<float>(n)), 0.6f);
                    rebuilt.push_back(c + nrm * (0.3f * b));
                    other.push_back(c - nrm * (0.7f * b));
                    green.push_back(c + nrm * (0.3f * b - 0.5f * mainW - 2.0f));
                }
                for (int k = n; k >= 0; --k) {
                    const float s = s0 + len * static_cast<float>(k) / static_cast<float>(n);
                    glm::vec2 t;
                    const glm::vec2 c = along(mp, s, &t);
                    const glm::vec2 nrm(-t.y, t.x);
                    const float b = wid * std::pow(std::sin(3.14159f * static_cast<float>(k) / static_cast<float>(n)), 0.6f);
                    green.push_back(c - nrm * (0.7f * b - 0.5f * laneW - 2.0f));
                }
                done = true;
            }
        }
        mp = rebuilt;
        if (other.size() > 2) {
            VLane an;
            an.pts = other;
            an.width = laneW + 0.5f;
            an.parent = 0;
            an.depth = 1;
            an.name = "Am Anger";
            lanes.push_back(std::move(an));
        }
    }

    // How close a point of a new lane may come to the streets already there --
    // room for a plot on each side -- and whether it may meet one there.
    auto clearOf = [&](glm::vec2 p, int self, int parent, float sAlong, float& dist, int& hit,
                       glm::vec2& hitAt, glm::vec2& hitTan) {
        dist = 1e30f;
        hit = -1;
        for (int k = 0; k < static_cast<int>(lanes.size()); ++k) {
            if (k == self) continue;
            if (k == parent && sAlong < 40.0f) continue;   // still leaving it
            glm::vec2 at, tn;
            const float d = nearestOn(lanes[static_cast<std::size_t>(k)].pts, p, nullptr, &at, &tn);
            if (d < dist) { dist = d; hit = k; hitAt = at; hitTan = tn; }
        }
    };

    // Grow a lane from `start` heading `dir`, bending as it goes. Ends on a
    // street it reaches square enough (a T), short of one it would run beside,
    // at the edge (or on through it as a track), or at `maxLen`.
    std::vector<glm::vec2> starts;
    auto grow = [&](int parent, float sOn, int side, float angle, float maxLen, float width,
                    bool mayLeave, int depth) {
        glm::vec2 tan;
        const glm::vec2 p0 = along(lanes[static_cast<std::size_t>(parent)].pts, sOn, &tan);
        if (!inside(p0, 0.95f)) return false;
        for (const glm::vec2& s : starts)
            if (glm::length(s - p0) < 38.0f) return false;
        if (green.size() > 2 && insidePoly(green, p0)) return false;
        VLane ln;
        ln.width  = width;
        ln.parent = parent;
        ln.depth  = depth;
        ln.pts.push_back(p0);
        glm::vec2 h = turnBy(tan, static_cast<float>(side) * angle);
        float bend = rng.range(-0.010f, 0.010f);
        float run = 0.0f;
        bool  out = false;
        const int self = static_cast<int>(lanes.size());
        for (int step = 0; step < 80; ++step) {
            bend = glm::clamp(bend + rng.range(-0.004f, 0.004f), -0.018f, 0.018f);
            h = glm::normalize(turnBy(h, bend * 9.0f));
            const glm::vec2 q = ln.pts.back() + h * 9.0f;
            run += 9.0f;
            if (!inside(q, out ? 1.45f : 1.0f)) {
                if (!out && mayLeave && run > 40.0f) {
                    out = true;                         // on into the fields
                } else {
                    if (out) ln.pts.push_back(q);
                    break;
                }
            }
            if (green.size() > 2 && insidePoly(green, q)) break;
            float d; int hit; glm::vec2 at, tn;
            clearOf(q, self, parent, run, d, hit, at, tn);
            if (hit >= 0 && d < 0.5f * (width + lanes[static_cast<std::size_t>(hit)].width) + 9.0f) {
                // Reached a street: meet it if the angle makes a junction of it.
                if (std::abs(glm::dot(h, tn)) < std::cos(glm::radians(32.0f)) && run > 35.0f && !out) {
                    ln.pts.push_back(at);
                    ln.leaves = 0;
                }
                break;
            }
            if (hit >= 0 && d < 26.0f && std::abs(glm::dot(h, tn)) > 0.6f) break;   // would run beside it
            ln.pts.push_back(q);
            if (run >= maxLen && !out) break;
        }
        if (polyLen(ln.pts) < 32.0f) return false;
        starts.push_back(p0);
        lanes.push_back(std::move(ln));
        return true;
    };

    // --- The fork at the core (Haufendorf) ----------------------------------------
    if (form == 1) {
        const int side = rng.next() < 0.5f ? 1 : -1;
        grow(0, coreS + rng.range(-10.0f, 10.0f), side, glm::radians(rng.range(28.0f, 42.0f)),
             2.2f * hz, 0.5f * (laneW + mainW), true, 1);
        if (lanes.size() > 1) lanes.back().name = side > 0 ? "Obergasse" : "Untergasse";
    }

    // --- The lanes ---------------------------------------------------------------
    {
        const float area = 4.0f * hx * hz;
        const int want = form == 1 ? std::clamp(static_cast<int>(std::lround(area / 8500.0f)), 3, 14)
                       : form == 2 ? std::clamp(static_cast<int>(std::lround(hx / 55.0f)), 1, 5)
                                   : std::clamp(static_cast<int>(std::lround(area / 22000.0f)), 1, 5);
        int made = 0;
        for (int attempt = 0; attempt < want * 14 && made < want; ++attempt) {
            // Off the main street mostly; in a Haufendorf off lanes too, and
            // crowding towards the core.
            int parent = 0;
            if (form == 1 && lanes.size() > 1 && rng.next() < 0.45f)
                parent = 1 + static_cast<int>(rng.next() * static_cast<float>(lanes.size() - 1)) %
                                 static_cast<int>(lanes.size() - 1);
            const VLane& P = lanes[static_cast<std::size_t>(parent)];
            if (P.depth >= 2) continue;
            const float plen = polyLen(P.pts);
            float s;
            if (parent == 0) {
                const float spread = form == 1 ? 0.45f * hx : 0.9f * hx;
                s = coreS + spread * (rng.next() + rng.next() - 1.0f) * 1.4f;
            } else {
                s = rng.range(25.0f, std::max(26.0f, plen - 20.0f));
            }
            s = glm::clamp(s, 20.0f, plen - 20.0f);
            const int side = rng.next() < 0.5f ? 1 : -1;
            const float ang = form == 2 ? glm::radians(rng.range(68.0f, 88.0f))
                                        : glm::radians(rng.range(35.0f, 85.0f));
            const float len = form == 2 ? rng.range(35.0f, 0.9f * hz)
                                        : rng.range(45.0f, 0.75f * std::max(hx, hz) + 40.0f);
            const bool leave = form != 2 && rng.next() < 0.3f;
            if (grow(parent, s, side, ang, len, laneW, leave, P.depth + 1)) ++made;
        }
    }

    // --- Names -----------------------------------------------------------------------
    {
        std::vector<std::uint32_t> taken;
        int outNo = 0;
        for (std::size_t i = 1; i < lanes.size(); ++i) {
            VLane& ln = lanes[i];
            if (!ln.name.empty()) continue;
            // A lane that runs on out of the village is named for where it goes.
            if (!inside(ln.pts.back(), 1.0f) && outNo < 3) {
                ln.name = kOutNames[(hashU(g.seed ^ 0x7a7U) + static_cast<std::uint32_t>(outNo++)) %
                                    (sizeof(kOutNames) / sizeof(kOutNames[0]))];
                continue;
            }
            const auto n = static_cast<std::uint32_t>(sizeof(kLaneNames) / sizeof(kLaneNames[0]));
            std::uint32_t k = hash3(g.seed ^ 0x1a7eU, static_cast<std::uint32_t>(i), 3U) % n;
            // The lane nearest the church is the Kirchgasse when it is free.
            if (glm::length(ln.pts.front() - core) < 45.0f &&
                std::find(taken.begin(), taken.end(), 0U) == taken.end())
                k = 0;
            while (std::find(taken.begin(), taken.end(), k) != taken.end()) k = (k + 1) % n;
            taken.push_back(k);
            ln.name = kLaneNames[k];
        }
    }

    // --- The church ---------------------------------------------------------------
    // On the green, or beside the street at the core with its churchyard round
    // it -- the one building every village has and none stands without.
    std::vector<Obb> taken;
    auto fitsAll = [&](const Obb& o, float pad) {
        glm::vec2 cs[4];
        o.corners(cs);
        for (const VLane& ln : lanes)
            for (const glm::vec2& c : cs)
                if (nearestOn(ln.pts, c) < 0.5f * ln.width + pad) return false;
        // ...and no street through the middle of it either.
        for (const VLane& ln : lanes)
            for (const glm::vec2& p : ln.pts) {
                const glm::vec2 d = p - o.c;
                if (std::abs(glm::dot(d, o.u)) < o.hu && std::abs(glm::dot(d, glm::vec2(-o.u.y, o.u.x))) < o.hv)
                    return false;
            }
        for (const Obb& t : taken)
            if (overlaps(o, t, 0.0f)) return false;
        return true;
    };
    auto addLot = [&](glm::vec2 pos, glm::vec2 front, float w, float d, int street, int kind,
                      int civicKind, std::uint32_t h) {
        Lot lot;
        lot.pos    = world(pos);
        lot.front  = glm::normalize(worldDir(front));
        lot.width  = w;
        lot.depth  = d;
        lot.zone   = Zone::Houses;
        lot.block  = -1;
        lot.side   = -1;
        lot.kind   = kind;
        lot.hash   = h;
        lot.street = street;
        lot.civic  = civicKind;
        L.lots.push_back(lot);
    };
    if (r.civic[static_cast<std::size_t>(civic::Kind::Church)].count > 0) {
        const float W = 15.0f, D = 26.0f;
        bool placed = false;
        if (green.size() > 2) {
            // On the green, its nave along it, the door to one tip.
            const glm::vec2 front = coreTan;
            Obb yard;
            yard.c = core; yard.u = glm::vec2(-front.y, front.x); yard.hu = 0.5f * W + 6.0f; yard.hv = 0.5f * D + 6.0f;
            taken.push_back(yard);
            addLot(core, front, W, D, 0, 0, static_cast<int>(civic::Kind::Church), hashU(g.seed ^ 0xc4c4U));
            placed = true;
        }
        for (int tryN = 0; tryN < 12 && !placed; ++tryN) {
            const float s = coreS + static_cast<float>((tryN / 2) * ((tryN & 1) ? 12 : -12));
            glm::vec2 t;
            const glm::vec2 c = along(lanes[0].pts, s, &t);
            const int side = (tryN & 2) ? 1 : -1;
            const glm::vec2 n = glm::vec2(-t.y, t.x) * static_cast<float>(side);
            const glm::vec2 pos = c + n * (0.5f * mainW + 7.0f + 0.5f * D);
            Obb yard;
            yard.c = pos; yard.u = t; yard.hu = 0.5f * W + 6.0f; yard.hv = 0.5f * D + 5.0f;
            if (!fitsAll(yard, 1.0f)) continue;
            taken.push_back(yard);
            addLot(pos, -n, W, D, 0, 0, static_cast<int>(civic::Kind::Church), hashU(g.seed ^ 0xc4c4U));
            placed = true;
        }
    }

    // --- What else the rule asks for ----------------------------------------------------
    // Each on a plot of its own beside a street, where such a thing stands in a
    // village: the Dorfgemeinschaftshaus and the library by the church, the
    // fire station and the kindergarten among the houses, the school further
    // out, the petrol station and the supermarket on the main road at the edge,
    // where the land is and the traffic passes. Before the houses, which fill in
    // round them.
    {
        struct Want { civic::Kind kind; float w, d, ring; bool mainOnly; };
        auto plotOf = [](civic::Kind k) -> Want {
            // The plot each model needs at least (CivicGen: the fire station's
            // hall and apron, the hall's forecourt, the kindergarten's garden).
            switch (k) {
                case civic::Kind::TownHall:      return {k, 30.0f, 30.0f, 0.15f, false};
                case civic::Kind::FireStation:   return {k, 19.0f, 28.0f, 0.55f, false};
                case civic::Kind::School:        return {k, 46.0f, 40.0f, 0.65f, false};
                case civic::Kind::Kindergarten:  return {k, 32.0f, 30.0f, 0.5f,  false};
                case civic::Kind::PetrolStation: return {k, 42.0f, 36.0f, 1.0f,  true};
                case civic::Kind::Supermarket:   return {k, 42.0f, 38.0f, 1.0f,  true};
                case civic::Kind::Police:        return {k, 26.0f, 24.0f, 0.4f,  false};
                case civic::Kind::Library:       return {k, 34.0f, 32.0f, 0.2f,  false};
                case civic::Kind::Museum:        return {k, 36.0f, 38.0f, 0.2f,  false};
                default:                         return {k, 40.0f, 36.0f, 0.5f,  false};
            }
        };
        std::vector<Want> wants;
        for (int k = 0; k < civic::kKinds; ++k) {
            const civic::Kind kind = static_cast<civic::Kind>(k);
            // Nothing a village would have out of these: no estate, no plant,
            // no tip, no station. The church is placed above.
            if (kind == civic::Kind::Church || kind == civic::Kind::Industry ||
                kind == civic::Kind::PowerPlant || kind == civic::Kind::Landfill)
                continue;
            for (int n = 0; n < r.civic[static_cast<std::size_t>(k)].count; ++n) wants.push_back(plotOf(kind));
        }
        const float coreR = 0.55f * std::min(hx, hz) + 25.0f;
        for (const Want& w : wants) {
            float best = 1e30f;
            Obb bestYard;
            glm::vec2 bestPos(0.0f), bestFront(0.0f);
            int bestStreet = -1;
            for (std::size_t li = 0; li < lanes.size(); ++li) {
                if (w.mainOnly && li != 0) continue;
                const VLane& ln = lanes[li];
                const float len = polyLen(ln.pts);
                for (float sAt = 10.0f; sAt < len - 10.0f; sAt += 6.0f)
                    for (int side = -1; side <= 1; side += 2) {
                        glm::vec2 t;
                        const glm::vec2 c = along(ln.pts, sAt, &t);
                        const float ring = glm::length(c - core) / coreR;
                        const float score = std::abs(ring - w.ring) + 0.15f * unit(hash3(g.seed ^ 0xc1f1U,
                            static_cast<std::uint32_t>(li * 1000) + static_cast<std::uint32_t>(sAt),
                            static_cast<std::uint32_t>(side + 2)));
                        if (score >= best) continue;
                        const glm::vec2 n = glm::vec2(-t.y, t.x) * static_cast<float>(side);
                        const glm::vec2 pos = c + n * (0.5f * ln.width + 3.0f + 0.5f * w.d);
                        if (!inside(pos, w.ring > 0.9f ? 1.25f : 1.05f)) continue;
                        Obb yard;
                        yard.c = pos; yard.u = t; yard.hu = 0.5f * w.w + 3.0f; yard.hv = 0.5f * w.d + 2.5f;
                        if (!fitsAll(yard, 0.8f)) continue;
                        if (green.size() > 2 && insidePoly(green, pos)) continue;
                        best = score; bestYard = yard; bestPos = pos; bestFront = -n;
                        bestStreet = static_cast<int>(li);
                    }
            }
            if (bestStreet < 0) continue;
            taken.push_back(bestYard);
            addLot(bestPos, bestFront, w.w, w.d, bestStreet, 0, static_cast<int>(w.kind),
                   hash3(g.seed ^ 0xc1f2U, static_cast<std::uint32_t>(w.kind), static_cast<std::uint32_t>(taken.size())));
        }
    }

    // --- The plots, along every street ------------------------------------------------
    // Both sides, from the core outwards: close and right on the street at the
    // heart, behind front gardens and with gaps further out, and an empty plot
    // now and then -- an orchard, a paddock. Each house turned to its street,
    // gable on or eaves on, and never quite square.
    {
        std::vector<glm::vec2> houseWD;
        for (int k = 0; k < houseKinds(Zone::Houses); ++k) {
            const housegen::Params hp = housePrototype(r, Zone::Houses, k);
            houseWD.push_back({hp.width, hp.depth});
        }
        const float coreR = 0.55f * std::min(hx, hz) + 25.0f;
        for (std::size_t li = 0; li < lanes.size(); ++li) {
            const VLane& ln = lanes[li];
            const float len = polyLen(ln.pts);
            for (int side = -1; side <= 1; side += 2) {
                std::uint32_t hs = hash3(g.seed ^ 0x9107U, static_cast<std::uint32_t>(li),
                                         static_cast<std::uint32_t>(side + 2));
                float s = 8.0f + 10.0f * unit(hs);
                int guard = 0;
                while (s < len - 6.0f && guard++ < 300) {
                    hs = hashU(hs + 0x51U);
                    glm::vec2 t;
                    const glm::vec2 c = along(ln.pts, s, &t);
                    const glm::vec2 n = glm::vec2(-t.y, t.x) * static_cast<float>(side);
                    // How far out: 0 at the heart, 1 at the rim -- the village's own
                    // ellipse, not a circle round the church, so a long village is
                    // dense along its length. A Straßendorf is dense to its ends.
                    const float dCore = std::min(glm::length(c - core) / coreR,
                                                 std::sqrt((c.x / hx) * (c.x / hx) + (c.y / hz) * (c.y / hz)) /
                                                     std::max(rim(c), 0.5f));
                    const float outer = form == 2 && li == 0 ? 0.0f : smooth01(0.45f, 0.95f, dCore);
                    const bool dense = outer < 0.5f;
                    if (!inside(c, 1.02f)) { s += 6.0f; continue; }
                    // An empty plot -- an orchard, a paddock -- more of them outwards.
                    if (unit(hashU(hs ^ 0x1U)) < 0.04f + 0.16f * outer) {
                        s += 12.0f + 14.0f * unit(hashU(hs ^ 0x2U));
                        continue;
                    }
                    const int kind = static_cast<int>(hashU(hs ^ 0x3U) % houseWD.size());
                    const glm::vec2 wd = houseWD[static_cast<std::size_t>(kind)];
                    // Gable to the street, or eaves: a Straßendorf stands gable
                    // on almost to a house, round a green they turn their long
                    // side to it, a Haufendorf mixes the two.
                    const float gableShare = form == 2 ? 0.85f : form == 3 ? 0.4f : 0.55f;
                    const bool gable = unit(hashU(hs ^ 0x4U)) < gableShare;
                    const float alongExt = gable ? wd.x : wd.y;      // what it takes of the street
                    const float acrossExt = gable ? wd.y : wd.x;
                    const float setback = 0.8f + 2.0f * unit(hashU(hs ^ 0x5U)) +
                                          5.0f * outer * unit(hashU(hs ^ 0x9U));
                    const float skew = glm::radians(10.0f * (unit(hashU(hs ^ 0x6U)) * 2.0f - 1.0f));
                    const glm::vec2 pos = c + t * (0.5f * alongExt) +
                                          n * (0.5f * ln.width + setback + 0.5f * acrossExt);
                    // The house's own front: towards the street, or along it.
                    glm::vec2 front = gable ? -n : (unit(hashU(hs ^ 0x7U)) < 0.5f ? t : -t);
                    front = turnBy(front, skew);
                    Obb o;
                    o.c = pos;
                    o.u = glm::vec2(-front.y, front.x);
                    o.hu = 0.5f * wd.x + 0.3f;   // the eaves
                    o.hv = 0.5f * wd.y + 0.3f;
                    // Turned askew, or on the outside of a bend, a corner comes
                    // closer to the street than the setback: stand it back until
                    // the nearest corner keeps it.
                    for (int it = 0; it < 3; ++it) {
                        glm::vec2 cs[4];
                        o.corners(cs);
                        float nearest = 1e30f;
                        for (const glm::vec2& q : cs) nearest = std::min(nearest, nearestOn(ln.pts, q));
                        const float want = 0.5f * ln.width + std::max(setback, 0.6f);
                        if (nearest >= want - 0.05f) break;
                        o.c += n * (want - nearest);
                    }
                    // The claim: the house, a yard beside it, and the garden behind.
                    Obb claim;
                    claim.u  = t;
                    claim.hu = 0.5f * alongExt + (dense ? 1.5f : 4.0f);
                    claim.hv = 0.5f * (acrossExt + (dense ? 5.0f : 12.0f));
                    claim.c  = o.c + n * (dense ? 2.5f : 6.0f) * 0.5f;
                    bool ok = inside(o.c, 1.08f) && fitsAll(o, 0.6f);
                    if (ok)
                        for (const Obb& tk : taken)
                            if (overlaps(claim, tk, 0.0f)) { ok = false; break; }
                    if (ok && green.size() > 2) {
                        glm::vec2 cs[4];
                        o.corners(cs);
                        for (const glm::vec2& q : cs)
                            if (insidePoly(green, q)) { ok = false; break; }
                    }
                    if (!ok) { s += 4.0f; continue; }
                    taken.push_back(claim);
                    addLot(o.c, front, wd.x, wd.y, static_cast<int>(li), kind, -1, hs | 1U);
                    s += alongExt + 1.5f + (3.0f + 10.0f * outer) * unit(hashU(hs ^ 0x8U));
                }
            }
        }
    }

    // --- Into the world ------------------------------------------------------------
    for (const VLane& ln : lanes) {
        Street st;
        for (const glm::vec2& p : ln.pts) st.pts.push_back(world(p));
        st.width  = ln.width;
        st.avenue = false;   // the main street has the right of way (Layout::main), not lights
        st.name   = ln.name;
        st.leaves = ln.leaves;
        st.line   = -1;
        L.streets.push_back(std::move(st));
    }
    L.main = 0;
    L.core = world(core);
    for (const glm::vec2& p : green) L.green.push_back(world(p));
    // The gates: on the main street eight metres in from each end, the road off
    // it at forty degrees -- and on a fork that leaves the village, the same.
    auto gateAt = [&](const VLane& ln, bool atEnd, float sign) {
        const float len = polyLen(ln.pts);
        glm::vec2 t;
        const glm::vec2 p = along(ln.pts, atEnd ? len - 8.0f : 8.0f, &t);
        const glm::vec2 outward = atEnd ? t : -t;
        L.gates.push_back(world(p));
        L.gateDirs.push_back(glm::normalize(worldDir(turnBy(outward, glm::radians(40.0f) * sign))));
    };
    const float gs = unit(hashU(g.seed ^ 0x6a7eU)) < 0.5f ? 1.0f : -1.0f;
    gateAt(lanes[0], false, gs);
    gateAt(lanes[0], true, -gs);
    // The fork, where it runs on out of the village: a third way in.
    if (form == 1 && lanes.size() > 1 && (lanes[1].name == "Obergasse" || lanes[1].name == "Untergasse") &&
        !inside(lanes[1].pts.back(), 1.0f))
        gateAt(lanes[1], true, gs);
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
        // A village builds farmhouses and old family houses, not town villas.
        static const Preset kVillage[5] = {Preset::CountryHouse, Preset::Classic, Preset::LargeFamily,
                                           Preset::Cottage, Preset::MultiGen};
        housegen::applyPreset(p, (r.built().pattern == 1 ? kVillage : kMix)[glm::clamp(kind, 0, 4)]);
    }
    p.facadeColor = r.facadeColor;
    p.roofColor   = r.roofColor;
    // A street's plinths are a mid grey render, not the House panel's anthracite.
    p.plinthColor = {0.34f, 0.34f, 0.33f};
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
    if (lay.organic) return lay.lots;   // planned along the lanes, with them
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
                lot.side  = k;
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
    p.props = props::ensurePalette(materials);
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
    // ...and the same glass with a room lit behind it, for the windows that are
    // lit after dark: warm, and a dimmer one (a lamp in the back of the room).
    auto litGlass = [&](const char* name, glm::vec3 light, float strength) {
        const fitzel::AssetId id = variantOf(materials, p.houses.glass, name,
                                             glm::vec3(0.10f, 0.12f, 0.14f));
        for (MaterialDef& m : materials)
            if (m.assetId == id) {
                m.emission         = light;
                m.emissionStrength = strength;
            }
        return id;
    };
    p.houseGlassLit = {litGlass("City House Window Lit", {1.00f, 0.80f, 0.52f}, 1.6f),
                       litGlass("City House Window Dim", {0.95f, 0.86f, 0.70f}, 0.55f)};
    // The colour variants: copies of the facade and roof materials (and the
    // block facade, window grid and all), picked per building in derive().
    // The houses' render and tiles are textured and coloured by their tint
    // (housegen::tintSurface): a variant's colour goes there too.
    auto surfaceVariant = [&](const fitzel::AssetId& src, const std::string& name, glm::vec3 c) {
        const fitzel::AssetId id = variantOf(materials, src, name, c);
        for (MaterialDef& m : materials)
            if (m.assetId == id) housegen::tintSurface(m, c);
        return id;
    };
    p.houseWalls = {p.houses.facade};
    for (std::size_t k = 0; k < std::size(kHouseWalls); ++k)
        p.houseWalls.push_back(surfaceVariant(p.houses.facade,
                                              "City House Facade " + std::to_string(k + 2), kHouseWalls[k]));
    p.houseRoofs = {p.houses.roof};
    for (std::size_t k = 0; k < std::size(kRoofs); ++k)
        p.houseRoofs.push_back(surfaceVariant(p.houses.roof,
                                              "City House Roof " + std::to_string(k + 2), kRoofs[k]));
    // The strip round the houses: rounded pebbles, their own colour
    // (tools/gen-ground-textures.py, gravel_01), a flat grey without them.
    p.houseGravel = variantOf(materials, p.civic.gravel, "City House Gravel", glm::vec3(0.42f, 0.41f, 0.39f));
    for (MaterialDef& m : materials)
        if (m.assetId == p.houseGravel) {
            m.texId       = fitzel::AssetId::fromString("1bbda553c0024ffc8d49c6b7a8b73a48");
            m.normalTexId = fitzel::AssetId::fromString("47414e7193814f98a0af9907c0a5dfb1");
            m.tint        = glm::vec3(0.8f);
            m.roughness   = 0.95f;
        }
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
    struct Part {
        fitzel::AssetId  material;
        fitzel::MeshData mesh;        // local frame
        int              bucket = -1; // the glass: which share of the panes (else -1)
    };
    std::vector<Part> parts;
    float width = 10.0f, depth = 10.0f, eaves = 6.0f;
    // The front door, for the house number beside it: its centre along the
    // street face (local x) and half its width; the plinth it stands on.
    bool  hasDoor = false;
    float doorX = 0.0f, doorHalf = 0.5f, plinth = 0.0f;
    float storey = 2.8f;   // the ground floor's height (a shop's sign goes over it)
};

// The panes of a house type's glass are dealt into this many shares; each house
// lights some shares and leaves the rest dark (see derive), so two copies of one
// house type are not lit alike.
constexpr int kGlassShares = 4;

// The glass of a house, pane by pane into kGlassShares meshes. A pane is what
// its triangles hold together through shared corners (by position: its faces
// have their own normals, so no index joins them); which share it goes to is a
// hash of where it is.
std::vector<fitzel::MeshData> sharePanes(const fitzel::MeshData& g) {
    const std::size_t n = g.vertices.size();
    std::vector<std::uint32_t> parent(n);
    for (std::size_t i = 0; i < n; ++i) parent[i] = static_cast<std::uint32_t>(i);
    auto find = [&](std::uint32_t x) {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    };
    auto join = [&](std::uint32_t a, std::uint32_t b) {
        a = find(a); b = find(b);
        if (a != b) parent[std::max(a, b)] = std::min(a, b);
    };
    std::map<std::tuple<int, int, int>, std::uint32_t> at;
    for (std::size_t i = 0; i < n; ++i) {
        const glm::vec3 p = g.vertices[i].position;
        const auto k = std::make_tuple(static_cast<int>(std::lround(p.x * 1000.0f)),
                                       static_cast<int>(std::lround(p.y * 1000.0f)),
                                       static_cast<int>(std::lround(p.z * 1000.0f)));
        const auto it = at.find(k);
        if (it == at.end()) at.emplace(k, static_cast<std::uint32_t>(i));
        else join(static_cast<std::uint32_t>(i), it->second);
    }
    for (std::size_t t = 0; t + 2 < g.indices.size(); t += 3) {
        join(g.indices[t], g.indices[t + 1]);
        join(g.indices[t + 1], g.indices[t + 2]);
    }
    std::vector<fitzel::MeshData> out(kGlassShares);
    std::vector<std::vector<std::int64_t>> remap(kGlassShares, std::vector<std::int64_t>(n, -1));
    for (std::size_t t = 0; t + 2 < g.indices.size(); t += 3) {
        const glm::vec3 r = g.vertices[find(g.indices[t])].position;
        const std::uint32_t h = static_cast<std::uint32_t>(std::lround(r.x * 37.0f + 5000.0f)) * 73856093u ^
                                static_cast<std::uint32_t>(std::lround(r.y * 53.0f + 5000.0f)) * 19349663u ^
                                static_cast<std::uint32_t>(std::lround(r.z * 41.0f + 5000.0f)) * 83492791u;
        const std::size_t b = (h >> 7) % kGlassShares;
        for (int c = 0; c < 3; ++c) {
            const std::uint32_t v = g.indices[t + static_cast<std::size_t>(c)];
            std::int64_t& to = remap[b][v];
            if (to < 0) {
                to = static_cast<std::int64_t>(out[b].vertices.size());
                out[b].vertices.push_back(g.vertices[v]);
            }
            out[b].indices.push_back(static_cast<std::uint32_t>(to));
        }
    }
    return out;
}

HouseProto makeHouse(const housegen::Params& p, const housegen::Palette& pal) {
    HouseProto hp;
    hp.width = p.width;
    hp.depth = p.depth;
    hp.eaves = p.plinth + p.storeys * p.storeyHeight;
    // Where the front door is: the plan's (the street side is its north wall,
    // which the house's local frame puts at z = -depth/2). And the doors
    // themselves: the shell leaves out the leaves (they stand open, into rooms
    // that are not there), so every outside door on the ground floor is closed
    // with a panel set into its opening -- else the front door, the number
    // beside it, is a hole through to the garden.
    fitzel::MeshData doorPanels, glassPanels;
    {
        const housegen::Plan plan = housegen::layout(p);
        const float W = plan.params.width, D = plan.params.depth;
        auto panel = [](fitzel::MeshData& md, const glm::vec3 (&q)[4], glm::vec3 n) {
            const auto base = static_cast<std::uint32_t>(md.vertices.size());
            for (const glm::vec3& c : q) {
                fitzel::Vertex v{};
                v.position = c;
                v.normal   = n;
                v.uv = glm::vec2(std::abs(n.x) > 0.5f ? c.z : c.x, c.y);
                md.vertices.push_back(v);
            }
            const bool front = glm::dot(glm::cross(q[1] - q[0], q[2] - q[0]), n) > 0.0f;
            if (front) md.indices.insert(md.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
            else       md.indices.insert(md.indices.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
        };
        if (!plan.levels.empty()) {
            const housegen::LevelPlan& lv = plan.levels.front();
            for (const housegen::Opening& o : lv.openings) {
                using K = housegen::Opening::Kind;
                if (o.exterior == housegen::Side::None || (o.kind != K::EntryDoor && o.kind != K::FrenchDoor))
                    continue;
                const float y0 = lv.z, y1 = lv.z + o.head;
                fitzel::MeshData& md = o.kind == K::EntryDoor ? doorPanels : glassPanels;
                if (o.alongX) {
                    const float z  = 0.5f * (o.r.y0 + o.r.y1) - 0.5f * D;
                    const float x0 = o.r.x0 - 0.5f * W, x1 = o.r.x1 - 0.5f * W;
                    const glm::vec3 q[4] = {{x0, y0, z}, {x1, y0, z}, {x1, y1, z}, {x0, y1, z}};
                    panel(md, q, {0.0f, 0.0f, o.exterior == housegen::Side::N ? -1.0f : 1.0f});
                } else {
                    const float x  = 0.5f * (o.r.x0 + o.r.x1) - 0.5f * W;
                    const float z0 = o.r.y0 - 0.5f * D, z1 = o.r.y1 - 0.5f * D;
                    const glm::vec3 q[4] = {{x, y0, z0}, {x, y0, z1}, {x, y1, z1}, {x, y1, z0}};
                    panel(md, q, {o.exterior == housegen::Side::W ? -1.0f : 1.0f, 0.0f, 0.0f});
                }
            }
        }
        hp.plinth = plan.params.plinth;
        hp.storey = plan.params.storeyHeight;
        if (!plan.levels.empty())
            for (const housegen::Opening& o : plan.levels.front().openings)
                if (o.kind == housegen::Opening::Kind::EntryDoor && o.alongX) {
                    hp.hasDoor  = true;
                    hp.doorX    = 0.5f * (o.r.x0 + o.r.x1) - 0.5f * plan.params.width;
                    hp.doorHalf = 0.5f * o.width();
                    break;
                }
    }
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
    for (auto& [m, acc] : byMat) {
        if (m != pal.glass) {
            hp.parts.push_back({m, std::move(acc.md), -1});
            continue;
        }
        std::vector<fitzel::MeshData> shares = sharePanes(acc.md);
        for (int b = 0; b < kGlassShares; ++b)
            if (!shares[static_cast<std::size_t>(b)].indices.empty())
                hp.parts.push_back({m, std::move(shares[static_cast<std::size_t>(b)]), b});
    }
    // The closed doors: the front door in the entrance's timber, a garden
    // door in the dark glass (never lit: nobody stands in a glass door).
    if (!doorPanels.indices.empty()) hp.parts.push_back({pal.timber, std::move(doorPanels), -1});
    if (!glassPanels.indices.empty()) hp.parts.push_back({pal.glass, std::move(glassPanels), -1});
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
    // The houses' gravel strips, one mesh each (they follow the ground): stable
    // addresses for the extras that point at them.
    std::deque<fitzel::MeshData> gravelMeshes;
    const int budget = std::max(r.budget, 0);

    // Which street every grid line is: the crossings, the bus stops and the
    // house numbers all ask.
    std::vector<int> zLine(static_cast<std::size_t>(lay.nx + 1), -1);
    std::vector<int> xLine(static_cast<std::size_t>(lay.nz + 1), -1);
    for (int s = 0; s < static_cast<int>(lay.streets.size()); ++s) {
        const Street& st = lay.streets[static_cast<std::size_t>(s)];
        auto& line = st.alongZ ? zLine : xLine;
        if (st.line >= 0 && st.line < static_cast<int>(line.size()))
            line[static_cast<std::size_t>(st.line)] = s;
    }
    // Who has the right of way where no lights stand -- and where the shops
    // are: the avenues and the main street, the one streetNames calls the
    // Hauptstrasse (the avenue nearest the middle, or the middle line).
    int mainStreet = -1, mainOff = 1 << 30;
    for (int i = 0; i < static_cast<int>(lay.streets.size()); ++i) {
        const Street& s = lay.streets[static_cast<std::size_t>(i)];
        if (!s.alongZ) continue;
        const int off = std::abs(s.line - (lay.nx + 1) / 2) * 2 + (s.avenue ? 0 : 1);
        if (off < mainOff) { mainOff = off; mainStreet = i; }
    }
    if (lay.main >= 0) mainStreet = lay.main;   // a village names its own
    auto major = [&](const Street& s) {
        return s.avenue || (mainStreet >= 0 && &s == &lay.streets[static_cast<std::size_t>(mainStreet)]);
    };
    // The street a lot fronts: the grid line its block side lies on.
    auto streetOf = [&](const Lot& lot) {
        if (lot.street >= 0) return lot.street;
        if (lot.block < 0) return -1;
        const Block& B = lay.blocks[static_cast<std::size_t>(lot.block)];
        switch (lot.side) {
            case 0:  return xLine[static_cast<std::size_t>(B.iz)];
            case 1:  return zLine[static_cast<std::size_t>(B.ix + 1)];
            case 2:  return xLine[static_cast<std::size_t>(B.iz + 1)];
            default: return zLine[static_cast<std::size_t>(B.ix)];
        }
    };
    // A model's parts welded in at `at`, turned `yaw` degrees about +Y.
    auto stamp = [&](const civic::Model& M, glm::vec3 at, float yaw, int chunk, bool shadow) {
        for (const auto& [mat, md] : M.parts) {
            city::Extra x;
            x.mesh        = &md;
            x.at          = at;
            x.yaw         = yaw;
            x.material    = mat;
            x.chunk       = chunk;
            x.castsShadow = shadow;
            extras.push_back(x);
        }
    };
    // The kilometre square the small things are merged in, not the buildings'
    // 500 m: a plate or a sign is a few dozen vertices in half a dozen
    // materials, and per building chunk those materials cost more draws than
    // the plates are worth culling for.
    auto furnitureChunk = [](glm::vec2 at) {
        return (1 << 28) + static_cast<int>(std::floor(at.x / 1000.0f)) * 8192 +
               static_cast<int>(std::floor(at.y / 1000.0f));
    };
    // A point in a building's own frame (local -Z its street face), turned by
    // `yaw` the way stamp() turns a model.
    auto turn = [](float yaw, glm::vec3 l) {
        const float rr = glm::radians(yaw), cs = std::cos(rr), sn = std::sin(rr);
        return glm::vec3(l.x * cs + l.z * sn, l.y, -l.x * sn + l.z * cs);
    };
    // The small things gathered while the buildings go up: where each number
    // plate goes (a house is numbered once all of its street's houses are
    // known), the plots left empty (a billboard may stand on one), and the
    // fronts of the schools and kindergartens (a zebra crossing).
    struct NumberSpot {
        int street; float along; bool odd; glm::vec3 at; float yaw; int chunk;
        std::size_t placed;   // the building (index into out.placed)
        std::string name;     // the street's name, as its road is called
    };
    std::vector<NumberSpot> numberSpots;
    std::vector<const Lot*> emptyPlots;
    std::vector<std::pair<glm::vec2, glm::vec2>> zebraAt;   // a point in the street, its direction
    auto numberHere = [&](const Lot& lot, glm::vec3 at, float yaw) {
        const int s = streetOf(lot);
        if (!r.houseNumbers || s < 0) return;
        const Street& st = lay.streets[static_cast<std::size_t>(s)];
        const glm::vec2 a = st.pts.front();
        const glm::vec2 d = glm::normalize(st.pts.back() - a);
        // The name the street goes by is its road's (renaming the road in the
        // Roads panel renames the address); the town's own before it is laid.
        std::string name = st.name;
        glm::vec2 c, rd;
        float half = 0.0f;
        if (roads.snap(lot.pos + lot.front * (0.5f * lot.depth + 3.0f),
                       glm::vec2(-lot.front.y, lot.front.x), c, rd, half)) {
            const int road = roads.nearestParallel(c, rd);
            if (road >= 0 && !ctx.roads[static_cast<std::size_t>(road)].name.empty())
                name = ctx.roads[static_cast<std::size_t>(road)].name;
        }
        // Called before the building joins out.placed: its index is the next.
        numberSpots.push_back({s, glm::dot(lot.pos - a, d), cross2(d, lot.pos - a) > 0.0f, at, yaw,
                               furnitureChunk(lot.pos), out.placed.size(), name});
    };

    // --- Public buildings and industry: a whole block each ----------------------
    // The plot is the block's building line, squared up with its longest side:
    // the biggest rectangle of that orientation that fits, found by shrinking
    // until all four corners are inside. The street the building addresses is
    // the one along that longest side.
    std::deque<civic::Model> models;   // stable addresses: the extras point into them
    std::vector<glm::vec2>   plants;   // power stations, where the power lines start
    // One public building (or an industrial estate) on a plot `W` wide along
    // its street and `D` deep, its front to the street: the ground measured, the
    // model built or the imported one stood there. A block's plot, or a
    // village's own (villageLayout plans its church with the lanes).
    auto buildCivic = [&](civic::Kind kind, glm::vec2 icen, glm::vec2 front, float W, float D,
                          Bare plotBare, Zone zone, std::uint32_t h) {
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
        if (inRoad)  { ++out.stats.skippedRoad;  return; }
        if (inWater) { ++out.stats.skippedWater; return; }
        const float diag = std::sqrt(W * W + D * D);
        if ((gHi - gLo) / std::max(diag, 1.0f) > std::max(r.maxSlope, 0.01f) * 1.5f) {
            ++out.stats.skippedSlope;
            return;
        }

        // A zebra crossing in front of a school, off the middle of its frontage
        // (the middle is where a bus stop goes).
        if (kind == civic::Kind::School || kind == civic::Kind::Kindergarten)
            zebraAt.push_back({icen + front * (0.5f * D + std::max(r.sidewalk, 0.0f) + 1.0f) +
                                   side * std::min(14.0f, 0.3f * W),
                               side});
        const CivicSlot& slot = r.civic[static_cast<std::size_t>(kind)];
        if (kind == civic::Kind::PowerPlant) plants.push_back(icen);
        if (slot.useModel && !slot.model.empty()) {
            // An imported model instead: centred on the block, standing on its
            // high side, turned to the street -- and, unless told otherwise,
            // scaled to fill the block (a model's units are anyone's guess).
            glm::vec3 lo(-0.5f), hi(0.5f);
            const bool known = ctx.modelBounds && ctx.modelBounds(slot.model, lo, hi);
            const glm::vec3 size = glm::max(hi - lo, glm::vec3(0.01f));
            const int   turn = ((slot.turn % 4) + 4) % 4;
            const float sx = (turn & 1) ? size.z : size.x, sz = (turn & 1) ? size.x : size.z;
            const float scale = known && slot.fitPlot ? 0.92f * std::min(W / sx, D / sz) : 1.0f;
            const float yaw   = yawFacing(front);
            const float baseY = gHi;
            glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(icen.x, baseY, icen.y));
            m = glm::rotate(m, glm::radians(yaw + 90.0f * static_cast<float>(turn)), glm::vec3(0, 1, 0));
            m = glm::scale(m, glm::vec3(scale));
            m = glm::translate(m, glm::vec3(-0.5f * (lo.x + hi.x), -lo.y, -0.5f * (lo.z + hi.z)));
            out.models.push_back({slot.model, m});
            if (known && r.collider) {
                city::Piece c;
                c.center  = glm::vec3(icen.x, baseY + 0.5f * size.y * scale, icen.y);
                c.half    = 0.5f * scale * glm::vec3(sx, size.y, sz);
                c.yaw     = yaw;
                c.collide = true;
                pcs.push_back(c);
                pcChunk.resize(pcs.size(), chunkOf(icen));
            }
            ++out.stats.civic[static_cast<std::size_t>(kind)];
            Placed pl;
            pl.pos    = icen;
            pl.radius = 0.5f * diag + 1.5f;
            pl.zone   = zone;
            out.placed.push_back(pl);
            ++out.stats.built;
            out.bare.push_back(plotBare);
            return;
        }
        models.push_back(civic::build(kind, W, D, h, pal.civic));
        const civic::Model& M = models.back();
        if (M.empty()) { ++out.stats.skippedEmpty; return; }
        out.bare.push_back(plotBare);

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

        ++out.stats.civic[static_cast<std::size_t>(kind)];
        Placed pl;
        pl.pos    = icen;
        pl.radius = 0.5f * diag + 1.5f;
        pl.zone   = zone;
        out.placed.push_back(pl);
        ++out.stats.built;
    };
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

        const glm::vec2 side(-front.y, front.x);
        // Nothing grows through a forecourt, a car park or a schoolyard: the
        // plot is kept bare out to the pavement (the building line's box in the
        // plot's axes -- within reason, on a crooked block).
        Bare plotBare{icen, side, 0.5f * W, 0.5f * D};
        for (const glm::vec2& c : inset) {
            plotBare.hu = std::max(plotBare.hu, std::abs(glm::dot(c - icen, side)));
            plotBare.hv = std::max(plotBare.hv, std::abs(glm::dot(c - icen, front)));
        }
        plotBare.hu = std::min(plotBare.hu, 0.5f * W + 6.0f) + 0.5f;
        plotBare.hv = std::min(plotBare.hv, 0.5f * D + 6.0f) + 0.5f;

        const civic::Kind kind = estate ? civic::Kind::Industry
                                        : static_cast<civic::Kind>(B.civic);
        const std::uint32_t h = hash3(r.seed ^ 0xc1c1cU, static_cast<std::uint32_t>(bi), 7U);
        buildCivic(kind, icen, front, W, D, plotBare, B.zone, h);
    }
    // A village's church: on a plot of its own beside the lane or on the green,
    // with its churchyard kept open round it.
    for (const Lot& lot : plan) {
        if (lot.civic < 0) continue;
        if (static_cast<int>(out.placed.size()) >= budget) { out.stats.budgetHit = true; break; }
        const glm::vec2 side(-lot.front.y, lot.front.x);
        buildCivic(static_cast<civic::Kind>(lot.civic), lot.pos, lot.front, lot.width, lot.depth,
                   Bare{lot.pos, side, 0.5f * lot.width + 4.0f, 0.5f * lot.depth + 4.0f}, Zone::Houses,
                   lot.hash | 1U);
    }

    for (const Lot& lot : plan) {
        if (lot.civic >= 0) continue;   // built above
        if (static_cast<int>(out.placed.size()) >= budget) { out.stats.budgetHit = true; break; }
        const std::uint32_t h = lot.hash;
        if (unit(hashU(h ^ 0x1dU)) > r.fill) {
            ++out.stats.skippedEmpty;
            emptyPlots.push_back(&lot);
            continue;
        }

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
                const float ring = r.towerRing > 1e-3f && lot.block >= 0
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
                bp.shopfronts   = lot.block >= 0 &&
                                  lay.blocks[static_cast<std::size_t>(lot.block)].ring < r.blockRing * 0.6f;
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
            // Paved round it; and on its street face (the podium's, on a
            // tower) a house number, and over a lit ground floor a shop.
            out.bare.push_back({lot.pos, u, 0.5f * lot.width + 0.5f, 0.5f * lot.depth + 0.5f});
            if (bp.podiumFloors > 0 || !tower) {
                const float fh    = bp.floorHeight;
                const float faceZ = -0.5f * lot.depth - 0.002f;
                const glm::vec3 nAt = turn(yaw, {-0.35f * lot.width, 0.0f, faceZ});
                const glm::vec2 n2(lot.pos.x + nAt.x, lot.pos.y + nAt.z);
                numberHere(lot, {n2.x, std::max(gLo + 0.86f * fh, ground(n2) + 2.1f), n2.y}, yaw);
                // A tower's lit podium is shops; so is many a block's lit ground
                // floor, and now and then one further out.
                const float shopChance = tower ? 0.8f : bp.shopfronts ? 0.55f : 0.25f;
                if (r.shopSigns && unit(hashU(h ^ 0x5409U)) < shopChance) {
                    const float maxW = std::min(0.5f * lot.width, 10.0f);
                    models.push_back(props::shopSign(props::shopName(hashU(h ^ 0x5410U)),
                                                     static_cast<int>(hashU(h ^ 0x5411U) % 4U), maxW,
                                                     pal.props));
                    const glm::vec3 sAt = turn(yaw, {0.1f * lot.width, 0.0f, faceZ});
                    const glm::vec2 s2(lot.pos.x + sAt.x, lot.pos.y + sAt.z);
                    const float sy = std::max(gLo + 0.775f * fh + 0.45f, ground(s2) + 2.8f);
                    stamp(models.back(), {s2.x, sy, s2.y}, yaw, furnitureChunk(lot.pos), false);
                    ++out.stats.shops;
                }
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
            for (const HouseProto::Part& part : hp.parts) {
                city::Extra x;
                x.mesh = &part.mesh;
                x.at   = glm::vec3(lot.pos.x, baseY, lot.pos.y);
                x.yaw  = yaw;
                const fitzel::AssetId& mat = part.material;
                x.material = mat == pal.houses.facade ? wall : mat == pal.houses.roof ? roof : mat;
                // Its windows: some shares of its panes lit (warm, or dimmer),
                // the rest dark -- as many as the rule's windowLit asks for.
                if (part.bucket >= 0 && pal.houseGlassLit.size() >= 2) {
                    const float u = unit(hashU(h ^ (0x9e37U + static_cast<std::uint32_t>(part.bucket) * 0x85ebU)));
                    const float lit = glm::clamp(r.windowLit, 0.0f, 1.0f);
                    if (u < lit * 0.7f)  x.material = pal.houseGlassLit[0];
                    else if (u < lit)    x.material = pal.houseGlassLit[1];
                }
                x.chunk    = chunk;
                extras.push_back(x);
            }
            // The number beside the front door, at head height.
            if (hp.hasDoor) {
                float lx = hp.doorX - (hp.doorHalf + 0.75f);       // the reader's right is -x
                if (lx < -0.5f * hp.width + 0.35f) lx = hp.doorX + hp.doorHalf + 0.75f;
                const glm::vec3 l = turn(yaw, {lx, 0.0f, -0.5f * hp.depth - 0.002f});
                numberHere(lot, {lot.pos.x + l.x, baseY + hp.plinth + 1.8f, lot.pos.y + l.z}, yaw);
            }
            // A shop on the ground floor of a terraced house on a main street
            // or in the old centre: its name over the half without the door.
            if (r.shopSigns && lot.zone == Zone::Rows && hp.hasDoor) {
                const int s = streetOf(lot);
                const bool onMain = s >= 0 && major(lay.streets[static_cast<std::size_t>(s)]);
                const float ring = lot.block >= 0 ? lay.blocks[static_cast<std::size_t>(lot.block)].ring : 1.0f;
                const float chance = onMain ? 0.7f : ring < 0.35f ? 0.4f : 0.0f;
                const bool left = hp.doorX > 0.0f;   // the door on the +x half: the shop on -x
                const float x0 = left ? -0.5f * hp.width + 0.3f : hp.doorX + hp.doorHalf + 0.5f;
                const float x1 = left ? hp.doorX - hp.doorHalf - 0.5f : 0.5f * hp.width - 0.3f;
                if (unit(hashU(h ^ 0x5412U)) < chance && x1 - x0 > 1.6f) {
                    models.push_back(props::shopSign(props::shopName(hashU(h ^ 0x5410U)),
                                                     static_cast<int>(hashU(h ^ 0x5411U) % 4U), x1 - x0,
                                                     pal.props));
                    const glm::vec3 l = turn(yaw, {0.5f * (x0 + x1), 0.0f, -0.5f * hp.depth - 0.002f});
                    stamp(models.back(),
                          {lot.pos.x + l.x, baseY + hp.plinth + hp.storey - 0.05f, lot.pos.y + l.z},
                          yaw, furnitureChunk(lot.pos), false);
                    ++out.stats.shops;
                }
            }
            // The gravel strip round its walls (the Traufstreifen): forty
            // centimetres of pebbles where the rain drips off the eaves, so the
            // render does not stand in the lawn. On the ground, side by side,
            // each side run past the corners so they close.
            if (lot.zone == Zone::Houses && pal.houseGravel.valid()) {
                gravelMeshes.emplace_back();
                fitzel::MeshData& gm = gravelMeshes.back();
                constexpr float kStrip = 0.4f, kIn = 0.03f, kLift = 0.025f;
                const float hw = 0.5f * hp.width, hd = 0.5f * hp.depth;
                // Each side: where it runs (local a -> b) and which way is out.
                const glm::vec3 sides[4][3] = {
                    {{-hw - kStrip, 0.0f, -hd}, {hw + kStrip, 0.0f, -hd}, {0.0f, 0.0f, -1.0f}},
                    {{hw, 0.0f, -hd - kStrip}, {hw, 0.0f, hd + kStrip}, {1.0f, 0.0f, 0.0f}},
                    {{hw + kStrip, 0.0f, hd}, {-hw - kStrip, 0.0f, hd}, {0.0f, 0.0f, 1.0f}},
                    {{-hw, 0.0f, hd + kStrip}, {-hw, 0.0f, -hd - kStrip}, {-1.0f, 0.0f, 0.0f}}};
                auto at = [&](glm::vec3 l) {
                    const glm::vec3 w = turn(yaw, l);
                    const glm::vec2 xz(lot.pos.x + w.x, lot.pos.y + w.z);
                    return glm::vec3(xz.x, ground(xz) + kLift, xz.y);
                };
                for (const auto& sd : sides) {
                    const glm::vec3 a = sd[0], b = sd[1], n = sd[2];
                    const int steps = std::max(1, static_cast<int>(std::ceil(glm::length(b - a) / 1.0f)));
                    for (int i = 0; i < steps; ++i) {
                        const glm::vec3 p0 = glm::mix(a, b, static_cast<float>(i) / steps);
                        const glm::vec3 p1 = glm::mix(a, b, static_cast<float>(i + 1) / steps);
                        const glm::vec3 q[4] = {at(p0 - n * kIn), at(p1 - n * kIn),
                                                at(p1 + n * kStrip), at(p0 + n * kStrip)};
                        if (wet({q[2].x, q[2].z}) || wet({q[3].x, q[3].z})) continue;
                        const auto base = static_cast<std::uint32_t>(gm.vertices.size());
                        for (const glm::vec3& v : q) {
                            fitzel::Vertex vx{};
                            vx.position = v;
                            vx.normal   = {0.0f, 1.0f, 0.0f};
                            vx.uv       = glm::vec2(v.x, v.z) * 1.25f;   // 0.8 m of pebbles a repeat
                            gm.vertices.push_back(vx);
                        }
                        // Up, whichever way the side runs round.
                        const glm::vec3 up = glm::cross(q[1] - q[0], q[3] - q[0]);
                        if (up.y > 0.0f)
                            gm.indices.insert(gm.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
                        else
                            gm.indices.insert(gm.indices.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
                    }
                }
                if (!gm.vertices.empty()) {
                    city::Extra x;
                    x.mesh        = &gm;
                    x.material    = pal.houseGravel;
                    x.chunk       = chunk;
                    x.castsShadow = false;
                    extras.push_back(x);
                }
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

    // --- House numbers: up each street, odd on one side, even on the other ----
    // From the street's first point to its last; every building its own number,
    // a terrace house as much as a tower.
    if (!numberSpots.empty()) {
        std::sort(numberSpots.begin(), numberSpots.end(), [](const NumberSpot& a, const NumberSpot& b) {
            if (a.street != b.street) return a.street < b.street;
            if (a.odd != b.odd) return a.odd;
            return a.along < b.along;
        });
        std::map<std::string, std::size_t> plates;   // one model per number, stamped where needed
        int  street = -1, next = 1;
        bool odd = true;
        for (const NumberSpot& s : numberSpots) {
            if (s.street != street || s.odd != odd) {
                street = s.street;
                odd    = s.odd;
                next   = odd ? 1 : 2;
            }
            const std::string num = std::to_string(next);
            next += 2;
            auto it = plates.find(num);
            if (it == plates.end()) {
                models.push_back(props::houseNumber(num, pal.props));
                it = plates.emplace(num, models.size() - 1).first;
            }
            stamp(models[it->second], s.at, s.yaw, s.chunk, false);
            if (s.placed < out.placed.size()) {
                out.placed[s.placed].street = s.name;
                out.placed[s.placed].number = next - 2;
            }
            ++out.stats.numbers;
        }
    }

    // --- Pavements: the sidewalk band of every block, paved, on a kerb ---------
    // Each block side gets a strip from the carriageway's edge to the building
    // line. The edge is the REAL road's (snapped to the laid spline, which bows
    // between the grid's nodes), the strip stops where a crossing's apron starts
    // -- the apron reaches `margin` past the far carriageway, so the corner is
    // cut on the same line the asphalt ends -- and the corner is closed by a fan
    // to where the two building lines meet. Dropped over water and where some
    // other road runs through the block.
    //
    // The HEIGHT is the road's, not the ground's: a pavement is laid level with
    // the carriageway beside it (a kerb higher), across its whole width, and
    // eased along it -- not draped over every bump in the terrain. The town's
    // streets grade a level bed that wide beside themselves (RoadSystem::bed),
    // so the ground under the slabs is the road's height too. Only where no
    // road height is known (a preview before the streets are laid) does the
    // ground stand in, riding its highest point within a stride so no hump
    // shows through.
    constexpr float kKerb = 0.12f;
    const bool paved = r.pavements && r.sidewalk > 0.3f;
    auto ride = [&](glm::vec2 p) {
        float g = -1e9f;
        for (int a = -1; a <= 1; ++a)
            for (int c = -1; c <= 1; ++c)
                g = std::max(g, ground(p + glm::vec2(a * 0.75f, c * 0.75f)));
        return g;
    };
    // The pavement's top at `p`: the nearest road's surface plus the kerb.
    auto paveY = [&](glm::vec2 p) {
        float y = 0.0f;
        return (roads.surfaceNear(p, y) ? y : ride(p)) + kKerb;
    };
    // A village's pavement runs along its main street only (see "A village's
    // pavement" below): on the side its houses stand on, from the first to the
    // last of them, and on both sides through the core. Worked out here from
    // the plots, because what stands beside it needs to know where it is.
    // walkFrom/walkTo are metres along the main street, [0] its left side and
    // [1] its right (the way the street is drawn); from > to = none.
    float walkFrom[2], walkTo[2];
    villageWalks(r, lay, walkFrom, walkTo);
    const float villageWalkW = glm::clamp(r.sidewalk, 1.2f, 3.0f);
    // On that pavement: within its band beside the main street, on a side and
    // a stretch it runs along.
    auto onVillageWalk = [&](glm::vec2 p) {
        if (!lay.organic || lay.main < 0) return false;
        const Street& M = lay.streets[static_cast<std::size_t>(lay.main)];
        float t = 0.0f;
        glm::vec2 q, d;
        const float dist = nearestOn(M.pts, p, &t, &q, &d);
        const int side = cross2(d, p - q) > 0.0f ? 0 : 1;
        return t >= walkFrom[side] && t <= walkTo[side] &&
               dist < 0.5f * M.width + villageWalkW + 0.3f;
    };
    // Where a thing beside a street stands: on the pavement's top, or -- in a
    // village, off its one pavement -- on the ground.
    auto standY = [&](glm::vec2 p) {
        return paved && (!lay.organic || onVillageWalk(p)) ? paveY(p) : ground(p);
    };
    // Street-lamp spots, gathered along the kerbs below and kept at the end,
    // once every other piece of furniture has its place (see "Street lamps").
    struct LampSpot { glm::vec2 at, facing; };
    std::vector<LampSpot> lampSpots;
    // Gullies in the gutter, gathered along the kerbs too: where (on the road
    // surface) and turned to lie along the kerb.
    struct GullySpot { glm::vec3 at; float yaw; };
    std::vector<GullySpot> gullySpots;
    // Lamps along every paved street: the rule's prefabs, or -- with none --
    // the standard lamp (TownLamps draws it).
    const bool lamps = r.lampEvery > 4.0f;
    // An avenue's kerb is farther out than a street's: halfway between the two
    // half-widths tells them apart.
    const float avenueHalf = 0.25f * (r.built().streetWidth + r.built().avenueWidth);
    const float lampInset  = glm::clamp(r.lampInset, 0.2f, std::max(0.3f, r.sidewalk - 0.3f));
    std::deque<fitzel::MeshData> paveMeshes;   // stable addresses for the extras
    if (paved) {
        const float w      = r.sidewalk;
        const float margin = 1.0f;   // RoadJunction::Params::margin, the roads' default
        const float minSin = std::sin(glm::radians(22.0f));   // ... and minAngleDeg
        // A flat polygon, wound to face `outward`. `uv`, when given, is where each
        // corner lies in the paving, in metres along and across the strip it
        // belongs to; without, the slabs on top are laid square to the town's own
        // axes (a rotated town's pavement is no diagonal chequer) and the kerb
        // stones run along the faces.
        auto addFaceUv = [&lay](fitzel::MeshData& md, std::vector<glm::vec3> p,
                                std::vector<glm::vec2> uv, glm::vec3 outward) {
            glm::vec3 n(0.0f);
            for (std::size_t i = 0; i < p.size(); ++i) {
                const glm::vec3& u = p[i];
                const glm::vec3& v = p[(i + 1) % p.size()];
                n += glm::vec3((u.y - v.y) * (u.z + v.z), (u.z - v.z) * (u.x + v.x),
                               (u.x - v.x) * (u.y + v.y));
            }
            if (glm::dot(n, n) < 1e-12f) return;
            if (glm::dot(n, outward) < 0.0f) {
                std::reverse(p.begin(), p.end());
                std::reverse(uv.begin(), uv.end());
                n = -n;
            }
            n = glm::normalize(n);
            const auto base = static_cast<std::uint32_t>(md.vertices.size());
            const bool up = std::abs(n.y) > 0.5f;
            for (std::size_t k = 0; k < p.size(); ++k) {
                const glm::vec3& v = p[k];
                fitzel::Vertex vx{};
                vx.position = v;
                vx.normal   = n;
                // Paving slabs: half a metre a tile on top, the kerb stones along.
                const glm::vec2 g(v.x, v.z);
                vx.uv = k < uv.size() ? uv[k] * 0.5f
                      : up ? glm::vec2(glm::dot(g, lay.axisX), glm::dot(g, lay.axisZ)) * 0.5f
                           : glm::vec2(std::abs(n.x) > std::abs(n.z) ? v.z : v.x, v.y) * 0.5f;
                md.vertices.push_back(vx);
            }
            for (std::uint32_t i = 1; i + 1 < p.size(); ++i) {
                md.indices.push_back(base);
                md.indices.push_back(base + i);
                md.indices.push_back(base + i + 1);
            }
        };
        auto addFace = [&](fitzel::MeshData& md, std::vector<glm::vec3> p, glm::vec3 outward) {
            addFaceUv(md, std::move(p), {}, outward);
        };
        // Top at the side's own height `y` (the road's, see above); the kerb and
        // the inner edge reach down past whichever is lower, road or ground.
        auto top = [&](glm::vec2 p, float y) { return glm::vec3(p.x, y + kKerb, p.y); };
        // The kerb stones: a strip along the road-side edge, flush with the
        // slabs, its top edge bevelled toward the carriageway. Their own
        // material, lighter than the paving, so the edge reads as a kerb and not
        // as the side of a slab.
        constexpr float kKerbW = 0.18f, kBevel = 0.03f;
        auto inward = [&](glm::vec2 o, glm::vec2 i, float d) {
            const float len = glm::length(i - o);
            return len > 1e-4f ? o + (i - o) * std::min(d / len, 0.45f) : o;
        };
        auto bevel = [&](glm::vec2 p, float y) { return glm::vec3(p.x, y + kKerb - kBevel, p.y); };
        auto low = [&](glm::vec2 p, float y) {
            return glm::vec3(p.x, std::min(ground(p), y) - 0.15f, p.y);
        };
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
        // One kerb run from the road edge o0-o1 to its inner line k0-k1:
        // bevel, top, and the face down to the carriageway.
        // `along`, when given, is how far along its street each end of the run
        // lies, and the stones are laid by it rather than by the world's axes.
        auto kerbRunIn = [&](fitzel::MeshData& kerb, glm::vec2 o0, glm::vec2 o1, glm::vec2 b0,
                             glm::vec2 b1, glm::vec2 k0, glm::vec2 k1, float y0, float y1,
                             glm::vec2 out, const glm::vec2* along = nullptr) {
            const glm::vec3 l0 = low(o0, y0), l1 = low(o1, y1);
            const glm::vec3 e0 = bevel(o0, y0), e1 = bevel(o1, y1);
            const float a = along ? along->x : 0.0f, b = along ? along->y : 0.0f;
            auto uv = [&](std::vector<glm::vec2> v) { return along ? v : std::vector<glm::vec2>{}; };
            addFaceUv(kerb, {top(b0, y0), top(b1, y1), top(k1, y1), top(k0, y0)},
                      uv({{a, kBevel}, {b, kBevel}, {b, kKerbW}, {a, kKerbW}}), {0, 1, 0});
            addFaceUv(kerb, {e0, e1, top(b1, y1), top(b0, y0)},
                      uv({{a, 0.0f}, {b, 0.0f}, {b, kBevel}, {a, kBevel}}),
                      glm::vec3(out.x, 1.0f, out.y));
            addFaceUv(kerb, {l0, l1, e1, e0}, uv({{a, l0.y}, {b, l1.y}, {b, e1.y}, {a, e0.y}}),
                      glm::vec3(out.x, 0.0f, out.y));
        };
        for (std::size_t bi = 0; bi < lay.blocks.size(); ++bi) {
            const Block& B = lay.blocks[bi];
            glm::vec2 cen(0.0f);
            for (const glm::vec2& c : B.corner) cen += c;
            cen *= 0.25f;
            struct Side {
                std::vector<glm::vec2> O, I;   // kerb and building-line points
                std::vector<float>     Y;      // the road's height beside each (no kerb)
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
                // (and, into `y`, the road surface's height there)
                auto kerbAt = [&](float t, float* y = nullptr) {
                    const glm::vec2 c = a0 + d * t;
                    glm::vec2 at, rd;
                    float half = 0.0f, ry = 0.0f;
                    bool  hasY = false;
                    if (!roads.snap(c, d, at, rd, half, &ry, &hasY)) {
                        const glm::vec2 o = c + n * B.streetHalf[k];
                        if (y) *y = ride(o);
                        return o;
                    }
                    glm::vec2 rn(-rd.y, rd.x);
                    if (glm::dot(rn, n) < 0.0f) rn = -rn;
                    const glm::vec2 o = at + rn * half;
                    if (y) *y = hasY ? ry : ride(o);
                    return o;
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
                // Street lamps along this kerb. A street is lit from one side --
                // the block on the town's +X or +Z of it, so the same side all
                // the way along -- and an avenue from both (every street, with
                // lampBothSides), the two sides staggered by half a spacing so
                // the pools of light alternate instead of pairing up. Evenly
                // spread over what the kerb has between the crossings.
                if (lamps && s1 - s0 > 6.0f) {
                    const glm::vec2 across =
                        std::abs(glm::dot(d, lay.axisX)) >= std::abs(glm::dot(d, lay.axisZ))
                            ? lay.axisZ : lay.axisX;
                    const bool plus = glm::dot(n, across) > 0.0f;
                    const bool both = r.lampBothSides || B.streetHalf[k] > avenueHalf;
                    if (both || plus) {
                        const float span  = s1 - s0;
                        const int   count = std::max(1, static_cast<int>(std::lround(span / r.lampEvery)));
                        const float phase = !both ? 0.5f : plus ? 0.25f : 0.75f;
                        for (int i = 0; i < count; ++i) {
                            const float t = s0 + span * (static_cast<float>(i) + phase) / count;
                            const glm::vec2 at = kerbAt(t) + n * lampInset;
                            if (wet(at) || roads.clearance(at) < 0.15f) continue;
                            lampSpots.push_back({at, -n});
                        }
                    }
                }
                // A gully in the gutter every 30 m or so.
                if (r.manholes && s1 - s0 > 8.0f) {
                    const int count = std::max(1, static_cast<int>(std::lround((s1 - s0) / 30.0f)));
                    for (int i = 0; i < count; ++i) {
                        float y = 0.0f;
                        const float t = s0 + (s1 - s0) * (static_cast<float>(i) + 0.5f) / count;
                        const glm::vec2 at = kerbAt(t, &y) - n * 0.3f;
                        if (wet(at)) continue;
                        gullySpots.push_back({{at.x, y, at.y}, glm::degrees(std::atan2(-d.y, d.x))});
                    }
                }
                const int m = std::max(1, static_cast<int>(std::ceil((s1 - s0) / 2.0f)));
                std::vector<float> raw;
                for (int i = 0; i <= m; ++i) {
                    float y = 0.0f;
                    const glm::vec2 o = kerbAt(s0 + (s1 - s0) * static_cast<float>(i) / m, &y);
                    sd[k].O.push_back(o);
                    sd[k].I.push_back(o + n * w);
                    raw.push_back(y);
                }
                // Eased along the side, so what is left of a bump in the road's
                // own profile (or in the ground, without one) is a slope, not a dent.
                for (std::size_t i = 0; i < raw.size(); ++i) {
                    float sum = 0.0f, wsum = 0.0f;
                    for (int j = -3; j <= 3; ++j) {
                        const long q = static_cast<long>(i) + j;
                        if (q < 0 || q >= static_cast<long>(raw.size())) continue;
                        const float wt = 4.0f - static_cast<float>(std::abs(j));
                        sum += wt * raw[static_cast<std::size_t>(q)];
                        wsum += wt;
                    }
                    sd[k].Y.push_back(sum / wsum);
                }
                // Sampled every 2 m to follow the road's bow; on flat, straight
                // stretches most of those samples say nothing. Keep a sample only
                // where leaving it out would move the kerb or the height by more
                // than 2 cm (and every 15 m).
                std::vector<glm::vec2>& O = sd[k].O;
                std::vector<glm::vec2>& I = sd[k].I;
                std::vector<float>&     Y = sd[k].Y;
                auto off = [&](const std::vector<glm::vec2>& L, std::size_t a, std::size_t b,
                               std::size_t i) {
                    const glm::vec2 ab = L[b] - L[a];
                    const float l2 = glm::dot(ab, ab);
                    const float t  = l2 > 1e-8f ? glm::dot(L[i] - L[a], ab) / l2 : 0.0f;
                    const glm::vec2 q = L[a] + ab * t;
                    const float gy = glm::mix(Y[a], Y[b], t);
                    return std::max(glm::length(L[i] - q), std::abs(Y[i] - gy));
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
                std::vector<float> Y2;
                for (std::size_t i : keep) { O2.push_back(O[i]); I2.push_back(I[i]); Y2.push_back(Y[i]); }
                O = std::move(O2);
                I = std::move(I2);
                Y = std::move(Y2);
            }
            // Slabs and kerb stones in two meshes, one material each: a draw
            // more per chunk, and the edge stops being the side of a slab.
            paveMeshes.emplace_back();
            fitzel::MeshData& slab = paveMeshes.back();
            paveMeshes.emplace_back();
            fitzel::MeshData& kerb = paveMeshes.back();
            auto kerbRun = [&](glm::vec2 o0, glm::vec2 o1, glm::vec2 b0, glm::vec2 b1,
                               glm::vec2 k0, glm::vec2 k1, float y0, float y1, glm::vec2 out) {
                kerbRunIn(kerb, o0, o1, b0, b1, k0, k1, y0, y1, out);
            };
            for (int k = 0; k < 4; ++k) {
                const Side& S = sd[k];
                for (std::size_t i = 0; i + 1 < S.O.size(); ++i) {
                    const glm::vec2 o0 = S.O[i], o1 = S.O[i + 1], i0 = S.I[i], i1 = S.I[i + 1];
                    const float y0 = S.Y[i], y1 = S.Y[i + 1];
                    if (!usable({o0, o1, i1, i0})) continue;
                    const glm::vec2 k0 = inward(o0, i0, kKerbW), k1 = inward(o1, i1, kKerbW);
                    addFace(slab, {top(k0, y0), top(k1, y1), top(i1, y1), top(i0, y0)}, {0, 1, 0});
                    kerbRun(o0, o1, inward(o0, i0, kBevel), inward(o1, i1, kBevel), k0, k1, y0, y1,
                            -S.n);
                    addFace(slab, {low(i0, y0), low(i1, y1), top(i1, y1), top(i0, y0)},
                            glm::vec3(S.n.x, 0, S.n.y));
                    // Open ends where the strip breaks off (water, a foreign road).
                    if (i == 0 || !usable({S.O[i - 1], o0, i0, S.I[i - 1]}))
                        addFace(slab, {low(o0, y0), low(i0, y0), top(i0, y0), top(o0, y0)},
                                glm::vec3(-S.d.x, 0, -S.d.y));
                    if (i + 2 == S.O.size() || !usable({o1, S.O[i + 2], S.I[i + 2], i1}))
                        addFace(slab, {low(o1, y1), low(i1, y1), top(i1, y1), top(o1, y1)},
                                glm::vec3(S.d.x, 0, S.d.y));
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
                const float yp = P.Y.back(), ys = S.Y.front(), yc = 0.5f * (yp + ys);
                // The kerb goes round the corner on the chord between the two
                // sides' ends, starting where each side's own kerb stops.
                const glm::vec2 kp = inward(po, pi, kKerbW), ks = inward(so, si, kKerbW);
                addFace(slab, {top(c, yc), top(pi, yp), top(kp, yp)}, {0, 1, 0});
                addFace(slab, {top(c, yc), top(kp, yp), top(ks, ys)}, {0, 1, 0});
                addFace(slab, {top(c, yc), top(ks, ys), top(si, ys)}, {0, 1, 0});
                const glm::vec2 ch = so - po;
                glm::vec2 cn(-ch.y, ch.x);
                if (glm::dot(cn, c - po) > 0.0f) cn = -cn;
                kerbRun(po, so, inward(po, pi, kBevel), inward(so, si, kBevel), kp, ks, yp, ys,
                        glm::normalize(cn));
                // The corner's inner edge, where the fan meets the plots.
                addFace(slab, {low(pi, yp), low(c, yc), top(c, yc), top(pi, yp)}, glm::vec3(P.n.x, 0, P.n.y));
                addFace(slab, {low(c, yc), low(si, ys), top(si, ys), top(c, yc)}, glm::vec3(S.n.x, 0, S.n.y));
            }
            // The block's walk: the middle of its pavement, side after side, if
            // the pavement goes all the way round (a gap over water or a road
            // through the block would walk people across it).
            {
                bool whole = true;
                for (int k = 0; k < 4 && whole; ++k) {
                    const Side& S = sd[k];
                    whole = S.O.size() >= 2;
                    for (std::size_t i = 0; i + 1 < S.O.size() && whole; ++i)
                        whole = usable({S.O[i], S.O[i + 1], S.I[i + 1], S.I[i]});
                }
                if (whole) {
                    std::vector<glm::vec3> walk;
                    for (int k = 0; k < 4; ++k)
                        for (std::size_t i = 0; i < sd[k].O.size(); ++i) {
                            const glm::vec2 m = 0.5f * (sd[k].O[i] + sd[k].I[i]);
                            walk.push_back({m.x, sd[k].Y[i] + kKerb, m.y});
                        }
                    out.walks.push_back(std::move(walk));
                }
            }
            const int chunk = chunkOf(cen);
            if (!slab.vertices.empty()) {
                city::Extra x;
                x.mesh     = &slab;
                x.material = pal.civic.pavement;
                x.chunk    = chunk;
                extras.push_back(x);
            }
            if (!kerb.vertices.empty()) {
                city::Extra x;
                x.mesh     = &kerb;
                x.material = pal.civic.kerb;
                x.chunk    = chunk;
                extras.push_back(x);
            }
        }

        // --- A village's pavement ------------------------------------------------
        // A village has no blocks to go round. Its pavement is a strip along the
        // main street, where walkFrom/walkTo above put it, on the same kerb as a
        // town's -- and in front of the kerb, on the carriageway, a gutter of
        // setts, the way a village street in the Black Forest has it. The strip
        // breaks where a lane comes in (its carriageway is no pavement) and the
        // lanes have none of their own: their asphalt runs out into the verge.
        if (lay.organic && lay.main >= 0) {
            const Street& M = lay.streets[static_cast<std::size_t>(lay.main)];
            // The setts stand a touch proud of the asphalt -- and of an apron,
            // which lies 1 cm over the ribbon where a lane comes in.
            constexpr float kGutter = 0.32f, kGutterLift = 0.02f;
            for (int side = 0; side < 2; ++side) {
                const float t0 = walkFrom[side], t1 = walkTo[side];
                if (t1 - t0 < 4.0f) continue;
                const float sgn = side == 0 ? 1.0f : -1.0f;
                // The kerb at `t` along the main street, on the real road's edge,
                // with the road's height there and which road it is.
                auto kerbAt = [&](float t, float& y, glm::vec2& nOut, glm::vec2& dOut, int& road) {
                    glm::vec2 d;
                    const glm::vec2 c = along(M.pts, t, &d);
                    glm::vec2 n = glm::vec2(-d.y, d.x) * sgn;
                    glm::vec2 at, rd;
                    float half = 0.5f * M.width, ry = 0.0f;
                    bool  hasY = false;
                    dOut = d;
                    road = -1;
                    if (!roads.snap(c, d, at, rd, half, &ry, &hasY, &road)) {
                        nOut = n;
                        const glm::vec2 o = c + n * (0.5f * M.width);
                        y = ride(o);
                        return o;
                    }
                    glm::vec2 rn(-rd.y, rd.x);
                    if (glm::dot(rn, n) < 0.0f) rn = -rn;
                    nOut = rn;
                    const glm::vec2 o = at + rn * half;
                    y = hasY ? ry : ride(o);
                    return o;
                };
                const int m = std::max(1, static_cast<int>(std::ceil((t1 - t0) / 2.0f)));
                std::vector<float> T, raw, Y;
                for (int i = 0; i <= m; ++i) {
                    float y = 0.0f;
                    glm::vec2 n, d;
                    int road = -1;
                    T.push_back(t0 + (t1 - t0) * static_cast<float>(i) / m);
                    kerbAt(T.back(), y, n, d, road);
                    raw.push_back(y);
                }
                // Eased along, as a town's is.
                for (std::size_t i = 0; i < raw.size(); ++i) {
                    float sum = 0.0f, wsum = 0.0f;
                    for (int j = -3; j <= 3; ++j) {
                        const long q = static_cast<long>(i) + j;
                        if (q < 0 || q >= static_cast<long>(raw.size())) continue;
                        const float wt = 4.0f - static_cast<float>(std::abs(j));
                        sum += wt * raw[static_cast<std::size_t>(q)];
                        wsum += wt;
                    }
                    Y.push_back(sum / wsum);
                }
                // The strip across at `t`: kerb, inner edge, the outward normal,
                // the street's heading, the eased height and the main street's
                // road there (a street broken by water is laid as several).
                struct Cut { glm::vec2 o, i, n, d; float y; int road; };
                auto cutAt = [&](float t) {
                    Cut c;
                    float yRaw = 0.0f;
                    c.o = kerbAt(t, yRaw, c.n, c.d, c.road);
                    c.i = c.o + c.n * villageWalkW;
                    const float f = glm::clamp((t - t0) / (t1 - t0), 0.0f, 1.0f) * m;
                    const int k = std::min(static_cast<int>(f), m - 1);
                    c.y = glm::mix(Y[static_cast<std::size_t>(k)], Y[static_cast<std::size_t>(k + 1)],
                                   f - static_cast<float>(k));
                    return c;
                };
                // Where the strip may lie: dry, off every other carriageway by a
                // few centimetres -- the pavement runs right up to a lane's
                // setts -- and not into the main street's own past the hair the
                // kerb needs to stand on its edge (the inside of a sharp bend).
                auto clear = [&](glm::vec2 p, int road) {
                    return !wet(p) && roads.clearance(p, road) > 0.05f && roads.clearance(p) > -0.02f;
                };
                std::vector<Cut> C;
                for (float t : T) C.push_back(cutAt(t));
                auto ok = [&](std::size_t i) {
                    const glm::vec2 c = 0.25f * (C[i].o + C[i + 1].o + C[i].i + C[i + 1].i);
                    return clear(C[i].o, C[i].road) && clear(C[i + 1].o, C[i + 1].road) &&
                           clear(C[i].i, C[i].road) && clear(C[i + 1].i, C[i + 1].road) &&
                           clear(c, C[i].road) && roads.clearance(c) > 0.2f;
                };
                // How far one line of the strip -- the kerb (0), its back edge (1)
                // or the gutter's outer edge on the carriageway (2) -- runs on
                // from `ta`, still clear, toward `tb` before a lane stops it: in
                // quarter metres, then halved down to a centimetre. `lane` says
                // a carriageway stopped it rather than water.
                auto lineAt = [&](const Cut& c, int which) {
                    return which == 0 ? c.o : which == 1 ? c.i : c.o - c.n * kGutter;
                };
                auto reach = [&](float ta, float tb, int which, bool& lane) {
                    auto fine = [&](float t) {
                        const Cut c = cutAt(t);
                        const glm::vec2 p = lineAt(c, which);
                        // The gutter lies on the main street: only another road
                        // covering it stops it.
                        return which == 2 ? !wet(p) && roads.clearance(p, c.road) > 0.0f
                                          : clear(p, c.road);
                    };
                    lane = false;
                    const int steps = std::max(1, static_cast<int>(std::ceil(std::abs(tb - ta) / 0.25f)));
                    float a = ta;
                    for (int k = 1; k <= steps; ++k) {
                        const float b = glm::mix(ta, tb, static_cast<float>(k) / steps);
                        if (fine(b)) { a = b; continue; }
                        float lo = a, hi = b;
                        for (int h = 0; h < 5; ++h) {
                            const float mid = 0.5f * (lo + hi);
                            (fine(mid) ? lo : hi) = mid;
                        }
                        lane = !wet(lineAt(cutAt(hi), which));
                        return lo;
                    }
                    return a;
                };
                // In pieces of some forty metres, each in the chunk it lies in.
                std::vector<glm::vec3> walk;
                fitzel::MeshData* slab = nullptr;
                fitzel::MeshData* kerb = nullptr;
                glm::vec2 pieceAt(0.0f);
                int pieceN = 0;
                auto flush = [&]() {
                    if (!slab) return;
                    const int chunk = chunkOf(pieceAt / static_cast<float>(std::max(pieceN, 1)));
                    if (!slab->vertices.empty()) {
                        city::Extra x;
                        x.mesh = slab; x.material = pal.civic.pavement; x.chunk = chunk;
                        extras.push_back(x);
                    }
                    if (!kerb->vertices.empty()) {
                        city::Extra x;
                        x.mesh = kerb; x.material = pal.civic.kerb; x.chunk = chunk;
                        extras.push_back(x);
                    }
                    slab = kerb = nullptr;
                    pieceAt = glm::vec2(0.0f);
                    pieceN = 0;
                };
                // People walk each unbroken run there and back (a walk is a loop),
                // turning where a lane comes in rather than crossing it -- a town's
                // walkers keep to their block the same way.
                auto endWalk = [&]() {
                    if (walk.size() >= 3) {
                        std::vector<glm::vec3> loop = walk;
                        loop.insert(loop.end(), walk.rbegin(), walk.rend());
                        out.walks.push_back(std::move(loop));
                    }
                    walk.clear();
                };
                // Each unbroken run of the two-metre stations, laid as one strip.
                // Where a lane breaks it, the strip does not stop at the last
                // whole station, square across with bare ground before the lane:
                // its kerb and its back edge each run on to the lane's own edge,
                // so the end lies along the lane, however slanted the mouth. And
                // the last metre and a half sinks to a dropped kerb, three
                // centimetres over the road, the way a pavement meets a side
                // street one crosses on foot -- not a step standing in the grass.
                constexpr float kRamp = 1.5f, kDropped = 0.03f;
                // ...as long as nothing but that lane is in the way between the
                // two ends (its edge runs through them, so the middle may touch it).
                auto endClear = [&](const Cut& co, const Cut& ci) {
                    const glm::vec2 p = 0.5f * (co.o + ci.i);
                    return !wet(p) && roads.clearance(p, co.road) > 0.0f;
                };
                for (int qa = 0; qa < m;) {
                    if (!ok(static_cast<std::size_t>(qa))) { ++qa; continue; }
                    int qb = qa;
                    while (qb + 1 < m && ok(static_cast<std::size_t>(qb + 1))) ++qb;
                    const float aT = T[static_cast<std::size_t>(qa)];
                    const float bT = T[static_cast<std::size_t>(qb + 1)];
                    float oS = aT, iS = aT, oE = bT, iE = bT;
                    bool rampS = false, rampE = false;
                    if (qa > 0) {
                        bool lo = false, li = false;
                        oS = reach(aT, std::max(aT - 8.0f, t0), 0, lo);
                        iS = reach(aT, std::max(aT - 8.0f, t0), 1, li);
                        const Cut co = cutAt(oS), ci = cutAt(iS);
                        rampS = (lo || li) && endClear(co, ci);
                        if (!rampS) oS = iS = aT;
                    }
                    if (qb + 1 < m) {
                        bool lo = false, li = false;
                        oE = reach(bT, std::min(bT + 8.0f, t1), 0, lo);
                        iE = reach(bT, std::min(bT + 8.0f, t1), 1, li);
                        const Cut co = cutAt(oE), ci = cutAt(iE);
                        rampE = (lo || li) && endClear(co, ci);
                        if (!rampE) oE = iE = bT;
                    }
                    // The gutter stops where the lane's carriageway begins too --
                    // at a slanted mouth that is short of where the kerb does.
                    bool unused = false;
                    const float gS = rampS ? reach(aT, oS, 2, unused) : oS;
                    const float gE = rampE ? reach(bT, oE, 2, unused) : oE;
                    // The cross-sections: half a metre apart over the ramps (and
                    // the slanted ends), the two-metre stations in between. Each
                    // is where it cuts the kerb and where it cuts the back edge,
                    // in metres along the street.
                    const float mid = 0.5f * (aT + bT);
                    const float a2 = rampS ? std::min(aT + kRamp, mid) : aT;
                    const float b2 = rampE ? std::max(bT - kRamp, mid) : bT;
                    std::vector<glm::vec2> sec;   // (t at the kerb, t at the back)
                    auto piece = [&](glm::vec2 from, glm::vec2 to) {
                        if (sec.empty()) sec.push_back(from);
                        const float len = std::max(std::abs(to.x - from.x), std::abs(to.y - from.y));
                        if (len < 1e-3f) return;
                        const int n = static_cast<int>(std::ceil(len / 0.5f));
                        for (int k = 1; k <= n; ++k)
                            sec.push_back(glm::mix(from, to, static_cast<float>(k) / n));
                    };
                    piece({oS, iS}, {a2, a2});
                    for (int k = qa + 1; k <= qb; ++k) {
                        const float t = T[static_cast<std::size_t>(k)];
                        if (t > a2 + 0.01f && t < b2 - 0.01f) sec.push_back({t, t});
                    }
                    if (b2 > sec.back().x + 0.01f) sec.push_back({b2, b2});
                    piece({b2, b2}, {oE, iE});
                    // The kerb sinks toward a lane: how far below its full height,
                    // by how far this edge still is from its own end.
                    auto drop = [&](float t, float tS, float tE) {
                        float dd = 1e9f;
                        if (rampS) dd = std::min(dd, t - tS);
                        if (rampE) dd = std::min(dd, tE - t);
                        const float e = glm::clamp(dd / kRamp, 0.0f, 1.0f);
                        return (kKerb - kDropped) * (1.0f - e * e * (3.0f - 2.0f * e));
                    };
                    std::vector<Cut> CO, CI;
                    std::vector<float> yo, yi;   // the road height the kerb is set on, lowered
                    for (const glm::vec2& q : sec) {
                        CO.push_back(cutAt(q.x));
                        CI.push_back(cutAt(q.y));
                        yo.push_back(CO.back().y - drop(q.x, oS, oE));
                        yi.push_back(CI.back().y - drop(q.y, iS, iE));
                        const glm::vec2 m0 = 0.5f * (CO.back().o + CI.back().i);
                        walk.push_back({m0.x, 0.5f * (yo.back() + yi.back()) + kKerb, m0.y});
                    }
                    for (std::size_t j = 0; j + 1 < sec.size(); ++j) {
                        if (!slab || pieceN >= 20) {
                            flush();
                            paveMeshes.emplace_back();
                            slab = &paveMeshes.back();
                            paveMeshes.emplace_back();
                            kerb = &paveMeshes.back();
                        }
                        pieceAt += CO[j].o;
                        ++pieceN;
                        const glm::vec2 o0 = CO[j].o, o1 = CO[j + 1].o, i0 = CI[j].i, i1 = CI[j + 1].i;
                        const float uo0 = sec[j].x, uo1 = sec[j + 1].x;
                        const float ui0 = sec[j].y, ui1 = sec[j + 1].y;
                        const glm::vec2 n = glm::normalize(CO[j].n + CO[j + 1].n);
                        const glm::vec2 d = glm::normalize(CO[j].d + CO[j + 1].d);
                        const glm::vec2 k0 = inward(o0, i0, kKerbW), k1 = inward(o1, i1, kKerbW);
                        // The slabs laid along the street, not along the world's axes.
                        addFaceUv(*slab, {top(k0, yo[j]), top(k1, yo[j + 1]), top(i1, yi[j + 1]), top(i0, yi[j])},
                                  {{uo0, kKerbW}, {uo1, kKerbW}, {ui1, villageWalkW}, {ui0, villageWalkW}},
                                  {0, 1, 0});
                        const glm::vec2 u(uo0, uo1);
                        kerbRunIn(*kerb, o0, o1, inward(o0, i0, kBevel), inward(o1, i1, kBevel), k0, k1,
                                  yo[j], yo[j + 1], -n, &u);
                        addFace(*slab, {low(i0, yi[j]), low(i1, yi[j + 1]), top(i1, yi[j + 1]), top(i0, yi[j])},
                                glm::vec3(n.x, 0, n.y));
                        if (j == 0)
                            addFace(*slab, {low(o0, yo[j]), low(i0, yi[j]), top(i0, yi[j]), top(o0, yo[j])},
                                    glm::vec3(-d.x, 0, -d.y));
                        if (j + 2 == sec.size())
                            addFace(*slab, {low(o1, yo[j + 1]), low(i1, yi[j + 1]), top(i1, yi[j + 1]),
                                            top(o1, yo[j + 1])},
                                    glm::vec3(d.x, 0, d.y));
                        // The gutter: setts on the carriageway along the kerb's foot --
                        // the paving at a quarter of its size, so a slab is a sett --
                        // at the road's own height, wherever the kerb sinks.
                        // Clipped to [gS, gE] along the kerb.
                        const float c0 = std::max(uo0, gS), c1 = std::min(uo1, gE);
                        if (c1 > c0 + 1e-3f) {
                            const float span = std::max(uo1 - uo0, 1e-6f);
                            const float f0 = (c0 - uo0) / span, f1 = (c1 - uo0) / span;
                            const glm::vec2 go0 = glm::mix(o0, o1, f0), go1 = glm::mix(o0, o1, f1);
                            const glm::vec2 gn0 = glm::normalize(glm::mix(CO[j].n, CO[j + 1].n, f0));
                            const glm::vec2 gn1 = glm::normalize(glm::mix(CO[j].n, CO[j + 1].n, f1));
                            const glm::vec2 g0 = go0 - gn0 * kGutter, g1 = go1 - gn1 * kGutter;
                            const float y0 = glm::mix(CO[j].y, CO[j + 1].y, f0);
                            const float y1 = glm::mix(CO[j].y, CO[j + 1].y, f1);
                            const auto base = static_cast<std::uint32_t>(slab->vertices.size());
                            const glm::vec3 gp[4] = {{g0.x, y0 + kGutterLift, g0.y},
                                                     {g1.x, y1 + kGutterLift, g1.y},
                                                     {go1.x, y1 + kGutterLift, go1.y},
                                                     {go0.x, y0 + kGutterLift, go0.y}};
                            const glm::vec2 guv[4] = {{c0, -kGutter}, {c1, -kGutter}, {c1, 0.0f}, {c0, 0.0f}};
                            for (int k = 0; k < 4; ++k) {
                                fitzel::Vertex vx{};
                                vx.position = gp[k];
                                vx.normal   = {0.0f, 1.0f, 0.0f};
                                vx.uv       = guv[k] * 2.0f;
                                slab->vertices.push_back(vx);
                            }
                            // Facing up whichever side of the street it is on.
                            const bool ccw = cross2(g1 - g0, go0 - g0) > 0.0f;
                            if (ccw)
                                slab->indices.insert(slab->indices.end(),
                                                     {base, base + 3, base + 2, base, base + 2, base + 1});
                            else
                                slab->indices.insert(slab->indices.end(),
                                                     {base, base + 1, base + 2, base, base + 2, base + 3});
                        }
                    }
                    endWalk();
                    qa = qb + 1;
                }
                flush();
                endWalk();
            }
        }
    }

    // --- A village's lamps -------------------------------------------------------
    // Along the lanes at the edge of the carriageway, one side of each and
    // further apart than in town, clear of the houses -- and along the main
    // street on its pavement, on the side the pavement runs all the way.
    if (lamps && lay.organic) {
        for (int si = 0; si < static_cast<int>(lay.streets.size()); ++si) {
            const Street& st = lay.streets[static_cast<std::size_t>(si)];
            const float len   = polyLen(st.pts);
            const float every = std::max(r.lampEvery, 12.0f);
            float side  = (hashU(r.seed ^ 0x1a4dU ^ static_cast<std::uint32_t>(si)) & 1U) ? 1.0f : -1.0f;
            float inset = 0.7f;
            if (si == lay.main) {
                const int walkSide = walkTo[0] - walkFrom[0] >= walkTo[1] - walkFrom[1] ? 0 : 1;
                if (walkTo[walkSide] > walkFrom[walkSide]) {
                    side  = walkSide == 0 ? 1.0f : -1.0f;
                    inset = glm::clamp(r.lampInset, 0.3f, villageWalkW - 0.3f);
                }
            }
            for (float t = 0.5f * every; t < len - 6.0f; t += every) {
                glm::vec2 d;
                const glm::vec2 p = along(st.pts, t, &d);
                glm::vec2 c, rd;
                float half = 0.5f * st.width, y = 0.0f;
                bool hasY = false;
                if (!roads.snap(p, d, c, rd, half, &y, &hasY)) continue;
                // Sided by the street's own direction, which the road's need not be.
                glm::vec2 n = glm::vec2(-rd.y, rd.x);
                if (glm::dot(n, glm::vec2(-d.y, d.x) * side) < 0.0f) n = -n;
                const glm::vec2 at = c + n * (half + inset);
                if (wet(at) || roads.clearance(at) < 0.15f) continue;
                bool free = true;
                for (const Placed& pl : out.placed)
                    if (glm::length(pl.pos - at) < pl.radius) { free = false; break; }
                if (free) lampSpots.push_back({at, -n});
            }
        }
    }

    // --- At the crossings: street-name signs and traffic lights -----------------
    // The names are the ROADS' names where the roads are laid -- so renaming a
    // street in the Roads panel renames its signs -- and the town's own before
    // that (a preview has no roads to ask).
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
    // `y` is the height it stands at; by default the pavement's top (standY).
    auto place = [&](const civic::Model& M, glm::vec2 at, glm::vec2 front, float radius,
                     float y = std::numeric_limits<float>::quiet_NaN()) {
        const float yaw = yawFacing(front);
        if (std::isnan(y)) y = standY(at);
        const int chunk = furnitureChunk(at);
        const float rr = glm::radians(yaw), cs = std::cos(rr), sn = std::sin(rr);
        stamp(M, glm::vec3(at.x, y, at.y), yaw, chunk, true);
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
    // Clear of every piece of furniture already standing.
    auto roomFor = [&](glm::vec2 p, float radius) {
        for (const Placed& f : out.furniture)
            if (glm::length(f.pos - p) < f.radius + radius) return false;
        return true;
    };
    // One model per traffic sign and one bin, stamped wherever they stand.
    std::array<const civic::Model*, static_cast<std::size_t>(props::Sign::Count)> signOf{};
    auto signModel = [&](props::Sign s) -> const civic::Model& {
        const civic::Model*& m = signOf[static_cast<std::size_t>(s)];
        if (!m) { models.push_back(props::trafficSign(s, pal.props)); m = &models.back(); }
        return *m;
    };
    const civic::Model* binM = nullptr;
    auto binModel = [&]() -> const civic::Model& {
        if (!binM) { models.push_back(props::bin(pal.props)); binM = &models.back(); }
        return *binM;
    };
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

            glm::vec2 signPost(1e9f);   // where the name sign went (the column keeps off it)
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
                    signPost = post;
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
            bool signalled = false;
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
                    if (lit) {
                        ++out.stats.lights;
                        out.signals.push_back(p);
                        signalled = true;
                    }
                }
            }

            // Right of way where nothing else says it: the major street's
            // approaches get the yellow diamond, the minor's the triangle (now
            // and then the octagon). Where an avenue or the main street meets
            // a side street, it has it; where two side streets meet in the
            // built-up part of town, the one parallel to the main street; out
            // among the family houses it stays right-before-left, as it does in
            // a German town -- no sign at all.
            if (r.trafficSigns && !signalled) {
                bool mx = major(SX), mz = major(SZ);
                if (mx == mz) {
                    bool built = false;
                    for (int bz = j - 1; bz <= j && !built; ++bz)
                        for (int bx = i - 1; bx <= i && !built; ++bx) {
                            const Zone z = zoneAt(bx, bz);
                            built = z == Zone::Towers || z == Zone::Blocks || z == Zone::Rows;
                        }
                    if (built && !mx) {
                        mx = false;
                        mz = true;
                    }
                }
                if (mx != mz) {
                    const bool stop = unit(hash3(r.seed ^ 0x5709U, static_cast<std::uint32_t>(i),
                                                 static_cast<std::uint32_t>(j))) < 0.2f;
                    const glm::vec2 dirs[4] = {dX, -dX, dZ, -dZ};
                    for (int q = 0; q < 4; ++q) {
                        const glm::vec2 d = dirs[q];          // the traffic's heading
                        const bool alongX = q < 2;
                        const float own = alongX ? hx : hz, other = alongX ? hz : hx;
                        const glm::vec2 right(-d.y, d.x);
                        const glm::vec2 at = p - d * (other + 2.8f) + right * (own + 0.45f);
                        if (!freeGround(at) || !roomFor(at, 0.4f) || glm::length(at - signPost) < 0.8f)
                            continue;
                        const bool onMajor = alongX ? mx : mz;
                        place(signModel(onMajor ? props::Sign::PriorityRoad
                                        : stop  ? props::Sign::Stop
                                                : props::Sign::GiveWay),
                              at, -d, 0.4f);
                        ++out.stats.trafficSigns;
                    }
                }
            }
            // An advertising column on a corner -- in the town, not out among
            // the family houses -- where the pavement has room for one.
            if (r.adverts > 0.0f && paved && r.sidewalk >= 2.8f &&
                unit(hash3(r.seed ^ 0xad01U, static_cast<std::uint32_t>(i),
                           static_cast<std::uint32_t>(j))) < 0.35f * r.adverts) {
                bool town = false;
                for (int bz = j - 1; bz <= j && !town; ++bz)
                    for (int bx = i - 1; bx <= i && !town; ++bx) {
                        const Zone z = zoneAt(bx, bz);
                        town = z == Zone::Towers || z == Zone::Blocks || z == Zone::Rows;
                    }
                for (int q = 3; q >= 0 && town; --q) {
                    const float a = (q & 1) ? -1.0f : 1.0f, b = (q & 2) ? -1.0f : 1.0f;
                    const glm::vec2 at = p + dX * (a * (hz + 1.9f)) + dZ * (b * (hx + 1.9f));
                    if (!freeGround(at) || !roomFor(at, 1.0f) || glm::length(at - signPost) < 1.6f)
                        continue;
                    models.push_back(props::advertColumn(
                        hash3(r.seed ^ 0xad02U, static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(j)),
                        pal.props));
                    place(models.back(), at, glm::normalize(p - at), 0.8f);
                    ++out.stats.adverts;
                    break;
                }
            }
        }

    // --- A village's junctions ------------------------------------------------------
    // No grid nodes: every lane meets another street at a T. Where it does, its
    // name on a post at the corner -- both streets' names, as at a crossing --
    // and "give way" for the traffic coming out of it (a stop now and then).
    if (lay.organic) {
        for (int li = 0; li < static_cast<int>(lay.streets.size()); ++li) {
            const Street& L1 = lay.streets[static_cast<std::size_t>(li)];
            if (L1.pts.size() < 2) continue;
            for (int end = 0; end < 2; ++end) {
                const glm::vec2 tip = end == 0 ? L1.pts.front() : L1.pts.back();
                const glm::vec2 next = end == 0 ? L1.pts[1] : L1.pts[L1.pts.size() - 2];
                // The street this end stands on, if any.
                int other = -1;
                glm::vec2 dO(1.0f, 0.0f);
                for (int o = 0; o < static_cast<int>(lay.streets.size()) && other < 0; ++o) {
                    if (o == li) continue;
                    const Street& S = lay.streets[static_cast<std::size_t>(o)];
                    glm::vec2 at, tn;
                    if (nearestOn(S.pts, tip, nullptr, &at, &tn) < 0.5f * S.width + 0.5f) {
                        other = o;
                        dO = tn;
                    }
                }
                if (other < 0) continue;
                const Street& O = lay.streets[static_cast<std::size_t>(other)];
                const glm::vec2 dL = glm::normalize(next - tip);      // into the lane
                const float hO = 0.5f * O.width, hL = 0.5f * L1.width;
                const std::uint32_t hj = hash3(r.seed ^ 0x7e7eU, static_cast<std::uint32_t>(li),
                                               static_cast<std::uint32_t>(end));
                glm::vec2 signPost(1e9f);
                if (r.signs) {
                    const glm::vec2 side(-dL.y, dL.x);
                    for (int q = 0; q < 2; ++q) {
                        const glm::vec2 post = tip + dL * (hO + 1.5f) + side * ((q ? -1.0f : 1.0f) * (hL + 1.2f));
                        if (!freeGround(post) || !roomFor(post, 0.3f)) continue;
                        const float yawA = std::atan2(-dO.y, dO.x);
                        const float yawB = std::atan2(-dL.y, dL.x);
                        sign.textA  = nameOf(other, tip, dO);
                        sign.textB  = nameOf(li, tip + dL * (hO + 4.0f), dL);
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
                            x.castsShadow = mat != pal.signs.ink;
                            extras.push_back(x);
                        }
                        Placed pl;
                        pl.pos = post;
                        pl.radius = 0.3f;
                        out.furniture.push_back(pl);
                        signPost = post;
                        ++out.stats.signs;
                        break;
                    }
                }
                if (r.trafficSigns && O.width >= L1.width - 0.01f) {
                    const glm::vec2 d = -dL;                       // the traffic's heading, out of the lane
                    const glm::vec2 right(-d.y, d.x);
                    const glm::vec2 at = tip - d * (hO + 2.8f) + right * (hL + 0.45f);
                    if (freeGround(at) && roomFor(at, 0.4f) && glm::length(at - signPost) > 0.8f) {
                        place(signModel(unit(hj) < 0.2f ? props::Sign::Stop : props::Sign::GiveWay), at, -d, 0.4f);
                        ++out.stats.trafficSigns;
                    }
                }
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
                    out.stops.push_back({signAt, hd});
                    models.push_back(civic::busStopSign(name, pal.civic));
                    // The sign's faces point along the road, so both directions read it.
                    place(models.back(), signAt, hd, 0.8f);
                    // A bin beside the stop.
                    if (r.binEvery > 1.0f) {
                        const glm::vec2 binAt = signAt + hd * 1.2f;
                        if (freeGround(binAt) && roomFor(binAt, 0.3f)) {
                            place(binModel(), binAt, right, 0.35f);
                            ++out.stats.bins;
                        }
                    }
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

    // A village's bus: one stop, a pair either side of the main street at the core.
    if (lay.organic && r.busStopEvery > 1.0f && lay.main >= 0) {
        const Street& M = lay.streets[static_cast<std::size_t>(lay.main)];
        float sCore = 0.0f;
        nearestOn(M.pts, lay.core, &sCore);
        const bool shelter = true;
        for (int tryN = 0; tryN < 8 && out.stats.busStops == 0; ++tryN) {
            const float sAt = sCore + static_cast<float>((tryN / 2 + 1) * ((tryN & 1) ? 18 : -18));
            glm::vec2 d;
            const glm::vec2 mid = along(M.pts, sAt, &d);
            const float half = 0.5f * M.width;
            int placed = 0;
            for (int dirSign = 1; dirSign >= -1; dirSign -= 2) {
                const glm::vec2 hd = d * static_cast<float>(dirSign);
                const glm::vec2 right(-hd.y, hd.x);
                const glm::vec2 base = mid - hd * 6.0f;
                const glm::vec2 signAt = base + hd * 3.0f + right * (half + 0.45f);
                const glm::vec2 shelterAt = base + right * (half + 1.6f);
                if (!freeGround(signAt) || !roomFor(signAt, 0.8f) ||
                    (shelter && (!freeGround(shelterAt) || !roomFor(shelterAt, 2.6f))))
                    continue;
                bool clearOfHouses = true;
                for (const Placed& pl : out.placed)
                    if (glm::length(pl.pos - shelterAt) < pl.radius + 1.5f) { clearOfHouses = false; break; }
                if (!clearOfHouses) continue;
                out.stops.push_back({signAt, hd});
                models.push_back(civic::busStopSign(M.name.empty() ? r.name : r.name + ", " + M.name, pal.civic));
                place(models.back(), signAt, hd, 0.8f);
                models.push_back(civic::busShelter(pal.civic));
                place(models.back(), shelterAt, -right, 2.6f);
                ++placed;
            }
            out.stats.busStops += placed;
        }
    }

    // --- Where the streets leave town: its name ---------------------------------
    // On the right of every street coming in, out on the stub before the first
    // crossing; the end-of-town sign is its back.
    if (r.trafficSigns && !r.name.empty()) {
        models.push_back(props::townSign(r.name, pal.props));
        const civic::Model& TS = models.back();
        float signHalf = 0.5f;   // half its width: its posts keep off the carriageway
        for (const auto& part : TS.parts)
            for (const fitzel::Vertex& v : part.second.vertices)
                signHalf = std::max(signHalf, std::abs(v.position.x));
        for (const Street& st : lay.streets) {
            if (st.pts.size() < 3) continue;
            for (int end = 0; end < 2; ++end) {
                // Only where the street really leaves: a village lane ending in a
                // farmyard is not the way into town.
                if (!(st.leaves & (1U << end))) continue;
                const glm::vec2 tip  = end == 0 ? st.pts.front() : st.pts.back();
                const glm::vec2 edge = end == 0 ? st.pts[1] : st.pts[st.pts.size() - 2];
                const float stubLen = glm::length(edge - tip);
                if (stubLen < 8.0f) continue;
                const glm::vec2 in = (edge - tip) / stubLen;     // heading into town
                glm::vec2 c, rd;
                float half = 0.5f * st.width, y = 0.0f;
                bool hasY = false;
                if (!roads.snap(edge - in * (0.55f * stubLen), in, c, rd, half, &y, &hasY)) continue;
                const glm::vec2 right(-in.y, in.x);
                const glm::vec2 at = c + right * (half + 0.6f + signHalf);
                if (!freeGround(at) || !roomFor(at, signHalf)) continue;
                place(TS, at, -in, signHalf, ground(at));
                ++out.stats.trafficSigns;
            }
        }
    }

    // --- Zebra crossings outside the schools, their signs either side ------------
    if (r.trafficSigns)
        for (const auto& [from, along] : zebraAt) {
            glm::vec2 c, rd;
            float half = 0.0f, y = 0.0f;
            bool hasY = false;
            if (!roads.snap(from, along, c, rd, half, &y, &hasY) || !hasY || wet(c)) continue;
            const glm::vec2 n(-rd.y, rd.x);
            models.push_back(props::zebra(2.0f * half, pal.props));
            stamp(models.back(), {c.x, y, c.y}, glm::degrees(std::atan2(-n.y, n.x)), furnitureChunk(c),
                  false);
            for (int sgn = -1; sgn <= 1; sgn += 2) {
                const glm::vec2 hd = rd * static_cast<float>(sgn);   // one way along the street
                const glm::vec2 right(-hd.y, hd.x);
                const glm::vec2 at = c - hd * 2.2f + right * (half + 0.45f);
                if (!freeGround(at) || !roomFor(at, 0.4f)) continue;
                place(signModel(props::Sign::Crossing), at, -hd, 0.4f);
                ++out.stats.trafficSigns;
            }
        }

    // --- Billboards on some of the plots left empty -------------------------------
    // At the front of the plot, facing the street.
    if (r.adverts > 0.0f)
        for (const Lot* lot : emptyPlots) {
            const float chance = (lot->zone == Zone::Houses ? 0.25f : 0.7f) * r.adverts;
            if (unit(hashU(lot->hash ^ 0xb11bU)) >= chance || lot->width < 5.0f) continue;
            const glm::vec2 at = lot->pos + lot->front * (0.5f * lot->depth - 1.0f);
            if (!freeGround(at) || roads.clearance(at) < 1.5f || !roomFor(at, 2.0f)) continue;
            models.push_back(props::billboard(lot->hash, pal.props));
            place(models.back(), at, lot->front, 2.0f, ground(at));
            ++out.stats.adverts;
        }

    // --- Manhole covers down the streets, gullies in the gutters ------------------
    // A cover every 40 m or so between the crossings, in the middle of a lane,
    // now one side, now the other. Flat on the carriageway: no shadow, and no
    // place in the furniture (nothing has to keep off them).
    if (r.manholes) {
        models.push_back(props::manhole(pal.props));
        const civic::Model& MH = models.back();
        for (int s = 0; s < static_cast<int>(lay.streets.size()); ++s) {
            const Street& st = lay.streets[static_cast<std::size_t>(s)];
            const int count = st.alongZ ? lay.nz : lay.nx;
            const std::vector<int>& cross = st.alongZ ? xLine : zLine;
            auto crossHalf = [&](int k) {
                const int c = cross[static_cast<std::size_t>(k)];
                return c >= 0 ? 0.5f * lay.streets[static_cast<std::size_t>(c)].width : 4.0f;
            };
            for (int m = 0; m < count; ++m) {
                const glm::vec2 a = st.alongZ ? node(st.line, m) : node(m, st.line);
                const glm::vec2 b = st.alongZ ? node(st.line, m + 1) : node(m + 1, st.line);
                const float len = glm::length(b - a);
                if (len < 20.0f) continue;
                const glm::vec2 d = (b - a) / len;
                const std::uint32_t hs = hash3(r.seed ^ 0x3a40U, static_cast<std::uint32_t>(s),
                                               static_cast<std::uint32_t>(m));
                float t = crossHalf(m) + 5.0f + 10.0f * unit(hs);
                for (int k = 0; t < len - crossHalf(m + 1) - 5.0f && k < 64; ++k) {
                    const glm::vec2 q = a + d * t;
                    t += 36.0f + 12.0f * unit(hashU(hs ^ static_cast<std::uint32_t>(k + 1)));
                    glm::vec2 c, rd;
                    float half = 0.0f, y = 0.0f;
                    bool hasY = false;
                    if (!roads.snap(q, d, c, rd, half, &y, &hasY) || !hasY || wet(q)) continue;
                    const float off = ((k + static_cast<int>(hs & 1U)) % 2 == 0 ? 0.5f : -0.5f) * half;
                    const glm::vec2 at = c + glm::vec2(-rd.y, rd.x) * off;
                    stamp(MH, {at.x, y, at.y}, 0.0f, furnitureChunk(at), false);
                    ++out.stats.manholes;
                }
            }
        }
        models.push_back(props::gully(pal.props));
        const civic::Model& GU = models.back();
        for (const GullySpot& g : gullySpots) {
            stamp(GU, g.at, g.yaw, furnitureChunk({g.at.x, g.at.z}), false);
            ++out.stats.manholes;
        }
    }

    // --- Street lamps ---------------------------------------------------------------
    // The spots gathered along the kerbs, less those a traffic light, a stop or a
    // shelter already stands on; each gets one of the rule's prefabs by weight,
    // by a hash of where it stands (so re-deriving keeps every lamp's pick).
    // Only the placement is made here: a prefab is a whole entity tree -- a
    // model, a light -- that the merged town cannot hold, so TownLamps draws
    // and lights them.
    if (!lampSpots.empty()) {
        float total = 0.0f;
        for (const LampPrefab& lp : r.lampPrefabs)
            if (!lp.prefab.empty()) total += std::max(lp.weight, 0.0f);
        const std::size_t others = out.furniture.size();
        for (const LampSpot& s : lampSpots) {
            bool clear = true;
            for (std::size_t f = 0; f < others && clear; ++f)
                clear = glm::length(out.furniture[f].pos - s.at) > out.furniture[f].radius + 1.0f;
            if (!clear) continue;
            float pick = total * unit(hash3(r.seed ^ 0x1a3bU,
                                            static_cast<std::uint32_t>(std::lround(s.at.x * 10.0f)),
                                            static_cast<std::uint32_t>(std::lround(s.at.y * 10.0f))));
            int idx = -1;
            for (int i = 0; i < static_cast<int>(r.lampPrefabs.size()); ++i) {
                const LampPrefab& lp = r.lampPrefabs[static_cast<std::size_t>(i)];
                const float w = lp.prefab.empty() ? 0.0f : std::max(lp.weight, 0.0f);
                if (w <= 0.0f) continue;
                idx = i;
                if (pick < w) break;
                pick -= w;
            }
            if (idx < 0 && total > 0.0f) break;
            // idx -1: no prefab named -- the standard lamp.
            out.lamps.push_back({glm::vec3(s.at.x, standY(s.at), s.at.y), s.facing, idx});
            Placed pl;
            pl.pos    = s.at;
            pl.radius = 0.6f;
            out.furniture.push_back(pl);
            ++out.stats.lamps;
            // Now and then a litter bin beside it.
            if (r.binEvery > 1.0f &&
                unit(hash3(r.seed ^ 0xb1b1U, static_cast<std::uint32_t>(std::lround(s.at.x * 10.0f)),
                           static_cast<std::uint32_t>(std::lround(s.at.y * 10.0f)))) <
                    r.lampEvery / r.binEvery) {
                const glm::vec2 at = s.at + glm::vec2(-s.facing.y, s.facing.x) * 1.0f;
                if (!wet(at) && roads.clearance(at) > 0.15f && roomFor(at, 0.3f)) {
                    place(binModel(), at, -s.facing, 0.35f);
                    ++out.stats.bins;
                }
            }
        }
    }

    // --- Power lines: from every power station out of town --------------------
    // Straight away from the town's centre, a pylon every 220 m or so -- each
    // one nudged along or across the line until it stands clear of roads, water
    // and anything built -- and the wires sagging between them.
    std::deque<fitzel::MeshData> wireMeshes;
    if (r.powerLines && r.powerLineLength > 50.0f && !plants.empty()) {
        std::vector<glm::vec3> attach;
        models.push_back(civic::pylon(pal.civic, &attach));
        const civic::Model& PY = models.back();
        const glm::vec2 townC = r.built().center;
        auto clearOf = [&](glm::vec2 q) {
            if (wet(q) || roads.clearance(q) < 7.0f) return false;
            for (const Placed& p : out.placed)
                if (glm::length(p.pos - q) < p.radius + 6.0f) return false;
            for (const Placed& p : out.furniture)
                if (glm::length(p.pos - q) < p.radius + 6.0f) return false;
            return true;
        };
        for (const glm::vec2& plant : plants) {
            glm::vec2 d = plant - townC;
            d = glm::length(d) > 1.0f ? glm::normalize(d) : lay.axisX;
            const glm::vec2 side(-d.y, d.x);
            std::vector<glm::vec2> towers;
            for (float s = 70.0f; s <= r.powerLineLength; s += 220.0f) {
                const glm::vec2 p = plant + d * s;
                bool ok = false;
                glm::vec2 q = p;
                for (float along : {0.0f, 25.0f, -25.0f, 50.0f})
                    for (float off : {0.0f, 15.0f, -15.0f, 30.0f, -30.0f})
                        if (!ok) {
                            q = p + d * along + side * off;
                            ok = clearOf(q);
                        }
                if (ok) towers.push_back(q);
            }
            if (towers.size() < 2) continue;
            // Each pylon turned to bisect the line's bend at it.
            std::vector<glm::mat4> frames;
            for (std::size_t i = 0; i < towers.size(); ++i) {
                const glm::vec2 in  = i > 0 ? glm::normalize(towers[i] - towers[i - 1]) : d;
                const glm::vec2 out2 = i + 1 < towers.size() ? glm::normalize(towers[i + 1] - towers[i]) : d;
                glm::vec2 h = in + out2;
                h = glm::length(h) > 1e-3f ? glm::normalize(h) : d;
                const float yaw = glm::degrees(std::atan2(h.x, h.y));   // local +z along h
                const glm::vec2 q = towers[i];
                const float y = ground(q);
                const int chunk = chunkOf(q);
                for (const auto& [mat, md] : PY.parts) {
                    city::Extra x;
                    x.mesh     = &md;
                    x.at       = glm::vec3(q.x, y, q.y);
                    x.yaw      = yaw;
                    x.material = mat;
                    x.chunk    = chunk;
                    extras.push_back(x);
                }
                const float rr = glm::radians(yaw), cs = std::cos(rr), sn = std::sin(rr);
                glm::mat4 f(1.0f);
                f[0] = glm::vec4(cs, 0.0f, -sn, 0.0f);
                f[2] = glm::vec4(sn, 0.0f, cs, 0.0f);
                f[3] = glm::vec4(q.x, y, q.y, 1.0f);
                frames.push_back(f);
                if (r.collider)
                    for (const city::Piece& sp : PY.solids) {
                        city::Piece c = sp;
                        c.center = glm::vec3(f * glm::vec4(sp.center, 1.0f));
                        c.yaw    = yaw;
                        pcs.push_back(c);
                        pcChunk.push_back(chunk);
                    }
                Placed pl;
                pl.pos    = q;
                pl.radius = 7.0f;
                out.furniture.push_back(pl);
                ++out.stats.pylons;
            }
            // The wires, span by span, in one mesh per line.
            wireMeshes.emplace_back();
            fitzel::MeshData& wires = wireMeshes.back();
            for (std::size_t i = 0; i + 1 < frames.size(); ++i)
                for (const glm::vec3& a : attach) {
                    const glm::vec3 A = glm::vec3(frames[i] * glm::vec4(a, 1.0f));
                    const glm::vec3 B = glm::vec3(frames[i + 1] * glm::vec4(a, 1.0f));
                    civic::cable(wires, A, B, 0.03f * glm::length(B - A));
                }
            city::Extra x;
            x.mesh     = &wires;
            x.material = pal.civic.darkGrey;
            x.chunk    = (1 << 28) + static_cast<int>(std::floor(plant.x / 1000.0f)) * 8192 +
                         static_cast<int>(std::floor(plant.y / 1000.0f));
            x.castsShadow = false;   // a hand's breadth wide at forty metres
            extras.push_back(x);
        }
    }

    city::merge(pcs, pcChunk, out.district, &extras);
    out.district.placed = out.stats.built;
    return out;
}

} // namespace cityplan
