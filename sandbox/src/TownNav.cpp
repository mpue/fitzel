#include "TownNav.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <queue>
#include <set>
#include <unordered_map>
#include <utility>

#include "CivicGen.hpp"

namespace townnav {

namespace {

glm::vec2 xz(const glm::vec3& p) { return {p.x, p.z}; }

// Where segments a0-a1 and b0-b1 cross, as the parameter along a; false when
// they do not.
bool crosses(glm::vec2 a0, glm::vec2 a1, glm::vec2 b0, glm::vec2 b1) {
    const glm::vec2 r = a1 - a0, s = b1 - b0;
    const float den = r.x * s.y - r.y * s.x;
    if (std::abs(den) < 1e-6f) return false;
    const glm::vec2 q = b0 - a0;
    const float t = (q.x * s.y - q.y * s.x) / den;
    const float u = (q.x * r.y - q.y * r.x) / den;
    return t >= 0.0f && t <= 1.0f && u >= 0.0f && u <= 1.0f;
}

// The nearest point of a road's centreline to `p`: which road, how far along
// it (metres from its start), which side (+1 left of its run, -1 right), and
// the distance.
struct OnRoad {
    int   road = -1;
    float along = 0.0f, dist = 1e30f;
    int   side = 1;
};

OnRoad nearestRoad(const std::vector<cityplan::RoadLine>& roads, glm::vec2 p, bool namedOnly) {
    OnRoad best;
    for (std::size_t r = 0; r < roads.size(); ++r) {
        const auto& pts = roads[r].pts;
        if (namedOnly && roads[r].name.empty()) continue;
        float run = 0.0f;
        for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
            const glm::vec2 a = pts[i], b = pts[i + 1], ab = b - a;
            const float len2 = glm::dot(ab, ab);
            const float len  = std::sqrt(len2);
            const float t = len2 > 1e-8f ? glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
            const float d = glm::length(p - (a + ab * t));
            if (d < best.dist) {
                best.road  = static_cast<int>(r);
                best.dist  = d;
                best.along = run + len * t;
                best.side  = (ab.x * (p.y - a.y) - ab.y * (p.x - a.x)) >= 0.0f ? 1 : -1;
            }
            run += len;
        }
    }
    return best;
}

struct CivicName { const char* name; const char* kind; };

CivicName civicName(int k) {
    switch (static_cast<civic::Kind>(k)) {
        case civic::Kind::Church:        return {"Kirche", "church"};
        case civic::Kind::Police:        return {"Polizeiwache", "police"};
        case civic::Kind::FireStation:   return {"Feuerwache", "firestation"};
        case civic::Kind::Hospital:      return {"Krankenhaus", "hospital"};
        case civic::Kind::Industry:      return {"Gewerbegebiet", "industry"};
        case civic::Kind::TownHall:      return {"Rathaus", "townhall"};
        case civic::Kind::School:        return {"Schule", "school"};
        case civic::Kind::Kindergarten:  return {"Kindergarten", "kindergarten"};
        case civic::Kind::Library:       return {"Bibliothek", "library"};
        case civic::Kind::Museum:        return {"Museum", "museum"};
        case civic::Kind::Theatre:       return {"Theater", "theatre"};
        case civic::Kind::Pool:          return {"Schwimmbad", "pool"};
        case civic::Kind::PetrolStation: return {"Tankstelle", "petrol"};
        case civic::Kind::PowerPlant:    return {"Kraftwerk", "powerplant"};
        case civic::Kind::Landfill:      return {"Wertstoffhof", "landfill"};
        case civic::Kind::Station:       return {"Bahnhof", "station"};
        default:                         return {"Gebäude", "civic"};
    }
}

const char* buildingKind(cityplan::Zone z) {
    switch (z) {
        case cityplan::Zone::Houses:
        case cityplan::Zone::Rows:     return "home";
        case cityplan::Zone::Blocks:   return "flat";
        case cityplan::Zone::Towers:   return "office";
        case cityplan::Zone::Industry: return "works";
        default:                       return nullptr;
    }
}

} // namespace

// --- The network ---------------------------------------------------------------

