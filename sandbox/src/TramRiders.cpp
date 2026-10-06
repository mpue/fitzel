#include "TramRiders.hpp"

#include <algorithm>
#include <cmath>

#include "TrafficSim.hpp"
#include "TramSim.hpp"

namespace {

constexpr int   kPlaces    = 8;       // standing places per car, four by each doorway
constexpr int   kMaxWait   = 5;       // people waiting at one stop
constexpr float kReach     = 14.0f;   // a stop farther than this from a pavement has nobody
constexpr float kNotice    = 7.0f;    // passing this close, a walker may take the tram
constexpr float kTakes     = 0.35f;   // ...and does, this often
constexpr float kAskAgain  = 90.0f;   // seconds before the same walker is asked again
constexpr float kGiveUp    = 300.0f;  // seconds a person waits before walking on

// Standing place `q` (0..7) in a car: four around each doorway, two each side
// of the aisle, clear of the doorway itself.
glm::vec3 placeLocal(bool cab, int q) {
    float dz[2];
    tramsim::doorsOf(cab, dz);
    const int j = q / 4, k = q % 4;
    const float x = (k & 1) ? 0.42f : -0.42f;
    const float z = dz[j] + ((k & 2) ? 0.55f : -0.55f);
    return {x, tramsim::kFloor, z};
}

glm::vec3 xform(const glm::mat4& m, const glm::vec3& p) {
    return glm::vec3(m * glm::vec4(p, 1.0f));
}

glm::vec2 flat(const glm::vec3& v) {
    glm::vec2 f(v.x, v.z);
    const float l = glm::length(f);
    return l > 1e-5f ? f / l : glm::vec2(0.0f, 1.0f);
}

} // namespace

float TramRiders::rand01() {
    m_rng ^= m_rng << 13;
    m_rng ^= m_rng >> 17;
    m_rng ^= m_rng << 5;
    return static_cast<float>(m_rng & 0xFFFFFFu) / 16777216.0f;
}

// --- Stops ----------------------------------------------------------------------------

void TramRiders::rebuild(const tramsim::Sim& trams, const traffic::Sim& people) {
    m_stops.clear();
    for (int li = 0; li < trams.lineCount(); ++li)
        for (int hi = 0; hi < trams.halfCount(li); ++hi) {
            const std::vector<float>& xs = trams.stopsOn(li, hi);
            for (int k = 0; k < static_cast<int>(xs.size()); ++k) {
                Stop s;
                s.line = li; s.half = hi; s.index = k;
                s.x     = xs[static_cast<std::size_t>(k)];
                s.front = trams.pointOn(li, hi, s.x);
                const glm::vec3 a = trams.pointOn(li, hi, s.x - tramsim::kLength * 0.5f - 1.0f);
                const glm::vec3 b = trams.pointOn(li, hi, s.x - tramsim::kLength * 0.5f + 1.0f);
                const glm::vec2 d = flat(b - a);
                s.dir   = {d.x, 0.0f, d.y};
                s.right = {-d.y, 0.0f, d.x};
                s.terminus = trams.lineOpen(li) && k == static_cast<int>(xs.size()) - 1;
                const glm::vec3 mid = trams.pointOn(li, hi, s.x - tramsim::kLength * 0.5f) +
                                      s.right * (tramsim::kHalfWidth + 1.2f);
                int walk = -1, dir = 1;
                float ws = 0.0f;
                if (people.joinWalk(mid, d, walk, ws, dir, s.entry))
                    s.served = glm::length(glm::vec2(s.entry.x - mid.x, s.entry.z - mid.z)) < kReach &&
                               std::abs(s.entry.y - mid.y) < 3.0f;
                m_stops.push_back(s);
            }
        }
}

int TramRiders::stopOf(const tramsim::Sim& trams, int tram) const {
    const tramsim::Sim::Info in = trams.info(tram);
    if (in.dwell <= 0.0f) return -1;
    for (int i = 0; i < static_cast<int>(m_stops.size()); ++i) {
        const Stop& s = m_stops[static_cast<std::size_t>(i)];
        if (s.line == in.line && s.half == in.half && s.index == in.next) return i;
    }
    return -1;
}

int TramRiders::stopsServed() const {
    int n = 0;
    for (const Stop& s : m_stops) n += s.served ? 1 : 0;
    return n;
}

// --- Walking ---------------------------------------------------------------------------

