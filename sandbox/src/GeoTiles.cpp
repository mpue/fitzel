#include "GeoTiles.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <map>
#include <set>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace geo {

namespace {

constexpr double kPi = 3.14159265358979323846;

double clampLat(double lat) { return std::clamp(lat, -85.05112878, 85.05112878); }

std::int64_t fixed7(double deg) { return static_cast<std::int64_t>(std::llround(deg * 1e7)); }
double unfix7(std::int64_t v) { return static_cast<double>(v) * 1e-7; }

bool sameAt(const LonLat& a, const LonLat& b) {
    return fixed7(a.lon) == fixed7(b.lon) && fixed7(a.lat) == fixed7(b.lat);
}

} // namespace

// --- Tiles ----------------------------------------------------------------------------

double tileX(double lon, int z) { return (lon + 180.0) / 360.0 * std::ldexp(1.0, z); }

double tileY(double lat, int z) {
    const double r = clampLat(lat) * kPi / 180.0;
    return (1.0 - std::log(std::tan(r) + 1.0 / std::cos(r)) / kPi) / 2.0 * std::ldexp(1.0, z);
}

double lonOf(double tx, int z) { return tx / std::ldexp(1.0, z) * 360.0 - 180.0; }

double latOf(double ty, int z) {
    const double n = kPi - 2.0 * kPi * ty / std::ldexp(1.0, z);
    return 180.0 / kPi * std::atan(0.5 * (std::exp(n) - std::exp(-n)));
}

BBox tileBounds(const TileId& t) {
    BBox b;
    b.w = lonOf(t.x, t.z);
    b.e = lonOf(t.x + 1, t.z);
    b.n = latOf(t.y, t.z);
    b.s = latOf(t.y + 1, t.z);
    return b;
}

std::vector<TileId> tilesCovering(const BBox& b, int z) {
    std::vector<TileId> out;
    const int n = 1 << z;
    const int x0 = std::clamp(static_cast<int>(std::floor(tileX(b.w, z))), 0, n - 1);
    const int x1 = std::clamp(static_cast<int>(std::floor(tileX(b.e, z))), 0, n - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(tileY(b.n, z))), 0, n - 1);
    const int y1 = std::clamp(static_cast<int>(std::floor(tileY(b.s, z))), 0, n - 1);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) out.push_back({z, x, y});
    return out;
}

TileId metaOf(const TileId& t) {
    if (t.z <= kMetaZoom) return t;
    const int sh = t.z - kMetaZoom;
    return {kMetaZoom, t.x >> sh, t.y >> sh};
}

std::vector<TileId> childrenAt(const TileId& t, int z) {
    std::vector<TileId> out;
    if (z < t.z) return out;
    const int k = 1 << (z - t.z);
    for (int y = 0; y < k; ++y)
        for (int x = 0; x < k; ++x) out.push_back({z, t.x * k + x, t.y * k + y});
    return out;
}

double metresPerDegLat(double lat) {
    const double r = lat * kPi / 180.0;
    return 111132.92 - 559.82 * std::cos(2 * r) + 1.175 * std::cos(4 * r) - 0.0023 * std::cos(6 * r);
}

double metresPerDegLon(double lat) {
    const double r = lat * kPi / 180.0;
    return 111412.84 * std::cos(r) - 93.5 * std::cos(3 * r) + 0.118 * std::cos(5 * r);
}

BBox squareAround(LonLat c, double size) {
    const double hx = 0.5 * size / std::max(metresPerDegLon(c.lat), 1.0);
    const double hz = 0.5 * size / metresPerDegLat(c.lat);
    return {c.lat - hz, c.lon - hx, c.lat + hz, c.lon + hx};
}

Frame frameAt(LonLat origin) {
    Frame f;
    f.origin = origin;
    f.mx = metresPerDegLon(origin.lat);
    f.mz = metresPerDegLat(origin.lat);
    return f;
}

