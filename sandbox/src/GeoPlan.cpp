#include "GeoPlan.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
// road meets this one: both have to pass through exactly that point). The
// indices of the points kept, in order.
std::vector<std::size_t> simplifyIdx(const std::vector<glm::vec2>& l, float tol, const std::vector<char>& keep) {
    std::vector<std::size_t> idx;
    if (l.size() < 3) {
        for (std::size_t i = 0; i < l.size(); ++i) idx.push_back(i);
        return idx;
    }
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
    for (std::size_t i = 0; i < l.size(); ++i)
        if (use[i]) idx.push_back(i);
    return idx;
}

std::vector<glm::vec2> simplify(const std::vector<glm::vec2>& l, float tol, const std::vector<char>& keep) {
    std::vector<glm::vec2> out;
    for (std::size_t i : simplifyIdx(l, tol, keep)) out.push_back(l[i]);
    return out;
}

// Where segments ab and cd cross: t along ab, u along cd (false if they do not).
bool segCross(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec2 d, float& t, float& u) {
    const glm::vec2 r = b - a, q = d - c;
    const float den = cross2(r, q);
    if (std::fabs(den) < 1e-9f) return false;
    t = cross2(c - a, q) / den;
    u = cross2(c - a, r) / den;
    return t >= 0.0f && t <= 1.0f && u >= 0.0f && u <= 1.0f;
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
    return true;
}

// A road under something: a tunnel through a hill, or the short underpass where
// it passes under a railway embankment (the caller tells the two apart).
bool roadTunnel(const geo::Feature& f) {
    const std::string tun = f.tagOr("tunnel");
    return tun == "yes" || tun == "building_passage" || tun == "culvert";
}

// --- Railways --------------------------------------------------------------------------------------

// What a railway way is laid as -- false for what has no track to lay: an
// abandoned line (the rails are gone), one only planned or being built, a
// platform. A disused line still has its rails, rusting.
bool railClassOf(const geo::Feature& f, RailClass& c) {
    const std::string* r = f.tag("railway");
    if (!r || f.is("area", "yes")) return false;
    const bool yard = oneOf(f.tagOr("service"), {"siding", "yard", "spur", "crossover"});
    if (*r == "rail" || *r == "preserved") c = yard ? RailClass::Siding : RailClass::Main;
    else if (*r == "light_rail" || *r == "subway") c = yard ? RailClass::Siding : RailClass::Light;
    else if (*r == "narrow_gauge") c = RailClass::Narrow;
    else if (*r == "tram") c = RailClass::Tram;
    else if (*r == "disused") c = RailClass::Disused;
    else return false;
    return true;
}

