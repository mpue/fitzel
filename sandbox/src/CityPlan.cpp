#include "CityPlan.hpp"

#include <algorithm>
#include <cmath>
#include <map>
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
    struct Seg { glm::vec2 a, b; float half; };
    std::vector<Seg> segs;
    float cell = 40.0f;
    std::unordered_map<std::int64_t, std::vector<int>> cells;

    static std::int64_t key(int x, int z) {
        return (static_cast<std::int64_t>(x) << 32) ^ static_cast<std::uint32_t>(z);
    }
    void build(const std::vector<RoadLine>& roads) {
        for (const RoadLine& r : roads)
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
                    segs.push_back({a, b, r.half});
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
// culling need something to bite on, so a town is welded in 160 m squares.
int chunkOf(glm::vec2 p) {
    const int cx = static_cast<int>(std::floor(p.x / 160.0f));
    const int cz = static_cast<int>(std::floor(p.y / 160.0f));
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
            r.budget = 3000;
            break;
        case Preset::Suburb:
            r.grid.sizeX = 480.0f; r.grid.sizeZ = 360.0f;
            r.grid.blockX = 90.0f; r.grid.blockZ = 60.0f; r.grid.organic = 0.30f;
            r.grid.streetWidth = 6.0f; r.grid.avenueWidth = 9.0f; r.grid.avenueEvery = 0;
            r.towerRing = 0.0f; r.blockRing = 0.0f; r.rowRing = 0.0f;
            r.parkChance = 0.06f; r.centrePark = true;
            r.houseLot = 20.0f; r.houseSetback = 6.0f; r.fill = 0.92f;
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
        {"maxBridge", g.maxBridge},
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
        if (B.zone == Zone::Park) continue;

        // --- The building line --------------------------------------------------
        // Each side pulled in by its own street's half-width plus the pavement;
        // the corners are where neighbouring pulled-in sides meet.
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
        std::vector<glm::vec2> inset(4);
        bool ok = true;
        for (int k = 0; k < 4; ++k)
            ok &= intersect(lp[(k + 3) % 4], ld[(k + 3) % 4], lp[k], ld[k], inset[k]);
        if (!ok) continue;
        // Folded through itself (streets wider than the block between them)?
        // Then the pulled-in outline winds the other way round, and nothing fits.
        float area = 0.0f, area0 = 0.0f;
        for (int k = 0; k < 4; ++k) {
            area  += cross2(inset[k], inset[(k + 1) % 4]);
            area0 += cross2(B.corner[k], B.corner[(k + 1) % 4]);
        }
        if (area * area0 <= 0.0f || std::abs(area) * 0.5f < 150.0f) continue;
        glm::vec2 icen(0.0f);
        for (const glm::vec2& c : inset) icen += c;
        icen *= 0.25f;

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

Palettes ensurePalettes(std::vector<MaterialDef>& materials, const Rule& r) {
    Palettes p;
    p.towers = buildings::ensurePalette(materials, towerLook(r));
    p.blocks = buildings::ensurePalette(materials, blockLook(r));
    p.houses = housegen::ensurePalette(materials, housePrototype(r, Zone::Houses, 0),
                                       "City House");
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
            city::flatten(es, glm::vec3(lot.pos.x, gLo, lot.pos.y), yaw, pcs, lo, hi);
        } else {
            // --- HouseGen -------------------------------------------------------
            // Stood on the HIGH corner, never buried: a front door below ground is
            // worse than a plinth that shows. The foundation under it fills the
            // drop down to the low corner.
            const HouseProto& hp = proto(lot.zone, lot.kind);
            const float baseY = gHi;
            for (const auto& [mat, md] : hp.parts) {
                city::Extra x;
                x.mesh = &md;
                x.at   = glm::vec3(lot.pos.x, baseY, lot.pos.y);
                x.yaw  = yaw;
                x.material = mat;
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

    city::merge(pcs, pcChunk, out.district, &extras);
    out.district.placed = out.stats.built;
    return out;
}

} // namespace cityplan