// --- Features -------------------------------------------------------------------------

const std::string* Feature::tag(const char* k) const {
    for (const auto& kv : tags)
        if (kv.first == k) return &kv.second;
    return nullptr;
}

std::string Feature::tagOr(const char* k, const char* d) const {
    const std::string* v = tag(k);
    return v ? *v : std::string(d);
}

bool Feature::is(const char* k, const char* v) const {
    const std::string* t = tag(k);
    return t && *t == v;
}

std::uint64_t featureId(int osmType, std::uint64_t osmId) {
    return (static_cast<std::uint64_t>(osmType & 3) << 62) | (osmId & ((1ull << 62) - 1));
}

std::string featureIdText(std::uint64_t id) {
    static const char kType[4] = {'n', 'w', 'r', 'x'};
    return std::string(1, kType[(id >> 62) & 3]) + std::to_string(id & ((1ull << 62) - 1));
}

namespace {
std::uint64_t parseIdText(const std::string& s) {
    if (s.empty()) return 0;
    const int type = s[0] == 'n' ? 0 : s[0] == 'w' ? 1 : s[0] == 'r' ? 2 : 3;
    std::uint64_t v = 0;
    for (std::size_t i = 1; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) v = v * 10 + static_cast<std::uint64_t>(s[i] - '0');
    return featureId(type, v);
}
} // namespace

bool keepTag(const std::string& k) {
    static const std::set<std::string> keep = {
        "building", "building:levels", "building:min_level", "height", "min_height",
        "roof:shape", "roof:levels", "roof:colour", "roof:material", "roof:height",
        "building:colour", "building:material", "building:use",
        "amenity", "shop", "office", "craft", "tourism", "historic", "religion", "denomination",
        "healthcare", "industrial", "man_made", "power", "sport", "public_transport",
        "name", "place", "population",
        "highway", "service", "lanes", "width", "surface", "oneway", "bridge", "tunnel", "layer",
        "junction", "tracktype", "sidewalk", "maxspeed", "ref", "area",
        "railway", "waterway", "intermittent",
        "natural", "landuse", "leisure", "water", "wetland", "landcover", "crop", "produce",
        "leaf_type", "leaf_cycle", "genus", "species", "denotation", "circumference",
        "barrier", "wall", "type"};
    return keep.count(k) != 0;
}

std::string overpassQuery(const BBox& b, int timeoutSec) {
    char box[160];
    std::snprintf(box, sizeof(box), "(%.7f,%.7f,%.7f,%.7f)", b.s, b.w, b.n, b.e);
    const std::string B = box;
    std::string q = "[out:json][timeout:" + std::to_string(timeoutSec) + "][maxsize:1073741824];\n(\n";
    for (const char* sel : {"way[\"building\"]", "relation[\"building\"][\"type\"=\"multipolygon\"]",
                            "way[\"highway\"]", "way[\"railway\"]", "way[\"waterway\"]",
                            "way[\"natural\"]", "relation[\"natural\"][\"type\"=\"multipolygon\"]",
                            "way[\"landuse\"]", "relation[\"landuse\"][\"type\"=\"multipolygon\"]",
                            "way[\"leisure\"]", "relation[\"leisure\"][\"type\"=\"multipolygon\"]",
                            "way[\"amenity\"]", "relation[\"amenity\"][\"type\"=\"multipolygon\"]",
                            "way[\"man_made\"]", "node[\"natural\"=\"tree\"]", "node[\"place\"]"})
        q += "  " + std::string(sel) + B + ";\n";
    q += ");\nout geom qt;\n";
    return q;
}

