#include "TrafficSim.hpp"

#include <algorithm>
#include <cmath>

#include "CivicGen.hpp"   // civic::signalPhase

namespace traffic {

namespace {

std::uint32_t nextRand(std::uint32_t& s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}
float unitRand(std::uint32_t& s) { return static_cast<float>(nextRand(s) & 0xffffffU) / 16777215.0f; }

// A lane's right-hand side for a heading in XZ (x east, z south): heading north
// (0,-1) has its right to the east (1,0).
glm::vec2 rightOf(glm::vec2 d) { return {-d.y, d.x}; }

float cross2(glm::vec2 a, glm::vec2 b) { return a.x * b.y - a.y * b.x; }

// The turn's control point: where the incoming lane's line meets the outgoing
// one's. Straight on it is the middle; a U-turn bulges on past the stop line.
glm::vec2 controlPoint(const Lane& a, const Lane& b) {
    const float den = cross2(a.dir, b.dir);
    const glm::vec2 mid = 0.5f * (a.p1 + b.p0);
    if (std::abs(den) < 0.2f)
        return glm::dot(a.dir, b.dir) > 0.0f ? mid : mid + a.dir * 6.0f;
    const float t = cross2(b.p0 - a.p1, b.dir) / den;
    return a.p1 + a.dir * t;
}

glm::vec2 bezier(glm::vec2 p0, glm::vec2 c, glm::vec2 p2, float t, glm::vec2& tangent) {
    const float u = 1.0f - t;
    tangent = 2.0f * u * (c - p0) + 2.0f * t * (p2 - c);
    return u * u * p0 + 2.0f * u * t * c + t * t * p2;
}

} // namespace

float kindLength(Kind k) {
    switch (k) {
        case Kind::Bus:   return 12.0f;
        case Kind::Truck: return 9.5f;
        default:          return 4.3f;
    }
}

float kindSpeed(Kind k) {
    switch (k) {
        case Kind::Bus:   return 11.0f;   // ~40 km/h
        case Kind::Truck: return 11.5f;
        default:          return 13.5f;   // ~50 km/h, a town's limit
    }
}

float jolt(glm::vec3 vel, glm::vec3 asked, glm::vec3 spin, glm::vec3 askedSpin, float reach) {
    return std::max(glm::length(vel - asked), glm::length(spin - askedSpin) * reach);
}

float Lane::heightAt(float s) const {
    if (y.empty()) return 0.0f;
    const float f = std::clamp(s / Sim::kStep, 0.0f, static_cast<float>(y.size() - 1));
    const std::size_t i = std::min(static_cast<std::size_t>(f), y.size() - 1);
    const std::size_t j = std::min(i + 1, y.size() - 1);
    return y[i] + (y[j] - y[i]) * (f - static_cast<float>(i));
}

void Sim::clear() {
    ++m_generation;
    m_nodes.clear();
    m_lanes.clear();
    m_vehicles.clear();
    m_blocks.clear();
    m_walkers.clear();
    m_walks.clear();
    m_walkLen.clear();
}

void Sim::build(const std::vector<cityplan::Rule>& rules,
                const std::vector<const cityplan::Town*>& towns, std::uint32_t seed,
                const std::function<void(Vehicle&)>& dress,
                const std::function<void(Walker&)>& dressWalker) {
    clear();
    for (std::size_t i = 0; i < rules.size() && i < towns.size(); ++i) {
        if (!towns[i] || !rules[i].enabled) continue;
        cityplan::Rule r = rules[i];
        r.seed ^= seed * 0x9e3779b9U;
        addTown(r, *towns[i], static_cast<int>(i), dress, dressWalker);
    }
}

bool Sim::addDriver(int entity, glm::vec2 pos, glm::vec2 heading, Kind kind, float length,
                    float vmax) {
    if (glm::length(heading) < 1e-4f) heading = {0.0f, 1.0f};
    heading = glm::normalize(heading);
    int   best = -1;
    float bestCost = 60.0f, bestS = 0.0f;
    for (int l = 0; l < static_cast<int>(m_lanes.size()); ++l) {
        const Lane& L = m_lanes[static_cast<std::size_t>(l)];
        const float along = glm::clamp(glm::dot(pos - L.p0, L.dir), 0.0f, L.len);
        const float dist  = glm::length(pos - (L.p0 + L.dir * along));
        // Mostly how far, a little which way: a car parked across the street
        // from its lane still takes its own side.
        const float cost = dist + 6.0f * (1.0f - glm::dot(L.dir, heading));
        if (cost < bestCost) { bestCost = cost; best = l; bestS = along; }
    }
    if (best < 0) return false;
    Vehicle v;
    v.kind   = kind;
    v.lane   = best;
    v.length = std::max(length, 1.0f);
    v.vmax   = std::max(vmax, 0.5f);
    v.s      = std::clamp(bestS, std::min(v.length, m_lanes[static_cast<std::size_t>(best)].len),
                          m_lanes[static_cast<std::size_t>(best)].len);
    v.entity = entity;
    v.uid    = m_nextUid++;
    v.rng    = 0x9e3779b9U ^ static_cast<std::uint32_t>(entity * 7919 + 1);
    v.next   = pickNext(v);
    m_vehicles.push_back(v);
    return true;
}

bool Sim::removeVehicle(std::uint32_t uid) {
    const auto it = std::find_if(m_vehicles.begin(), m_vehicles.end(),
                                 [uid](const Vehicle& v) { return v.uid == uid; });
    if (it == m_vehicles.end()) return false;
    m_vehicles.erase(it);
    return true;
}

void Sim::removeDriver(int entity) {
    m_vehicles.erase(std::remove_if(m_vehicles.begin(), m_vehicles.end(),
                                    [entity](const Vehicle& v) { return v.entity == entity; }),
                     m_vehicles.end());
}

void Sim::removeDrivers() {
    m_vehicles.erase(std::remove_if(m_vehicles.begin(), m_vehicles.end(),
                                    [](const Vehicle& v) { return v.entity >= 0; }),
                     m_vehicles.end());
}

void Sim::setObstacles(const std::vector<Obstacle>& obstacles) {
    // A lane is followed on into the crossings at its ends, along its own line:
    // a wreck in the middle of one stops who would drive through it straight
    // on (they wait at the stop line), not only who is on the lane it lies in.
    constexpr float kIntoCrossing = 12.0f;
    m_blocks.clear();
    for (const Obstacle& o : obstacles)
        for (int l = 0; l < static_cast<int>(m_lanes.size()); ++l) {
            const Lane& L = m_lanes[static_cast<std::size_t>(l)];
            const glm::vec2 side = rightOf(L.dir);
            // The box's reach along the lane, across it and up.
            float ra = 0.0f, rs = 0.0f, ry = 0.0f;
            for (const glm::vec3& a : o.axes) {
                ra += std::abs(a.x * L.dir.x + a.z * L.dir.y);
                rs += std::abs(a.x * side.x + a.z * side.y);
                ry += std::abs(a.y);
            }
            const glm::vec2 d = glm::vec2(o.center.x, o.center.z) - L.p0;
            const float s = glm::dot(d, L.dir);
            // A car keeps to the middle of its lane and is about 2 m wide.
            if (std::abs(glm::dot(d, side)) > rs + 1.1f) continue;
            if (s + ra < -kIntoCrossing || s - ra > L.len + kIntoCrossing) continue;
            // ...and it is in the street, not on a bridge over it or under it.
            const float y = L.heightAt(s);
            if (o.center.y - ry > y + 2.5f || o.center.y + ry < y - 0.5f) continue;
            m_blocks.push_back({l, s - ra, s + ra, glm::dot(o.vel, L.dir), o.giveWay});
        }
}

void Sim::addTown(const cityplan::Rule& r, const cityplan::Town& t, int town,
                  const std::function<void(Vehicle&)>& dress,
                  const std::function<void(Walker&)>& dressWalker) {
    const cityplan::Layout lay = cityplan::layout(r);
    if (lay.nx <= 0 || lay.nz <= 0) return;
    std::uint32_t rng = (r.seed ^ 0x7a11c0deU) | 1U;

    // --- Nodes, and which of them have lights ----------------------------------
    const int base = static_cast<int>(m_nodes.size());
    for (const glm::vec2& p : lay.nodes) m_nodes.push_back({p, false, {}});
    const int nodeEnd = static_cast<int>(m_nodes.size());
    for (const glm::vec2& s : t.signals)
        for (int n = base; n < nodeEnd; ++n)
            if (glm::distance(m_nodes[static_cast<std::size_t>(n)].pos, s) < 2.0f)
                m_nodes[static_cast<std::size_t>(n)].signal = true;
    auto nodeAt = [&](int i, int j) { return base + j * (lay.nx + 1) + i; };

    // --- Lanes: two per street between neighbouring nodes ----------------------
    std::vector<int> zLine(static_cast<std::size_t>(lay.nx + 1), -1);
    std::vector<int> xLine(static_cast<std::size_t>(lay.nz + 1), -1);
    for (int s = 0; s < static_cast<int>(lay.streets.size()); ++s) {
        const cityplan::Street& st = lay.streets[static_cast<std::size_t>(s)];
        auto& line = st.alongZ ? zLine : xLine;
        if (st.line >= 0 && st.line < static_cast<int>(line.size()))
            line[static_cast<std::size_t>(st.line)] = s;
    }
    auto halfOf = [&](int street) {
        return street >= 0 ? 0.5f * lay.streets[static_cast<std::size_t>(street)].width : 3.5f;
    };
    const int laneBase = static_cast<int>(m_lanes.size());
    for (const cityplan::Street& st : lay.streets) {
        const int count = st.alongZ ? lay.nz : lay.nx;
        const float half = 0.5f * st.width;
        const std::vector<int>& cross = st.alongZ ? xLine : zLine;
        for (int m = 0; m < count; ++m) {
            const int a = st.alongZ ? nodeAt(st.line, m) : nodeAt(m, st.line);
            const int b = st.alongZ ? nodeAt(st.line, m + 1) : nodeAt(m + 1, st.line);
            const float ha = halfOf(cross[static_cast<std::size_t>(m)]);
            const float hb = halfOf(cross[static_cast<std::size_t>(m + 1)]);
            for (int d = 0; d < 2; ++d) {
                const int from = d == 0 ? a : b, to = d == 0 ? b : a;
                const float trimFrom = (d == 0 ? ha : hb) + 1.5f, trimTo = (d == 0 ? hb : ha) + 1.5f;
                const glm::vec2 pf = m_nodes[static_cast<std::size_t>(from)].pos;
                const glm::vec2 pt = m_nodes[static_cast<std::size_t>(to)].pos;
                Lane L;
                L.from = from;
                L.to   = to;
                L.axis = st.alongZ ? 1 : 0;
                L.half = half;
                L.dir  = glm::normalize(pt - pf);
                const glm::vec2 off = rightOf(L.dir) * (0.5f * half);
                L.p0  = pf + L.dir * trimFrom + off;
                L.p1  = pt - L.dir * trimTo + off;
                L.len = glm::dot(L.p1 - L.p0, L.dir);
                if (L.len < 4.0f) continue;
                // Only where a road is laid: every sample on a surface.
                bool drivable = true;
                const int n = static_cast<int>(std::ceil(L.len / kStep));
                for (int k = 0; k <= n && drivable; ++k) {
                    const glm::vec2 p = L.p0 + L.dir * std::min(k * kStep, L.len);
                    float y = 0.0f;
                    drivable = surfaceAt && surfaceAt(p, y);
                    L.y.push_back(y);
                }
                if (!drivable) continue;
                m_nodes[static_cast<std::size_t>(from)].out.push_back(static_cast<int>(m_lanes.size()));
                m_lanes.push_back(std::move(L));
            }
        }
    }
    const int laneEnd = static_cast<int>(m_lanes.size());

    // --- Bus stops onto the lanes that pass them ---------------------------------
    for (const cityplan::Stop& st : t.stops) {
        int best = -1;
        float bestLat = 1e9f, bestAlong = 0.0f;
        for (int l = laneBase; l < laneEnd; ++l) {
            const Lane& L = m_lanes[static_cast<std::size_t>(l)];
            if (glm::dot(L.dir, st.heading) < 0.9f) continue;
            const float along = glm::dot(st.pos - L.p0, L.dir);
            const float lat   = std::abs(glm::dot(st.pos - L.p0, rightOf(L.dir)));
            if (along < 0.0f || along > L.len - 1.0f || lat > L.half + 5.0f) continue;
            if (lat < bestLat) { bestLat = lat; best = l; bestAlong = along; }
        }
        if (best < 0) continue;
        Lane& L = m_lanes[static_cast<std::size_t>(best)];
        // The bus pulls up with its front a little past the sign.
        L.stops.push_back(std::min(bestAlong + 2.0f, L.len - 0.5f));
        std::sort(L.stops.begin(), L.stops.end());
    }

    // --- Vehicles ------------------------------------------------------------------
    if (r.traffic > 0.0f && laneEnd > laneBase) {
        float km = 0.0f;
        for (int l = laneBase; l < laneEnd; ++l) km += m_lanes[static_cast<std::size_t>(l)].len;
        km *= 0.5f / 1000.0f;   // two lanes per street
        const int want = std::min(static_cast<int>(std::lround(r.traffic * km)), 400);
        for (int i = 0; i < want; ++i) {
            Vehicle v;
            v.rng = nextRand(rng) | 1U;
            const float pick = unitRand(rng);
            v.kind = pick < r.busShare ? Kind::Bus
                   : pick < r.busShare + r.truckShare ? Kind::Truck : Kind::Car;
            v.length = kindLength(v.kind);
            v.vmax   = kindSpeed(v.kind) * (0.9f + 0.2f * unitRand(rng));
            v.look   = static_cast<int>(nextRand(rng) % 5);
            v.town   = town;
            if (dress) dress(v);
            bool placed = false;
            for (int tries = 0; tries < 30 && !placed; ++tries) {
                const int l = laneBase + static_cast<int>(nextRand(rng) % static_cast<std::uint32_t>(laneEnd - laneBase));
                const Lane& L = m_lanes[static_cast<std::size_t>(l)];
                if (L.len < v.length + 2.0f) continue;
                const float s = v.length + (L.len - v.length) * unitRand(rng);
                bool clear = true;
                for (const Vehicle& o : m_vehicles)
                    if (o.lane == l && std::abs(o.s - s) < 0.5f * (o.length + v.length) + 6.0f) clear = false;
                if (!clear) continue;
                v.lane = l;
                v.s    = s;
                placed = true;
            }
            if (!placed) continue;
            v.next = pickNext(v);
            v.uid  = m_nextUid++;
            m_vehicles.push_back(v);
        }
    }

    // --- People on the walks round the blocks --------------------------------------
    for (const std::vector<glm::vec3>& w : t.walks) {
        if (w.size() < 3) continue;
        std::vector<float> cum{0.0f};
        for (std::size_t i = 0; i < w.size(); ++i) {
            const glm::vec3& a = w[i];
            const glm::vec3& b = w[(i + 1) % w.size()];
            cum.push_back(cum.back() + glm::length(glm::vec2(b.x - a.x, b.z - a.z)));
        }
        const int index = static_cast<int>(m_walks.size());
        m_walks.push_back(w);
        m_walkLen.push_back(cum);
        const int n = std::min(static_cast<int>(std::lround(r.people * cum.back() / 100.0f)), 200);
        for (int i = 0; i < n; ++i) {
            Walker p;
            p.walk  = index;
            p.s     = cum.back() * unitRand(rng);
            p.dir   = (nextRand(rng) & 1U) ? 1 : -1;
            p.speed = 1.1f + 0.5f * unitRand(rng);
            p.phase = 6.2831853f * unitRand(rng);
            p.look  = static_cast<int>(nextRand(rng) % 4);
            p.town  = town;
            p.rng   = nextRand(rng) | 1U;
            if (dressWalker) dressWalker(p);
            m_walkers.push_back(p);
        }
    }
}

int Sim::pickNext(Vehicle& v) const {
    if (v.lane < 0 || v.lane >= static_cast<int>(m_lanes.size())) return -1;
    const Lane& L = m_lanes[static_cast<std::size_t>(v.lane)];
    const Node& N = m_nodes[static_cast<std::size_t>(L.to)];
    std::vector<int> options;
    for (int o : N.out)
        if (m_lanes[static_cast<std::size_t>(o)].to != L.from) options.push_back(o);
    if (options.empty()) options = N.out;   // a dead end: turn round
    if (options.empty()) return -1;
    return options[nextRand(v.rng) % options.size()];
}

float Sim::turnLength(int a, int b) const {
    if (a < 0 || b < 0) return 0.0f;
    const Lane& A = m_lanes[static_cast<std::size_t>(a)];
    const Lane& B = m_lanes[static_cast<std::size_t>(b)];
    const glm::vec2 c = controlPoint(A, B);
    float len = 0.0f;
    glm::vec2 t, prev = A.p1;
    for (int k = 1; k <= 12; ++k) {
        const glm::vec2 p = bezier(A.p1, c, B.p0, static_cast<float>(k) / 12.0f, t);
        len += glm::length(p - prev);
        prev = p;
    }
    return std::max(len, 0.5f);
}

void Sim::turnPoint(int a, int b, float s, glm::vec2& p, glm::vec2& t) const {
    const Lane& A = m_lanes[static_cast<std::size_t>(a)];
    const Lane& B = m_lanes[static_cast<std::size_t>(b)];
    const glm::vec2 c = controlPoint(A, B);
    // By distance along the curve, not by its parameter: a Bezier's parameter
    // runs faster through the wide part than the tight one, and a point slid
    // along by a steady `s` would speed up and slow down through the corner.
    // The same 12 chords turnLength measures, so s = turnLength is the end.
    float u = 1.0f, acc = 0.0f, uPrev = 0.0f;
    glm::vec2 prev = A.p1;
    for (int k = 1; k <= 12; ++k) {
        const float uk = static_cast<float>(k) / 12.0f;
        const glm::vec2 q = bezier(A.p1, c, B.p0, uk, t);
        const float seg = glm::length(q - prev);
        if (acc + seg >= s) {
            u = uPrev + (uk - uPrev) * (seg > 1e-6f ? std::clamp((s - acc) / seg, 0.0f, 1.0f) : 0.0f);
            break;
        }
        acc  += seg;
        prev  = q;
        uPrev = uk;
    }
    p = bezier(A.p1, c, B.p0, std::clamp(u, 0.0f, 1.0f), t);
    if (glm::dot(t, t) < 1e-8f) t = A.dir;
    t = glm::normalize(t);
}

float Sim::turnSpeed(int a, int b, float vmax) const {
    // As fast as the corner allows at a comfortable sideways pull (a town
    // driver's ~0.25 g), its radius being the turn's length over the angle it
    // turns through. A fixed turning speed took a village's 2 m corner at the
    // pace of a wide city one -- 1.7 g, the car flung round it.
    const float c = glm::clamp(glm::dot(m_lanes[static_cast<std::size_t>(a)].dir,
                                        m_lanes[static_cast<std::size_t>(b)].dir), -1.0f, 1.0f);
    const float angle = std::acos(c);
    if (angle < 0.35f) return vmax;   // straight on
    constexpr float kSideways = 2.5f;   // m/s^2
    const float radius = turnLength(a, b) / angle;
    return std::clamp(std::sqrt(kSideways * radius), 2.0f, vmax);
}

bool Sim::mustStop(int l, float dist, float v, double clock) const {
    const Lane& L = m_lanes[static_cast<std::size_t>(l)];
    if (!m_nodes[static_cast<std::size_t>(L.to)].signal) return false;
    civic::SignalLamps lamps[2];
    civic::signalPhase(clock, lamps[0], lamps[1]);
    const civic::SignalLamps& me = lamps[L.axis == 0 ? 0 : 1];
    if (me.red) return true;                    // red, and red + amber
    if (me.green) return false;
    // Amber: stop if that can still be done without slamming on the brakes.
    return dist > v * v / (2.0f * 3.5f) + 1.0f;
}

void Sim::step(float dt, double clock) {
    if (dt <= 0.0f) return;
    dt = std::min(dt, 0.1f);

    // --- People ----------------------------------------------------------------
    for (Walker& p : m_walkers) {
        if (p.led) {
            if (p.strides) p.phase += p.speed * dt * 6.2831853f / 1.4f;
            continue;
        }
        const float len = m_walkLen[static_cast<std::size_t>(p.walk)].back();
        if (len <= 0.0f) continue;
        p.s = std::fmod(p.s + static_cast<float>(p.dir) * p.speed * dt + len, len);
        p.phase += p.speed * dt * 6.2831853f / 1.4f;   // a stride every 1.4 m
    }

    // --- Who is where: per lane, sorted by position -----------------------------
    // A turning vehicle already counts on the lane it is turning into, at the
    // (negative) distance it still has to go -- so whoever follows it in sees it.
    // An obstacle's block counts by its far end, like a vehicle's front bumper;
    // its `who` is -1 - its index in m_blocks.
    struct Slot { float pos; int who; };
    std::vector<std::vector<Slot>> onLane(m_lanes.size());
    for (int i = 0; i < static_cast<int>(m_vehicles.size()); ++i) {
        const Vehicle& v = m_vehicles[static_cast<std::size_t>(i)];
        if (v.turning && v.next >= 0)
            onLane[static_cast<std::size_t>(v.next)].push_back({-(turnLength(v.lane, v.next) - v.s), i});
        else
            onLane[static_cast<std::size_t>(v.lane)].push_back({v.s, i});
    }
    for (int k = 0; k < static_cast<int>(m_blocks.size()); ++k)
        onLane[static_cast<std::size_t>(m_blocks[static_cast<std::size_t>(k)].lane)]
            .push_back({m_blocks[static_cast<std::size_t>(k)].front, -1 - k});
    for (auto& l : onLane)
        std::sort(l.begin(), l.end(), [](const Slot& a, const Slot& b) { return a.pos < b.pos; });
    auto backOf = [&](const Slot& o) {
        return o.who >= 0 ? o.pos - m_vehicles[static_cast<std::size_t>(o.who)].length
                          : m_blocks[static_cast<std::size_t>(-1 - o.who)].back;
    };
    auto speedOf = [&](const Slot& o) {
        return o.who >= 0 ? m_vehicles[static_cast<std::size_t>(o.who)].v
                          : m_blocks[static_cast<std::size_t>(-1 - o.who)].v;
    };

    for (int i = 0; i < static_cast<int>(m_vehicles.size()); ++i) {
        Vehicle& v = m_vehicles[static_cast<std::size_t>(i)];
        if (v.dwell > 0.0f) {
            v.dwell -= dt;
            v.v = 0.0f;
            continue;
        }
        const Lane& L = m_lanes[static_cast<std::size_t>(v.lane)];
        const float heavy = v.kind == Kind::Car ? 1.0f : 0.65f;
        const float aMax = 1.8f * heavy, bComf = 2.5f;
        float gap = 1e9f, vLead = 0.0f;
        auto lead = [&](float g, float vl) { if (g < gap) { gap = g; vLead = vl; } };

        // The vehicle ahead: on this lane, else the first on the next one --
        // and any obstacle before it (a long wreck's near end can be nearer
        // than the far end it is sorted by).
        const int regLane = v.turning ? v.next : v.lane;
        const float regPos = v.turning ? -(turnLength(v.lane, v.next) - v.s) : v.s;
        // Caught INSIDE an obstacle -- a tram set down on it, a wreck landed on
        // it -- a car drives on out rather than wait inside for ever (the tram
        // would wait for it in turn). Doing so it is committed, and gives way to
        // no right of way either.
        bool escaping = false;
        // A tram's right of way (Obstacle::giveWay) holds back only who has not
        // yet committed: once in it, or in a turn, a car drives on out -- and
        // on through the crossing ahead, whatever right of way lies beyond it,
        // because the tram it is clearing the way for is right behind it.
        bool clearing = v.turning;
        auto yieldsTo = [&](const Slot& o, float pos) {
            if (o.who >= 0) return true;
            const Block& b = m_blocks[static_cast<std::size_t>(-1 - o.who)];
            if (b.giveWay) {
                // (The right of way comes in pieces along the track: in one of
                // them, a car is clearing all of them.)
                if (!clearing && !escaping && !v.turning && pos < b.back - 0.5f) return true;
                clearing = true;
                return false;
            }
            if (pos > b.back + 0.3f) { escaping = true; clearing = true; return false; }
            return true;
        };
        auto giveWayBlock = [&](const Slot& o) {
            return o.who < 0 && m_blocks[static_cast<std::size_t>(-1 - o.who)].giveWay;
        };
        // Anything standing in the crossing ahead -- past this lane's stop line,
        // on its line -- is waited for AT the stop line, not inside the crossing.
        auto inCrossing = [&](const Slot& o) {
            return !v.turning && o.who < 0 &&
                   m_blocks[static_cast<std::size_t>(-1 - o.who)].back > L.len - 1.0f;
        };
        bool found = false;
        if (regLane >= 0)
            for (const Slot& o : onLane[static_cast<std::size_t>(regLane)])
                if (o.who != i && o.pos > regPos) {
                    if (!yieldsTo(o, regPos)) continue;
                    if (inCrossing(o)) lead(L.len - v.s - 0.3f, 0.0f);
                    else               lead(backOf(o) - regPos, speedOf(o));
                    found = true;
                    if (o.who >= 0) break;
                }
        float v0 = v.vmax;
        if (!v.turning) {
            const float toEnd = L.len - v.s;
            if (!found && v.next >= 0) {
                const float tl = turnLength(v.lane, v.next);
                const auto& nl = onLane[static_cast<std::size_t>(v.next)];
                for (const Slot& o : nl)
                    if (o.who != i) {
                        if (clearing && giveWayBlock(o)) continue;
                        lead(toEnd + tl + backOf(o), speedOf(o));
                        // Keep the crossing clear: into it only with room for the
                        // whole car beyond it. Else wait at the stop line -- a car
                        // standing in the crossing blocks the cross traffic, and a
                        // tram crossing there.
                        if (backOf(o) < v.length + 1.5f && speedOf(o) < 2.0f)
                            lead(toEnd - 0.3f, 0.0f);
                        if (o.who >= 0) break;
                    }
            }
            // The stop line, when the light says so.
            if (mustStop(v.lane, toEnd, v.v, clock)) lead(toEnd - 0.3f, 0.0f);
            // A bus calls at the next stop it has not called at yet.
            if (v.kind == Kind::Bus)
                for (float st : L.stops) {
                    if (st < v.s - 0.5f || (v.servedLane == v.lane && v.servedS == st)) continue;
                    // Brought to rest AT the stop: the model keeps its minimum
                    // gap to anything it follows, so the stop is set that far on.
                    const float d = st - v.s;
                    lead(d + 2.0f, 0.0f);
                    if (std::abs(d) < 1.0f && v.v < 0.5f) {
                        v.dwell      = 8.0f;
                        v.servedLane = v.lane;
                        v.servedS    = st;
                    }
                    break;
                }
            // Slow for the turn ahead: a speed that brakes comfortably to the
            // turning speed by the stop line.
            if (v.next >= 0) {
                const float vTurn = turnSpeed(v.lane, v.next, v.vmax);
                v0 = std::min(v0, std::sqrt(vTurn * vTurn + 2.0f * bComf * std::max(toEnd, 0.0f)));
            }
        } else {
            v0 = std::min(v0, turnSpeed(v.lane, v.next, v.vmax));
        }

        // Intelligent Driver Model.
        const float s0 = 2.0f, T = 1.2f;
        const float sStar = s0 + std::max(0.0f, v.v * T + v.v * (v.v - vLead) / (2.0f * std::sqrt(aMax * bComf)));
        const float g = std::max(gap, 0.01f);
        float acc = aMax * (1.0f - std::pow(v.v / std::max(v0, 0.1f), 4.0f) - (sStar / g) * (sStar / g));
        acc = std::max(acc, -9.0f);
        v.v = std::max(0.0f, v.v + acc * dt);
        // Never into the one ahead, whatever the numbers say.
        const float ds = std::min(v.v * dt, std::max(gap - 0.2f, 0.0f));
        if (ds < v.v * dt) v.v = ds / dt;
        v.s += ds;

        // Onwards: lane -> turn -> next lane.
        if (!v.turning && v.s >= L.len) {
            if (v.next < 0) { v.s = L.len; v.v = 0.0f; v.next = pickNext(v); continue; }
            v.s -= L.len;
            v.turning = true;
        }
        if (v.turning) {
            const float tl = turnLength(v.lane, v.next);
            if (v.s >= tl) {
                v.s -= tl;
                v.prev    = v.lane;   // its rear is still in the turn (see pose)
                v.lane    = v.next;
                v.turning = false;
                v.next    = pickNext(v);
            }
        }
    }

    // What the wheels need: distance for their spin, the heading's turn rate
    // for their steering (smoothed -- a lane joint is a kink, not a swerve).
    for (Vehicle& v : m_vehicles) {
        v.odo += v.v * dt;
        const Pose p = pose(v);
        const float H = std::atan2(p.heading.x, p.heading.y);
        if (v.haveH) {
            float d = H - v.lastH;
            if (d > 3.14159265f) d -= 6.2831853f;
            if (d < -3.14159265f) d += 6.2831853f;
            v.yawRate += (d / dt - v.yawRate) * std::min(1.0f, dt * 6.0f);
        }
        v.lastH = H;
        v.haveH = true;
    }
}

Sim::PathPoint Sim::inTurn(int a, int b, float d) const {
    PathPoint q;
    turnPoint(a, b, d, q.p, q.t);
    const Lane& A = m_lanes[static_cast<std::size_t>(a)];
    const Lane& B = m_lanes[static_cast<std::size_t>(b)];
    const float u = std::clamp(d / turnLength(a, b), 0.0f, 1.0f);
    q.y = A.y.back() + (B.y.front() - A.y.back()) * u;
    return q;
}

Sim::PathPoint Sim::onLane(int lane, int prev, float d) const {
    const Lane& L = m_lanes[static_cast<std::size_t>(lane)];
    PathPoint q;
    if (d >= 0.0f || prev < 0) {        // on it (or before it, for one that never turned in)
        q.p = L.p0 + L.dir * d;
        q.t = L.dir;
        q.y = L.heightAt(d);
        return q;
    }
    const float tl = turnLength(prev, lane);
    if (tl + d >= 0.0f) return inTurn(prev, lane, tl + d);
    const Lane& P = m_lanes[static_cast<std::size_t>(prev)];   // further back still
    const float e = P.len + tl + d;
    q.p = P.p0 + P.dir * e;
    q.t = P.dir;
    q.y = P.heightAt(e);
    return q;
}

Sim::PathPoint Sim::at(const Vehicle& v, float d) const {
    if (!v.turning || v.next < 0) return onLane(v.lane, v.prev, d);
    // Turning: d runs along the turn, the lane it is leaving lies behind 0.
    const float tl = turnLength(v.lane, v.next);
    if (d < 0.0f) return onLane(v.lane, v.prev, m_lanes[static_cast<std::size_t>(v.lane)].len + d);
    if (d <= tl)  return inTurn(v.lane, v.next, d);
    return onLane(v.next, v.lane, d - tl);
}

Pose Sim::pose(const Vehicle& v) const {
    // Where its two axles are on its path, and the body between them: it
    // points from the rear one to the front one, sits at their middle and
    // pitches with the road between them. A body that spans a corner cuts it
    // the way a real one does, and nothing jumps as the front moves from lane
    // to turn to lane -- a camera shooting a car sees every such jump.
    const float wb  = 0.6f * v.length;
    const float mid = v.s - 0.5f * v.length;
    const PathPoint f = at(v, mid + 0.5f * wb), r = at(v, mid - 0.5f * wb);
    Pose p;
    const glm::vec2 d = f.p - r.p;
    const float len = glm::length(d);
    p.heading = len > 1e-4f ? d / len : f.t;
    const glm::vec2 m = 0.5f * (f.p + r.p);
    p.pos   = {m.x, 0.5f * (f.y + r.y), m.y};
    p.pitch = std::atan2(f.y - r.y, std::max(len, 1e-3f));
    return p;
}

void Sim::lead(int walker, glm::vec3 at, glm::vec2 face, bool striding) {
    if (walker < 0 || walker >= static_cast<int>(m_walkers.size())) return;
    Walker& w = m_walkers[static_cast<std::size_t>(walker)];
    w.led     = true;
    w.strides = striding;
    w.at      = at;
    if (glm::dot(face, face) > 1e-8f) w.face = glm::normalize(face);
}

bool Sim::joinWalk(glm::vec3 p, glm::vec2 face, int& walk, float& s, int& dir,
                   glm::vec3& at) const {
    float best = 1e30f;
    for (std::size_t k = 0; k < m_walks.size(); ++k) {
        const std::vector<glm::vec3>& pts = m_walks[k];
        const std::vector<float>& cum = m_walkLen[k];
        for (std::size_t i = 0; i < pts.size(); ++i) {
            const glm::vec3& a = pts[i];
            const glm::vec3& b = pts[(i + 1) % pts.size()];
            const glm::vec2 ab(b.x - a.x, b.z - a.z);
            const float L2 = glm::dot(ab, ab);
            if (L2 < 1e-8f) continue;
            const glm::vec2 q(p.x - a.x, p.z - a.z);
            const float u = std::clamp(glm::dot(q, ab) / L2, 0.0f, 1.0f);
            const glm::vec2 d = q - ab * u;
            const float d2 = glm::dot(d, d);
            if (d2 >= best) continue;
            best = d2;
            walk = static_cast<int>(k);
            s    = cum[i] + (cum[i + 1] - cum[i]) * u;
            dir  = glm::dot(ab, face) >= 0.0f ? 1 : -1;
        }
    }
    if (best >= 1e30f) return false;
    Walker w;
    w.walk = walk;
    w.s    = s;
    w.dir  = dir;
    at = pose(w).pos;
    return true;
}

void Sim::release(int walker, int walk, float s, int dir) {
    if (walker < 0 || walker >= static_cast<int>(m_walkers.size())) return;
    if (walk < 0 || walk >= static_cast<int>(m_walks.size())) return;
    Walker& w = m_walkers[static_cast<std::size_t>(walker)];
    w.led     = false;
    w.strides = false;
    w.walk    = walk;
    w.s       = s;
    w.dir     = dir < 0 ? -1 : 1;
}

Pose Sim::pose(const Walker& w) const {
    Pose p;
    if (w.led) {
        p.pos     = w.at;
        p.heading = w.face;
        p.bob     = w.strides ? 0.035f * std::abs(std::sin(w.phase)) : 0.0f;
        return p;
    }
    const std::vector<glm::vec3>& pts = m_walks[static_cast<std::size_t>(w.walk)];
    const std::vector<float>& cum = m_walkLen[static_cast<std::size_t>(w.walk)];
    const auto it = std::upper_bound(cum.begin(), cum.end(), w.s);
    const std::size_t i = std::min(static_cast<std::size_t>(std::max<std::ptrdiff_t>(it - cum.begin() - 1, 0)),
                                   pts.size() - 1);
    const glm::vec3& a = pts[i];
    const glm::vec3& b = pts[(i + 1) % pts.size()];
    const float segLen = std::max(cum[i + 1] - cum[i], 1e-4f);
    const float u = std::clamp((w.s - cum[i]) / segLen, 0.0f, 1.0f);
    const glm::vec3 q = a + (b - a) * u;
    glm::vec2 d(b.x - a.x, b.z - a.z);
    d = glm::length(d) > 1e-5f ? glm::normalize(d) : glm::vec2(1.0f, 0.0f);
    if (w.dir < 0) d = -d;
    // Keep right, so the two directions pass each other.
    const glm::vec2 side = rightOf(d) * 0.45f;
    p.pos     = {q.x + side.x, q.y, q.z + side.y};
    p.heading = d;
    p.bob     = 0.035f * std::abs(std::sin(w.phase));
    return p;
}

} // namespace traffic