void Nav::build(const std::vector<std::vector<glm::vec3>>& walks,
                const std::vector<cityplan::RoadLine>& roads) {
    m_pts.clear();
    m_edges.clear();
    m_crossings = 0;

    // A block corner: where the walk turns. Only there does anyone cross.
    struct Corner { int node; int walk; };
    std::vector<Corner> corners;
    auto link = [&](int a, int b, float cost) {
        m_edges[static_cast<std::size_t>(a)].push_back({b, cost});
        m_edges[static_cast<std::size_t>(b)].push_back({a, cost});
    };
    for (std::size_t w = 0; w < walks.size(); ++w) {
        const auto& loop = walks[w];
        const int n = static_cast<int>(loop.size());
        if (n < 3) continue;
        const int base = static_cast<int>(m_pts.size());
        m_pts.insert(m_pts.end(), loop.begin(), loop.end());
        m_edges.resize(m_pts.size());
        for (int i = 0; i < n; ++i) {
            const int j = (i + 1) % n;
            link(base + i, base + j, glm::length(loop[static_cast<std::size_t>(j)] -
                                                 loop[static_cast<std::size_t>(i)]));
            const glm::vec2 p0 = xz(loop[static_cast<std::size_t>((i + n - 1) % n)]);
            const glm::vec2 p1 = xz(loop[static_cast<std::size_t>(i)]);
            const glm::vec2 p2 = xz(loop[static_cast<std::size_t>(j)]);
            const glm::vec2 a = p1 - p0, b = p2 - p1;
            const float la = glm::length(a), lb = glm::length(b);
            if (la < 1e-3f || lb < 1e-3f) continue;
            if (glm::dot(a, b) / (la * lb) < std::cos(glm::radians(25.0f)))
                corners.push_back({base + i, static_cast<int>(w)});
        }
    }

    // Across the street: a corner to the nearest corner of each other block
    // within reach, when the line between them crosses exactly one street, and
    // that one roughly square on -- not diagonally over a junction (two
    // streets), not along the street (a shallow angle).
    constexpr float kReach = 28.0f;
    const float squareOn = std::cos(glm::radians(55.0f));
    std::unordered_map<long long, std::vector<int>> grid;   // 32 m cells -> corners
    auto cellOf = [](glm::vec2 p) {
        return std::make_pair(static_cast<int>(std::floor(p.x / 32.0f)),
                              static_cast<int>(std::floor(p.y / 32.0f)));
    };
    auto key = [](int cx, int cz) {
        return (static_cast<long long>(cx) << 32) ^ static_cast<unsigned int>(cz);
    };
    for (std::size_t c = 0; c < corners.size(); ++c) {
        const auto [cx, cz] = cellOf(xz(m_pts[static_cast<std::size_t>(corners[c].node)]));
        grid[key(cx, cz)].push_back(static_cast<int>(c));
    }
    std::set<std::pair<int, int>> made;
    for (const Corner& c : corners) {
        const glm::vec2 p = xz(m_pts[static_cast<std::size_t>(c.node)]);
        const auto [cx, cz] = cellOf(p);
        std::map<int, std::pair<float, int>> bestPerWalk;   // walk -> (dist, node)
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                const auto it = grid.find(key(cx + dx, cz + dz));
                if (it == grid.end()) continue;
                for (const int o : it->second) {
                    const Corner& k = corners[static_cast<std::size_t>(o)];
                    if (k.walk == c.walk) continue;
                    const glm::vec2 q = xz(m_pts[static_cast<std::size_t>(k.node)]);
                    const float d = glm::length(q - p);
                    if (d > kReach || d < 0.5f) continue;
                    int over = 0;
                    bool square = true;
                    for (const auto& r : roads) {
                        if (r.pts.size() < 2) continue;
                        for (std::size_t i = 0; i + 1 < r.pts.size() && over < 2; ++i) {
                            if (!crosses(p, q, r.pts[i], r.pts[i + 1])) continue;
                            ++over;
                            const glm::vec2 rd = r.pts[i + 1] - r.pts[i];
                            const float rl = glm::length(rd);
                            if (rl > 1e-4f && std::abs(glm::dot(rd / rl, (q - p) / d)) > squareOn)
                                square = false;
                        }
                        if (over >= 2) break;
                    }
                    if (over != 1 || !square) continue;
                    const auto had = bestPerWalk.find(k.walk);
                    if (had == bestPerWalk.end() || d < had->second.first)
                        bestPerWalk[k.walk] = {d, k.node};
                }
            }
        for (const auto& [walk, b] : bestPerWalk) {
            const auto pr = std::minmax(c.node, b.second);
            if (!made.insert(pr).second) continue;
            // A little reluctance to step into the road: of two ways about as
            // long, the one with fewer crossings.
            link(c.node, b.second, b.first * 1.3f + 3.0f);
            ++m_crossings;
        }
    }
}

int Nav::nearest(glm::vec2 p) const {
    int best = -1;
    float bd = 1e30f;
    for (std::size_t i = 0; i < m_pts.size(); ++i) {
        const glm::vec2 d = xz(m_pts[i]) - p;
        const float dd = glm::dot(d, d);
        if (dd < bd) { bd = dd; best = static_cast<int>(i); }
    }
    return best;
}

