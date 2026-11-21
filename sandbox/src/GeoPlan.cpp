#include "GeoPlan.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <numeric>
#include <set>
#include <unordered_map>

#include "CivicGen.hpp"
#include "HouseGen.hpp"
#include "RiverGen.hpp"

namespace geoplan {

namespace {

// --- Small things ------------------------------------------------------------------------

std::uint32_t mix32(std::uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33;
    return static_cast<std::uint32_t>(x);
}
float unit(std::uint32_t h) { return static_cast<float>(h & 0xffffffU) / 16777215.0f; }

float smooth01(float e0, float e1, float x) {
    const float t = glm::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float cross2(glm::vec2 a, glm::vec2 b) { return a.x * b.y - a.y * b.x; }

float polyArea(const std::vector<glm::vec2>& p) {
    double a = 0.0;
    for (std::size_t i = 0; i < p.size(); ++i) {
        const glm::vec2& u = p[i];
        const glm::vec2& v = p[(i + 1) % p.size()];
        a += static_cast<double>(u.x) * v.y - static_cast<double>(v.x) * u.y;
    }
    return static_cast<float>(0.5 * a);
}

glm::vec2 polyCentre(const std::vector<glm::vec2>& p) {
    double a = 0.0, cx = 0.0, cy = 0.0;
    const glm::vec2 o = p.empty() ? glm::vec2(0.0f) : p[0];
    for (std::size_t i = 0; i < p.size(); ++i) {
        const glm::vec2 u = p[i] - o, v = p[(i + 1) % p.size()] - o;
        const double c = static_cast<double>(u.x) * v.y - static_cast<double>(v.x) * u.y;
        a += c; cx += (u.x + v.x) * c; cy += (u.y + v.y) * c;
    }
    if (std::fabs(a) < 1e-9) {
        glm::vec2 m(0.0f);
        for (const glm::vec2& q : p) m += q;
        return p.empty() ? m : m / static_cast<float>(p.size());
    }
    return o + glm::vec2(static_cast<float>(cx / (3.0 * a)), static_cast<float>(cy / (3.0 * a)));
}

float segDist(glm::vec2 p, glm::vec2 a, glm::vec2 b, glm::vec2* at = nullptr) {
    const glm::vec2 ab = b - a;
    const float L = glm::dot(ab, ab);
    const float t = L > 1e-12f ? glm::clamp(glm::dot(p - a, ab) / L, 0.0f, 1.0f) : 0.0f;
    const glm::vec2 q = a + ab * t;
    if (at) *at = q;
    return glm::length(p - q);
}

bool insidePoly(const std::vector<glm::vec2>& poly, glm::vec2 p) {
    bool in = false;
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const glm::vec2& a = poly[i];
        const glm::vec2& b = poly[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) in = !in;
    }
    return in;
}

// Distance from p to a polygon's boundary, 0 inside it.
float polyDist(const std::vector<glm::vec2>& poly, glm::vec2 p) {
    if (poly.size() >= 3 && insidePoly(poly, p)) return 0.0f;
    float d = 1e9f;
    for (std::size_t i = 0; i < poly.size(); ++i) d = std::min(d, segDist(p, poly[i], poly[(i + 1) % poly.size()]));
    return d;
}

float lineLength(const std::vector<glm::vec2>& l) {
    float s = 0.0f;
    for (std::size_t i = 1; i < l.size(); ++i) s += glm::length(l[i] - l[i - 1]);
    return s;
}

std::vector<glm::vec2> convexHull(std::vector<glm::vec2> p) {
    std::sort(p.begin(), p.end(), [](glm::vec2 a, glm::vec2 b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    p.erase(std::unique(p.begin(), p.end()), p.end());
    if (p.size() < 3) return p;
    std::vector<glm::vec2> h(2 * p.size());
    std::size_t k = 0;
    for (std::size_t i = 0; i < p.size(); ++i) {
        while (k >= 2 && cross2(h[k - 1] - h[k - 2], p[i] - h[k - 2]) <= 0.0f) --k;
        h[k++] = p[i];
    }
    for (std::size_t i = p.size() - 1, t = k + 1; i > 0; --i) {
        while (k >= t && cross2(h[k - 1] - h[k - 2], p[i - 1] - h[k - 2]) <= 0.0f) --k;
        h[k++] = p[i - 1];
    }
    h.resize(k - 1);
    return h;
}

// The smallest rectangle round a footprint (rotating calipers on its hull): what
// a generator that builds boxes can stand on it.
void minRect(const std::vector<glm::vec2>& poly, glm::vec2& c, glm::vec2& u, float& len, float& wid) {
    const std::vector<glm::vec2> h = convexHull(poly);
    float best = 1e30f;
    c = polyCentre(poly); u = {1.0f, 0.0f}; len = wid = 0.0f;
    if (h.size() < 3) return;
    for (std::size_t i = 0; i < h.size(); ++i) {
        glm::vec2 e = h[(i + 1) % h.size()] - h[i];
        const float el = glm::length(e);
        if (el < 1e-4f) continue;
        e /= el;
        const glm::vec2 n(-e.y, e.x);
        float a0 = 1e30f, a1 = -1e30f, b0 = 1e30f, b1 = -1e30f;
        for (const glm::vec2& q : h) {
            const float a = glm::dot(q, e), b = glm::dot(q, n);
            a0 = std::min(a0, a); a1 = std::max(a1, a);
            b0 = std::min(b0, b); b1 = std::max(b1, b);
        }
        const float area = (a1 - a0) * (b1 - b0);
        if (area < best) {
            best = area;
            const float L = a1 - a0, W = b1 - b0;
            c = e * (0.5f * (a0 + a1)) + n * (0.5f * (b0 + b1));
            if (L >= W) { u = e; len = L; wid = W; }
            else        { u = n; len = W; wid = L; }
        }
    }
}

// Douglas-Peucker, never dropping a point `keep` says must stay (where another
// road meets this one: both have to pass through exactly that point).
std::vector<glm::vec2> simplify(const std::vector<glm::vec2>& l, float tol, const std::vector<char>& keep) {
    if (l.size() < 3) return l;
    std::vector<char> use(l.size(), 0);
    use.front() = use.back() = 1;
    for (std::size_t i = 0; i < l.size() && i < keep.size(); ++i) use[i] |= keep[i];
    std::vector<std::pair<std::size_t, std::size_t>> st;
    // Split at every kept point, then DP within each run.
    std::size_t a = 0;
    for (std::size_t i = 1; i < l.size(); ++i)
        if (use[i]) { st.emplace_back(a, i); a = i; }
    while (!st.empty()) {
        const auto [i0, i1] = st.back();
        st.pop_back();
        float worst = -1.0f;
        std::size_t wi = i0;
        for (std::size_t i = i0 + 1; i < i1; ++i) {
            const float d = segDist(l[i], l[i0], l[i1]);
            if (d > worst) { worst = d; wi = i; }
        }
        if (worst > tol) {
            use[wi] = 1;
            st.emplace_back(i0, wi);
            st.emplace_back(wi, i1);
        }
    }
    std::vector<glm::vec2> out;
    std::vector<char> outKeep;
    for (std::size_t i = 0; i < l.size(); ++i)
        if (use[i]) out.push_back(l[i]);
    return out;
}

float parseMetres(const std::string& s) {
    if (s.empty()) return 0.0f;
    char* end = nullptr;
    const float v = std::strtof(s.c_str(), &end);
    if (end == s.c_str()) return 0.0f;
    // "12 ft" happens; feet are a third of a metre.
    if (s.find("ft") != std::string::npos || s.find('\'') != std::string::npos) return v * 0.3048f;
    return v;
}

int parseInt(const std::string& s) {
    if (s.empty()) return 0;
    return static_cast<int>(std::lround(std::strtof(s.c_str(), nullptr)));
}

bool oneOf(const std::string& v, std::initializer_list<const char*> list) {
    for (const char* x : list)
        if (v == x) return true;
    return false;
}

// --- The land, as a raster ------------------------------------------------------------------

enum Land : std::uint8_t {
    LNone, LFarmland, LMeadow, LResidential, LCommercial, LIndustrial, LFarmyard, LAllotments,
    LOrchard, LPark, LCemetery, LScrub, LForest, LRock, LWetland, LWater, LRoad, LBuilding, LCount
};

const char* landName(int l) {
    static const char* n[LCount] = {"", "farmland", "meadow", "residential", "commercial", "industrial",
                                    "farmyard", "allotments", "orchard", "park", "cemetery", "scrub",
                                    "forest", "rock", "wetland", "water", "road", "building"};
    return l >= 0 && l < LCount ? n[l] : "";
}

// What a land-use area is, or LNone for one the plan ignores.
Land landOf(const geo::Feature& f) {
    const std::string lu = f.tagOr("landuse"), nat = f.tagOr("natural"), lei = f.tagOr("leisure"),
                      am = f.tagOr("amenity"), ww = f.tagOr("waterway");
    if (nat == "water" || ww == "riverbank" || lu == "reservoir" || lu == "basin" || nat == "bay") return LWater;
    if (nat == "wetland") return LWetland;
    if (lu == "forest" || nat == "wood") return LForest;
    if (nat == "scrub" || nat == "heath") return LScrub;
    if (oneOf(nat, {"bare_rock", "scree", "shingle", "rock", "stone", "glacier", "sand", "beach"})) return LRock;
    if (lu == "cemetery" || am == "grave_yard") return LCemetery;
    if (oneOf(lei, {"park", "garden", "playground", "pitch", "golf_course", "recreation_ground", "sports_centre",
                    "stadium", "track", "dog_park", "nature_reserve"}) ||
        lu == "recreation_ground" || lu == "village_green")
        return LPark;
    if (oneOf(lu, {"orchard", "vineyard", "plant_nursery"})) return LOrchard;
    if (lu == "allotments") return LAllotments;
    if (lu == "farmyard") return LFarmyard;
    if (oneOf(lu, {"industrial", "railway", "quarry", "landfill", "construction", "brownfield", "military",
                   "port", "depot", "garages"}))
        return LIndustrial;
    if (oneOf(lu, {"commercial", "retail"}) || oneOf(am, {"parking", "marketplace"})) return LCommercial;
    if (lu == "residential" || oneOf(am, {"school", "university", "college", "kindergarten", "hospital"})) return LResidential;
    if (oneOf(lu, {"farmland", "greenhouse_horticulture"})) return LFarmland;
    if (oneOf(lu, {"meadow", "grass", "flowerbed"}) || oneOf(nat, {"grassland", "fell"})) return LMeadow;
    return LNone;
}

// Which land wins where two overlap: the more particular one, drawn later.
int landRank(Land l) {
    static const int r[LCount] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 10, 11, 12, 13, 14, 15, 16, 17};
    return r[l];
}

struct Raster {
    int nx = 0, nz = 0;
    float x0 = 0.0f, z0 = 0.0f, cell = 4.0f;   // centre of cell (0, 0)
    std::vector<std::uint8_t> v;
    void init(glm::vec2 lo, float side, float c) {
        cell = c;
        nx = nz = std::max(2, static_cast<int>(std::ceil(side / c)));
        x0 = lo.x + 0.5f * c;
        z0 = lo.y + 0.5f * c;
        v.assign(static_cast<std::size_t>(nx) * nz, 0);
    }
    std::uint8_t at(glm::vec2 p) const {
        const int i = static_cast<int>(std::floor((p.x - x0) / cell + 0.5f));
        const int j = static_cast<int>(std::floor((p.y - z0) / cell + 0.5f));
        if (i < 0 || j < 0 || i >= nx || j >= nz) return 0;
        return v[static_cast<std::size_t>(j) * nx + i];
    }
    glm::vec2 centre(int i, int j) const { return {x0 + cell * static_cast<float>(i), z0 + cell * static_cast<float>(j)}; }
    // Even-odd over every ring (so holes are holes); `put` decides per cell.
    void fill(const std::vector<std::vector<glm::vec2>>& rings, const std::function<void(std::uint8_t&)>& put) {
        float zlo = 1e30f, zhi = -1e30f;
        for (const auto& r : rings)
            for (const glm::vec2& p : r) { zlo = std::min(zlo, p.y); zhi = std::max(zhi, p.y); }
        const int j0 = std::max(0, static_cast<int>(std::floor((zlo - z0) / cell))),
                  j1 = std::min(nz - 1, static_cast<int>(std::ceil((zhi - z0) / cell)));
        std::vector<float> xs;
        for (int j = j0; j <= j1; ++j) {
            const float z = z0 + cell * static_cast<float>(j);
            xs.clear();
            for (const auto& r : rings)
                for (std::size_t k = 0; k < r.size(); ++k) {
                    const glm::vec2& a = r[k];
                    const glm::vec2& b = r[(k + 1) % r.size()];
                    if ((a.y > z) != (b.y > z)) xs.push_back(a.x + (z - a.y) / (b.y - a.y) * (b.x - a.x));
                }
            std::sort(xs.begin(), xs.end());
            for (std::size_t k = 0; k + 1 < xs.size(); k += 2) {
                const int i0 = std::max(0, static_cast<int>(std::ceil((xs[k] - x0) / cell)));
                const int i1 = std::min(nx - 1, static_cast<int>(std::floor((xs[k + 1] - x0) / cell)));
                for (int i = i0; i <= i1; ++i) put(v[static_cast<std::size_t>(j) * nx + i]);
            }
        }
    }
    void stroke(const std::vector<glm::vec2>& l, float half, const std::function<void(std::uint8_t&)>& put) {
        for (std::size_t k = 1; k < l.size(); ++k) {
            const glm::vec2 a = l[k - 1], b = l[k];
            const glm::vec2 lo = glm::min(a, b) - glm::vec2(half), hi = glm::max(a, b) + glm::vec2(half);
            const int i0 = std::max(0, static_cast<int>(std::floor((lo.x - x0) / cell))),
                      i1 = std::min(nx - 1, static_cast<int>(std::ceil((hi.x - x0) / cell)));
            const int j0 = std::max(0, static_cast<int>(std::floor((lo.y - z0) / cell))),
                      j1 = std::min(nz - 1, static_cast<int>(std::ceil((hi.y - z0) / cell)));
            for (int j = j0; j <= j1; ++j)
                for (int i = i0; i <= i1; ++i)
                    if (segDist(centre(i, j), a, b) <= half) put(v[static_cast<std::size_t>(j) * nx + i]);
        }
    }
};

// --- A grid of things by where they are -------------------------------------------------------

struct Buckets {
    float cell = 40.0f;
    std::unordered_map<std::int64_t, std::vector<int>> m;
    static std::int64_t key(int i, int j) { return (static_cast<std::int64_t>(i) << 32) ^ static_cast<std::uint32_t>(j); }
    void add(glm::vec2 lo, glm::vec2 hi, int id) {
        const int i0 = static_cast<int>(std::floor(lo.x / cell)), i1 = static_cast<int>(std::floor(hi.x / cell));
        const int j0 = static_cast<int>(std::floor(lo.y / cell)), j1 = static_cast<int>(std::floor(hi.y / cell));
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) m[key(i, j)].push_back(id);
    }
    template <class F> void near(glm::vec2 p, float r, F&& f) const {
        const int i0 = static_cast<int>(std::floor((p.x - r) / cell)), i1 = static_cast<int>(std::floor((p.x + r) / cell));
        const int j0 = static_cast<int>(std::floor((p.y - r) / cell)), j1 = static_cast<int>(std::floor((p.y + r) / cell));
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) {
                const auto it = m.find(key(i, j));
                if (it != m.end())
                    for (int id : it->second) f(id);
            }
    }
};

// --- Roads ---------------------------------------------------------------------------------------

bool roadClassOf(const geo::Feature& f, RoadClass& c) {
    const std::string* h = f.tag("highway");
    if (!h) return false;
    std::string v = *h;
    if (v.size() > 5 && v.compare(v.size() - 5, 5, "_link") == 0) v = v.substr(0, v.size() - 5);
    if (v == "motorway") c = RoadClass::Motorway;
    else if (v == "trunk") c = RoadClass::Trunk;
    else if (v == "primary") c = RoadClass::Primary;
    else if (v == "secondary") c = RoadClass::Secondary;
    else if (v == "tertiary") c = RoadClass::Tertiary;
    else if (v == "unclassified" || v == "road") c = RoadClass::Unclassified;
    else if (v == "residential") c = RoadClass::Residential;
    else if (v == "living_street") c = RoadClass::LivingStreet;
    else if (v == "pedestrian") c = RoadClass::Pedestrian;
    else if (v == "service") {
        const std::string s = f.tagOr("service");
        if (!s.empty() && s != "alley") return false;   // driveways, parking aisles: the plots' own
        c = RoadClass::Service;
    } else if (v == "track") {
        // The good ones only: a grade-5 track is two ruts in a field.
        const std::string t = f.tagOr("tracktype");
        if (t == "grade4" || t == "grade5") return false;
        c = RoadClass::Track;
    } else {
        return false;
    }
    if (f.is("area", "yes")) return false;
    const std::string tun = f.tagOr("tunnel");
    if (tun == "yes" || tun == "building_passage" || tun == "culvert") return false;
    return true;
}

float classWidth(RoadClass c, bool oneway) {
    switch (c) {
    case RoadClass::Motorway:     return oneway ? 10.5f : 22.0f;
    case RoadClass::Trunk:        return oneway ? 8.5f : 12.0f;
    case RoadClass::Primary:      return oneway ? 6.5f : 8.5f;
    case RoadClass::Secondary:    return oneway ? 6.0f : 7.5f;
    case RoadClass::Tertiary:     return 6.5f;
    case RoadClass::Unclassified: return 5.5f;
    case RoadClass::Residential:  return 5.5f;
    case RoadClass::LivingStreet: return 4.5f;
    case RoadClass::Pedestrian:   return 5.0f;
    case RoadClass::Service:      return 3.5f;
    case RoadClass::Track:        return 3.0f;
    default:                      return 5.5f;
    }
}

float classMinWidth(RoadClass c) {
    return c == RoadClass::Track ? 2.5f : c == RoadClass::Service ? 3.0f : 3.5f;
}

// --- Trees ---------------------------------------------------------------------------------------

std::string speciesOf(const geo::Feature& f, std::uint32_t h, bool street, bool park) {
    std::string genus = f.tagOr("genus");
    const std::string sp = f.tagOr("species");
    if (genus.empty() && !sp.empty()) genus = sp.substr(0, sp.find(' '));
    for (char& c : genus) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (oneOf(genus, {"quercus", "robinia", "castanea"})) return "Oak";
    if (oneOf(genus, {"fagus", "carpinus", "fraxinus", "ulmus"})) return "Beech";
    if (oneOf(genus, {"acer", "tilia", "platanus", "aesculus", "prunus", "malus", "pyrus", "sorbus", "crataegus"})) return "Maple";
    if (oneOf(genus, {"betula", "alnus"})) return "Birch";
    if (oneOf(genus, {"picea", "abies", "pseudotsuga", "taxus", "thuja"})) return "Spruce";
    if (oneOf(genus, {"pinus", "larix"})) return "Scots pine";
    if (genus == "populus") return "Lombardy poplar";
    if (genus == "salix") return "Weeping willow";
    if (f.is("leaf_type", "needleleaved")) return unit(h) < 0.6f ? "Spruce" : "Scots pine";
    // Unsaid: a street is lined with limes and maples, a park has its oaks and
    // beeches, a garden a bit of everything.
    const float u = unit(h);
    if (street) return u < 0.7f ? "Maple" : "Oak";
    if (park) return u < 0.4f ? "Oak" : u < 0.75f ? "Beech" : "Maple";
    return u < 0.35f ? "Maple" : u < 0.6f ? "Oak" : u < 0.8f ? "Birch" : "Beech";
}

float speciesHeight(const std::string& s) {
    if (s == "Oak") return 18.0f;
    if (s == "Beech") return 24.0f;
    if (s == "Maple") return 16.0f;
    if (s == "Birch") return 17.0f;
    if (s == "Spruce") return 27.0f;
    if (s == "Scots pine") return 21.0f;
    if (s == "Lombardy poplar") return 24.0f;
    if (s == "Weeping willow") return 12.0f;
    return 5.0f;
}

// --- Colours named on the map -----------------------------------------------------------------

bool colourOf(std::string s, glm::vec3& c) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (s.size() == 7 && s[0] == '#') {
        const long v = std::strtol(s.c_str() + 1, nullptr, 16);
        c = glm::vec3((v >> 16) & 255, (v >> 8) & 255, v & 255) / 255.0f;
        return true;
    }
    static const std::map<std::string, glm::vec3> named = {
        {"red", {0.56f, 0.23f, 0.15f}}, {"darkred", {0.42f, 0.16f, 0.12f}}, {"brown", {0.40f, 0.25f, 0.17f}},
        {"maroon", {0.38f, 0.15f, 0.12f}}, {"orange", {0.66f, 0.36f, 0.20f}}, {"terracotta", {0.62f, 0.31f, 0.20f}},
        {"grey", {0.42f, 0.42f, 0.43f}}, {"gray", {0.42f, 0.42f, 0.43f}}, {"darkgrey", {0.27f, 0.27f, 0.28f}},
        {"darkgray", {0.27f, 0.27f, 0.28f}}, {"black", {0.15f, 0.15f, 0.16f}}, {"anthracite", {0.20f, 0.21f, 0.22f}},
        {"lightgrey", {0.62f, 0.62f, 0.62f}}, {"silver", {0.62f, 0.63f, 0.64f}}, {"green", {0.30f, 0.45f, 0.36f}},
        {"white", {0.93f, 0.92f, 0.88f}}, {"beige", {0.87f, 0.80f, 0.66f}}, {"yellow", {0.90f, 0.80f, 0.52f}},
        {"cream", {0.92f, 0.88f, 0.76f}}, {"tan", {0.78f, 0.66f, 0.50f}}};
    const auto it = named.find(s);
    if (it == named.end()) return false;
    c = it->second;
    return true;
}