glm::vec3 TramRiders::world(const Pt& q, const Person& p, const tramsim::Sim& trams) const {
    if (!q.inCar || p.tram < 0) return q.p;
    return xform(trams.carFrame(p.tram, p.car), q.p);
}

// One step along the path; true once it is walked.
bool TramRiders::walk(Person& p, float dt, const tramsim::Sim& trams) {
    float left = p.speed * dt;
    while (!p.path.empty()) {
        const glm::vec3 to = world(p.path.front(), p, trams);
        const glm::vec3 d = to - p.pos;
        const float len = glm::length(glm::vec2(d.x, d.z));
        if (len > 1e-3f) p.face = glm::vec2(d.x, d.z) / len;
        if (len <= left) {
            p.pos = to;
            left -= len;
            p.path.erase(p.path.begin());
            continue;
        }
        p.pos += d * (left / len);
        return false;
    }
    return true;
}

void TramRiders::board(Person& p, int tram, const tramsim::Sim& trams) {
    std::vector<int>& taken = m_places[static_cast<std::size_t>(tram)];
    // The free place whose doorway is nearest.
    float best = 1e30f;
    int bc = -1, bq = -1;
    for (int c = 0; c < tramsim::kSections; ++c) {
        const glm::mat4 f = trams.carFrame(tram, c);
        const float s = static_cast<float>(trams.doorSide(tram, c));
        float dz[2];
        tramsim::doorsOf(tramsim::isCab(c), dz);
        for (int q = 0; q < kPlaces; ++q) {
            if (taken[static_cast<std::size_t>(c * kPlaces + q)] != 0) continue;
            const glm::vec3 door = xform(f, {s * (tramsim::kHalfWidth + 0.5f), 0.0f, dz[q / 4]});
            const float d = glm::length(door - p.pos);
            if (d < best) { best = d; bc = c; bq = q; }
        }
    }
    if (bc < 0) return;   // full: wait for the next one
    taken[static_cast<std::size_t>(bc * kPlaces + bq)] = p.walker + 1;
    const glm::mat4 f = trams.carFrame(tram, bc);
    const float s = static_cast<float>(trams.doorSide(tram, bc));
    float dz[2];
    tramsim::doorsOf(tramsim::isCab(bc), dz);
    const float z = dz[bq / 4];
    p.st = St::Boarding;
    p.tram = tram; p.car = bc; p.place = bq;
    p.path.clear();
    p.path.push_back({xform(f, {s * (tramsim::kHalfWidth + 0.5f), 0.0f, z}), false});
    p.path.push_back({{s * (tramsim::kWallIn - 0.15f), tramsim::kFloor, z}, true});
    p.path.push_back({placeLocal(tramsim::isCab(bc), bq), true});
}

void TramRiders::alight(Person& p, const tramsim::Sim& trams, const traffic::Sim& people) {
    const glm::mat4 f = trams.carFrame(p.tram, p.car);
    const float s = static_cast<float>(trams.doorSide(p.tram, p.car));
    float dz[2];
    tramsim::doorsOf(tramsim::isCab(p.car), dz);
    const float z = dz[p.place / 4];
    const glm::vec3 out = xform(f, {s * (tramsim::kHalfWidth + 0.7f), 0.0f, z});
    const glm::vec2 away = flat(xform(f, {s, 0.0f, z}) - xform(f, {0.0f, 0.0f, z}));
    glm::vec3 at;
    if (!people.joinWalk(out, away, p.joinWalk, p.joinS, p.joinDir, at)) return;
    m_places[static_cast<std::size_t>(p.tram)][static_cast<std::size_t>(p.car * kPlaces + p.place)] = 0;
    p.st = St::Alighting;
    p.path.clear();
    p.path.push_back({{s * (tramsim::kWallIn - 0.15f), tramsim::kFloor, z}, true});
    p.path.push_back({out, false});
    p.path.push_back({at, false});
}

void TramRiders::leave(Person& p, const traffic::Sim& people) {
    glm::vec3 at;
    if (!people.joinWalk(p.pos, p.face, p.joinWalk, p.joinS, p.joinDir, at)) return;
    p.st = St::Leaving;
    p.path.clear();
    p.path.push_back({at, false});
}

// --- The frame -------------------------------------------------------------------------