std::vector<glm::vec3> Nav::path(glm::vec2 from, glm::vec2 to) const {
    const int a = nearest(from), b = nearest(to);
    if (a < 0 || b < 0) return {};
    if (a == b) return {m_pts[static_cast<std::size_t>(a)]};
    const glm::vec2 goal = xz(m_pts[static_cast<std::size_t>(b)]);
    std::vector<float> cost(m_pts.size(), 1e30f);
    std::vector<int>   prev(m_pts.size(), -1);
    using Item = std::pair<float, int>;   // (cost + estimate, node)
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
    cost[static_cast<std::size_t>(a)] = 0.0f;
    open.push({glm::length(xz(m_pts[static_cast<std::size_t>(a)]) - goal), a});
    while (!open.empty()) {
        const auto [f, n] = open.top();
        open.pop();
        if (n == b) break;
        const float g = cost[static_cast<std::size_t>(n)];
        if (f - glm::length(xz(m_pts[static_cast<std::size_t>(n)]) - goal) > g + 1e-3f) continue;
        for (const Edge& e : m_edges[static_cast<std::size_t>(n)]) {
            const float ng = g + e.cost;
            if (ng < cost[static_cast<std::size_t>(e.to)]) {
                cost[static_cast<std::size_t>(e.to)] = ng;
                prev[static_cast<std::size_t>(e.to)] = n;
                open.push({ng + glm::length(xz(m_pts[static_cast<std::size_t>(e.to)]) - goal), e.to});
            }
        }
    }
    if (prev[static_cast<std::size_t>(b)] < 0) return {};
    std::vector<glm::vec3> out;
    for (int n = b; n >= 0; n = prev[static_cast<std::size_t>(n)]) {
        out.push_back(m_pts[static_cast<std::size_t>(n)]);
        if (n == a) break;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

// --- Streets and places -------------------------------------------------------------

std::string streetAt(const std::vector<cityplan::RoadLine>& roads, glm::vec2 p,
                     float maxDist, float* dist) {
    const OnRoad r = nearestRoad(roads, p, true);
    if (dist) *dist = r.dist;
    if (r.road < 0 || r.dist > maxDist) return {};
    return roads[static_cast<std::size_t>(r.road)].name;
}

std::vector<Place> places(int index, const cityplan::Rule& rule, const cityplan::Town& town,
                          const std::vector<cityplan::RoadLine>& roads, const Nav& nav) {
    std::vector<Place> out;
    if (nav.empty()) return out;
    // On the pavement nearest `at`, if there is pavement near enough.
    auto spot = [&](glm::vec2 at, float reach, Place& p) {
        const int n = nav.nearest(at);
        if (n < 0) return false;
        const glm::vec3 q = nav.point(n);
        if (glm::length(xz(q) - at) > reach) return false;
        p.pos  = q;
        p.at   = at;
        p.town = index;
        p.street = streetAt(roads, xz(q), 30.0f);
        return true;
    };

    // The civic blocks and the parks, named by what they are -- and by their
    // street too when the town has more than one of a kind.
    const cityplan::Layout lay = cityplan::layout(rule);
    std::vector<Place> civic;
    for (const cityplan::Block& b : lay.blocks) {
        const glm::vec2 c = 0.25f * (b.corner[0] + b.corner[1] + b.corner[2] + b.corner[3]);
        Place p;
        if (b.civic >= 0) {
            const CivicName cn = civicName(b.civic);
            p.name = cn.name;
            p.kind = cn.kind;
        } else if (b.zone == cityplan::Zone::Park) {
            p.name = "Park";
            p.kind = "park";
        } else {
            continue;
        }
        if (spot(c, 80.0f, p)) civic.push_back(std::move(p));
    }
    std::map<std::string, int> seen;
    for (const Place& p : civic) ++seen[p.kind];
    for (Place& p : civic) {
        if (seen[p.kind] > 1 && !p.street.empty()) p.name += " (" + p.street + ")";
        else if (p.kind == "park" && seen[p.kind] == 1) p.name = "Stadtpark";
        out.push_back(std::move(p));
    }

    for (const cityplan::Stop& s : town.stops) {
        Place p;
        p.kind = "stop";
        if (!spot(s.pos, 15.0f, p)) continue;
        p.name = p.street.empty() ? std::string("Bushaltestelle")
                                  : "Bushaltestelle " + p.street;
        out.push_back(std::move(p));
    }

    // The buildings, with house numbers: along each street from where it
    // starts, odd on its left, even on its right.
    struct Addr { std::size_t place; float along; int side; };
    std::map<int, std::vector<Addr>> byRoad;
    std::vector<Place> houses;
    for (const cityplan::Placed& b : town.placed) {
        const char* kind = buildingKind(b.zone);
        if (!kind) continue;
        Place p;
        p.kind = kind;
        if (!spot(b.pos, b.radius + 25.0f, p)) continue;
        const OnRoad r = nearestRoad(roads, b.pos, true);
        if (r.road < 0 || r.dist > b.radius + 40.0f) continue;
        p.street = roads[static_cast<std::size_t>(r.road)].name;
        byRoad[r.road].push_back({houses.size(), r.along, r.side});
        houses.push_back(std::move(p));
    }
    for (auto& [road, list] : byRoad) {
        std::sort(list.begin(), list.end(),
                  [](const Addr& a, const Addr& b) { return a.along < b.along; });
        int odd = 1, even = 2;
        for (const Addr& a : list) {
            Place& p = houses[a.place];
            if (a.side > 0) { p.number = odd;  odd  += 2; }
            else            { p.number = even; even += 2; }
            p.name = p.street + " " + std::to_string(p.number);
        }
    }
    for (Place& p : houses) out.push_back(std::move(p));
    return out;
}

} // namespace townnav