// The place's own clock: Central European Time over most of Europe, the zone
// its longitude falls in elsewhere -- good enough for the sun, which is all it
// is for.
void clockOf(double lat, double lon, float& utc, bool& summer) {
    const bool europe = lat > 34.0 && lat < 72.0 && lon > -25.0 && lon < 45.0;
    if (europe) {
        utc = lon < -5.5 ? 0.0f : (lon > 22.5 && lat < 66.0) ? 2.0f : 1.0f;
        if (lon > 32.0) utc = 3.0f;
        summer = lon < 32.0;
        return;
    }
    utc = static_cast<float>(std::lround(lon / 15.0));
    summer = (lat > 25.0 && lon < -50.0 && lon > -170.0) || lat < -30.0;
}

} // namespace

// --- Names ------------------------------------------------------------------------------------

const char* useName(Use u) {
    static const char* n[] = {"house", "farmhouse", "terraced house", "semi-detached house", "bungalow",
                              "town villa", "cottage", "apartments", "commercial", "offices", "shops",
                              "industrial", "barn", "church", "school", "kindergarten", "hospital",
                              "fire station", "police", "town hall", "station", "supermarket", "hotel",
                              "museum", "library", "theatre", "cinema", "car park", "department store",
                              "garage", "shed", "other"};
    const int i = static_cast<int>(u);
    return i >= 0 && i < static_cast<int>(Use::Count) ? n[i] : "?";
}

