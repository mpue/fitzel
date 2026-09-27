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

float Lane::heightAt(float s) const {
    if (y.empty()) return 0.0f;
    const float f = std::clamp(s / Sim::kStep, 0.0f, static_cast<float>(y.size() - 1));
    const std::size_t i = std::min(static_cast<std::size_t>(f), y.size() - 1);
    const std::size_t j = std::min(i + 1, y.size() - 1);
    return y[i] + (y[j] - y[i]) * (f - static_cast<float>(i));
}

void Sim::clear() {
    m_nodes.clear();
    m_lanes.clear();
    m_vehicles.clear();
    m_walkers.clear();
    m_walks.clear();
    m_walkLen.clear();
}

void Sim::build(const std::vector<cityplan::Rule>& rules,
                const std::vector<const cityplan::Town*>& towns, std::uint32_t seed,
                const std::function<void(Vehicle&)>& dress) {
    clear();
    for (std::size_t i = 0; i < rules.size() && i < towns.size(); ++i) {
        if (!towns[i] || !rules[i].enabled) continue;
        cityplan::Rule r = rules[i];
        r.seed ^= seed * 0x9e3779b9U;
        addTown(r, *towns[i], static_cast<int>(i), dress);
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
    v.rng    = 0x9e3779b9U ^ static_cast<std::uint32_t>(entity * 7919 + 1);
    v.next   = pickNext(v);
    m_vehicles.push_back(v);
    return true;
}

void Sim::removeDrivers() {
    m_vehicles.erase(std::remove_if(m_vehicles.begin(), m_vehicles.end(),
                                    [](const Vehicle& v) { return v.entity >= 0; }),
                     m_vehicles.end());
}

void Sim::addTown(const cityplan::Rule& r, const cityplan::Town& t, int town,
                  const std::function<void(Vehicle&)>& dress) {
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
    const float u = std::clamp(s / turnLength(a, b), 0.0f, 1.0f);
    p = bezier(A.p1, controlPoint(A, B), B.p0, u, t);
    if (glm::dot(t, t) < 1e-8f) t = A.dir;
    t = glm::normalize(t);
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
        const float len = m_walkLen[static_cast<std::size_t>(p.walk)].back();
        if (len <= 0.0f) continue;
        p.s = std::fmod(p.s + static_cast<float>(p.dir) * p.speed * dt + len, len);
        p.phase += p.speed * dt * 6.2831853f / 1.4f;   // a stride every 1.4 m
    }

    // --- Who is where: per lane, sorted by position -----------------------------
    // A turning vehicle already counts on the lane it is turning into, at the
    // (negative) distance it still has to go -- so whoever follows it in sees it.
    struct Slot { float pos; int who; };
    std::vector<std::vector<Slot>> onLane(m_lanes.size());
    for (int i = 0; i < static_cast<int>(m_vehicles.size()); ++i) {
        const Vehicle& v = m_vehicles[static_cast<std::size_t>(i)];
        if (v.turning && v.next >= 0)
            onLane[static_cast<std::size_t>(v.next)].push_back({-(turnLength(v.lane, v.next) - v.s), i});
        else
            onLane[static_cast<std::size_t>(v.lane)].push_back({v.s, i});
    }
    for (auto& l : onLane)
        std::sort(l.begin(), l.end(), [](const Slot& a, const Slot& b) { return a.pos < b.pos; });

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

        // The vehicle ahead: on this lane, else the first on the next one.
        const int regLane = v.turning ? v.next : v.lane;
        const float regPos = v.turning ? -(turnLength(v.lane, v.next) - v.s) : v.s;
        bool found = false;
        if (regLane >= 0)
            for (const Slot& o : onLane[static_cast<std::size_t>(regLane)])
                if (o.who != i && o.pos > regPos) {
                    const Vehicle& w = m_vehicles[static_cast<std::size_t>(o.who)];
                    lead(o.pos - w.length - regPos, w.v);
                    found = true;
                    break;
                }
        float v0 = v.vmax;
        if (!v.turning) {
            const float toEnd = L.len - v.s;
            if (!found && v.next >= 0) {
                const float tl = turnLength(v.lane, v.next);
                const auto& nl = onLane[static_cast<std::size_t>(v.next)];
                for (const Slot& o : nl)
                    if (o.who != i) {
                        const Vehicle& w = m_vehicles[static_cast<std::size_t>(o.who)];
                        lead(toEnd + tl + o.pos - w.length, w.v);
                        break;
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
                const float c = glm::dot(L.dir, m_lanes[static_cast<std::size_t>(v.next)].dir);
                const float vTurn = c > 0.94f ? v.vmax : c > -0.5f ? 5.5f : 3.0f;
                v0 = std::min(v0, std::sqrt(vTurn * vTurn + 2.0f * bComf * std::max(toEnd, 0.0f)));
            }
        } else {
            const float c = glm::dot(L.dir, m_lanes[static_cast<std::size_t>(v.next)].dir);
            v0 = std::min(v0, c > 0.94f ? v.vmax : c > -0.5f ? 5.5f : 3.0f);
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

Pose Sim::pose(const Vehicle& v) const {
    Pose p;
    const Lane& L = m_lanes[static_cast<std::size_t>(v.lane)];
    const float c = v.s - 0.5f * v.length;   // the vehicle's middle
    if (!v.turning) {
        const glm::vec2 xz = L.p0 + L.dir * c;
        p.pos     = {xz.x, L.heightAt(c), xz.y};
        p.heading = L.dir;
        const float wb = 0.6f * v.length;
        p.pitch = std::atan2(L.heightAt(c + 0.5f * wb) - L.heightAt(c - 0.5f * wb), wb);
        return p;
    }
    const Lane& N = m_lanes[static_cast<std::size_t>(v.next)];
    const float tl = turnLength(v.lane, v.next);
    glm::vec2 xz, t;
    if (c >= 0.0f) {
        turnPoint(v.lane, v.next, c, xz, t);
    } else {                        // the middle is still on the lane behind
        xz = L.p1 + L.dir * c;
        t  = L.dir;
    }
    const float u = std::clamp(c / tl, 0.0f, 1.0f);
    p.pos     = {xz.x, L.y.back() + (N.y.front() - L.y.back()) * u, xz.y};
    p.heading = t;
    return p;
}

Pose Sim::pose(const Walker& w) const {
    Pose p;
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