namespace {

// Does a closed way with these tags mean an area (else a closed line, like a
// roundabout or a ring road)? OSM's own rule of thumb, as the renderers apply it.
bool closedIsArea(const std::vector<std::pair<std::string, std::string>>& tags) {
    std::string area, highway, natural, barrier, waterway, railway, manmade;
    bool any = false;
    for (const auto& [k, v] : tags) {
        if (k == "area") area = v;
        else if (k == "highway") highway = v;
        else if (k == "natural") natural = v;
        else if (k == "barrier") barrier = v;
        else if (k == "waterway") waterway = v;
        else if (k == "railway") railway = v;
        else if (k == "man_made") manmade = v;
        if (k == "building" || k == "landuse" || k == "leisure" || k == "amenity" || k == "place" ||
            k == "water" || k == "wetland" || k == "landcover" || k == "shop" || k == "tourism")
            any = true;
    }
    if (area == "yes") return true;
    if (area == "no") return false;
    if (!highway.empty()) return highway == "pedestrian" && area == "yes";
    if (!natural.empty() && natural != "coastline" && natural != "tree_row" && natural != "cliff" &&
        natural != "ridge" && natural != "arete")
        any = true;
    if (waterway == "riverbank" || waterway == "dock") any = true;
    if (!manmade.empty() && manmade != "embankment" && manmade != "pipeline" && manmade != "cutline") any = true;
    if (!barrier.empty() && !any) return false;
    if (!railway.empty() && !any) return railway == "platform";
    return any;
}

void keepTags(const nlohmann::json& t, std::vector<std::pair<std::string, std::string>>& out) {
    if (!t.is_object()) return;
    for (auto it = t.begin(); it != t.end(); ++it)
        if (it.value().is_string() && keepTag(it.key())) out.emplace_back(it.key(), it.value().get<std::string>());
}

std::vector<LonLat> geomOf(const nlohmann::json& g) {
    std::vector<LonLat> pts;
    if (!g.is_array()) return pts;
    pts.reserve(g.size());
    for (const nlohmann::json& p : g)
        if (p.is_object() && p.contains("lat") && p.contains("lon"))
            pts.push_back({p["lon"].get<double>(), p["lat"].get<double>()});
    return pts;
}

// Join a multipolygon's member ways end to end into closed rings. A way that
// cannot be closed (the relation is broken, or reaches far past what was
// fetched) is closed with a straight edge if it is most of a ring, else dropped.
std::vector<std::vector<LonLat>> assemble(std::vector<std::vector<LonLat>> ways) {
    std::vector<std::vector<LonLat>> rings;
    std::vector<char> used(ways.size(), 0);
    for (std::size_t i = 0; i < ways.size(); ++i) {
        if (used[i] || ways[i].size() < 2) continue;
        used[i] = 1;
        std::vector<LonLat> ring = ways[i];
        bool grew = true;
        while (!sameAt(ring.front(), ring.back()) && grew) {
            grew = false;
            for (std::size_t k = 0; k < ways.size(); ++k) {
                if (used[k] || ways[k].size() < 2) continue;
                const std::vector<LonLat>& w = ways[k];
                if (sameAt(w.front(), ring.back())) {
                    ring.insert(ring.end(), w.begin() + 1, w.end());
                } else if (sameAt(w.back(), ring.back())) {
                    ring.insert(ring.end(), w.rbegin() + 1, w.rend());
                } else if (sameAt(w.back(), ring.front())) {
                    ring.insert(ring.begin(), w.begin(), w.end() - 1);
                } else if (sameAt(w.front(), ring.front())) {
                    std::vector<LonLat> r(w.rbegin(), w.rend());
                    ring.insert(ring.begin(), r.begin(), r.end() - 1);
                } else {
                    continue;
                }
                used[k] = 1;
                grew = true;
            }
        }
        if (sameAt(ring.front(), ring.back())) ring.pop_back();
        if (ring.size() >= 3) rings.push_back(std::move(ring));
    }
    return rings;
}

} // namespace