void TramRiders::clear(traffic::Sim& people) {
    for (Person& p : m_people) {
        int walk = -1, dir = 1;
        float s = 0.0f;
        glm::vec3 at;
        if (people.joinWalk(p.pos, p.face, walk, s, dir, at)) people.release(p.walker, walk, s, dir);
    }
    m_people.clear();
    m_inStreet.clear();
    m_tramGen = -1;
}

void TramRiders::step(float dt, tramsim::Sim& trams, traffic::Sim& people) {
    m_inStreet.clear();
    if (dt <= 0.0f) return;
    if (trams.generation() != m_tramGen || people.generation() != m_peopleGen) {
        // The people are the town's: a town rebuilt has new ones (the old are
        // gone with it), trams started over put ours back on the walks.
        if (people.generation() == m_peopleGen) clear(people);
        else m_people.clear();
        m_tramGen   = trams.generation();
        m_peopleGen = people.generation();
        rebuild(trams, people);
        m_places.assign(static_cast<std::size_t>(trams.tramCount()),
                        std::vector<int>(tramsim::kSections * kPlaces, 0));
        m_arrived.assign(static_cast<std::size_t>(trams.tramCount()), Arrival{});
        m_lastAsked.assign(people.walkers().size(), -1e9f);
        m_busy.assign(people.walkers().size(), 0);
    }
    m_clock += dt;
    if (m_stops.empty() || people.walkers().empty()) return;

    // Arriving at a stop: one stop fewer to go for whoever rides, and out at
    // the last one -- where there is a pavement to get out onto.
    std::vector<int> at(static_cast<std::size_t>(trams.tramCount()), -1);
    for (int i = 0; i < trams.tramCount(); ++i) {
        const int st = stopOf(trams, i);
        at[static_cast<std::size_t>(i)] = st;
        Arrival& a = m_arrived[static_cast<std::size_t>(i)];
        if (st < 0) { a.on = false; continue; }
        const Stop& s = m_stops[static_cast<std::size_t>(st)];
        if (a.on && a.line == s.line && a.half == s.half && a.next == s.index) continue;
        a = {s.line, s.half, s.index, true};
        if (!s.served) continue;
        for (Person& p : m_people)
            if (p.st == St::Riding && p.tram == i && (--p.ridesLeft <= 0 || s.terminus))
                p.ridesLeft = 0;
    }

    // Everyone on their way.
    for (Person& p : m_people) {
        switch (p.st) {
        case St::ToStop:
            if (walk(p, dt, trams)) { p.st = St::Waiting; p.waited = 0.0f; }
            break;
        case St::Waiting: {
            p.face = flat(-m_stops[static_cast<std::size_t>(p.stop)].right);
            p.waited += dt;
            for (int i = 0; i < trams.tramCount() && p.st == St::Waiting; ++i)
                if (at[static_cast<std::size_t>(i)] == p.stop && trams.doors(i) > 0.95f)
                    board(p, i, trams);
            if (p.st == St::Waiting && p.waited > kGiveUp) leave(p, people);
            break;
        }
        case St::Boarding:
            if (at[static_cast<std::size_t>(p.tram)] < 0) {
                // It left without them after all: still outside, back to the
                // platform; already through the doorway, aboard.
                if (p.path.size() >= 2) {
                    m_places[static_cast<std::size_t>(p.tram)]
                            [static_cast<std::size_t>(p.car * kPlaces + p.place)] = 0;
                    p.st = St::Waiting;
                    p.path.clear();
                    p.tram = -1;
                    break;
                }
                p.path.clear();
            }
            trams.holdDoors(p.tram);
            if (walk(p, dt, trams)) { p.st = St::Riding; ++m_boarded; }
            break;
        case St::Riding: {
            const glm::mat4 f = trams.carFrame(p.tram, p.car);
            const glm::vec3 local = placeLocal(tramsim::isCab(p.car), p.place);
            p.pos  = xform(f, local);
            p.face = flat(xform(f, {local.x > 0.0f ? -1.0f : 1.0f, 0.0f, local.z}) - xform(f, local));
            if (p.ridesLeft <= 0 && at[static_cast<std::size_t>(p.tram)] >= 0 && trams.doors(p.tram) > 0.95f)
                alight(p, trams, people);
            break;
        }
        case St::Alighting:
            if (p.path.size() >= 2) trams.holdDoors(p.tram);
            if (walk(p, dt, trams)) {
                people.release(p.walker, p.joinWalk, p.joinS, p.joinDir);
                p.walker = -1 - p.walker;   // done: dropped below
                ++m_alighted;
            }
            break;
        case St::Leaving:
            if (walk(p, dt, trams)) {
                people.release(p.walker, p.joinWalk, p.joinS, p.joinDir);
                p.walker = -1 - p.walker;
            }
            break;
        }
        if (p.walker < 0) continue;
        const bool moving = p.st == St::ToStop || p.st == St::Leaving ||
                            ((p.st == St::Boarding || p.st == St::Alighting) && !p.path.empty());
        people.lead(p.walker, p.pos, p.face, moving);
        const bool street = p.st == St::ToStop || p.st == St::Leaving ||
                            (p.st == St::Boarding && p.path.size() >= 2) ||
                            (p.st == St::Alighting && p.path.size() <= 2);
        if (street) m_inStreet.push_back(p.pos);
    }
    for (const Person& p : m_people)
        if (p.walker < 0) m_busy[static_cast<std::size_t>(-1 - p.walker)] = 0;
    m_people.erase(std::remove_if(m_people.begin(), m_people.end(),
                                  [](const Person& p) { return p.walker < 0; }),
                   m_people.end());

    // Who passes a stop may take the tram from it (after the others have
    // moved: one led from now on takes their first step next frame).
    if (m_clock >= m_nextLook) {
        m_nextLook = m_clock + 0.5f;
        std::vector<int> waiting(m_stops.size(), 0);
        for (const Person& p : m_people)
            if (p.st == St::ToStop || p.st == St::Waiting) ++waiting[static_cast<std::size_t>(p.stop)];
        const std::vector<traffic::Walker>& ws = people.walkers();
        for (int w = 0; w < static_cast<int>(ws.size()); ++w) {
            if (ws[static_cast<std::size_t>(w)].led || m_busy[static_cast<std::size_t>(w)]) continue;
            if (m_clock - m_lastAsked[static_cast<std::size_t>(w)] < kAskAgain) continue;
            const glm::vec3 pos = people.pose(ws[static_cast<std::size_t>(w)]).pos;
            for (int si = 0; si < static_cast<int>(m_stops.size()); ++si) {
                const Stop& s = m_stops[static_cast<std::size_t>(si)];
                if (!s.served) continue;
                if (glm::length(glm::vec2(pos.x - s.entry.x, pos.z - s.entry.z)) > kNotice) continue;
                if (std::abs(pos.y - s.entry.y) > 3.0f) continue;
                m_lastAsked[static_cast<std::size_t>(w)] = m_clock;
                if (waiting[static_cast<std::size_t>(si)] >= kMaxWait || rand01() > kTakes) break;
                Person p;
                p.walker = w;
                p.stop   = si;
                p.pos    = pos;
                p.speed  = std::clamp(ws[static_cast<std::size_t>(w)].speed, 0.9f, 1.6f);
                p.ridesLeft = 1 + static_cast<int>(rand01() * 4.0f);
                // A place along the platform, between the sign and the tram's tail.
                const float xs = s.x - 2.0f - rand01() * (tramsim::kLength - 4.0f);
                p.spot = trams.pointOn(s.line, s.half, xs) +
                         s.right * (tramsim::kHalfWidth + 1.0f + 0.6f * rand01());
                p.path.push_back({p.spot, false});
                p.face = flat(p.spot - pos);
                // Led from here on, from where they are; they set off next frame.
                people.lead(w, pos, p.face, true);
                m_people.push_back(p);
                m_busy[static_cast<std::size_t>(w)] = 1;
                ++waiting[static_cast<std::size_t>(si)];
                break;
            }
        }
    }
}

// --- For checks --------------------------------------------------------------------------

int TramRiders::waiting() const {
    int n = 0;
    for (const Person& p : m_people) n += (p.st == St::ToStop || p.st == St::Waiting) ? 1 : 0;
    return n;
}

int TramRiders::riding() const {
    int n = 0;
    for (const Person& p : m_people) n += p.st == St::Riding ? 1 : 0;
    return n;
}

int TramRiders::ridingOn(int tram) const {
    int n = 0;
    for (const Person& p : m_people) n += (p.st == St::Riding && p.tram == tram) ? 1 : 0;
    return n;
}

std::vector<TramRiders::Seen> TramRiders::riders() const {
    std::vector<Seen> out;
    for (const Person& p : m_people)
        if (p.st == St::Riding)
            out.push_back({p.walker, p.tram, p.car, placeLocal(tramsim::isCab(p.car), p.place)});
    return out;
}