const char* settlementName(Settlement s) {
    static const char* n[] = {"city", "town", "village", "hamlet", "farmstead"};
    const int i = static_cast<int>(s);
    return i >= 0 && i < static_cast<int>(Settlement::Count) ? n[i] : "?";
}

const char* buildName(Build b) {
    static const char* n[] = {"left out", "house", "terraced house", "block", "tower", "hall", "public building"};
    const int i = static_cast<int>(b);
    return i >= 0 && i < static_cast<int>(Build::Count) ? n[i] : "?";
}

const char* roadClassName(RoadClass c) {
    static const char* n[] = {"motorway", "trunk road", "primary", "secondary", "tertiary", "unclassified",
                              "residential", "living street", "pedestrian", "service", "track"};
    const int i = static_cast<int>(c);
    return i >= 0 && i < static_cast<int>(RoadClass::Count) ? n[i] : "?";
}

// --- What a square needs --------------------------------------------------------------------------

Need tilesFor(geo::LonLat c, float size) {
    Need n;
    n.osm = geo::tilesCovering(geo::squareAround(c, size), geo::kGeoZoom);
    // The ground reaches past the square: the blend into the land around it
    // happens there, not on the square's edge.
    n.dem = geo::tilesCovering(geo::squareAround(c, size + 1400.0), kDemZoom);
    n.coarse = geo::tilesCovering(geo::squareAround(c, kCoarseSize), kCoarseZoom);
    return n;
}

void addTile(AreaData& a, std::vector<geo::Feature>&& tile) {
    // Lines and buildings come once; an area's pieces (clipped per tile) all stay.
    std::set<std::uint64_t> have;
    for (const geo::Feature& f : a.features)
        if (f.geom != geo::Feature::Geom::Area || f.has("building")) have.insert(f.id);
    for (geo::Feature& f : tile) {
        const bool unique = f.geom != geo::Feature::Geom::Area || f.has("building");
        if (unique && !have.insert(f.id).second) continue;
        a.features.push_back(std::move(f));
    }
}

// --- The plan ---------------------------------------------------------------------------------------