bool fromOverpass(const std::string& text, std::vector<Feature>& out, std::string* err) {
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        if (err) *err = "Overpass answered something that is not JSON";
        return false;
    }
    if (j.contains("remark") && j["remark"].is_string()) {
        const std::string rem = j["remark"].get<std::string>();
        // "runtime error: Query timed out" and friends: the answer is cut short,
        // and caching it would make the hole permanent.
        if (rem.find("error") != std::string::npos) {
            if (err) *err = "Overpass: " + rem;
            return false;
        }
    }
    const nlohmann::json& el = j["elements"];
    if (!el.is_array()) {
        if (err) *err = "Overpass answered without elements";
        return false;
    }
    for (const nlohmann::json& e : el) {
        if (!e.is_object() || !e.contains("type") || !e.contains("id")) continue;
        const std::string type = e["type"].get<std::string>();
        Feature f;
        keepTags(e.value("tags", nlohmann::json::object()), f.tags);
        if (f.tags.empty()) continue;
        const std::uint64_t oid = e["id"].get<std::uint64_t>();
        if (type == "node") {
            if (!e.contains("lat") || !e.contains("lon")) continue;
            f.id = featureId(0, oid);
            f.geom = Feature::Geom::Point;
            f.rings.push_back({{e["lon"].get<double>(), e["lat"].get<double>()}});
            f.hole.push_back(0);
        } else if (type == "way") {
            std::vector<LonLat> pts = geomOf(e.value("geometry", nlohmann::json::array()));
            if (pts.size() < 2) continue;
            f.id = featureId(1, oid);
            const bool closed = pts.size() >= 4 && sameAt(pts.front(), pts.back());
            if (closed && closedIsArea(f.tags)) {
                pts.pop_back();
                f.geom = Feature::Geom::Area;
            } else {
                f.geom = Feature::Geom::Line;
            }
            f.rings.push_back(std::move(pts));
            f.hole.push_back(0);
        } else if (type == "relation") {
            f.id = featureId(2, oid);
            f.geom = Feature::Geom::Area;
            std::vector<std::vector<LonLat>> outer, inner;
            if (!e.contains("members") || !e["members"].is_array()) continue;
            for (const nlohmann::json& m : e["members"]) {
                if (!m.is_object() || m.value("type", std::string()) != "way") continue;
                std::vector<LonLat> pts = geomOf(m.value("geometry", nlohmann::json::array()));
                if (pts.size() < 2) continue;
                (m.value("role", std::string()) == "inner" ? inner : outer).push_back(std::move(pts));
            }
            for (auto& r : assemble(std::move(outer))) { f.rings.push_back(std::move(r)); f.hole.push_back(0); }
            for (auto& r : assemble(std::move(inner))) { f.rings.push_back(std::move(r)); f.hole.push_back(1); }
            if (f.rings.empty()) continue;
        } else {
            continue;
        }
        out.push_back(std::move(f));
    }
    return true;
}

LonLat ringCentre(const std::vector<LonLat>& r) {
    if (r.empty()) return {};
    double a = 0.0, cx = 0.0, cy = 0.0;
    const double ox = r[0].lon, oy = r[0].lat;
    for (std::size_t i = 0; i < r.size(); ++i) {
        const LonLat& p = r[i];
        const LonLat& q = r[(i + 1) % r.size()];
        const double px = p.lon - ox, py = p.lat - oy, qx = q.lon - ox, qy = q.lat - oy;
        const double c = px * qy - qx * py;
        a += c;
        cx += (px + qx) * c;
        cy += (py + qy) * c;
    }
    if (std::fabs(a) < 1e-18) {
        LonLat m{0.0, 0.0};
        for (const LonLat& p : r) { m.lon += p.lon; m.lat += p.lat; }
        m.lon /= static_cast<double>(r.size());
        m.lat /= static_cast<double>(r.size());
        return m;
    }
    return {ox + cx / (3.0 * a), oy + cy / (3.0 * a)};
}

double ringAreaDeg(const std::vector<LonLat>& r) {
    double a = 0.0;
    for (std::size_t i = 0; i < r.size(); ++i) {
        const LonLat& p = r[i];
        const LonLat& q = r[(i + 1) % r.size()];
        a += p.lon * q.lat - q.lon * p.lat;
    }
    return 0.5 * a;
}