// How steep a line may climb, and half the width of its formation (the level
// top of the embankment the ballast lies on).
float railGrade(RailClass c) {
    return c == RailClass::Light || c == RailClass::Narrow ? 0.04f : 0.025f;
}
float railHalfFormation(RailClass c) {
    return c == RailClass::Narrow ? 2.4f : 3.2f;
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

const char* railClassName(RailClass c) {
    static const char* n[] = {"main line", "light rail", "narrow gauge", "siding", "disused", "tram"};
    const int i = static_cast<int>(c);
    return i >= 0 && i < static_cast<int>(RailClass::Count) ? n[i] : "?";
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
    {
        const float side = size + 1200.0f;
        fitzel::TerrainHeightfield::Level& F = hf->fine;
        F.cell = glm::clamp(side / 1100.0f, 3.0f, 10.0f);
        F.nx = F.nz = static_cast<int>(std::ceil(side / F.cell)) + 1;
        F.x0 = C.x - 0.5f * F.cell * static_cast<float>(F.nx - 1);
        F.z0 = C.y - 0.5f * F.cell * static_cast<float>(F.nz - 1);
        F.h.assign(static_cast<std::size_t>(F.nx) * F.nz, 0.0f);
        int have = 0, fine = 0;
        std::vector<char> got(F.h.size(), 0);
        for (int j = 0; j < F.nz; ++j)
            for (int i = 0; i < F.nx; ++i) {
                float h = 0.0f;
                const glm::vec2 w{F.x0 + F.cell * i, F.z0 + F.cell * j};
                if (fineDem.sample(LL(w), h)) ++fine;
                else if (!coarseDem.sample(LL(w), h)) continue;
                ++have;
                got[static_cast<std::size_t>(j) * F.nx + i] = 1;
                F.h[static_cast<std::size_t>(j) * F.nx + i] = h;
            }
        // A tile that did not come is a hole, not a pit down to sea level: its
        // cells take the ground beside them, along the row, then down the column.
        if (have > 0 && have < static_cast<int>(F.h.size())) {
            auto fillLine = [&](std::size_t start, std::size_t step, int n) {
                int last = -1;
                for (int k = 0; k < n; ++k) {
                    const std::size_t c = start + step * static_cast<std::size_t>(k);
                    if (!got[c]) continue;
                    const float hv = F.h[c];
                    for (int m = last + 1; m < k; ++m) {
                        const std::size_t cm = start + step * static_cast<std::size_t>(m);
                        F.h[cm] = last < 0 ? hv : glm::mix(F.h[start + step * static_cast<std::size_t>(last)], hv,
                                                            static_cast<float>(m - last) / static_cast<float>(k - last));
                        got[cm] = 2;
                    }
                    last = k;
                }
                if (last >= 0)
                    for (int m = last + 1; m < n; ++m) {
                        F.h[start + step * static_cast<std::size_t>(m)] = F.h[start + step * static_cast<std::size_t>(last)];
                        got[start + step * static_cast<std::size_t>(m)] = 2;
                    }
            };
            for (int j = 0; j < F.nz; ++j) fillLine(static_cast<std::size_t>(j) * F.nx, 1, F.nx);
            for (int i = 0; i < F.nx; ++i) fillLine(static_cast<std::size_t>(i), static_cast<std::size_t>(F.nx), F.nz);
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
        if (have == 0) S.notes.push_back("No elevation data came: the ground is flat. Load the square again.");
        else if (fine * 10 < static_cast<int>(F.h.size()) * 9)
            S.notes.push_back("Part of the square's own elevation tiles did not come: the ground there is the coarse "
                              "one round it. Load the square again.");

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

    auto keyOf = [](const geo::LonLat& q) {
        return (static_cast<std::int64_t>(std::llround(q.lon * 1e7)) << 32) ^ static_cast<std::uint32_t>(std::llround(q.lat * 1e7));
    };

    // --- Railways, as the map draws them: one way per track ------------------------------------
    // Joined end to end where exactly two ways of a kind meet (not at a switch,
    // where a third leaves), each segment remembering whether it is on the
    // ground, on a bridge or in a tunnel.
    enum : std::uint8_t { kGround = 0, kBridge = 1, kTunnel = 2 };
    struct RailWay { std::vector<glm::vec2> pts; std::vector<std::int64_t> keys; std::vector<std::uint8_t> seg; RailClass cls; std::string name; };
    std::vector<RailWay> railWays;
    {
        std::unordered_map<std::int64_t, int> railUse;
        for (const geo::Feature& f : A.features) {
            RailClass cls;
            if (f.geom != geo::Feature::Geom::Line || !railClassOf(f, cls) || f.rings.empty() || f.rings[0].size() < 2) continue;
            RailWay w;
            w.cls = cls;
            w.name = f.tagOr("name");
            // A line the map knows only by its number (the ref of its route).
            if (w.name.empty() && f.has("ref")) w.name = "Strecke " + f.tagOr("ref");
            const std::string tun = f.tagOr("tunnel"), br = f.tagOr("bridge");
            // Through a building or under a gallery the track is still on the ground.
            const bool tunnel = !tun.empty() && tun != "no" && tun != "building_passage" && tun != "avalanche_protector";
            const std::uint8_t kind = tunnel ? kTunnel : (!br.empty() && br != "no") ? kBridge : kGround;
            for (const geo::LonLat& q : f.rings[0]) {
                w.pts.push_back(W(q));
                w.keys.push_back(keyOf(q));
            }
            w.seg.assign(w.pts.size() - 1, kind);
            std::set<std::int64_t> mine(w.keys.begin(), w.keys.end());
            for (std::int64_t k : mine) ++railUse[k];
            railWays.push_back(std::move(w));
        }
        std::unordered_map<std::int64_t, std::vector<int>> ends;
        for (int i = 0; i < static_cast<int>(railWays.size()); ++i) {
            ends[railWays[static_cast<std::size_t>(i)].keys.front()].push_back(i);
            ends[railWays[static_cast<std::size_t>(i)].keys.back()].push_back(i);
        }
        std::vector<char> dead(railWays.size(), 0);
        auto flip = [](RailWay& w) {
            std::reverse(w.pts.begin(), w.pts.end());
            std::reverse(w.keys.begin(), w.keys.end());
            std::reverse(w.seg.begin(), w.seg.end());
        };
        bool merged = true;
        while (merged) {
            merged = false;
            for (auto& [k, list] : ends) {
                std::vector<int> live;
                for (int i : list) if (!dead[static_cast<std::size_t>(i)]) live.push_back(i);
                list = live;
                if (live.size() != 2 || live[0] == live[1] || railUse[k] > 2) continue;
                RailWay& a = railWays[static_cast<std::size_t>(live[0])];
                RailWay& b = railWays[static_cast<std::size_t>(live[1])];
                if (a.cls != b.cls) continue;
                if (a.keys.front() == k) flip(a);
                if (b.keys.back() == k) flip(b);
                if (a.keys.back() != k || b.keys.front() != k) continue;
                a.pts.insert(a.pts.end(), b.pts.begin() + 1, b.pts.end());
                a.keys.insert(a.keys.end(), b.keys.begin() + 1, b.keys.end());
                a.seg.insert(a.seg.end(), b.seg.begin(), b.seg.end());
                if (a.name.empty()) a.name = b.name;
                dead[static_cast<std::size_t>(live[1])] = 1;
                ends[a.keys.back()].push_back(live[0]);
                merged = true;
                break;
            }
        }
        std::vector<RailWay> kept;
        for (std::size_t i = 0; i < railWays.size(); ++i)
            if (!dead[i]) kept.push_back(std::move(railWays[i]));
        railWays.swap(kept);
    }
    // Every track segment that is not underground, for "does this road cross a
    // railway" -- in the buckets by index into railSegs.
    struct RailSeg { glm::vec2 a, b; int way, k; };
    std::vector<RailSeg> railSegs;
    Buckets railB;
    railB.cell = 40.0f;
    for (int wi = 0; wi < static_cast<int>(railWays.size()); ++wi) {
        const RailWay& w = railWays[static_cast<std::size_t>(wi)];
        if (w.cls == RailClass::Tram) continue;   // in the street: crossings are junctions
        for (int k = 0; k + 1 < static_cast<int>(w.pts.size()); ++k) {
            if (w.seg[static_cast<std::size_t>(k)] == kTunnel) continue;
            const glm::vec2 a = w.pts[static_cast<std::size_t>(k)], b = w.pts[static_cast<std::size_t>(k + 1)];
            railB.add(glm::min(a, b), glm::max(a, b), static_cast<int>(railSegs.size()));
            railSegs.push_back({a, b, wi, k});
        }
    }
    // Does the polyline cross a track? Where (on the line and on the track).
    auto crossesRail = [&](glm::vec2 a, glm::vec2 b, glm::vec2* at = nullptr) {
        bool hit = false;
        railB.near(0.5f * (a + b), 0.5f * glm::length(b - a) + 1.0f, [&](int si) {
            if (hit) return;
            const RailSeg& r = railSegs[static_cast<std::size_t>(si)];
            float t, u;
            if (segCross(a, b, r.a, r.b, t, u)) {
                hit = true;
                if (at) *at = glm::mix(a, b, t);
            }
        });
        return hit;
    };

    // --- Roads, joined into streets ------------------------------------------------------------
    // A road's segments remember whether they are on a bridge: a bridge joins
    // the street it carries, and where it crosses a railway it becomes a deck.
    struct Way { std::vector<glm::vec2> pts; std::vector<std::int64_t> keys; std::vector<std::uint8_t> br; RoadClass cls; std::string name; bool oneway, bridge; float width; };
    std::vector<Way> ways;
    // Where a road passes under a track in a short tunnel: the track is
    // bridged over it there, the road kept at grade (half its width with it).
    std::vector<std::pair<glm::vec2, float>> underpasses;
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
        if (roadTunnel(f)) {
            // A tunnel is left out -- unless it is the short underpass of a
            // railway embankment, which is a gap in the embankment, not a hole
            // in a hill: the road stays, the track crosses it on a bridge.
            glm::vec2 at;
            bool under = false;
            for (std::size_t k = 1; k < w.pts.size() && !under; ++k) under = crossesRail(w.pts[k - 1], w.pts[k], &at);
            if (!under || lineLength(w.pts) > 120.0f) continue;
            underpasses.emplace_back(at, 0.5f * w.width);
        }
        w.br.assign(w.pts.size() - 1, w.bridge ? 1 : 0);
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
        auto sameStreet = [&](const Way& a, const Way& b) { return a.cls == b.cls && a.name == b.name; };
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
                if (a.keys.front() == k) {
                    std::reverse(a.pts.begin(), a.pts.end()); std::reverse(a.keys.begin(), a.keys.end()); std::reverse(a.br.begin(), a.br.end());
                }
                if (b.keys.back() == k) {
                    std::reverse(b.pts.begin(), b.pts.end()); std::reverse(b.keys.begin(), b.keys.end()); std::reverse(b.br.begin(), b.br.end());
                }
                if (a.keys.back() != k || b.keys.front() != k) continue;
                a.pts.insert(a.pts.end(), b.pts.begin() + 1, b.pts.end());
                a.keys.insert(a.keys.end(), b.keys.begin() + 1, b.keys.end());
                a.br.insert(a.br.end(), b.br.begin(), b.br.end());
                a.bridge = a.bridge || b.bridge;
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
            std::vector<std::uint8_t> runBr;   // per segment of the run
            auto flush = [&] {
                if (run.size() >= 2 && lineLength(run) > 12.0f) {
                    Road r;
                    const std::vector<std::size_t> idx = simplifyIdx(run, 0.9f, runKeep);
                    // Thin out what is closer than a spline wants between its
                    // control points -- but never a meeting point, nor where a
                    // bridge begins or ends.
                    std::vector<std::size_t> thin;
                    for (std::size_t i = 0; i < idx.size(); ++i) {
                        const bool end = i == 0 || i + 1 == idx.size();
                        const std::size_t o = idx[i];
                        const bool edge = o > 0 && o + 1 < run.size() && runBr[o - 1] != runBr[o];
                        if (!end && !edge && !thin.empty() && glm::length(run[o] - run[thin.back()]) < 3.0f) continue;
                        thin.push_back(o);
                    }
                    for (std::size_t o : thin) r.pts.push_back(run[o]);
                    // Its bridges over a railway, as decks.
                    for (std::size_t i = 0; i + 1 < thin.size();) {
                        if (!runBr[thin[i]]) { ++i; continue; }
                        std::size_t e = i + 1;
                        while (e + 1 < thin.size() && runBr[thin[e]]) ++e;
                        bool overRail = false;
                        for (std::size_t k = i + 1; k <= e && !overRail; ++k) overRail = crossesRail(r.pts[k - 1], r.pts[k]);
                        if (overRail) r.decks.emplace_back(static_cast<int>(i), static_cast<int>(e));
                        i = e;
                    }
                    r.cls = w.cls;
                    r.name = w.name;
                    r.bridge = w.bridge;
                    r.width = w.width;
                    if (r.pts.size() >= 2) R.roads.push_back(std::move(r));
                }
                run.clear();
                runKeep.clear();
                runBr.clear();
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
                    if (!run.empty()) runBr.push_back(w.br[i - 1]);
                    run.push_back(w.pts[i]);
                    const bool edge = i > 0 && i + 1 < w.pts.size() && w.br[i - 1] != w.br[i];
                    runKeep.push_back(uses[w.keys[i]] > 1 || edge ? 1 : 0);
                } else if (!run.empty()) {
                    const glm::vec2 a = w.pts[i - 1], b = w.pts[i];
                    float l0 = 0.0f, l1 = 1.0f;
                    for (int s = 0; s < 20; ++s) {
                        const float t = 0.5f * (l0 + l1);
                        if (inSquare(glm::mix(a, b, t), m)) l0 = t; else l1 = t;
                    }
                    runBr.push_back(w.br[i - 1]);
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
        s.decks = r.decks;
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

    // --- Railways: the level of each line, its formation in the ground, its bridges -----------------
    // A railway does not follow the land: it runs on a smooth line no steeper
    // than its trains climb, banked up where the land falls away and cut into it
    // where it rises -- and clears whatever it bridges and whatever bridges it.
    // Each line is levelled (the main lines first, the sidings joining them at
    // the level they found), then the ground is graded to the levels: a flat
    // formation under the track, 1:1.5 slopes down or up to the land beside it,
    // steep walls where a road bridge stands over the cutting. Under a bridge
    // and over a tunnel the land is left as it is.
    {
        fitzel::TerrainHeightfield::Level& F = hf->fine;
        const fitzel::TerrainHeightfield::Level orig = F;   // the land before any track
        struct Line {
            std::vector<glm::vec2> q;          // stations, at most 4 m apart
            std::vector<float> s;              // their distance along the line
            std::vector<std::uint8_t> seg;     // per stretch q[i] -> q[i+1]: ground, bridge, tunnel
            std::vector<float> lvl;            // the formation at each station
            std::vector<std::pair<int, int>> decks;   // bridges with their approaches (stations)
            RailClass cls = RailClass::Main;
            std::string name;
            int way = -1;
        };
        std::vector<Line> lines;
        auto addRun = [&](int way, const std::vector<glm::vec2>& pts, const std::vector<std::uint8_t>& seg) {
            if (pts.size() < 2 || lineLength(pts) < 15.0f) return;
            const RailWay& w = railWays[static_cast<std::size_t>(way)];
            Line L;
            L.cls = w.cls;
            L.name = w.name;
            L.way = way;
            L.q.push_back(pts[0]);
            L.s.push_back(0.0f);
            for (std::size_t k = 0; k + 1 < pts.size(); ++k) {
                const float len = glm::length(pts[k + 1] - pts[k]);
                const int n = std::max(1, static_cast<int>(std::ceil(len / 4.0f)));
                for (int j = 1; j <= n; ++j) {
                    L.q.push_back(glm::mix(pts[k], pts[k + 1], static_cast<float>(j) / static_cast<float>(n)));
                    L.s.push_back(L.s.back() + len / static_cast<float>(n));
                    L.seg.push_back(seg[k]);
                }
            }
            lines.push_back(std::move(L));
        };
        // Cut to the square (and a little past it, as the streets are).
        for (int wi = 0; wi < static_cast<int>(railWays.size()); ++wi) {
            const RailWay& w = railWays[static_cast<std::size_t>(wi)];
            const float m = 40.0f;
            std::vector<glm::vec2> run;
            std::vector<std::uint8_t> seg;
            auto cut = [&](glm::vec2 a, glm::vec2 b, bool aIn) {
                float l0 = 0.0f, l1 = 1.0f;
                for (int it = 0; it < 20; ++it) {
                    const float t = 0.5f * (l0 + l1);
                    if (inSquare(glm::mix(a, b, t), m) == aIn) l0 = t; else l1 = t;
                }
                return glm::mix(a, b, aIn ? l0 : l1);
            };
            for (std::size_t i = 0; i < w.pts.size(); ++i) {
                const bool in = inSquare(w.pts[i], m);
                if (in) {
                    if (run.empty() && i > 0) run.push_back(cut(w.pts[i - 1], w.pts[i], false));
                    if (!run.empty()) seg.push_back(w.seg[i - 1]);
                    run.push_back(w.pts[i]);
                } else if (!run.empty()) {
                    seg.push_back(w.seg[i - 1]);
                    run.push_back(cut(w.pts[i - 1], w.pts[i], true));
                    addRun(wi, run, seg);
                    run.clear();
                    seg.clear();
                }
            }
            addRun(wi, run, seg);
        }

        // Every stretch of every line, for what crosses what.
        Buckets lineB;
        lineB.cell = 40.0f;
        std::vector<std::pair<int, int>> lineSeg;   // (line, stretch)
        for (int li = 0; li < static_cast<int>(lines.size()); ++li)
            for (int k = 0; k + 1 < static_cast<int>(lines[static_cast<std::size_t>(li)].q.size()); ++k) {
                const glm::vec2 a = lines[static_cast<std::size_t>(li)].q[static_cast<std::size_t>(k)];
                const glm::vec2 b = lines[static_cast<std::size_t>(li)].q[static_cast<std::size_t>(k + 1)];
                lineB.add(glm::min(a, b), glm::max(a, b), static_cast<int>(lineSeg.size()));
                lineSeg.emplace_back(li, k);
            }
        // Bridge a line over a stretch `half` metres either side of where it
        // crosses something the map gives no bridge for: a road in a short
        // tunnel under it, a river under it.
        auto bridgeAt = [&](int li, int k, glm::vec2 at, float half) {
            Line& L = lines[static_cast<std::size_t>(li)];
            const float s0 = L.s[static_cast<std::size_t>(k)] + glm::length(at - L.q[static_cast<std::size_t>(k)]);
            for (std::size_t j = 0; j + 1 < L.q.size(); ++j) {
                const float mid = 0.5f * (L.s[j] + L.s[j + 1]);
                if (std::fabs(mid - s0) <= half + 2.0f && L.seg[j] == kGround) L.seg[j] = kBridge;
            }
        };
        for (const auto& [at, half] : underpasses) {
            float best = 8.0f;
            int bl = -1, bk = -1;
            lineB.near(at, 8.0f, [&](int si) {
                const auto [li, k] = lineSeg[static_cast<std::size_t>(si)];
                if (lines[static_cast<std::size_t>(li)].cls == RailClass::Tram) return;
                const float d = segDist(at, lines[static_cast<std::size_t>(li)].q[static_cast<std::size_t>(k)],
                                        lines[static_cast<std::size_t>(li)].q[static_cast<std::size_t>(k + 1)]);
                if (d < best) { best = d; bl = li; bk = k; }
            });
            // The road and its verges pass under, with room to spare.
            if (bl >= 0) bridgeAt(bl, bk, at, half + 5.5f);
        }
        for (const worldgen::River& rv : out.rivers)
            for (std::size_t k = 1; k < rv.points.size(); ++k) {
                const glm::vec2 a = rv.points[k - 1], b = rv.points[k];
                lineB.near(0.5f * (a + b), 0.5f * glm::length(b - a) + 1.0f, [&](int si) {
                    const auto [li, j] = lineSeg[static_cast<std::size_t>(si)];
                    Line& L = lines[static_cast<std::size_t>(li)];
                    if (L.cls == RailClass::Tram || L.seg[static_cast<std::size_t>(j)] != kGround) return;
                    float t, u;
                    if (segCross(L.q[static_cast<std::size_t>(j)], L.q[static_cast<std::size_t>(j + 1)], a, b, t, u))
                        bridgeAt(li, j, glm::mix(L.q[static_cast<std::size_t>(j)], L.q[static_cast<std::size_t>(j + 1)], t),
                                 0.5f * rv.width + 3.0f);
                });
            }

        // The streets' segments, and their decks over the railway.
        Buckets roadB;
        roadB.cell = 40.0f;
        std::vector<std::pair<int, int>> roadSeg;
        for (int ri = 0; ri < static_cast<int>(R.roads.size()); ++ri)
            for (int k = 1; k < static_cast<int>(R.roads[static_cast<std::size_t>(ri)].pts.size()); ++k) {
                const glm::vec2 a = R.roads[static_cast<std::size_t>(ri)].pts[static_cast<std::size_t>(k - 1)];
                const glm::vec2 b = R.roads[static_cast<std::size_t>(ri)].pts[static_cast<std::size_t>(k)];
                roadB.add(glm::min(a, b), glm::max(a, b), static_cast<int>(roadSeg.size()));
                roadSeg.emplace_back(ri, k);
            }
        auto onDeck = [&](const Road& r, int k) {
            for (const auto& [a, b] : r.decks) if (k > a && k <= b) return true;
            return false;
        };

        // A light railway down the middle of a street's carriageway is a
        // street tramway: laid into the road, not on a bed of its own.
        for (Line& L : lines) {
            if (L.cls != RailClass::Light) continue;
            int inStreet = 0;
            for (const glm::vec2& q : L.q) {
                bool in = false;
                roadB.near(q, 12.0f, [&](int si) {
                    const auto [ri, rk] = roadSeg[static_cast<std::size_t>(si)];
                    const Road& r = R.roads[static_cast<std::size_t>(ri)];
                    if (r.cls == RoadClass::Service || r.cls == RoadClass::Track) return;
                    in |= segDist(q, r.pts[static_cast<std::size_t>(rk - 1)], r.pts[static_cast<std::size_t>(rk)]) < 0.5f * r.width - 0.5f;
                });
                inStreet += in;
            }
            if (inStreet * 10 > static_cast<int>(L.q.size()) * 6) {
                L.cls = RailClass::Tram;
                std::fill(L.seg.begin(), L.seg.end(), kGround);
            }
        }

        // Main lines first: a siding takes the level of the line it leaves.
        std::vector<int> order(lines.size());
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            const Line& x = lines[static_cast<std::size_t>(a)];
            const Line& y = lines[static_cast<std::size_t>(b)];
            if (x.cls != y.cls) return x.cls < y.cls;
            return x.s.back() > y.s.back();
        });
        Buckets laidB;   // stations of the lines levelled so far: (line << 20) | station
        laidB.cell = 20.0f;
        // Where a road passes under a track's bridge: which line, where on it,
        // which road, where on that. The track rises a little for it; the road
        // dips under it for the rest of its headroom (see the troughs below).
        struct Under { int line, k; float t; int road, rk; float u; glm::vec2 at; };
        std::vector<Under> under;
        constexpr float kHeadroom = 6.7f;   // road surface to deck top: 4.7 m clear, a 2 m deck
        constexpr float kRailRise = 3.2f;   // what the track climbs for it at most
        const float ext = std::max(4.0f, 1.5f * F.cell);   // a deck's reach onto its formation
        for (int li : order) {
            Line& L = lines[static_cast<std::size_t>(li)];
            const std::size_t n = L.q.size();
            L.lvl.assign(n, 0.0f);
            std::vector<float> g(n);
            for (std::size_t i = 0; i < n; ++i) g[i] = orig.sample(L.q[i].x, L.q[i].y);
            if (L.cls == RailClass::Tram) {
                // In the street, at the street's level: nothing to grade.
                L.lvl = g;
                continue;
            }
            // Runs of stretches off the ground: [a, b) in stretches, stations a..b.
            std::vector<std::pair<int, int>> offRuns;
            for (std::size_t j = 0; j + 1 < n;) {
                if (L.seg[j] == kGround) { ++j; continue; }
                std::size_t e = j;
                while (e + 1 < n && L.seg[e] == L.seg[j]) ++e;
                offRuns.emplace_back(static_cast<int>(j), static_cast<int>(e));
                j = e;
            }
            // Under a bridge and in a tunnel the land is not the line's: across
            // them the line runs between the land at their ends.
            for (const auto& [a, b] : offRuns)
                for (int i = a + 1; i < b; ++i)
                    g[static_cast<std::size_t>(i)] = glm::mix(g[static_cast<std::size_t>(a)], g[static_cast<std::size_t>(b)],
                        (L.s[static_cast<std::size_t>(i)] - L.s[static_cast<std::size_t>(a)]) /
                        std::max(L.s[static_cast<std::size_t>(b)] - L.s[static_cast<std::size_t>(a)], 1e-3f));
            // The land along it, evened over 60 m.
            std::vector<float> p(n);
            for (std::size_t i = 0, j0 = 0, j1 = 0; i < n; ++i) {
                while (L.s[j0] < L.s[i] - 30.0f) ++j0;
                while (j1 + 1 < n && L.s[j1 + 1] <= L.s[i] + 30.0f) ++j1;
                float sum = 0.0f;
                for (std::size_t j = j0; j <= j1; ++j) sum += g[j];
                p[i] = sum / static_cast<float>(j1 - j0 + 1);
            }
            // What it has to clear, and what it has to meet.
            std::vector<float> lob(n, -1e9f), hib(n, 1e9f);
            for (const auto& [a, b] : offRuns) {
                if (L.seg[static_cast<std::size_t>(a)] != kBridge) continue;
                float need = -1e9f;
                for (int k = a; k < b; ++k) {
                    const glm::vec2 qa = L.q[static_cast<std::size_t>(k)], qb = L.q[static_cast<std::size_t>(k + 1)];
                    float t, u;
                    // A road under it: the track rises a little, the road dips.
                    roadB.near(0.5f * (qa + qb), 4.0f, [&](int si) {
                        const auto [ri, rk] = roadSeg[static_cast<std::size_t>(si)];
                        const Road& r = R.roads[static_cast<std::size_t>(ri)];
                        if (onDeck(r, rk)) return;
                        if (segCross(qa, qb, r.pts[static_cast<std::size_t>(rk - 1)], r.pts[static_cast<std::size_t>(rk)], t, u)) {
                            const glm::vec2 at = glm::mix(qa, qb, t);
                            need = std::max(need, orig.sample(at.x, at.y) + kRailRise);
                            under.push_back({li, k, t, ri, rk, u, at});
                        }
                    });
                    // Another line under it: its catenary too.
                    railB.near(0.5f * (qa + qb), 4.0f, [&](int si) {
                        const RailSeg& o = railSegs[static_cast<std::size_t>(si)];
                        if (o.way == L.way) return;
                        if (segCross(qa, qb, o.a, o.b, t, u)) {
                            const glm::vec2 at = glm::mix(qa, qb, t);
                            need = std::max(need, orig.sample(at.x, at.y) + 7.5f);
                        }
                    });
                    for (const worldgen::River& rv : out.rivers)
                        for (std::size_t m = 1; m < rv.points.size(); ++m)
                            if (segCross(qa, qb, rv.points[m - 1], rv.points[m], t, u)) {
                                const glm::vec2 at = glm::mix(qa, qb, t);
                                need = std::max(need, orig.sample(at.x, at.y) + 3.0f);
                            }
                }
                // Over nothing the map names (a valley, a path): clear of the land.
                if (need < -1e8f)
                    for (int i = a; i <= b; ++i)
                        need = std::max(need, orig.sample(L.q[static_cast<std::size_t>(i)].x, L.q[static_cast<std::size_t>(i)].y) + 2.5f);
                for (int i = a; i <= b; ++i) lob[static_cast<std::size_t>(i)] = std::max(lob[static_cast<std::size_t>(i)], need);
            }
            // A road bridge over it: low enough to pass under its deck, wires and
            // all -- with a metre to spare, as the road evens its own profile over
            // about ten metres and its bridge ends sink a little toward the cutting.
            for (std::size_t k = 0; k + 1 < n; ++k) {
                if (L.seg[k] == kTunnel) continue;
                const glm::vec2 qa = L.q[k], qb = L.q[k + 1];
                roadB.near(0.5f * (qa + qb), 4.0f, [&](int si) {
                    const auto [ri, rk] = roadSeg[static_cast<std::size_t>(si)];
                    const Road& r = R.roads[static_cast<std::size_t>(ri)];
                    for (const auto& [da, db] : r.decks) {
                        if (rk <= da || rk > db) continue;
                        float t, u;
                        if (!segCross(qa, qb, r.pts[static_cast<std::size_t>(rk - 1)], r.pts[static_cast<std::size_t>(rk)], t, u)) continue;
                        const glm::vec2 ea = r.pts[static_cast<std::size_t>(da)], eb = r.pts[static_cast<std::size_t>(db)];
                        const float deck = std::min(orig.sample(ea.x, ea.y), orig.sample(eb.x, eb.y));
                        const float sAt = L.s[k] + t * (L.s[k + 1] - L.s[k]);
                        for (std::size_t i = 0; i < n; ++i)
                            if (std::fabs(L.s[i] - sAt) <= 0.5f * r.width + 3.0f) hib[i] = std::min(hib[i], deck - 8.8f);
                    }
                });
            }
            // Where it leaves a line already levelled, joins it or runs beside it
            // (a yard, the other track of a double line): on that line's
            // formation, at its level.
            // Beside it, not across it: a line crossing under another's bridge
            // keeps its own level.
            auto onGround = [](const Line& o, std::size_t i) {
                return (i > 0 && o.seg[i - 1] == kGround) || (i + 1 < o.q.size() && o.seg[i] == kGround);
            };
            auto along = [](const Line& o, std::size_t i) {
                const glm::vec2 d = o.q[std::min(i + 1, o.q.size() - 1)] - o.q[i > 0 ? i - 1 : 0];
                return glm::dot(d, d) > 1e-6f ? glm::normalize(d) : glm::vec2(1.0f, 0.0f);
            };
            for (std::size_t i = 0; i < n; ++i) {
                // Its ends meet the line they leave whatever they start on.
                const bool end = i == 0 || i + 1 == n;
                if (!onGround(L, i) && !end) continue;
                const glm::vec2 dir = along(L, i);
                float best = 6.0f, at = 0.0f;
                bool found = false;
                laidB.near(L.q[i], 6.0f, [&](int code) {
                    const Line& o = lines[static_cast<std::size_t>(code >> 20)];
                    const std::size_t oi = static_cast<std::size_t>(code & 0xfffff);
                    if (!onGround(o, oi)) return;
                    const float d = glm::length(o.q[oi] - L.q[i]);
                    if (!(end && d < 3.0f) && std::fabs(glm::dot(dir, along(o, oi))) < 0.94f) return;
                    if (d < best) { best = d; at = o.lvl[oi]; found = true; }
                });
                if (found) { lob[i] = std::max(lob[i], at); hib[i] = std::min(hib[i], at); }
            }
            // No steeper than the line may climb: halfway between the highest
            // such line under the land and the lowest over it -- the hills cut,
            // the hollows filled, about evenly.
            const float G = railGrade(L.cls);
            std::vector<float> up = p, dn = p;
            for (std::size_t i = 1; i < n; ++i) {
                const float ds = L.s[i] - L.s[i - 1];
                up[i] = std::min(up[i], up[i - 1] + G * ds);
                dn[i] = std::max(dn[i], dn[i - 1] - G * ds);
            }
            for (std::size_t i = n - 1; i-- > 0;) {
                const float ds = L.s[i + 1] - L.s[i];
                up[i] = std::min(up[i], up[i + 1] + G * ds);
                dn[i] = std::max(dn[i], dn[i + 1] - G * ds);
            }
            for (std::size_t i = 0; i < n; ++i) p[i] = 0.5f * (up[i] + dn[i]);
            // ...then what it must clear, ramped up to (and down from) at the
            // same grade.
            std::vector<float> lc = lob, hc = hib;
            for (std::size_t i = 1; i < n; ++i) {
                const float ds = L.s[i] - L.s[i - 1];
                lc[i] = std::max(lc[i], lc[i - 1] - G * ds);
                hc[i] = std::min(hc[i], hc[i - 1] + G * ds);
            }
            for (std::size_t i = n - 1; i-- > 0;) {
                const float ds = L.s[i + 1] - L.s[i];
                lc[i] = std::max(lc[i], lc[i + 1] - G * ds);
                hc[i] = std::min(hc[i], hc[i + 1] + G * ds);
            }
            for (std::size_t i = 0; i < n; ++i) p[i] = std::min(std::max(p[i], lc[i]), hc[i]);
            // A deck is straight: from the formation a few metres before the
            // bridge to the formation a few metres after it.
            for (const auto& [a, b] : offRuns) {
                if (L.seg[static_cast<std::size_t>(a)] != kBridge) continue;
                int a2 = a, b2 = b;
                while (a2 > 0 && L.s[static_cast<std::size_t>(a)] - L.s[static_cast<std::size_t>(a2)] < ext &&
                       L.seg[static_cast<std::size_t>(a2 - 1)] == kGround) --a2;
                while (b2 + 1 < static_cast<int>(n) && L.s[static_cast<std::size_t>(b2)] - L.s[static_cast<std::size_t>(b)] < ext &&
                       L.seg[static_cast<std::size_t>(b2)] == kGround) ++b2;
                if (!L.decks.empty() && a2 <= L.decks.back().second) L.decks.back().second = b2;
                else L.decks.emplace_back(a2, b2);
            }
            for (const auto& [a, b] : L.decks)
                for (int i = a + 1; i < b; ++i)
                    p[static_cast<std::size_t>(i)] = glm::mix(p[static_cast<std::size_t>(a)], p[static_cast<std::size_t>(b)],
                        (L.s[static_cast<std::size_t>(i)] - L.s[static_cast<std::size_t>(a)]) /
                        std::max(L.s[static_cast<std::size_t>(b)] - L.s[static_cast<std::size_t>(a)], 1e-3f));
            L.lvl = p;
            for (std::size_t i = 0; i < n; ++i) {
                laidB.add(L.q[i], L.q[i], (li << 20) | static_cast<int>(i));
                if (i + 1 < n && L.seg[i] != kGround) continue;
                const float land = orig.sample(L.q[i].x, L.q[i].y);
                S.deepestCut = std::max(S.deepestCut, land - p[i]);
                S.highestBank = std::max(S.highestBank, p[i] - land);
            }
        }

        // Each road under a track's bridge, dipping as deep as its headroom
        // needs: level under the bridge and for a little either side, then up
        // to its own ground at a road's grade. A metre more than it needs: the
        // road smooths its profile over about ten metres.
        struct Trough { int road; float s0, depth; };
        std::vector<Trough> troughs;
        float deepestDip = 0.0f;
        for (const Under& un : under) {
            const Line& L = lines[static_cast<std::size_t>(un.line)];
            const float deck = glm::mix(L.lvl[static_cast<std::size_t>(un.k)], L.lvl[static_cast<std::size_t>(un.k + 1)], un.t);
            const float lack = kHeadroom - (deck - orig.sample(un.at.x, un.at.y));
            if (lack < 0.2f) continue;
            const Road& r = R.roads[static_cast<std::size_t>(un.road)];
            float s0 = 0.0f;
            for (int k = 1; k < un.rk; ++k) s0 += glm::length(r.pts[static_cast<std::size_t>(k)] - r.pts[static_cast<std::size_t>(k - 1)]);
            s0 += un.u * glm::length(r.pts[static_cast<std::size_t>(un.rk)] - r.pts[static_cast<std::size_t>(un.rk - 1)]);
            troughs.push_back({un.road, s0, lack + 0.8f});
            deepestDip = std::max(deepestDip, lack);
        }

        // --- The formation, graded into the ground -------------------------------------------------
        // Each cell of the fine grid takes the nearest stretch of track within
        // reach: flat at its level across the formation (wide enough that the
        // grid's own cells cannot tilt the track), the land's slope meeting it at
        // 1:1.5 beyond -- or a wall, where a road bridge stands over a cutting.
        const float reach = 45.0f;
        const std::size_t cells = static_cast<std::size_t>(F.nx) * F.nz;
        std::vector<float> bestD(cells, 1e9f), bestF(cells, 0.0f), bestHw(cells, 0.0f);
        std::vector<char> bestKeep(cells, 0);
        auto cellOf = [&](glm::vec2 w, int& i, int& j) {
            i = static_cast<int>(std::lround((w.x - F.x0) / F.cell));
            j = static_cast<int>(std::lround((w.y - F.z0) / F.cell));
        };
        // The stretches on the ground first, then the bridges and tunnels: a
        // cell on a track's formation stays on it even beside a bridge's end,
        // and a line passing under another's bridge keeps its own formation.
        for (int pass = 0; pass < 2; ++pass)
        for (const Line& L : lines) {
            if (L.cls == RailClass::Tram) continue;
            const float hw = std::max(railHalfFormation(L.cls), 1.45f * F.cell);
            for (std::size_t k = 0; k + 1 < L.q.size(); ++k) {
                const glm::vec2 a = L.q[k], b = L.q[k + 1];
                const bool keep = L.seg[k] != kGround;
                if (keep != (pass == 1)) continue;
                int i0, j0, i1, j1;
                cellOf(glm::min(a, b) - glm::vec2(hw + reach), i0, j0);
                cellOf(glm::max(a, b) + glm::vec2(hw + reach), i1, j1);
                i0 = std::max(i0, 0); j0 = std::max(j0, 0);
                i1 = std::min(i1, F.nx - 1); j1 = std::min(j1, F.nz - 1);
                const glm::vec2 ab = b - a;
                const float L2 = std::max(glm::dot(ab, ab), 1e-8f);
                for (int j = j0; j <= j1; ++j)
                    for (int i = i0; i <= i1; ++i) {
                        const glm::vec2 w{F.x0 + F.cell * i, F.z0 + F.cell * j};
                        const float t = glm::clamp(glm::dot(w - a, ab) / L2, 0.0f, 1.0f);
                        const float d = glm::length(w - (a + ab * t));
                        const std::size_t c = static_cast<std::size_t>(j) * F.nx + i;
                        if (d > hw + reach || d >= bestD[c]) continue;
                        if (keep && bestD[c] <= bestHw[c]) continue;
                        bestD[c] = d;
                        bestF[c] = glm::mix(L.lvl[k], L.lvl[k + 1], t);
                        bestHw[c] = hw;
                        bestKeep[c] = keep ? 1 : 0;
                    }
            }
        }
        // Where a road bridge stands over a line, the cutting has walls: its
        // abutments stand on the land, not on a slope that fell away under them.
        std::vector<char> walled(cells, 0);
        for (const Road& r : R.roads)
            for (const auto& [da, db] : r.decks)
                for (int k = da + 1; k <= db; ++k) {
                    const glm::vec2 a = r.pts[static_cast<std::size_t>(k - 1)], b = r.pts[static_cast<std::size_t>(k)];
                    const float half = 0.5f * r.width + 4.0f;
                    int i0, j0, i1, j1;
                    cellOf(glm::min(a, b) - glm::vec2(half + 6.0f), i0, j0);
                    cellOf(glm::max(a, b) + glm::vec2(half + 6.0f), i1, j1);
                    for (int j = std::max(j0, 0); j <= std::min(j1, F.nz - 1); ++j)
                        for (int i = std::max(i0, 0); i <= std::min(i1, F.nx - 1); ++i)
                            if (segDist({F.x0 + F.cell * i, F.z0 + F.cell * j}, a, b) <= half)
                                walled[static_cast<std::size_t>(j) * F.nx + i] = 1;
                }
        for (std::size_t c = 0; c < cells; ++c) {
            if (bestD[c] > 1e8f || bestKeep[c]) continue;
            const float d = bestD[c], f = bestF[c], hw = bestHw[c];
            if (d <= hw) { F.h[c] = f; continue; }
            const float e = (d - hw) * (walled[c] ? 4.0f : 1.0f / 1.5f);
            F.h[c] = f + glm::clamp(orig.h[c] - f, -e, e);
        }

        // The troughs, cut after the formation: under a bridge the land was
        // left alone, and the road's dip goes on into the land either side.
        for (const Trough& tr : troughs) {
            const Road& r = R.roads[static_cast<std::size_t>(tr.road)];
            const float flat = 18.0f, ramp = tr.depth / 0.07f;
            const float half = 0.5f * r.width + 2.5f;
            float acc = 0.0f;
            for (std::size_t k = 1; k < r.pts.size(); ++k) {
                const glm::vec2 a = r.pts[k - 1], b = r.pts[k];
                const float len = glm::length(b - a), s1 = acc;
                acc += len;
                if (s1 > tr.s0 + flat + ramp || acc < tr.s0 - flat - ramp) continue;
                const float reachOut = half + 0.5f * tr.depth + 1.0f;
                int i0, j0, i1, j1;
                cellOf(glm::min(a, b) - glm::vec2(reachOut), i0, j0);
                cellOf(glm::max(a, b) + glm::vec2(reachOut), i1, j1);
                const glm::vec2 ab = b - a;
                const float L2 = std::max(glm::dot(ab, ab), 1e-8f);
                for (int j = std::max(j0, 0); j <= std::min(j1, F.nz - 1); ++j)
                    for (int i = std::max(i0, 0); i <= std::min(i1, F.nx - 1); ++i) {
                        const glm::vec2 w{F.x0 + F.cell * i, F.z0 + F.cell * j};
                        const float t = glm::clamp(glm::dot(w - a, ab) / L2, 0.0f, 1.0f);
                        const float d = glm::length(w - (a + ab * t));
                        const float along = std::fabs(s1 + t * len - tr.s0);
                        float dip = tr.depth - 0.07f * std::max(along - flat, 0.0f);
                        // Its sides walls, at 2:1 up to the land beside it.
                        dip -= 2.0f * std::max(d - half, 0.0f);
                        if (dip <= 0.0f) continue;
                        // Not into a track's formation: its edge is the abutment.
                        const std::size_t c = static_cast<std::size_t>(j) * F.nx + i;
                        if (!bestKeep[c] && bestD[c] <= bestHw[c] + 0.5f) continue;
                        F.h[c] = std::min(F.h[c], orig.h[c] - dip);
                    }
            }
        }
        if (!troughs.empty()) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%zu road%s dip under the railway's bridges, up to %.1f m.", troughs.size(),
                          troughs.size() == 1 ? "" : "s", deepestDip);
            S.notes.push_back(buf);
        }

        // --- Platforms ---------------------------------------------------------------------------
        // Its middle line, found by slicing the outline across its length; the
        // ground under it levelled to the track beside it.
        for (const geo::Feature& f : A.features) {
            if (!f.is("railway", "platform") || f.rings.empty() || f.rings[0].size() < 2) continue;
            // An underground station's platforms are not on the surface.
            if (std::atoi(f.tagOr("layer").c_str()) < 0 || (f.has("tunnel") && !f.is("tunnel", "no"))) continue;
            std::vector<glm::vec2> poly;
            for (const geo::LonLat& q : f.rings[0]) poly.push_back(W(q));
            if (!inSquare(polyCentre(poly), 20.0f)) continue;
            Platform pf;
            pf.name = f.tagOr("name");
            if (f.geom == geo::Feature::Geom::Area && poly.size() >= 3) {
                glm::vec2 c, u;
                float len = 0.0f, wid = 0.0f;
                minRect(poly, c, u, len, wid);
                if (len < 15.0f || wid < 1.5f) continue;
                const glm::vec2 nv(-u.y, u.x);
                std::vector<float> widths;
                for (float t = -0.5f * len + 1.0f; t <= 0.5f * len - 1.0f; t += 4.0f) {
                    const glm::vec2 o = c + u * t;
                    float v0 = 1e9f, v1 = -1e9f;
                    for (std::size_t k = 0; k < poly.size(); ++k) {
                        const glm::vec2 a = poly[k], b = poly[(k + 1) % poly.size()];
                        const float da = glm::dot(a - o, u), db = glm::dot(b - o, u);
                        if ((da > 0.0f) == (db > 0.0f)) continue;
                        const glm::vec2 x = glm::mix(a, b, da / (da - db));
                        const float v = glm::dot(x - o, nv);
                        v0 = std::min(v0, v);
                        v1 = std::max(v1, v);
                    }
                    if (v1 - v0 < 1.0f) continue;
                    pf.pts.push_back(o + nv * (0.5f * (v0 + v1)));
                    widths.push_back(v1 - v0);
                }
                if (pf.pts.size() < 2) continue;
                std::sort(widths.begin(), widths.end());
                pf.width = glm::clamp(widths[widths.size() / 2], 2.0f, 16.0f);
                pf.pts.front() = c + u * (-0.5f * len) + (pf.pts.front() - c - u * glm::dot(pf.pts.front() - c, u));
                pf.pts.back() = c + u * (0.5f * len) + (pf.pts.back() - c - u * glm::dot(pf.pts.back() - c, u));
            } else {
                pf.pts = poly;
                pf.width = 3.0f;
            }
            // Only alongside a track that was laid, for most of its length.
            int near = 0;
            for (const glm::vec2& q : pf.pts) {
                int i, j;
                cellOf(q, i, j);
                if (i < 0 || j < 0 || i >= F.nx || j >= F.nz) continue;
                const std::size_t c = static_cast<std::size_t>(j) * F.nx + i;
                near += bestD[c] < 0.5f * pf.width + 6.0f && !bestKeep[c];
            }
            if (near * 10 < static_cast<int>(pf.pts.size()) * 6) continue;
            // Level under it, at its track's level.
            for (std::size_t k = 1; k < pf.pts.size(); ++k) {
                const glm::vec2 a = pf.pts[k - 1], b = pf.pts[k];
                const float half = 0.5f * pf.width + F.cell;
                int i0, j0, i1, j1;
                cellOf(glm::min(a, b) - glm::vec2(half), i0, j0);
                cellOf(glm::max(a, b) + glm::vec2(half), i1, j1);
                for (int j = std::max(j0, 0); j <= std::min(j1, F.nz - 1); ++j)
                    for (int i = std::max(i0, 0); i <= std::min(i1, F.nx - 1); ++i) {
                        const std::size_t c = static_cast<std::size_t>(j) * F.nx + i;
                        if (bestD[c] > 1e8f || bestKeep[c]) continue;
                        if (segDist({F.x0 + F.cell * i, F.z0 + F.cell * j}, a, b) <= half) F.h[c] = bestF[c];
                    }
            }
            std::vector<char> keepAll;
            pf.pts = simplify(pf.pts, 0.15f, keepAll);
            R.platforms.push_back(std::move(pf));
        }
        S.platforms = static_cast<int>(R.platforms.size());

        // --- What is laid: the tracks between the tunnels, their bridges -------------------------------
        struct Span2 { std::vector<glm::vec2> pts; std::string name; int line; };
        std::vector<Span2> decks;
        for (int li = 0; li < static_cast<int>(lines.size()); ++li) {
            const Line& L = lines[static_cast<std::size_t>(li)];
            const std::size_t n = L.q.size();
            for (std::size_t j = 0; j + 1 < n; ++j)
                if (L.seg[j] == kTunnel && (j == 0 || L.seg[j - 1] != kTunnel)) ++S.railTunnels;
            // Pieces between the tunnels.
            for (std::size_t a = 0; a + 1 < n;) {
                if (L.seg[a] == kTunnel) { ++a; continue; }
                std::size_t b = a;
                while (b + 1 < n && L.seg[b] != kTunnel) ++b;
                // Stations a..b. Keep the deck ends; thin the rest.
                std::vector<glm::vec2> pts(L.q.begin() + static_cast<std::ptrdiff_t>(a), L.q.begin() + static_cast<std::ptrdiff_t>(b + 1));
                std::vector<char> keep(pts.size(), 0);
                for (const auto& [da, db] : L.decks) {
                    if (da >= static_cast<int>(a) && da <= static_cast<int>(b)) keep[static_cast<std::size_t>(da) - a] = 1;
                    if (db >= static_cast<int>(a) && db <= static_cast<int>(b)) keep[static_cast<std::size_t>(db) - a] = 1;
                }
                // Long straights get a point every 60 m: the level is the ground's
                // there, and a spline span much longer sags off the formation.
                for (std::size_t i = 0; i < pts.size(); ++i)
                    if (static_cast<int>(L.s[a + i] / 60.0f) != static_cast<int>(L.s[a + (i == 0 ? 0 : i - 1)] / 60.0f)) keep[i] = 1;
                // ...and where its level bends: the foot of a ramp, the low point
                // under a road bridge.
                for (std::size_t i = 1; i + 1 < pts.size(); ++i) {
                    const float s0 = L.s[a + i - 1], s1 = L.s[a + i], s2 = L.s[a + i + 1];
                    const float g0 = (L.lvl[a + i] - L.lvl[a + i - 1]) / std::max(s1 - s0, 1e-3f);
                    const float g1 = (L.lvl[a + i + 1] - L.lvl[a + i]) / std::max(s2 - s1, 1e-3f);
                    if (std::fabs(g1 - g0) > 0.006f) keep[i] = 1;
                }
                const std::vector<std::size_t> idx = simplifyIdx(pts, L.cls == RailClass::Tram ? 0.3f : 0.12f, keep);
                Rail rl;
                rl.cls = L.cls;
                rl.name = L.name;
                for (std::size_t i : idx) {
                    rl.pts.push_back(pts[i]);
                    rl.level.push_back(L.lvl[a + i]);
                }
                for (const auto& [da, db] : L.decks) {
                    if (da < static_cast<int>(a) || db > static_cast<int>(b)) continue;
                    int ia = -1, ib = -1;
                    for (std::size_t k = 0; k < idx.size(); ++k) {
                        if (idx[k] + a == static_cast<std::size_t>(da)) ia = static_cast<int>(k);
                        if (idx[k] + a == static_cast<std::size_t>(db)) ib = static_cast<int>(k);
                    }
                    if (ia < 0 || ib <= ia) continue;
                    rl.bridges.emplace_back(ia, ib);
                    decks.push_back({std::vector<glm::vec2>(rl.pts.begin() + ia, rl.pts.begin() + ib + 1), L.name, li});
                }
                if (rl.pts.size() >= 2 && lineLength(rl.pts) > 10.0f) {
                    S.railKm[static_cast<std::size_t>(rl.cls)] += lineLength(rl.pts) / 1000.0f;
                    R.rails.push_back(std::move(rl));
                }
                a = b;
            }
        }
        // Two tracks of a line crossing side by side share one deck.
        std::vector<char> used(decks.size(), 0);
        auto resample = [](const std::vector<glm::vec2>& l, int n) {
            std::vector<glm::vec2> o;
            const float total = std::max(lineLength(l), 1e-3f);
            for (int k = 0; k < n; ++k) {
                float want = total * static_cast<float>(k) / static_cast<float>(n - 1), acc = 0.0f;
                glm::vec2 q = l.back();
                for (std::size_t i = 1; i < l.size(); ++i) {
                    const float seg = glm::length(l[i] - l[i - 1]);
                    if (acc + seg >= want) { q = glm::mix(l[i - 1], l[i], (want - acc) / std::max(seg, 1e-6f)); break; }
                    acc += seg;
                }
                o.push_back(q);
            }
            return o;
        };
        for (std::size_t i = 0; i < decks.size(); ++i) {
            if (used[i]) continue;
            used[i] = 1;
            RailBridge rb;
            rb.name = decks[i].name;
            rb.pts = decks[i].pts;
            for (std::size_t k = i + 1; k < decks.size(); ++k) {
                if (used[k] || decks[k].line == decks[i].line) continue;
                std::vector<glm::vec2> o = decks[k].pts;
                const glm::vec2 a = rb.pts.front(), b = rb.pts.back();
                if (glm::length(o.front() - b) + glm::length(o.back() - a) < glm::length(o.front() - a) + glm::length(o.back() - b))
                    std::reverse(o.begin(), o.end());
                if (glm::length(o.front() - a) > 9.0f || glm::length(o.back() - b) > 9.0f) continue;
                const int m = std::max(2, static_cast<int>(lineLength(rb.pts) / 5.0f) + 1);
                const std::vector<glm::vec2> x = resample(rb.pts, m), y = resample(o, m);
                float mean = 0.0f;
                for (int q = 0; q < m; ++q) mean += glm::length(x[static_cast<std::size_t>(q)] - y[static_cast<std::size_t>(q)]);
                mean /= static_cast<float>(m);
                if (mean < 2.5f || mean > 7.0f) continue;
                rb.pts.clear();
                for (int q = 0; q < m; ++q) rb.pts.push_back(0.5f * (x[static_cast<std::size_t>(q)] + y[static_cast<std::size_t>(q)]));
                std::vector<char> keepAll;
                rb.pts = simplify(rb.pts, 0.1f, keepAll);
                rb.tracks = 2;
                rb.spacing = mean;
                rb.width = mean + 5.6f;
                used[k] = 1;
                break;
            }
            R.railBridges.push_back(std::move(rb));
        }
        S.railBridges = static_cast<int>(R.railBridges.size());
        float km = 0.0f;
        for (float k : S.railKm) km += k;
        if (km > 0.0f) {
            char buf[240];
            std::snprintf(buf, sizeof(buf),
                          "%.1f km of track on its own formation: cut up to %.0f m into the land, banked up to %.0f m over "
                          "it, %d bridge%s%s.",
                          km, S.deepestCut, S.highestBank, S.railBridges, S.railBridges == 1 ? "" : "s",
                          S.railTunnels ? (", " + std::to_string(S.railTunnels) + " tunnel(s) left out: the track ends at the portal").c_str() : "");
            S.notes.push_back(buf);
        }
    }

    // The ground is final (the railways graded into it): found again by an id
    // that names what it is, so a scene of the same square from before is not
    // handed a ground that changed since.
    {
        std::uint64_t h = 1469598103934665603ULL;
        for (const float v : hf->fine.h) {
            std::uint32_t b;
            std::memcpy(&b, &v, sizeof(b));
            h = (h ^ b) * 1099511628211ULL;
        }
        char tag[24];
        std::snprintf(tag, sizeof(tag), "_%08x", static_cast<unsigned>(h ^ (h >> 32)));
        hf->id += tag;
        fitzel::registerHeightfield(hf);
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
        for (const Rail& r : R.rails)
            drawn.stroke(r.pts, railHalfFormation(r.cls) + 2.0f, [](std::uint8_t& c) { c = LRoad; });
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