void plan(const worldgen::Params& p, worldgen::Plan& out) {
    auto real = std::make_shared<Real>();
    Real& R = *real;
    Summary& S = R.summary;
    worldgen::Report& rep = out.report;
    if (!p.area || p.area->features.empty() && p.area->dem.empty()) {
        rep.ok = false;
        rep.why = "No map data yet: pick a square on the map and press Load.";
        out.real = real;
        return;
    }
    const AreaData& A = *p.area;
    const float size = glm::clamp(A.size, 300.0f, 8000.0f);
    const glm::vec2 C = p.center;                       // the square's centre in the world
    R.frame = geo::frameAt(A.center);
    auto W = [&](const geo::LonLat& ll) { return R.frame.toLocal(ll) + C; };
    auto LL = [&](glm::vec2 w) { return R.frame.toLonLat(w - C); };
    const glm::vec2 lo = C - glm::vec2(0.5f * size), hi = C + glm::vec2(0.5f * size);
    auto inSquare = [&](glm::vec2 q, float m = 0.0f) {
        return q.x >= lo.x - m && q.y >= lo.y - m && q.x <= hi.x + m && q.y <= hi.y + m;
    };

    // --- The ground ------------------------------------------------------------------------
    const geo::DemMosaic fineDem(A.dem), coarseDem(A.demCoarse);
    auto hf = std::make_shared<fitzel::TerrainHeightfield>();
    {
        char id[96];
        std::snprintf(id, sizeof(id), "geo_%.5f_%.5f_%.0f", A.center.lat, A.center.lon, size);
        hf->id = id;
    }
    auto ground = [&](glm::vec2 w, float& h) {
        const geo::LonLat ll = LL(w);
        if (fineDem.sample(ll, h)) return true;
        return coarseDem.sample(ll, h);
    };
    {
        const float side = size + 1200.0f;
        fitzel::TerrainHeightfield::Level& F = hf->fine;
        F.cell = glm::clamp(side / 1100.0f, 3.0f, 10.0f);
        F.nx = F.nz = static_cast<int>(std::ceil(side / F.cell)) + 1;
        F.x0 = C.x - 0.5f * F.cell * static_cast<float>(F.nx - 1);
        F.z0 = C.y - 0.5f * F.cell * static_cast<float>(F.nz - 1);
        F.h.assign(static_cast<std::size_t>(F.nx) * F.nz, 0.0f);
        int have = 0;
        for (int j = 0; j < F.nz; ++j)
            for (int i = 0; i < F.nx; ++i) {
                float h = 0.0f;
                if (ground({F.x0 + F.cell * i, F.z0 + F.cell * j}, h)) ++have;
                F.h[static_cast<std::size_t>(j) * F.nx + i] = h;
            }
        // One pass of a 3x3 box: the elevation model's pixels are a few metres
        // across and their steps would show as terraces on every slope.
        std::vector<float> b = F.h;
        for (int j = 1; j + 1 < F.nz; ++j)
            for (int i = 1; i + 1 < F.nx; ++i) {
                float s = 0.0f;
                for (int dj = -1; dj <= 1; ++dj)
                    for (int di = -1; di <= 1; ++di) s += F.h[static_cast<std::size_t>(j + dj) * F.nx + i + di];
                b[static_cast<std::size_t>(j) * F.nx + i] = s / 9.0f;
            }
        F.h.swap(b);
        if (have == 0) S.notes.push_back("No elevation data came: the ground is flat.");

        fitzel::TerrainHeightfield::Level& K = hf->coarse;
        if (!coarseDem.empty()) {
            K.cell = 40.0f;
            K.nx = K.nz = static_cast<int>(kCoarseSize / K.cell) + 1;
            K.x0 = C.x - 0.5f * K.cell * static_cast<float>(K.nx - 1);
            K.z0 = C.y - 0.5f * K.cell * static_cast<float>(K.nz - 1);
            K.h.assign(static_cast<std::size_t>(K.nx) * K.nz, 0.0f);
            for (int j = 0; j < K.nz; ++j)
                for (int i = 0; i < K.nx; ++i) {
                    float h = 0.0f;
                    const glm::vec2 w{K.x0 + K.cell * i, K.z0 + K.cell * j};
                    if (!coarseDem.sample(LL(w), h)) fineDem.sample(LL(w), h);
                    K.h[static_cast<std::size_t>(j) * K.nx + i] = h;
                }
        }
    }
    // The sea: the map draws a coastline, or much of the square is below it.
    bool coastline = false;
    for (const geo::Feature& f : A.features)
        if (f.is("natural", "coastline")) { coastline = true; break; }
    float absLo = 1e9f, absHi = -1e9f;
    int below = 0, inner = 0;
    {
        const fitzel::TerrainHeightfield::Level& F = hf->fine;
        for (int j = 0; j < F.nz; ++j)
            for (int i = 0; i < F.nx; ++i) {
                const glm::vec2 w{F.x0 + F.cell * i, F.z0 + F.cell * j};
                if (!inSquare(w)) continue;
                const float h = F.h[static_cast<std::size_t>(j) * F.nx + i];
                absLo = std::min(absLo, h);
                absHi = std::max(absHi, h);
                ++inner;
                if (h < -1.0f) ++below;
            }
    }
    if (inner == 0) { absLo = absHi = 0.0f; }
    const bool coast = coastline || (inner > 0 && below > inner / 20 && absLo < -4.0f);
    // No sea, but a lake filling the bottom of the square (Hallstatt, a reservoir
    // valley): the scene's one water level is the lake's. Its level is where the
    // elevation model meets its shore; its bed is sunk a little under that, so
    // the water covers it cleanly. Smaller lakes, and lakes up the slope, stay
    // dry basins (the scene has one water level).
    bool lake = false;
    float lakeLevel = 0.0f;
    if (!coast) {
        struct Lake { double area = 0.0; std::vector<float> shore; std::vector<std::vector<glm::vec2>> rings; std::string name; glm::vec2 lo{1e9f}, hi{-1e9f}; };
        std::map<std::uint64_t, Lake> lakes;
        for (const geo::Feature& f : A.features) {
            if (f.geom != geo::Feature::Geom::Area || landOf(f) != LWater) continue;
            const std::string wt = f.tagOr("water");
            if (wt == "river" || wt == "canal" || wt == "stream" || f.is("waterway", "riverbank")) continue;
            Lake& L = lakes[f.id];
            L.name = f.tagOr("name");
            for (std::size_t k = 0; k < f.rings.size(); ++k) {
                std::vector<glm::vec2> w;
                for (const geo::LonLat& q : f.rings[k]) {
                    w.push_back(W(q));
                    L.lo = glm::min(L.lo, w.back());
                    L.hi = glm::max(L.hi, w.back());
                    float h;
                    if (!(k < f.hole.size() && f.hole[k]) && fineDem.sample(q, h)) L.shore.push_back(h);
                }
                L.area += (k < f.hole.size() && f.hole[k] ? -1.0 : 1.0) * std::fabs(polyArea(w));
                L.rings.push_back(std::move(w));
            }
        }
        const Lake* best = nullptr;
        for (const auto& [id, L] : lakes)
            if (!L.shore.empty() && (!best || L.area > best->area)) best = &L;
        if (best && best->area > 0.03 * static_cast<double>(size) * size) {
            std::vector<float> sh = best->shore;
            std::sort(sh.begin(), sh.end());
            const float level = sh[sh.size() / 5];
            if (level <= absLo + 4.0f) {
                lake = true;
                lakeLevel = level;
                R.lakeName = best->name.empty() ? "the lake" : best->name;
                R.lakeFetch = std::max(500.0f, glm::length(best->hi - best->lo));
                fitzel::TerrainHeightfield::Level& F = hf->fine;
                Raster bed;
                bed.init({F.x0 - 0.5f * F.cell, F.z0 - 0.5f * F.cell}, F.cell * static_cast<float>(F.nx), F.cell);
                bed.fill(best->rings, [](std::uint8_t& c) { c = 1; });
                for (int j = 0; j < F.nz && j < bed.nz; ++j)
                    for (int i = 0; i < F.nx && i < bed.nx; ++i) {
                        float& h = F.h[static_cast<std::size_t>(j) * F.nx + i];
                        if (bed.v[static_cast<std::size_t>(j) * bed.nx + i] && h <= level + 1.5f) h = std::min(h, level - 2.0f);
                    }
                absLo = std::min(absLo, level - 2.0f);
            }
        }
    }
    const bool sea = coast || lake;
    const float waterAbs = coast ? 0.0f : lakeLevel + 0.3f;
    // Stored above the square's lowest point (a valley at 400 m reads as 0 m):
    // with a sea, as it is.
    const float offset = coast ? 0.0f : std::floor(absLo) - 2.0f;
    hf->elevationOffset = offset;
    for (float& h : hf->fine.h) h -= offset;
    for (float& h : hf->coarse.h) h -= offset;
    hf->blend = 250.0f;
    hf->fade  = 4000.0f;
    fitzel::registerHeightfield(hf);

    // --- The region, from the land -----------------------------------------------------------
    S.lowest = absLo; S.highest = absHi; S.relief = absHi - absLo; S.coast = coast;
    // The mean slope, on a 30 m step.
    {
        double sum = 0.0;
        int n = 0;
        for (float z = lo.y + 15.0f; z < hi.y; z += 30.0f)
            for (float x = lo.x + 15.0f; x < hi.x; x += 30.0f) {
                const float a = hf->fine.sample(x - 15.0f, z), b = hf->fine.sample(x + 15.0f, z);
                const float c = hf->fine.sample(x, z - 15.0f), d = hf->fine.sample(x, z + 15.0f);
                sum += std::sqrt((b - a) * (b - a) + (d - c) * (d - c)) / 30.0;
                ++n;
            }
        S.meanSlope = n ? static_cast<float>(sum / n) : 0.0f;
    }
    using worldgen::Region;
    Region region = Region::Uplands;
    char why[200];
    if (coast && S.relief > 350.0f && S.meanSlope > 0.22f) {
        region = Region::Fjord;
        std::snprintf(why, sizeof(why), "steep land (%.0f m, %.0f %%) falling into the sea", S.relief, S.meanSlope * 100.0f);
    } else if (coast) {
        region = Region::Coast;
        std::snprintf(why, sizeof(why), "on the sea, %.0f m of relief", S.relief);
    } else if (absHi > 1400.0f || S.relief > 650.0f || (S.relief > 300.0f && S.meanSlope > 0.25f)) {
        // High, or steep and deep: a valley under real mountains, even where the
        // square itself stays below the tree line.
        region = Region::Alpine;
        std::snprintf(why, sizeof(why), "%.0f m of relief, up to %.0f m above the sea", S.relief, absHi);
    } else if (S.relief < 70.0f && S.meanSlope < 0.035f) {
        region = Region::Lowland;
        std::snprintf(why, sizeof(why), "flat: %.0f m of relief, %.1f %% mean slope", S.relief, S.meanSlope * 100.0f);
    } else {
        region = Region::Uplands;
        std::snprintf(why, sizeof(why), "hills: %.0f m of relief, %.0f %% mean slope", S.relief, S.meanSlope * 100.0f);
    }
    S.region = region;
    S.regionWhy = why;
    R.region = region;

    // --- The land use, rasterised ------------------------------------------------------------
    const float margin = 300.0f;
    Raster land;
    land.init(lo - glm::vec2(margin), size + 2.0f * margin, 4.0f);
    std::vector<std::pair<int, const geo::Feature*>> areas;
    for (const geo::Feature& f : A.features) {
        if (f.geom != geo::Feature::Geom::Area || f.has("building")) continue;
        const Land l = landOf(f);
        if (l != LNone) areas.emplace_back(landRank(l), &f);
    }
    std::stable_sort(areas.begin(), areas.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    float needleArea = 0.0f, forestArea = 0.0f, taggedArea = 0.0f;
    int lakes = 0;
    for (const auto& [rank, fp] : areas) {
        const geo::Feature& f = *fp;
        const Land l = landOf(f);
        std::vector<std::vector<glm::vec2>> rings;
        for (const auto& r : f.rings) {
            std::vector<glm::vec2> w;
            w.reserve(r.size());
            for (const geo::LonLat& q : r) w.push_back(W(q));
            rings.push_back(std::move(w));
        }
        land.fill(rings, [&](std::uint8_t& c) { if (landRank(static_cast<Land>(c)) <= rank) c = static_cast<std::uint8_t>(l); });
        if (l == LForest) {
            float a = 0.0f;
            for (std::size_t k = 0; k < rings.size(); ++k) a += (f.hole.size() > k && f.hole[k] ? -1.0f : 1.0f) * std::fabs(polyArea(rings[k]));
            forestArea += a;
            const std::string lt = f.tagOr("leaf_type");
            if (lt == "needleleaved") needleArea += a;
            else if (lt == "mixed") needleArea += 0.5f * a;
            if (lt == "needleleaved" || lt == "mixed" || lt == "broadleaved") taggedArea += a;
        }
        if (l == LWater && f.tagOr("water") != "river" && !f.is("waterway", "riverbank") && f.tagOr("water") != "canal")
            ++lakes;
    }
    S.lakes = lakes;

    // --- Roads, joined into streets ------------------------------------------------------------
    struct Way { std::vector<glm::vec2> pts; std::vector<std::int64_t> keys; RoadClass cls; std::string name; bool oneway, bridge; float width; };
    std::vector<Way> ways;
    auto keyOf = [](const geo::LonLat& q) {
        return (static_cast<std::int64_t>(std::llround(q.lon * 1e7)) << 32) ^ static_cast<std::uint32_t>(std::llround(q.lat * 1e7));
    };
    std::unordered_map<std::int64_t, int> vertexUse;   // how many ways pass through a point
    for (const geo::Feature& f : A.features) {
        RoadClass cls;
        if (f.geom != geo::Feature::Geom::Line || !roadClassOf(f, cls) || f.rings.empty() || f.rings[0].size() < 2) continue;
        Way w;
        w.cls = cls;
        w.name = f.tagOr("name");
        w.oneway = f.is("oneway", "yes") || f.is("junction", "roundabout");
        w.bridge = f.is("bridge", "yes") || f.is("bridge", "viaduct");
        w.width = parseMetres(f.tagOr("width"));
        if (w.width <= 0.0f) {
            const int lanes = parseInt(f.tagOr("lanes"));
            if (lanes > 0) w.width = static_cast<float>(lanes) * 3.25f + (w.oneway ? 0.5f : 1.0f);
        }
        if (w.width <= 0.0f || w.width > 40.0f) w.width = classWidth(cls, w.oneway);
        for (const geo::LonLat& q : f.rings[0]) {
            w.pts.push_back(W(q));
            w.keys.push_back(keyOf(q));
        }
        std::set<std::int64_t> mine(w.keys.begin(), w.keys.end());
        for (std::int64_t k : mine) ++vertexUse[k];
        ways.push_back(std::move(w));
    }
    // Join ways end to end where exactly two of the same street meet.
    {
        std::unordered_map<std::int64_t, std::vector<int>> ends;
        for (int i = 0; i < static_cast<int>(ways.size()); ++i) {
            ends[ways[i].keys.front()].push_back(i);
            ends[ways[i].keys.back()].push_back(i);
        }
        std::vector<char> dead(ways.size(), 0);
        auto sameStreet = [&](const Way& a, const Way& b) { return a.cls == b.cls && a.name == b.name && a.bridge == b.bridge; };
        bool merged = true;
        while (merged) {
            merged = false;
            for (auto& [k, list] : ends) {
                std::vector<int> live;
                for (int i : list) if (!dead[i]) live.push_back(i);
                list = live;
                if (live.size() != 2 || live[0] == live[1]) continue;
                Way& a = ways[live[0]];
                Way& b = ways[live[1]];
                if (!sameStreet(a, b) || vertexUse[k] > 2) continue;
                // Orient: a ends at k, b starts at k.
                if (a.keys.front() == k) { std::reverse(a.pts.begin(), a.pts.end()); std::reverse(a.keys.begin(), a.keys.end()); }
                if (b.keys.back() == k) { std::reverse(b.pts.begin(), b.pts.end()); std::reverse(b.keys.begin(), b.keys.end()); }
                if (a.keys.back() != k || b.keys.front() != k) continue;
                a.pts.insert(a.pts.end(), b.pts.begin() + 1, b.pts.end());
                a.keys.insert(a.keys.end(), b.keys.begin() + 1, b.keys.end());
                a.width = std::max(a.width, b.width);
                a.oneway = a.oneway && b.oneway;
                dead[live[1]] = 1;
                ends[a.keys.back()].push_back(live[0]);
                merged = true;
                break;   // the map changed under the loop
            }
        }
        std::vector<Way> kept;
        for (std::size_t i = 0; i < ways.size(); ++i)
            if (!dead[i]) kept.push_back(std::move(ways[i]));
        ways.swap(kept);
    }

    // --- Buildings ---------------------------------------------------------------------------------
    std::vector<Building>& B = R.buildings;
    std::vector<const geo::Feature*> bf;
    for (const geo::Feature& f : A.features) {
        if (f.geom != geo::Feature::Geom::Area || !f.has("building") || f.rings.empty()) continue;
        const std::string t = f.tagOr("building");
        if (oneOf(t, {"no", "roof", "construction", "proposed", "demolished", "collapsed", "ruins", "disused", "abandoned"})) continue;
        // The biggest outer ring is the building (a multipolygon may have more).
        std::size_t best = 0;
        double ba = -1.0;
        for (std::size_t k = 0; k < f.rings.size(); ++k)
            if (!(k < f.hole.size() && f.hole[k])) {
                const double a = std::fabs(geo::ringAreaDeg(f.rings[k]));
                if (a > ba) { ba = a; best = k; }
            }
        Building b;
        b.id = f.id;
        for (const geo::LonLat& q : f.rings[best]) b.poly.push_back(W(q));
        if (b.poly.size() < 3) continue;
        b.area = std::fabs(polyArea(b.poly));
        if (b.area < 6.0f) continue;
        b.centre = polyCentre(b.poly);
        if (!inSquare(b.centre)) continue;
        minRect(b.poly, b.boxC, b.boxU, b.boxL, b.boxW);
        b.type = t;
        b.name = f.tagOr("name");
        b.levels = parseInt(f.tagOr("building:levels"));
        b.height = parseMetres(f.tagOr("height"));
        B.push_back(std::move(b));
        bf.push_back(&f);
    }
    S.buildings = static_cast<int>(B.size());
    Buckets bb;
    bb.cell = 40.0f;
    for (int i = 0; i < static_cast<int>(B.size()); ++i) {
        glm::vec2 a(1e9f), z(-1e9f);
        for (const glm::vec2& q : B[i].poly) { a = glm::min(a, q); z = glm::max(z, q); }
        bb.add(a, z, i);
    }
    // Coverage and party walls.
    for (int i = 0; i < static_cast<int>(B.size()); ++i) {
        Building& b = B[i];
        float built = 0.0f;
        std::set<int> seen;
        bb.near(b.centre, 60.0f, [&](int k) {
            if (!seen.insert(k).second) return;
            if (glm::length(B[k].centre - b.centre) <= 60.0f) built += B[k].area;
            if (k == i || b.attached) return;
            const float reach = 0.5f * (b.boxL + B[k].boxL) + 1.0f;
            if (glm::length(B[k].centre - b.centre) > reach) return;
            for (const glm::vec2& q : b.poly)
                if (polyDist(B[k].poly, q) < 0.5f) { b.attached = true; B[k].attached = true; break; }
        });
        b.coverage = std::min(1.0f, built / (3.14159f * 3600.0f));
    }
    // Settlements: buildings no further apart than a farm's yard, joined.
    std::vector<int> parent(B.size());
    std::iota(parent.begin(), parent.end(), 0);
    std::function<int(int)> root = [&](int x) { while (parent[x] != x) x = parent[x] = parent[parent[x]]; return x; };
    for (int i = 0; i < static_cast<int>(B.size()); ++i)
        bb.near(B[i].centre, 90.0f, [&](int k) {
            if (k <= i) return;
            const float gap = glm::length(B[k].centre - B[i].centre) - 0.35f * (B[i].boxL + B[k].boxL);
            if (gap < 55.0f) parent[root(k)] = root(i);
        });
    std::map<int, std::vector<int>> clusters;
    for (int i = 0; i < static_cast<int>(B.size()); ++i) clusters[root(i)].push_back(i);
    // Named places on the map, by rank.
    struct Named { std::string name; glm::vec2 at; int rank; int population; };
    std::vector<Named> named;
    for (const geo::Feature& f : A.features) {
        if (f.geom != geo::Feature::Geom::Point || !f.has("place") || !f.has("name")) continue;
        const std::string pl = f.tagOr("place");
        const int rank = pl == "city" ? 0 : pl == "town" ? 1 : pl == "village" ? 2 : pl == "suburb" ? 3 :
                         pl == "quarter" ? 4 : pl == "hamlet" ? 4 : pl == "neighbourhood" ? 5 : pl == "isolated_dwelling" ? 6 : 9;
        if (rank < 9) named.push_back({f.tagOr("name"), W(f.rings[0][0]), rank, parseInt(f.tagOr("population"))});
    }
    std::set<std::string> usedNames;
    for (auto& [rootId, members] : clusters) {
        Place pl;
        int main = 0;
        glm::vec2 c(0.0f);
        float density = 0.0f;
        pl.lo = glm::vec2(1e9f); pl.hi = glm::vec2(-1e9f);
        for (int i : members) {
            c += B[i].centre;
            density += B[i].coverage;
            if (B[i].area >= 40.0f) ++main;
            pl.lo = glm::min(pl.lo, B[i].centre);
            pl.hi = glm::max(pl.hi, B[i].centre);
        }
        c /= static_cast<float>(members.size());
        density /= static_cast<float>(members.size());
        for (int i : members) pl.radius = std::max(pl.radius, glm::length(B[i].centre - c));
        pl.centre = c;
        pl.buildings = main;
        pl.density = density;
        pl.kind = main >= 1200 ? Settlement::City : main >= 250 ? Settlement::Town :
                  main >= 30 ? Settlement::Village : main >= 6 ? Settlement::Hamlet : Settlement::Farmstead;
        if (pl.kind == Settlement::Village && density > 0.30f && main >= 120) pl.kind = Settlement::Town;
        const int idx = static_cast<int>(R.places.size());
        for (int i : members) B[i].settlement = idx;
        R.places.push_back(std::move(pl));
    }
    // Names, the biggest place first: a farm beside the market square must not
    // take the town's name. A place takes the most important named place within
    // its reach (the most people among equals), else an invented one.
    {
        std::vector<int> order(R.places.size());
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            return R.places[static_cast<std::size_t>(a)].buildings > R.places[static_cast<std::size_t>(b)].buildings;
        });
        int invented = 0;
        for (int i : order) {
            Place& pl = R.places[static_cast<std::size_t>(i)];
            const float reach = pl.kind == Settlement::Farmstead ? 150.0f : std::max(pl.radius, 350.0f) + 150.0f;
            const Named* best = nullptr;
            for (const Named& n : named) {
                if (usedNames.count(n.name)) continue;
                const float d = glm::length(n.at - pl.centre);
                if (d > reach) continue;
                if (!best || n.rank < best->rank ||
                    (n.rank == best->rank && (n.population > best->population ||
                                              (n.population == best->population && d < glm::length(best->at - pl.centre)))))
                    best = &n;
            }
            if (best) pl.name = best->name;
            else
                do pl.name = worldgen::placeName(p.seed, invented++);
                while (usedNames.count(pl.name) && invented < 1000);
            usedNames.insert(pl.name);
        }
    }
    S.places = 0;
    for (const Place& pl : R.places) S.places += pl.kind != Settlement::Farmstead ? 1 : 0;

    // --- What each building is, and what is built for it -------------------------------------------------
    using housegen::Preset;
    for (std::size_t bi = 0; bi < B.size(); ++bi) {
        Building& b = B[bi];
        const geo::Feature& f = *bf[bi];
        const std::string& t = b.type;
        const std::string am = f.tagOr("amenity"), shop = f.tagOr("shop"), tour = f.tagOr("tourism");
        const Land at = static_cast<Land>(land.at(b.centre));
        const Settlement st = b.settlement >= 0 ? R.places[static_cast<std::size_t>(b.settlement)].kind : Settlement::Farmstead;
        const bool urban = st == Settlement::City || st == Settlement::Town;
        const bool rural = !urban;
        const float ratio = b.boxW > 0.1f ? b.boxL / b.boxW : 1.0f;
        int civicKind = -1;
        Use u = Use::Other;
        // What the map says it is...
        if (oneOf(t, {"church", "cathedral", "chapel", "mosque", "synagogue", "temple", "religious", "monastery"}) || am == "place_of_worship")
            u = Use::Church;
        else if (oneOf(t, {"school", "university", "college"}) || oneOf(am, {"school", "university", "college"})) u = Use::School;
        else if (t == "kindergarten" || oneOf(am, {"kindergarten", "childcare"})) u = Use::Kindergarten;
        else if (t == "hospital" || am == "hospital" || f.is("healthcare", "hospital")) u = Use::Hospital;
        else if (t == "fire_station" || am == "fire_station") u = Use::FireStation;
        else if (am == "police") u = Use::Police;
        else if (t == "townhall" || am == "townhall") u = Use::TownHall;
        else if (t == "train_station" || f.is("railway", "station") || f.is("public_transport", "station")) u = Use::Station;
        else if (shop == "supermarket" || t == "supermarket") u = Use::Supermarket;
        else if (t == "hotel" || tour == "hotel") u = Use::Hotel;
        else if (am == "cinema") u = Use::Cinema;
        else if (am == "theatre") u = Use::Theatre;
        else if (am == "library") u = Use::Library;
        else if (tour == "museum") u = Use::Museum;
        else if (t == "parking" || (am == "parking" && b.levels > 1)) u = Use::ParkingGarage;
        else if (oneOf(shop, {"department_store", "mall"}) || (t == "retail" && b.area > 4000.0f)) u = Use::DepartmentStore;
        else if (oneOf(t, {"garage", "garages", "carport", "shed", "hut", "kiosk", "service", "transformer_tower",
                           "greenhouse", "container", "toilets", "bunker", "static_caravan", "boathouse", "shelter",
                           "tent", "bicycle_parking", "gazebo", "pavilion", "outbuilding", "allotment_house", "bridge"}))
            u = oneOf(t, {"garage", "garages", "carport"}) ? Use::Garage : Use::Shed;
        else if (oneOf(t, {"barn", "stable", "cowshed", "farm_auxiliary", "sty", "glasshouse", "slurry_tank"})) u = Use::Barn;
        else if (oneOf(t, {"industrial", "warehouse", "factory", "manufacture", "hangar", "storage_tank", "silo", "digester", "works"}))
            u = Use::Industrial;
        else if (t == "office" || f.has("office")) u = Use::Office;
        else if (t == "commercial") u = Use::Commercial;
        else if (oneOf(t, {"retail", "shop"}) || !shop.empty()) u = Use::Retail;
        else if (oneOf(t, {"apartments", "flats", "dormitory", "residential"})) u = Use::Apartments;
        else if (t == "terrace") u = Use::Terrace;
        else if (oneOf(t, {"semidetached_house", "semi"})) u = Use::SemiDetached;
        else if (t == "bungalow") u = Use::Bungalow;
        else if (t == "farm" || t == "farmhouse") u = Use::Farmhouse;
        else if (oneOf(t, {"house", "detached", "villa", "cabin", "dwelling_house", "yes", "residential"})) u = Use::House;
        if (am == "fuel") civicKind = static_cast<int>(civic::Kind::PetrolStation);
        // ...read in its neighbourhood: OSM's "yes" and "house" say little.
        if (u == Use::House && t == "yes") {
            if (b.area < 28.0f) u = Use::Shed;
            else if (at == LIndustrial && b.area > 250.0f) u = Use::Industrial;
            else if (at == LCommercial && b.area > 300.0f) u = Use::Retail;
            else if (at == LFarmyard && b.area > 260.0f) u = Use::Barn;
        }
        // A village's "apartments" or inn is still a village house: a big farm or
        // multi-generation house, not a slab of flats.
        bool bigHouse = false;
        if ((u == Use::Apartments || u == Use::Hotel) && rural && b.area <= 360.0f && b.levels <= 4) {
            u = Use::House;
            bigHouse = true;
        }
        if (u == Use::House || u == Use::Apartments) {
            if (b.area > 480.0f || b.levels >= 5) u = Use::Apartments;
            else if (u == Use::House && b.attached && urban && b.coverage > 0.32f)
                u = (b.levels >= 4 || b.area > 260.0f) ? Use::Apartments : Use::Terrace;
            else if (u == Use::House && (at == LFarmyard || (rural && b.area > 150.0f))) u = Use::Farmhouse;
            else if (u == Use::House && b.area < 65.0f) u = Use::Cottage;
            else if (u == Use::House && b.levels == 1) u = Use::Bungalow;
            else if (u == Use::House && urban && ratio < 1.25f && b.area > 115.0f && b.area < 270.0f && b.levels != 1)
                u = Use::Villa;
        }
        b.use = u;
        ++S.uses[static_cast<std::size_t>(u)];

        // --- What the editor builds for it ---------------------------------------------------------
        auto storeys = [&](int dflt) {
            if (b.levels > 0) return b.levels;
            if (b.height > 2.0f) return std::max(1, static_cast<int>(std::lround(b.height / 3.1f)));
            return dflt;
        };
        auto civicOf = [](Use x) -> int {
            switch (x) {
            case Use::Church:          return static_cast<int>(civic::Kind::Church);
            case Use::School:          return static_cast<int>(civic::Kind::School);
            case Use::Kindergarten:    return static_cast<int>(civic::Kind::Kindergarten);
            case Use::Hospital:        return static_cast<int>(civic::Kind::Hospital);
            case Use::FireStation:     return static_cast<int>(civic::Kind::FireStation);
            case Use::Police:          return static_cast<int>(civic::Kind::Police);
            case Use::TownHall:        return static_cast<int>(civic::Kind::TownHall);
            case Use::Station:         return static_cast<int>(civic::Kind::Station);
            case Use::Supermarket:     return static_cast<int>(civic::Kind::Supermarket);
            case Use::Hotel:           return static_cast<int>(civic::Kind::Hotel);
            case Use::Museum:          return static_cast<int>(civic::Kind::Museum);
            case Use::Library:         return static_cast<int>(civic::Kind::Library);
            case Use::Theatre:         return static_cast<int>(civic::Kind::Theatre);
            case Use::Cinema:          return static_cast<int>(civic::Kind::Cinema);
            case Use::ParkingGarage:   return static_cast<int>(civic::Kind::ParkingGarage);
            case Use::DepartmentStore: return static_cast<int>(civic::Kind::DepartmentStore);
            default:                   return -1;
            }
        };
        if (civicKind < 0) civicKind = civicOf(u);
        // A public building too small for its kind (the chapel of a cemetery, a
        // kiosk-sized "school" annex) is built as what it looks like instead.
        if (civicKind >= 0 && b.area < 90.0f) civicKind = -1;
        char buf[160];
        if (civicKind >= 0) {
            b.build = Build::Civic;
            b.civic = civicKind;
            std::snprintf(buf, sizeof(buf), "%s on the map", useName(u));
        } else if (u == Use::Garage || u == Use::Shed) {
            b.build = Build::Skip;
            std::snprintf(buf, sizeof(buf), "%s: nothing that small to build", useName(u));
        } else if (u == Use::Industrial || (u == Use::Barn && b.area > 380.0f)) {
            if (b.area >= 350.0f) {
                b.build = Build::Hall;
                b.civic = static_cast<int>(civic::Kind::Industry);
                std::snprintf(buf, sizeof(buf), "%s, %.0f m^2: a hall", useName(u), b.area);
            } else {
                b.build = Build::Block;
                b.floors = std::clamp(storeys(1), 1, 3);
                std::snprintf(buf, sizeof(buf), "small %s: a low block", useName(u));
            }
        } else if (u == Use::Apartments || u == Use::Commercial || u == Use::Office || u == Use::Retail) {
            const int dflt = st == Settlement::City ? (b.coverage > 0.4f ? 5 : 4) : st == Settlement::Town ? 3 : 2;
            b.floors = std::clamp(storeys(dflt), 1, 60);
            b.build = b.floors >= 10 ? Build::Tower : Build::Block;
            std::snprintf(buf, sizeof(buf), "%s, %d storeys%s", useName(u), b.floors, b.levels > 0 ? "" : " (guessed from the place)");
        } else if (u == Use::Terrace) {
            b.build = Build::Terraced;
            b.preset = static_cast<int>(Preset::Townhouse);
            b.floors = std::clamp(storeys(3), 1, 3);
            std::snprintf(buf, sizeof(buf), "%s: wall to wall with the next, %.0f %% built round it",
                          b.type == "terrace" ? "terrace on the map" : "attached in a dense street", b.coverage * 100.0f);
        } else {
            Preset pr = Preset::Classic;
            switch (u) {
            case Use::Farmhouse:    pr = b.area > 230.0f ? Preset::LargeFamily : Preset::CountryHouse; break;
            case Use::Barn:         pr = Preset::CountryHouse; break;
            case Use::SemiDetached: pr = Preset::SemiDetached; break;
            case Use::Bungalow:     pr = Preset::Bungalow; break;
            case Use::Villa:        pr = Preset::TownVilla; break;
            case Use::Cottage:      pr = Preset::Cottage; break;
            default:
                pr = b.area > 190.0f && b.levels != 1 ? Preset::MultiGen : rural ? Preset::CountryHouse : Preset::Classic;
                break;
            }
            if (bigHouse) pr = b.area > 200.0f ? Preset::LargeFamily : Preset::MultiGen;
            // Past what a family house can be: a block of its size instead.
            if (b.boxL > 22.0f || b.boxW > 16.0f) {
                b.build = Build::Block;
                b.floors = std::clamp(storeys(2), 1, 8);
                std::snprintf(buf, sizeof(buf), "%s too big for a family house (%.0f x %.0f m): a block",
                              useName(u), b.boxL, b.boxW);
            } else {
                b.build = Build::House;
                b.preset = static_cast<int>(pr);
                const int dfl = pr == Preset::Bungalow ? 1 : pr == Preset::TownVilla || pr == Preset::Classic ||
                                pr == Preset::MultiGen || pr == Preset::LargeFamily || pr == Preset::SemiDetached ? 2 : 1;
                b.floors = std::clamp(storeys(dfl), 1, 3);
                std::snprintf(buf, sizeof(buf), "%s, %.0f m^2 in a %s%s: %s", useName(u), b.area, settlementName(st),
                              b.attached ? " (attached)" : "", housegen::presetName(pr));
            }
        }
        b.why = buf;
        ++S.builds[static_cast<std::size_t>(b.build)];
        if (b.build == Build::House || b.build == Build::Terraced)
            ++S.housePresets[static_cast<std::size_t>(std::clamp(b.preset, 0, 15))];
    }

    // --- Roads: clipped, simplified, as wide as their street lets them be ------------------------------------
    {
        // Points where two streets meet stay put, whatever the simplification wants.
        std::unordered_map<std::int64_t, int> uses;
        for (const Way& w : ways) {
            std::set<std::int64_t> mine(w.keys.begin(), w.keys.end());
            for (std::int64_t k : mine) ++uses[k];
        }
        for (const Way& w : ways) {
            // Into runs inside the square (plus a little: a street runs on).
            const float m = 40.0f;
            std::vector<glm::vec2> run;
            std::vector<char> runKeep;
            auto flush = [&] {
                if (run.size() >= 2 && lineLength(run) > 12.0f) {
                    Road r;
                    r.pts = simplify(run, 0.9f, runKeep);
                    // Thin out what is closer than a spline wants between its
                    // control points -- but never a meeting point.
                    std::vector<glm::vec2> thin;
                    for (std::size_t i = 0; i < r.pts.size(); ++i) {
                        const bool end = i == 0 || i + 1 == r.pts.size();
                        if (!end && !thin.empty() && glm::length(r.pts[i] - thin.back()) < 3.0f) continue;
                        thin.push_back(r.pts[i]);
                    }
                    r.pts = thin;
                    r.cls = w.cls;
                    r.name = w.name;
                    r.bridge = w.bridge;
                    r.width = w.width;
                    if (r.pts.size() >= 2) R.roads.push_back(std::move(r));
                }
                run.clear();
                runKeep.clear();
            };
            for (std::size_t i = 0; i < w.pts.size(); ++i) {
                const bool in = inSquare(w.pts[i], m);
                if (in) {
                    if (run.empty() && i > 0) {
                        // Entering: from the border.
                        const glm::vec2 a = w.pts[i - 1], b = w.pts[i];
                        // Bisection for the crossing.
                        float l0 = 0.0f, l1 = 1.0f;
                        for (int s = 0; s < 20; ++s) {
                            const float t = 0.5f * (l0 + l1);
                            if (inSquare(glm::mix(a, b, t), m)) l1 = t; else l0 = t;
                        }
                        run.push_back(glm::mix(a, b, l1));
                        runKeep.push_back(1);
                    }
                    run.push_back(w.pts[i]);
                    runKeep.push_back(uses[w.keys[i]] > 1 ? 1 : 0);
                } else if (!run.empty()) {
                    const glm::vec2 a = w.pts[i - 1], b = w.pts[i];
                    float l0 = 0.0f, l1 = 1.0f;
                    for (int s = 0; s < 20; ++s) {
                        const float t = 0.5f * (l0 + l1);
                        if (inSquare(glm::mix(a, b, t), m)) l0 = t; else l1 = t;
                    }
                    run.push_back(glm::mix(a, b, l0));
                    runKeep.push_back(1);
                    flush();
                }
            }
            flush();
        }
        // As wide as the buildings either side leave room for.
        for (Road& r : R.roads) {
            std::vector<float> clear;
            for (std::size_t k = 1; k < r.pts.size(); ++k) {
                const glm::vec2 a = r.pts[k - 1], b = r.pts[k];
                const float L = glm::length(b - a);
                for (float s = 0.0f; s < L; s += 4.0f) {
                    const glm::vec2 q = glm::mix(a, b, s / std::max(L, 1e-3f));
                    float d = 40.0f;
                    bb.near(q, 20.0f, [&](int bi) { d = std::min(d, polyDist(B[bi].poly, q)); });
                    clear.push_back(d);
                }
            }
            if (!clear.empty()) {
                std::sort(clear.begin(), clear.end());
                const float p15 = clear[clear.size() * 15 / 100];
                const float room = 2.0f * (p15 - 0.4f);
                r.width = std::max(classMinWidth(r.cls), std::min(r.width, room));
            }
            ++S.roads[static_cast<std::size_t>(r.cls)];
            S.roadKm[static_cast<std::size_t>(r.cls)] += lineLength(r.pts) / 1000.0f;
        }
    }

    // --- Towns: the places big enough to be one, and the land between them ----------------------------------
    std::vector<int> placeTown(R.places.size(), -1);
    for (std::size_t i = 0; i < R.places.size(); ++i) {
        const Place& pl = R.places[i];
        if (pl.buildings < 12) continue;
        Town t;
        t.name = pl.name;
        t.kind = pl.kind;
        t.centre = 0.5f * (pl.lo + pl.hi);
        t.sizeX = std::max(80.0f, pl.hi.x - pl.lo.x + 80.0f);
        t.sizeZ = std::max(80.0f, pl.hi.y - pl.lo.y + 80.0f);
        t.preset = pl.kind == Settlement::City ? cityplan::Preset::City
                 : pl.kind == Settlement::Town ? cityplan::Preset::SmallTown : cityplan::Preset::Village;
        placeTown[i] = static_cast<int>(R.towns.size());
        R.towns.push_back(std::move(t));
    }
    const int landTown = static_cast<int>(R.towns.size());
    {
        Town t;
        t.name = "Umland";
        t.kind = Settlement::Farmstead;
        t.centre = C;
        t.sizeX = t.sizeZ = size;
        t.preset = cityplan::Preset::Village;
        t.land = true;
        R.towns.push_back(std::move(t));
    }
    // Which town a point belongs to: within 50 m of one of its buildings.
    Raster owner;
    owner.init(lo - glm::vec2(60.0f), size + 120.0f, 20.0f);
    for (const Building& b : B) {
        if (b.settlement < 0 || placeTown[static_cast<std::size_t>(b.settlement)] < 0) continue;
        const std::uint8_t id = static_cast<std::uint8_t>(std::min(placeTown[static_cast<std::size_t>(b.settlement)] + 1, 255));
        owner.stroke({b.centre, b.centre + glm::vec2(0.01f, 0.0f)}, 50.0f, [&](std::uint8_t& c) { if (!c) c = id; });
    }
    for (Road& r : R.roads) {
        std::map<int, int> votes;
        int n = 0;
        for (std::size_t k = 1; k < r.pts.size(); ++k) {
            const float L = glm::length(r.pts[k] - r.pts[k - 1]);
            for (float s = 0.0f; s < L; s += 10.0f) {
                ++votes[owner.at(glm::mix(r.pts[k - 1], r.pts[k], s / std::max(L, 1e-3f)))];
                ++n;
            }
        }
        int best = 0, bestN = -1;
        for (auto [id, c] : votes)
            if (id > 0 && c > bestN) { best = id; bestN = c; }
        if (best > 0 && bestN * 10 >= n * 3) {
            r.town = best - 1;
            r.rural = false;
        } else {
            r.town = landTown;
            r.rural = true;
        }
    }
    // Streets per town.
    std::vector<std::vector<int>> townRoads(R.towns.size());
    for (int i = 0; i < static_cast<int>(R.roads.size()); ++i) {
        const Road& r = R.roads[static_cast<std::size_t>(i)];
        Town& t = R.towns[static_cast<std::size_t>(r.town)];
        cityplan::Street s;
        s.pts = r.pts;
        s.width = r.width;
        s.avenue = !r.rural && (r.cls <= RoadClass::Secondary);
        s.name = r.name;
        s.cls = static_cast<int>(r.cls);
        s.rural = r.rural;
        s.leaves = 0;
        if (!inSquare(r.pts.front(), -30.0f)) s.leaves |= 1;
        if (!inSquare(r.pts.back(), -30.0f)) s.leaves |= 2;
        townRoads[static_cast<std::size_t>(r.town)].push_back(static_cast<int>(t.streets.size()));
        t.streets.push_back(std::move(s));
        (void)i;
    }
    // Every road segment, for "which way is the street".
    Buckets segs;
    segs.cell = 30.0f;
    std::vector<std::pair<int, int>> segOf;   // (road, segment)
    for (int ri = 0; ri < static_cast<int>(R.roads.size()); ++ri)
        for (int k = 1; k < static_cast<int>(R.roads[static_cast<std::size_t>(ri)].pts.size()); ++k) {
            const glm::vec2 a = R.roads[static_cast<std::size_t>(ri)].pts[static_cast<std::size_t>(k - 1)];
            const glm::vec2 b = R.roads[static_cast<std::size_t>(ri)].pts[static_cast<std::size_t>(k)];
            segs.add(glm::min(a, b), glm::max(a, b), static_cast<int>(segOf.size()));
            segOf.emplace_back(ri, k);
        }
    for (std::size_t bi = 0; bi < B.size(); ++bi) {
        const Building& b = B[bi];
        if (b.build == Build::Skip) continue;
        const int town = b.settlement >= 0 && placeTown[static_cast<std::size_t>(b.settlement)] >= 0
                             ? placeTown[static_cast<std::size_t>(b.settlement)] : landTown;
        // The street it faces: the nearest within reach.
        glm::vec2 near = b.boxC + glm::vec2(0.0f, -1.0f);
        float best = 1e9f;
        int bestRoad = -1;
        for (float reach : {60.0f, 200.0f}) {
            segs.near(b.boxC, reach, [&](int si) {
                const auto [ri, k] = segOf[static_cast<std::size_t>(si)];
                const Road& r = R.roads[static_cast<std::size_t>(ri)];
                glm::vec2 q;
                const float d = segDist(b.boxC, r.pts[static_cast<std::size_t>(k - 1)], r.pts[static_cast<std::size_t>(k)], &q);
                if (d < best) { best = d; near = q; bestRoad = ri; }
            });
            if (bestRoad >= 0) break;
        }
        glm::vec2 toStreet = near - b.boxC;
        if (glm::length(toStreet) < 1e-3f) toStreet = glm::vec2(0.0f, -1.0f);
        toStreet = glm::normalize(toStreet);
        // Snapped to a side of the box: the front is a wall, not a diagonal.
        const glm::vec2 u = b.boxU, v(-b.boxU.y, b.boxU.x);
        const float du = glm::dot(toStreet, u), dv = glm::dot(toStreet, v);
        cityplan::Lot lot;
        lot.pos = b.boxC;
        if (std::fabs(dv) >= std::fabs(du)) {
            lot.front = dv >= 0.0f ? v : -v;
            lot.width = b.boxL;   // along the street
            lot.depth = b.boxW;
        } else {
            lot.front = du >= 0.0f ? u : -u;
            lot.width = b.boxW;
            lot.depth = b.boxL;
        }
        lot.block = -1;
        lot.hash = mix32(b.id) | 1U;
        lot.floors = b.floors;
        switch (b.build) {
        case Build::House:
        case Build::Terraced: {
            lot.zone = b.build == Build::Terraced ? cityplan::Zone::Rows : cityplan::Zone::Houses;
            lot.kind = cityplan::realHouseKind(b.preset, b.floors, lot.width, lot.depth);
            break;
        }
        case Build::Block:  lot.zone = cityplan::Zone::Blocks; break;
        case Build::Tower:  lot.zone = cityplan::Zone::Towers; break;
        case Build::Hall:
        case Build::Civic:  lot.zone = cityplan::Zone::Houses; lot.civic = b.civic; break;
        default: break;
        }
        // The street within its own town it addresses (for its number).
        Town& t = R.towns[static_cast<std::size_t>(town)];
        float bs = 1e9f;
        for (int si : townRoads[static_cast<std::size_t>(town)]) {
            const cityplan::Street& s = t.streets[static_cast<std::size_t>(si)];
            for (std::size_t k = 1; k < s.pts.size(); ++k) {
                const float d = segDist(lot.pos, s.pts[k - 1], s.pts[k]);
                if (d < bs) { bs = d; lot.street = si; }
            }
        }
        if (bs > 80.0f) lot.street = -1;
        t.lots.push_back(lot);
    }

    // --- Rivers ---------------------------------------------------------------------------------------
    {
        struct Flow { std::vector<glm::vec2> pts; std::string name, kind; float width; std::int64_t a, b; };
        std::vector<Flow> flows;
        for (const geo::Feature& f : A.features) {
            if (f.geom != geo::Feature::Geom::Line || f.rings.empty() || f.rings[0].size() < 2) continue;
            const std::string ww = f.tagOr("waterway");
            if (!oneOf(ww, {"river", "canal", "stream"})) continue;
            const std::string tun = f.tagOr("tunnel");
            if (tun == "culvert" || tun == "yes") continue;
            Flow fl;
            for (const geo::LonLat& q : f.rings[0]) fl.pts.push_back(W(q));
            fl.name = f.tagOr("name");
            fl.kind = ww;
            fl.width = parseMetres(f.tagOr("width"));
            fl.a = keyOf(f.rings[0].front());
            fl.b = keyOf(f.rings[0].back());
            flows.push_back(std::move(fl));
        }
        // Downstream: a stretch whose end is the next one's start, same water.
        bool joined = true;
        while (joined) {
            joined = false;
            for (std::size_t i = 0; i < flows.size() && !joined; ++i)
                for (std::size_t k = 0; k < flows.size(); ++k) {
                    if (i == k || flows[i].b != flows[k].a || flows[i].kind != flows[k].kind || flows[i].name != flows[k].name) continue;
                    flows[i].pts.insert(flows[i].pts.end(), flows[k].pts.begin() + 1, flows[k].pts.end());
                    flows[i].b = flows[k].b;
                    flows[i].width = std::max(flows[i].width, flows[k].width);
                    flows.erase(flows.begin() + static_cast<std::ptrdiff_t>(k));
                    joined = true;
                    break;
                }
        }
        int brookNo = 0;
        for (Flow& fl : flows) {
            // The part in the square, as one run (a river leaving and coming back
            // is rare enough to take its longest stay).
            std::vector<glm::vec2> best, cur;
            for (const glm::vec2& q : fl.pts) {
                if (inSquare(q, 60.0f)) cur.push_back(q);
                else { if (lineLength(cur) > lineLength(best)) best = cur; cur.clear(); }
            }
            if (lineLength(cur) > lineLength(best)) best = cur;
            const float len = lineLength(best);
            const bool brook = fl.kind == "stream";
            if (best.size() < 2 || len < (brook ? 200.0f : 80.0f)) continue;
            // How wide the water on the map is, across the line.
            float width = fl.width;
            if (width <= 0.0f) {
                std::vector<float> ws;
                for (std::size_t k = 1; k < best.size(); ++k) {
                    const glm::vec2 d = glm::normalize(best[k] - best[k - 1] + glm::vec2(1e-6f));
                    const glm::vec2 n(-d.y, d.x), m = 0.5f * (best[k] + best[k - 1]);
                    if (land.at(m) != LWater) continue;
                    float a = 0.0f, b = 0.0f;
                    while (a < 250.0f && land.at(m + n * a) == LWater) a += 2.0f;
                    while (b < 250.0f && land.at(m - n * b) == LWater) b += 2.0f;
                    if (a < 250.0f && b < 250.0f) ws.push_back(a + b);
                }
                if (ws.size() >= 3) {
                    std::sort(ws.begin(), ws.end());
                    width = ws[ws.size() / 2];
                }
            }
            if (width <= 0.0f) width = fl.kind == "river" ? 14.0f : fl.kind == "canal" ? 12.0f : 2.5f;
            worldgen::River r;
            std::vector<char> keep;
            r.points = simplify(best, std::max(1.5f, 0.15f * width), keep);
            r.brook = brook;
            r.width = glm::clamp(width, 1.0f, 300.0f);
            r.preset = fl.kind == "canal" ? rivergen::Preset::Canal
                     : width >= 40.0f ? rivergen::Preset::WideRiver
                     : width >= 8.0f ? rivergen::Preset::River
                     : width >= 3.5f ? rivergen::Preset::Creek : rivergen::Preset::Brook;
            r.name = fl.name.empty() ? (brook ? "Bach " + std::to_string(++brookNo) : "Fluss") : fl.name;
            r.length = len;
            r.drop = hf->fine.sample(best.front().x, best.front().y) - hf->fine.sample(best.back().x, best.back().y);
            r.maxCut = worldgen::kMaxCut;
            out.rivers.push_back(std::move(r));
        }
        S.rivers = static_cast<int>(out.rivers.size());
    }

    // --- Trees -------------------------------------------------------------------------------------------
    {
        auto nearStreet = [&](glm::vec2 q) {
            bool yes = false;
            segs.near(q, 15.0f, [&](int si) {
                const auto [ri, k] = segOf[static_cast<std::size_t>(si)];
                const Road& r = R.roads[static_cast<std::size_t>(ri)];
                if (segDist(q, r.pts[static_cast<std::size_t>(k - 1)], r.pts[static_cast<std::size_t>(k)]) < 0.5f * r.width + 7.0f) yes = true;
            });
            return yes;
        };
        auto plant = [&](const geo::Feature& f, glm::vec2 q, std::uint32_t h) {
            if (!inSquare(q) || R.trees.size() >= 30000) return;
            Tree t;
            t.pos = q;
            t.species = speciesOf(f, h, nearStreet(q), land.at(q) == LPark || land.at(q) == LCemetery);
            const float mh = parseMetres(f.tagOr("height"));
            t.scale = mh > 1.0f ? glm::clamp(mh / speciesHeight(t.species), 0.4f, 1.6f) : 0.8f + 0.3f * unit(h ^ 0x9e37U);
            R.trees.push_back(std::move(t));
        };
        for (const geo::Feature& f : A.features) {
            if (f.geom == geo::Feature::Geom::Point && f.is("natural", "tree")) {
                plant(f, W(f.rings[0][0]), mix32(f.id));
            } else if (f.geom == geo::Feature::Geom::Line && f.is("natural", "tree_row") && !f.rings.empty()) {
                std::vector<glm::vec2> l;
                for (const geo::LonLat& q : f.rings[0]) l.push_back(W(q));
                std::uint32_t h = mix32(f.id);
                for (std::size_t k = 1; k < l.size(); ++k) {
                    const float L = glm::length(l[k] - l[k - 1]);
                    for (float s = 0.0f; s < L; s += 9.0f) plant(f, glm::mix(l[k - 1], l[k], s / std::max(L, 1e-3f)), h = mix32(h + 1));
                }
            }
        }
        S.trees = static_cast<int>(R.trees.size());
    }

    // --- The woods as the ecology's mask --------------------------------------------------------------------
    {
        auto mask = std::make_shared<ecology::Mask>();
        ecology::Mask& M = *mask;
        M.cell = 8.0f;
        const float side = size + 2.0f * 250.0f;
        M.n = std::min(1024, static_cast<int>(std::ceil(side / M.cell)) + 1);
        M.cell = side / static_cast<float>(M.n - 1);
        M.x0 = C.x - 0.5f * side;
        M.z0 = C.y - 0.5f * side;
        M.rgba.assign(static_cast<std::size_t>(M.n) * M.n * 4, 0);
        // Roads and buildings drawn into the land on top of the areas: no tree in
        // a street, none through a roof.
        Raster drawn = land;
        for (const Road& r : R.roads) drawn.stroke(r.pts, 0.5f * r.width + 1.5f, [](std::uint8_t& c) { c = LRoad; });
        for (const Building& b : B) drawn.fill({b.poly}, [](std::uint8_t& c) { c = LBuilding; });
        for (const worldgen::River& r : out.rivers) drawn.stroke(r.points, 0.5f * r.width + 1.0f, [](std::uint8_t& c) { c = LWater; });
        std::array<int, LCount> counts{};
        int inside = 0;
        for (int j = 0; j < M.n; ++j)
            for (int i = 0; i < M.n; ++i) {
                const glm::vec2 w{M.x0 + M.cell * i, M.z0 + M.cell * j};
                // The share of each kind of land in the texel, from the 4 m cells in it.
                float fr = 0.0f, st = 0.0f, fd = 0.0f;
                int n = 0;
                for (int dj = -1; dj <= 1; ++dj)
                    for (int di = -1; di <= 1; ++di) {
                        const Land l = static_cast<Land>(drawn.at(w + glm::vec2(di, dj) * (M.cell / 3.0f)));
                        ++n;
                        switch (l) {
                        case LForest:   fr += 1.0f; break;
                        case LScrub:    fr += 0.45f; break;
                        case LOrchard:  fr += 0.35f; st += 0.5f; break;
                        case LPark:     fr += 0.22f; st += 1.0f; break;
                        case LCemetery: fr += 0.28f; st += 1.0f; break;
                        case LWetland:  fr += 0.1f; fd += 0.5f; break;
                        case LResidential: case LCommercial: case LIndustrial: case LFarmyard: case LAllotments:
                            st += 1.0f; if (l == LIndustrial || l == LCommercial) fd += 0.6f; break;
                        case LFarmland: case LMeadow: case LRock: fd += 1.0f; break;
                        case LWater: case LRoad: case LBuilding: fd += 1.0f; st += 0.3f; break;
                        default: break;
                        }
                    }
                const float cover = inSquare(w, 250.0f) ? 1.0f - smooth01(-150.0f, 0.0f,
                    std::max(std::max(lo.x - 250.0f - w.x + 150.0f, w.x - hi.x - 250.0f + 150.0f),
                             std::max(lo.y - 250.0f - w.y + 150.0f, w.y - hi.y - 250.0f + 150.0f)) - 150.0f) : 0.0f;
                std::uint8_t* px = &M.rgba[(static_cast<std::size_t>(j) * M.n + i) * 4];
                px[0] = static_cast<std::uint8_t>(glm::clamp(fr / n, 0.0f, 1.0f) * 255.0f);
                px[1] = static_cast<std::uint8_t>(glm::clamp(st / n, 0.0f, 1.0f) * 255.0f);
                px[2] = static_cast<std::uint8_t>(glm::clamp(fd / n, 0.0f, 1.0f) * 255.0f);
                px[3] = static_cast<std::uint8_t>(glm::clamp(cover, 0.0f, 1.0f) * 255.0f);
                if (inSquare(w)) {
                    ++inside;
                    ++counts[static_cast<std::size_t>(drawn.at(w))];
                }
            }
        R.eco = mask;
        if (inside > 0) {
            S.forestShare = (counts[LForest] + 0.45f * counts[LScrub]) / static_cast<float>(inside);
            S.fieldShare = (counts[LFarmland] + counts[LMeadow]) / static_cast<float>(inside);
            S.builtShare = (counts[LBuilding] + counts[LRoad] + counts[LResidential] + counts[LCommercial] + counts[LIndustrial]) /
                           static_cast<float>(inside);
        }
        // Only from the woods that say what they are -- and only if enough do.
        S.needleShare = taggedArea > 1.0f && taggedArea > 0.2f * forestArea
                            ? glm::clamp(needleArea / taggedArea, 0.0f, 1.0f) : -1.0f;
    }

    // --- The look of the place -------------------------------------------------------------------------------
    worldgen::Character c = worldgen::character(region);
    clockOf(A.center.lat, A.center.lon, c.place.utcOffset, c.place.summerTime);
    c.place.latitude = static_cast<float>(A.center.lat);
    c.place.longitude = static_cast<float>(A.center.lon);
    c.place.day = p.look.place.day;
    c.place.legacy = false;
    c.forest = glm::clamp(0.15f + S.forestShare * 1.2f, 0.0f, 1.0f);
    c.towns = c.villages = c.farms = 0;
    c.seaShare = coast ? 0.1f : 0.0f;
    // The trees: the map's leaf types where it has them, else the region's mix,
    // conifers coming in with height. And whatever the single trees need.
    float needle = S.needleShare;
    if (needle < 0.0f) {
        switch (region) {
        case Region::Alpine: needle = 0.75f; break;
        case Region::Fjord:  needle = 0.8f; break;
        case Region::Coast:  needle = 0.45f; break;
        case Region::Lowland: needle = 0.2f; break;
        default: needle = 0.35f; break;
        }
        needle = glm::clamp(needle + (absLo - 300.0f) / 1500.0f, 0.05f, 0.95f);
    }
    c.trees.clear();
    const float broad = 1.0f - needle;
    c.trees.push_back({"Spruce", 0.65f * needle});
    c.trees.push_back({"Scots pine", 0.35f * needle});
    c.trees.push_back({"Beech", 0.45f * broad});
    c.trees.push_back({"Oak", 0.30f * broad});
    c.trees.push_back({"Birch", 0.15f * broad});
    c.trees.push_back({"Maple", 0.10f * broad});
    std::set<std::string> want;
    for (const Tree& t : R.trees) want.insert(t.species);
    for (const std::string& s : want) {
        bool have = false;
        for (const worldgen::Character::Tree& t : c.trees) have |= t.preset == s;
        if (!have) c.trees.push_back({s, 0.02f});
    }
    for (worldgen::Character::Tree& t : c.trees) t.share = std::max(t.share, 0.02f);
    // The roofs: the map's colour where enough buildings say one.
    {
        glm::vec3 sum(0.0f);
        int n = 0;
        for (std::size_t bi = 0; bi < B.size(); ++bi) {
            glm::vec3 col;
            if (colourOf(bf[bi]->tagOr("roof:colour"), col)) { sum += col; ++n; }
        }
        if (n >= 10 && n * 8 >= static_cast<int>(B.size())) c.roof = sum / static_cast<float>(n);
    }
    if (region == Region::Alpine && absHi + 200.0f > 1800.0f) c.treeLine = std::max(200.0f, 1850.0f - absLo);
    else c.treeLine = 0.0f;
    R.look = c;

    // --- The plan's own parts, as for a generated world ----------------------------------------------------------
    fitzel::TerrainSettings ts;
    {
        // The land past the measurements: gentle noise of the region's kind,
        // without its continents (the measured edge is the level it runs on at).
        const worldgen::Character& rc = worldgen::character(region);
        ts.heightScale = rc.heightScale * 0.5f;
        ts.ridgeScale = rc.ridgeScale * 0.5f;
        ts.continentAmp = 0.0f;
        ts.frequency = rc.frequency;
        ts.octaves = rc.octaves;
        ts.warpStrength = rc.warpStrength;
        ts.warpFrequency = rc.warpFrequency;
        ts.biomeFreq = rc.biomeFreq;
        ts.terrace = 0.0f;
        ts.valleyDepth = 0.0f;
        ts.seed = static_cast<float>(p.seed % 1000u) * 1.37f;
        ts.chunkSize = 64.0f;
        ts.resolution = 64;
        ts.heightfield = hf;
    }
    out.terrain = ts;
    out.sea = sea;
    out.seaLevel = sea ? waterAbs - offset : -1000.0f;
    out.treeLine = c.treeLine > 0.0f ? 1850.0f - offset : 0.0f;

    worldgen::Grid& g = out.grid;
    g.cell = glm::clamp(size / 256.0f, 6.0f, 24.0f);
    g.n = static_cast<int>(std::ceil(size / g.cell)) + 1;
    g.origin = C - glm::vec2(0.5f * size);
    const std::size_t gn = static_cast<std::size_t>(g.n) * g.n;
    g.height.resize(gn);
    g.slope.assign(gn, 0.0f);
    g.flow.assign(gn, 0.0f);
    g.use.assign(gn, worldgen::use::Free);
    for (int j = 0; j < g.n; ++j)
        for (int i = 0; i < g.n; ++i) {
            const glm::vec2 w = g.at(i, j);
            g.height[static_cast<std::size_t>(g.index(i, j))] = hf->fine.sample(w.x, w.y);
        }
    for (int j = 0; j < g.n; ++j)
        for (int i = 0; i < g.n; ++i) {
            auto H = [&](int a, int b) { return g.height[static_cast<std::size_t>(g.index(std::clamp(a, 0, g.n - 1), std::clamp(b, 0, g.n - 1)))]; };
            const float dx = (H(i + 1, j) - H(i - 1, j)) / (2.0f * g.cell), dz = (H(i, j + 1) - H(i, j - 1)) / (2.0f * g.cell);
            const std::size_t k = static_cast<std::size_t>(g.index(i, j));
            g.slope[k] = std::sqrt(dx * dx + dz * dz);
            const Land l = static_cast<Land>(land.at(g.at(i, j)));
            namespace u = worldgen::use;
            if (sea && g.height[k] < out.seaLevel) g.use[k] = u::Sea;
            else if (l == LWater) g.use[k] = u::River;
            else if (l == LResidential || l == LCommercial || l == LIndustrial || l == LFarmyard) g.use[k] = u::Town;
            else if (l == LFarmland) g.use[k] = u::Farm;
            else if (l == LRock || g.slope[k] > c.rockSlope) g.use[k] = u::Crag;
        }
    for (const Building& b : B) {
        int i, j;
        i = static_cast<int>(std::lround((b.centre.x - g.origin.x) / g.cell));
        j = static_cast<int>(std::lround((b.centre.y - g.origin.y) / g.cell));
        if (i >= 0 && j >= 0 && i < g.n && j < g.n) g.use[static_cast<std::size_t>(g.index(i, j))] = worldgen::use::Town;
    }
    for (const worldgen::River& r : out.rivers)
        for (std::size_t k = 1; k < r.points.size(); ++k) {
            const float L = glm::length(r.points[k] - r.points[k - 1]);
            for (float s = 0.0f; s < L; s += 0.5f * g.cell) {
                const glm::vec2 q = glm::mix(r.points[k - 1], r.points[k], s / std::max(L, 1e-3f));
                const int i = static_cast<int>(std::lround((q.x - g.origin.x) / g.cell));
                const int j = static_cast<int>(std::lround((q.y - g.origin.y) / g.cell));
                if (i >= 0 && j >= 0 && i < g.n && j < g.n) g.use[static_cast<std::size_t>(g.index(i, j))] = worldgen::use::River;
            }
        }
    // The horizon for the map's shade: the coarse ground.
    if (!hf->coarse.empty()) {
        worldgen::Horizon& hz = out.horizon;
        hz.n = hf->coarse.nx;
        hz.cell = hf->coarse.cell;
        hz.origin = {hf->coarse.x0, hf->coarse.z0};
        hz.height = hf->coarse.h;
        hz.top = *std::max_element(hz.height.begin(), hz.height.end());
    }
    rep.lowest = absLo - offset;
    rep.highest = absHi - offset;
    rep.seaLevel = out.seaLevel;
    for (const Town& t : R.towns)
        if (!t.land) (t.kind == Settlement::City || t.kind == Settlement::Town ? rep.towns : rep.villages)++;
    rep.roads = static_cast<int>(R.roads.size());
    for (const Road& r : R.roads) {
        rep.roadKm += lineLength(r.pts) / 1000.0f;
        if (r.bridge) ++rep.bridges;
    }
    for (const worldgen::River& r : out.rivers) {
        (r.brook ? rep.brooks : rep.rivers)++;
        rep.riverKm += r.length / 1000.0f;
    }
    rep.farms = 0;
    if (A.missing > 0) {
        S.notes.push_back(std::to_string(A.missing) + " of " + std::to_string(A.tiles) +
                          " map tiles could not be had: part of the square is empty.");
    }
    if (S.builds[static_cast<std::size_t>(Build::Skip)] > 0)
        S.notes.push_back(std::to_string(S.builds[static_cast<std::size_t>(Build::Skip)]) +
                          " garages, sheds and huts left out (nothing that small to build).");
    if (lake)
        S.notes.push_back(R.lakeName + " is the water level; the bed under it is sunk a little so the water covers it.");
    if (lakes - (lake ? 1 : 0) > 0)
        S.notes.push_back(std::to_string(lakes - (lake ? 1 : 0)) +
                          " other lake(s) and pond(s) on the map: their basins are in the ground, without water.");
    rep.ok = A.missing == 0;
    if (!S.notes.empty()) rep.why = S.notes.front();
    (void)landName;
    out.real = real;
}

} // namespace geoplan