namespace {

// Sutherland-Hodgman against the four sides of a box. The result is the part of
// the polygon inside, with stretches along the box's edge where it was cut --
// exactly what rasterising the pieces of every tile back together needs.
std::vector<LonLat> clipToBox(const std::vector<LonLat>& poly, const BBox& b) {
    std::vector<LonLat> cur = poly, next;
    for (int side = 0; side < 4 && !cur.empty(); ++side) {
        next.clear();
        auto inside = [&](const LonLat& p) {
            switch (side) {
            case 0: return p.lon >= b.w;
            case 1: return p.lon <= b.e;
            case 2: return p.lat >= b.s;
            default: return p.lat <= b.n;
            }
        };
        auto cut = [&](const LonLat& p, const LonLat& q) {
            double t = 0.0;
            switch (side) {
            case 0: t = (b.w - p.lon) / (q.lon - p.lon); break;
            case 1: t = (b.e - p.lon) / (q.lon - p.lon); break;
            case 2: t = (b.s - p.lat) / (q.lat - p.lat); break;
            default: t = (b.n - p.lat) / (q.lat - p.lat); break;
            }
            return LonLat{p.lon + (q.lon - p.lon) * t, p.lat + (q.lat - p.lat) * t};
        };
        for (std::size_t i = 0; i < cur.size(); ++i) {
            const LonLat& p = cur[i];
            const LonLat& q = cur[(i + 1) % cur.size()];
            const bool pi = inside(p), qi = inside(q);
            if (pi) next.push_back(p);
            if (pi != qi) next.push_back(cut(p, q));
        }
        std::swap(cur, next);
    }
    return cur.size() >= 3 ? cur : std::vector<LonLat>{};
}

BBox boundsOf(const std::vector<std::vector<LonLat>>& rings) {
    BBox b{1e9, 1e9, -1e9, -1e9};
    for (const auto& r : rings)
        for (const LonLat& p : r) {
            b.s = std::min(b.s, p.lat); b.n = std::max(b.n, p.lat);
            b.w = std::min(b.w, p.lon); b.e = std::max(b.e, p.lon);
        }
    return b;
}

bool isBuildingLike(const Feature& f) {
    return f.has("building");
}

} // namespace

std::vector<Feature> forTile(const std::vector<Feature>& all, const TileId& t) {
    std::vector<Feature> out;
    const BBox tb = tileBounds(t);
    for (const Feature& f : all) {
        if (f.rings.empty() || f.rings[0].empty()) continue;
        if (f.geom == Feature::Geom::Point) {
            if (tb.contains(f.rings[0][0]) && f.rings[0][0].lat < tb.n) out.push_back(f);
            continue;
        }
        const BBox fb = boundsOf(f.rings);
        if (!fb.overlaps(tb)) continue;
        if (f.geom == Feature::Geom::Line) {
            out.push_back(f);
            continue;
        }
        if (isBuildingLike(f)) {
            // Whole, in the one tile its centre is in (north edge exclusive, so a
            // centre on a border belongs to exactly one tile).
            const LonLat c = ringCentre(f.rings[0]);
            if (c.lon >= tb.w && c.lon < tb.e && c.lat >= tb.s && c.lat < tb.n) out.push_back(f);
            continue;
        }
        // Wholly inside: as it is. Else clipped, ring by ring.
        if (fb.w >= tb.w && fb.e <= tb.e && fb.s >= tb.s && fb.n <= tb.n) {
            out.push_back(f);
            continue;
        }
        Feature c;
        c.id = f.id;
        c.geom = f.geom;
        c.tags = f.tags;
        for (std::size_t k = 0; k < f.rings.size(); ++k) {
            std::vector<LonLat> r = clipToBox(f.rings[k], tb);
            if (r.size() < 3) continue;
            c.rings.push_back(std::move(r));
            c.hole.push_back(k < f.hole.size() ? f.hole[k] : 0);
        }
        bool anyOuter = false;
        for (std::uint8_t h : c.hole) anyOuter |= h == 0;
        if (anyOuter) out.push_back(std::move(c));
    }
    return out;
}

std::string writeTile(const std::vector<Feature>& fs, const TileId& t) {
    nlohmann::json j;
    j["v"] = 1;
    j["z"] = t.z; j["x"] = t.x; j["y"] = t.y;
    nlohmann::json arr = nlohmann::json::array();
    for (const Feature& f : fs) {
        nlohmann::json o;
        o["i"] = featureIdText(f.id);
        o["g"] = static_cast<int>(f.geom);
        nlohmann::json tg = nlohmann::json::object();
        for (const auto& [k, v] : f.tags) tg[k] = v;
        o["t"] = std::move(tg);
        nlohmann::json rs = nlohmann::json::array();
        for (const auto& r : f.rings) {
            nlohmann::json flat = nlohmann::json::array();
            for (const LonLat& p : r) { flat.push_back(fixed7(p.lon)); flat.push_back(fixed7(p.lat)); }
            rs.push_back(std::move(flat));
        }
        o["r"] = std::move(rs);
        bool anyHole = false;
        for (std::uint8_t h : f.hole) anyHole |= h != 0;
        if (anyHole) o["h"] = f.hole;
        arr.push_back(std::move(o));
    }
    j["f"] = std::move(arr);
    return j.dump();
}

bool parseTile(const std::string& text, std::vector<Feature>& out, std::string* err) {
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("f") || !j["f"].is_array()) {
        if (err) *err = "not a geometry tile";
        return false;
    }
    for (const nlohmann::json& o : j["f"]) {
        if (!o.is_object()) continue;
        Feature f;
        f.id = parseIdText(o.value("i", std::string()));
        f.geom = static_cast<Feature::Geom>(std::clamp(o.value("g", 0), 0, 2));
        if (o.contains("t") && o["t"].is_object())
            for (auto it = o["t"].begin(); it != o["t"].end(); ++it)
                if (it.value().is_string()) f.tags.emplace_back(it.key(), it.value().get<std::string>());
        if (o.contains("r") && o["r"].is_array())
            for (const nlohmann::json& r : o["r"]) {
                std::vector<LonLat> ring;
                if (!r.is_array()) continue;
                ring.reserve(r.size() / 2);
                for (std::size_t k = 0; k + 1 < r.size(); k += 2)
                    ring.push_back({unfix7(r[k].get<std::int64_t>()), unfix7(r[k + 1].get<std::int64_t>())});
                f.rings.push_back(std::move(ring));
            }
        f.hole.assign(f.rings.size(), 0);
        if (o.contains("h") && o["h"].is_array())
            for (std::size_t k = 0; k < o["h"].size() && k < f.hole.size(); ++k)
                f.hole[k] = static_cast<std::uint8_t>(o["h"][k].get<int>() != 0);
        if (!f.rings.empty()) out.push_back(std::move(f));
    }
    return true;
}

// --- Elevation ------------------------------------------------------------------------

DemMosaic::DemMosaic(const std::vector<DemTile>& tiles) {
    if (tiles.empty()) return;
    m_zoom = tiles.front().id.z;
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    for (const DemTile& t : tiles) {
        if (t.id.z != m_zoom || t.w <= 0 || t.h <= 0) continue;
        x0 = std::min(x0, t.id.x); x1 = std::max(x1, t.id.x);
        y0 = std::min(y0, t.id.y); y1 = std::max(y1, t.id.y);
    }
    if (x1 < x0) { m_zoom = -1; return; }
    m_x0 = x0; m_y0 = y0; m_nx = x1 - x0 + 1; m_ny = y1 - y0 + 1;
    m_grid.assign(static_cast<std::size_t>(m_nx) * m_ny, nullptr);
    for (const DemTile& t : tiles)
        if (t.id.z == m_zoom && t.w == 256 && t.h == 256 && t.m.size() == 256u * 256u)
            m_grid[static_cast<std::size_t>(t.id.y - m_y0) * m_nx + (t.id.x - m_x0)] = &t;
}

const float* DemMosaic::pixel(long long px, long long py) const {
    const long long tx = px >> 8, ty = py >> 8;
    if (tx < m_x0 || ty < m_y0 || tx >= m_x0 + m_nx || ty >= m_y0 + m_ny) return nullptr;
    const DemTile* t = m_grid[static_cast<std::size_t>(ty - m_y0) * m_nx + static_cast<std::size_t>(tx - m_x0)];
    if (!t) return nullptr;
    return &t->m[static_cast<std::size_t>(py & 255) * 256 + static_cast<std::size_t>(px & 255)];
}

bool DemMosaic::sample(LonLat p, float& out) const {
    if (m_zoom < 0) return false;
    // Pixel centres at +0.5.
    const double gx = tileX(p.lon, m_zoom) * 256.0 - 0.5;
    const double gy = tileY(p.lat, m_zoom) * 256.0 - 0.5;
    const double fx = std::floor(gx), fy = std::floor(gy);
    const long long ix = static_cast<long long>(fx), iy = static_cast<long long>(fy);
    const float tx = static_cast<float>(gx - fx), ty = static_cast<float>(gy - fy);
    const float* a = pixel(ix, iy);
    const float* b = pixel(ix + 1, iy);
    const float* c = pixel(ix, iy + 1);
    const float* d = pixel(ix + 1, iy + 1);
    // At the edge of what was fetched, the neighbours stand in for a missing one.
    const float* any = a ? a : b ? b : c ? c : d;
    if (!any) return false;
    const float va = a ? *a : *any, vb = b ? *b : va, vc = c ? *c : va, vd = d ? *d : (c ? vc : vb);
    const float top = va + (vb - va) * tx;
    const float bot = vc + (vd - vc) * tx;
    out = top + (bot - top) * ty;
    return true;
}

double DemMosaic::pixelMetres(double lat) const {
    if (m_zoom < 0) return 0.0;
    return 40075016.686 * std::cos(lat * kPi / 180.0) / (256.0 * std::ldexp(1.0, m_zoom));
}

// --- Search -----------------------------------------------------------------------------

bool parseSearch(const std::string& text, std::vector<Place>& out) {
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_array()) return false;
    auto num = [](const nlohmann::json& v) {
        if (v.is_number()) return v.get<double>();
        if (v.is_string()) return std::atof(v.get<std::string>().c_str());
        return 0.0;
    };
    for (const nlohmann::json& o : j) {
        if (!o.is_object()) continue;
        Place p;
        p.name = o.value("display_name", o.value("name", std::string()));
        p.at.lat = num(o.value("lat", nlohmann::json()));
        p.at.lon = num(o.value("lon", nlohmann::json()));
        p.kind = o.value("addresstype", o.value("type", std::string()));
        if (o.contains("boundingbox") && o["boundingbox"].is_array() && o["boundingbox"].size() == 4) {
            const nlohmann::json& b = o["boundingbox"];
            p.box = {num(b[0]), num(b[2]), num(b[1]), num(b[3])};
        } else {
            p.box = squareAround(p.at, 2000.0);
        }
        out.push_back(std::move(p));
    }
    return true;
}

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += static_cast<char>(c);
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

std::string urlDecode(const std::string& s) {
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') o += ' ';
        else if (s[i] == '%' && i + 2 < s.size()) {
            auto hv = [](char c) { return c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0'; };
            o += static_cast<char>(hv(s[i + 1]) * 16 + hv(s[i + 2]));
            i += 2;
        } else {
            o += s[i];
        }
    }
    return o;
}

} // namespace geo
