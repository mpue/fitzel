#include "TramSim.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <glm/gtc/matrix_transform.hpp>

#include "TramTrack.hpp"

namespace tramsim {

namespace {

constexpr float kInf     = std::numeric_limits<float>::infinity();
constexpr float kAccel   = 1.0f;   // m/s^2 pulling away
constexpr float kBrake   = 1.1f;   // m/s^2 a stop and a curve are braked for
constexpr float kHard    = 2.8f;   // m/s^2 for something in the way
constexpr float kSideAcc = 1.0f;   // m/s^2 a passenger stands through a curve
constexpr float kAfterTurn = 4.0f; // seconds a tram waits after changing ends

float wrapPi(float a) {
    while (a >  3.14159265f) a -= 6.28318531f;
    while (a < -3.14159265f) a += 6.28318531f;
    return a;
}

} // namespace

// --- A half: one track, the way it is driven ---------------------------------------

float Sim::Half::wrap(float x) const {
    if (!loop || len <= 0.0f) return std::clamp(x, 0.0f, len);
    x = std::fmod(x, len);
    return x < 0.0f ? x + len : x;
}

glm::vec3 Sim::Half::at(float x) const {
    if (pts.empty()) return glm::vec3(0.0f);
    if (pts.size() == 1) return pts.front();
    x = wrap(x);
    const auto it = std::upper_bound(st.begin(), st.end(), x);
    std::size_t i = it == st.begin() ? 0 : static_cast<std::size_t>(it - st.begin()) - 1;
    i = std::min(i, pts.size() - 2);
    const float span = std::max(st[i + 1] - st[i], 1e-5f);
    return glm::mix(pts[i], pts[i + 1], std::clamp((x - st[i]) / span, 0.0f, 1.0f));
}

float Sim::Half::project(const glm::vec3& p) const {
    float best = kInf, bx = 0.0f;
    for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
        const glm::vec2 a(pts[i].x, pts[i].z), b(pts[i + 1].x, pts[i + 1].z);
        const glm::vec2 q(p.x, p.z), ab = b - a;
        const float L2 = glm::dot(ab, ab);
        const float u = L2 > 1e-8f ? std::clamp(glm::dot(q - a, ab) / L2, 0.0f, 1.0f) : 0.0f;
        const glm::vec2 c = a + ab * u;
        const float d2 = glm::dot(q - c, q - c);
        if (d2 < best) { best = d2; bx = st[i] + (st[i + 1] - st[i]) * u; }
    }
    return bx;
}

// --- Lines -----------------------------------------------------------------------------

void Sim::setLines(const std::vector<Line>& lines) {
    ++m_generation;
    m_lines.clear();
    m_trams.clear();
    for (const Line& def : lines) {
        LineRt rt;
        rt.def  = def;
        rt.open = !def.closed;
        std::vector<glm::vec3> c = def.center;
        std::vector<char> road = def.onRoad;
        road.resize(c.size(), 0);
        if (def.closed && c.size() >= 2 && glm::distance(c.front(), c.back()) > 1e-4f) {
            c.push_back(c.front());
            road.push_back(road.front());
        }
        const std::size_t n = c.size();
        if (n < 2) { m_lines.push_back(std::move(rt)); continue; }
        std::vector<float> sc(n, 0.0f);
        for (std::size_t i = 1; i < n; ++i)
            sc[i] = sc[i - 1] + glm::length(glm::vec2(c[i].x - c[i - 1].x, c[i].z - c[i - 1].z));
        const float len = sc.back();
        const int tracks = std::clamp(def.tracks, 1, 2);

        // The right-hand side of the line at each sample (walking it from its start).
        std::vector<glm::vec3> right(n);
        for (std::size_t i = 0; i < n; ++i) {
            const glm::vec3 a = c[i > 0 ? i - 1 : (def.closed ? n - 2 : 0)];
            const glm::vec3 b = c[i + 1 < n ? i + 1 : (def.closed ? 1 : n - 1)];
            glm::vec2 t(b.x - a.x, b.z - a.z);
            if (glm::dot(t, t) < 1e-10f) t = glm::vec2(0.0f, 1.0f);
            t = glm::normalize(t);
            right[i] = glm::vec3(-t.y, 0.0f, t.x);
        }
        auto track = [&](int k) {
            std::vector<glm::vec3> p(n);
            for (std::size_t i = 0; i < n; ++i) {
                const float o = tramtrack::offset(sc[i], len, tracks, def.spacing, def.closed, k);
                const float rail = road[i] ? def.railTopRoad : def.railTopOff;
                p[i] = c[i] + right[i] * o + glm::vec3(0.0f, rail, 0.0f);
            }
            return p;
        };
        // centre station of each sample of a half, to place the stops on it
        std::vector<std::vector<float>> centreOf;
        auto addHalf = [&](std::vector<glm::vec3> pts, bool reversed) {
            Half h;
            h.loop = def.closed;
            std::vector<float> cs = sc;
            if (reversed) {
                std::reverse(pts.begin(), pts.end());
                for (std::size_t i = 0; i < n; ++i) cs[i] = len - sc[n - 1 - i];
            }
            h.pts = std::move(pts);
            h.st.assign(n, 0.0f);
            for (std::size_t i = 1; i < n; ++i)
                h.st[i] = h.st[i - 1] + glm::length(glm::vec2(h.pts[i].x - h.pts[i - 1].x,
                                                                h.pts[i].z - h.pts[i - 1].z));
            h.len = h.st.back();
            // How fast each stretch may be taken: the heading's turn per metre,
            // the worst of a few metres around, against a comfortable sideways pull.
            std::vector<float> kappa(n, 0.0f);
            for (std::size_t i = 1; i + 1 < n; ++i) {
                const glm::vec3 a = h.pts[i] - h.pts[i - 1], b = h.pts[i + 1] - h.pts[i];
                const float la = glm::length(glm::vec2(a.x, a.z)), lb = glm::length(glm::vec2(b.x, b.z));
                if (la < 1e-3f || lb < 1e-3f) continue;
                const float turn = std::abs(wrapPi(std::atan2(b.x, b.z) - std::atan2(a.x, a.z)));
                kappa[i] = turn / (0.5f * (la + lb));
            }
            h.vcurve.assign(n, def.vmax);
            for (std::size_t i = 0; i < n; ++i) {
                float k = 0.0f;
                for (std::size_t j = (i > 2 ? i - 2 : 0); j <= std::min(i + 2, n - 1); ++j)
                    k = std::max(k, kappa[j]);
                if (k > 1e-4f) h.vcurve[i] = std::clamp(std::sqrt(kSideAcc / k), 2.0f, def.vmax);
            }
            centreOf.push_back(cs);
            rt.halves.push_back(std::move(h));
        };
        addHalf(track(0), false);
        if (rt.open) addHalf(track(tracks == 2 ? 1 : 0), true);
        else if (tracks == 2) addHalf(track(1), true);

        // The stops: TramTrack's stations, mapped onto each half. On an open
        // half the one at its start is where it leaves from, not a stop.
        const std::vector<float> stations = tramtrack::stops(len, def.stopEvery, def.closed);
        for (std::size_t hi = 0; hi < rt.halves.size(); ++hi) {
            Half& h = rt.halves[hi];
            const std::vector<float>& cs = centreOf[hi];
            for (float s : stations) {
                const float sh = (hi == 0) ? s : (rt.open || tracks == 2 ? len - s : s);
                const auto it = std::upper_bound(cs.begin(), cs.end(), sh);
                std::size_t i = it == cs.begin() ? 0 : static_cast<std::size_t>(it - cs.begin()) - 1;
                i = std::min(i, n - 2);
                const float u = std::clamp((sh - cs[i]) / std::max(cs[i + 1] - cs[i], 1e-5f), 0.0f, 1.0f);
                const float x = h.st[i] + (h.st[i + 1] - h.st[i]) * u;
                if (rt.open && x < kLength + 1.0f) continue;
                h.stops.push_back(x);
            }
            std::sort(h.stops.begin(), h.stops.end());
            if (rt.open && (h.stops.empty() || h.stops.back() < h.len - tramtrack::kEndStop - 1.0f))
                h.stops.push_back(h.len - tramtrack::kEndStop);
        }
        m_lines.push_back(std::move(rt));
    }

    // The trams, spread evenly over what they run round.
    for (int li = 0; li < static_cast<int>(m_lines.size()); ++li) {
        const LineRt& rt = m_lines[static_cast<std::size_t>(li)];
        if (rt.halves.empty()) continue;
        int count = std::max(rt.def.trams, 0);
        if (rt.open && std::clamp(rt.def.tracks, 1, 2) == 1) count = std::min(count, 1);
        bool roomy = true;
        for (const Half& h : rt.halves) roomy &= h.len > kLength + 2.0f * tramtrack::kStub;
        if (!roomy) count = 0;
        for (int k = 0; k < count; ++k) {
            Tram t;
            t.line = li;
            if (rt.open) {
                const float total = rt.halves[0].len + rt.halves[1].len;
                float cpos = (total / count) * k;
                t.half = cpos < rt.halves[0].len ? 0 : 1;
                const Half& h = rt.halves[static_cast<std::size_t>(t.half)];
                t.x = std::clamp(t.half == 0 ? cpos : cpos - rt.halves[0].len,
                                 kLength + tramtrack::kEndStop, h.stops.back());
            } else {
                const int halves = static_cast<int>(rt.halves.size());
                t.half = k % halves;
                const int onHalf = (count + halves - 1 - t.half) / halves;
                const int j = k / halves;
                t.x = rt.halves[static_cast<std::size_t>(t.half)].len / std::max(onHalf, 1) * j;
            }
            t.next = firstStopAfter(rt.halves[static_cast<std::size_t>(t.half)], t.x);
            m_trams.push_back(t);
        }
    }
}

int Sim::firstStopAfter(const Half& h, float x) const {
    for (int i = 0; i < static_cast<int>(h.stops.size()); ++i)
        if (h.stops[static_cast<std::size_t>(i)] > x + 0.5f) return i;
    return h.loop ? 0 : static_cast<int>(h.stops.size());
}

// --- Driving -------------------------------------------------------------------------

void Sim::finishStop(Tram& t) {
    ++t.served;
    const LineRt& rt = m_lines[static_cast<std::size_t>(t.line)];
    const Half& h = rt.halves[static_cast<std::size_t>(t.half)];
    if (rt.open && t.next >= static_cast<int>(h.stops.size()) - 1) {
        changeEnds(t);   // the terminus
        return;
    }
    t.next = h.loop ? (t.next + 1) % std::max(static_cast<int>(h.stops.size()), 1) : t.next + 1;
}

void Sim::changeEnds(Tram& t) {
    const LineRt& rt = m_lines[static_cast<std::size_t>(t.line)];
    const Half& from = rt.halves[static_cast<std::size_t>(t.half)];
    const int other = 1 - t.half;
    const Half& to = rt.halves[static_cast<std::size_t>(other)];
    // Its tail is where its new front is: the whole tram stands in the stub,
    // where the two tracks are one.
    const glm::vec3 tail = from.at(t.x - kLength);
    t.half = other;
    t.x    = std::max(to.project(tail), kLength);
    t.v    = 0.0f;
    t.hold = kAfterTurn;
    t.next = firstStopAfter(to, t.x);
    ++t.flips;
}

void Sim::step(float dt, const std::vector<Blocker>& blockers) {
    if (dt <= 0.0f) return;
    dt = std::min(dt, 0.1f);

    // Every tram's cars, for the others to see: trams of other lines are in the
    // way like anything else; on its own line a tram keeps its distance below.
    std::vector<std::vector<Blocker>> carsOf(m_trams.size());
    for (std::size_t i = 0; i < m_trams.size(); ++i) {
        Section sec[kSections];
        sections(static_cast<int>(i), sec);
        for (const Section& s : sec) {
            carsOf[i].push_back({s.center, kHalfWidth});
            carsOf[i].push_back({s.center + s.dir * (kSectionLen * 0.5f - 1.0f), kHalfWidth});
            carsOf[i].push_back({s.center - s.dir * (kSectionLen * 0.5f - 1.0f), kHalfWidth});
        }
    }

    for (std::size_t ti = 0; ti < m_trams.size(); ++ti) {
        Tram& t = m_trams[ti];
        const LineRt& rt = m_lines[static_cast<std::size_t>(t.line)];
        const Half& h = rt.halves[static_cast<std::size_t>(t.half)];
        // The doors: open while it stands at a stop, shut for its last moments
        // there -- and always shut before it moves.
        {
            const float want = t.dwell > kDoorTime ? 1.0f : 0.0f;
            const float rate = dt / kDoorTime;
            t.door = want > t.door ? std::min(t.door + rate, want) : std::max(t.door - rate, want);
        }
        if (t.hold > 0.0f) { t.hold -= dt; t.v = 0.0f; continue; }
        if (t.dwell > 0.0f) {
            t.dwell -= dt;
            t.v = 0.0f;
            if (t.dwell <= 0.0f) finishStop(t);
            continue;
        }

        // The stop it is heading for.
        float dStop = kInf;
        if (!h.stops.empty() && t.next < static_cast<int>(h.stops.size())) {
            const float sx = h.stops[static_cast<std::size_t>(t.next)];
            dStop = h.loop ? h.wrap(sx - t.x) : sx - t.x;
            if (!h.loop && dStop < -0.5f) {   // passed it (a rebuilt line): carry on
                t.next = firstStopAfter(h, t.x);
                dStop = kInf;
            }
        }

        // The tram ahead on the same track.
        float gap = kInf;
        for (std::size_t oi = 0; oi < m_trams.size(); ++oi) {
            if (oi == ti) continue;
            const Tram& o = m_trams[oi];
            if (o.line != t.line || o.half != t.half) continue;
            float d = o.x - t.x;
            if (h.loop) d = h.wrap(d);
            if (d > 0.0f) gap = std::min(gap, d - kLength);
        }
        // Coming in to a terminus: where the two tracks close up and become one,
        // there is room for one tram. While another is anywhere in there -- in
        // the stub, or on its way out past the converging tracks -- wait short
        // of it. (One leaving never waits for one coming in, so the two cannot
        // block each other.)
        if (rt.open) {
            const float entry = h.len - tramtrack::kStub - tramtrack::kTaper - 2.0f;
            if (t.x <= entry + 0.5f &&
                h.len - t.x < tramtrack::kStub + tramtrack::kTaper + 80.0f) {
                bool taken = false;
                for (std::size_t oi = 0; oi < m_trams.size() && !taken; ++oi) {
                    if (oi == ti || m_trams[oi].line != t.line) continue;
                    Section sec[kSections];
                    sections(static_cast<int>(oi), sec);
                    for (const Section& s : sec)
                        for (float e : {-0.5f * kSectionLen, 0.5f * kSectionLen})
                            if (h.project(s.center + s.dir * e) > entry) taken = true;
                }
                if (taken) gap = std::min(gap, entry - t.x + 3.0f);
            }
        }

        // Anything standing on the track ahead.
        const float look = t.v * t.v / (2.0f * kHard) + 14.0f;
        float gapB = kInf;
        const glm::vec3 front = h.at(t.x);
        auto test = [&](const Blocker& b) {
            if (glm::length(glm::vec2(b.p.x - front.x, b.p.z - front.z)) > look + 6.0f) return;
            for (float d = 0.5f; d <= look && d < gapB; d += 1.0f) {
                if (!h.loop && t.x + d > h.len) break;
                const glm::vec3 p = h.at(t.x + d);
                if (std::abs(b.p.y - p.y) > 3.0f) continue;
                if (glm::length(glm::vec2(b.p.x - p.x, b.p.z - p.z)) < kHalfWidth + 0.3f + b.r) {
                    gapB = d;
                    return;
                }
            }
        };
        for (const Blocker& b : blockers) test(b);
        for (std::size_t oi = 0; oi < m_trams.size(); ++oi)
            if (m_trams[oi].line != t.line)
                for (const Blocker& b : carsOf[oi]) test(b);

        // How fast it may go: the line's top speed, the curves ahead, the stop,
        // and whatever is in front.
        float vlim = rt.def.vmax;
        {
            const auto it = std::upper_bound(h.st.begin(), h.st.end(), h.wrap(t.x));
            std::size_t i = it == h.st.begin() ? 0 : static_cast<std::size_t>(it - h.st.begin()) - 1;
            const float reach = rt.def.vmax * rt.def.vmax / (2.0f * kBrake) + 20.0f;
            float run = 0.0f;
            for (std::size_t k = 0; k < h.st.size() && run < reach; ++k) {
                const std::size_t j = (i + k) % h.st.size();
                if (!h.loop && i + k >= h.st.size()) break;
                const float d = std::max(run - (t.x - h.st[i]), 0.0f);
                vlim = std::min(vlim, std::sqrt(h.vcurve[j] * h.vcurve[j] + 2.0f * kBrake * d));
                const std::size_t jn = (j + 1) % h.st.size();
                run += (jn == 0) ? 0.0f : h.st[jn] - h.st[j];
            }
        }
        if (dStop < kInf)
            vlim = std::min(vlim, std::max(std::sqrt(2.0f * kBrake * std::max(dStop, 0.0f)),
                                           dStop > 0.3f ? 0.8f : 0.0f));
        const float room = std::min(gap - 3.0f, gapB - 1.5f);
        if (room < kInf) vlim = std::min(vlim, std::sqrt(2.0f * kHard * std::max(room, 0.0f)));

        if (vlim > t.v) t.v = std::min(t.v + kAccel * dt, vlim);
        else            t.v = std::max(vlim, t.v - kHard * 1.5f * dt);
        float step = t.v * dt;
        // Never through the stop, and never into anything.
        if (dStop < kInf) step = std::min(step, std::max(dStop, 0.0f));
        if (room < kInf) step = std::min(step, std::max(room + 1.0f, 0.0f));
        t.x += step;
        if (h.loop) t.x = h.wrap(t.x);
        else        t.x = std::min(t.x, h.len);

        if (dStop < kInf && dStop - step < 0.05f) {
            t.x = h.loop ? h.wrap(h.stops[static_cast<std::size_t>(t.next)])
                         : h.stops[static_cast<std::size_t>(t.next)];
            t.v = 0.0f;
            t.dwell = std::max(rt.def.dwell, 0.01f);
        }
    }
}

// --- Poses ---------------------------------------------------------------------------

void Sim::sections(int i, Section out[kSections]) const {
    const Tram& t = m_trams[static_cast<std::size_t>(i)];
    const Half& h = m_lines[static_cast<std::size_t>(t.line)].halves[static_cast<std::size_t>(t.half)];
    for (int k = 0; k < kSections; ++k) {
        const float xf = t.x - k * (kSectionLen + kGap);
        const glm::vec3 pf = h.at(xf), pb = h.at(xf - kSectionLen);
        Section& s = out[k];
        s.center = 0.5f * (pf + pb);
        glm::vec3 d = pf - pb;
        if (glm::dot(d, d) < 1e-8f) d = glm::vec3(0.0f, 0.0f, 1.0f);
        s.dir   = glm::normalize(d);
        s.yaw   = std::atan2(s.dir.x, s.dir.z);
        s.pitch = std::asin(std::clamp(s.dir.y, -1.0f, 1.0f));
    }
}

float Sim::speed(int i) const { return m_trams[static_cast<std::size_t>(i)].v; }

glm::mat4 Sim::carFrame(int i, int car) const {
    const Tram& t = m_trams[static_cast<std::size_t>(i)];
    // Changing ends turns the order of the cars round, not the cars.
    const int k = (t.flips & 1) ? kSections - 1 - car : car;
    Section sec[kSections];
    sections(i, sec);
    const Section& s = sec[k];
    glm::mat4 m = glm::translate(glm::mat4(1.0f), s.center);
    m = glm::rotate(m, s.yaw, glm::vec3(0.0f, 1.0f, 0.0f));
    m = glm::rotate(m, -s.pitch, glm::vec3(1.0f, 0.0f, 0.0f));
    if (backwards(t, car)) m = glm::rotate(m, 3.14159265f, glm::vec3(0.0f, 1.0f, 0.0f));
    return m;
}

// Whether a car faces against the way its tram drives: the last cab car does
// (both ends are fronts), and after a change of ends every car has turned.
bool Sim::backwards(const Tram& t, int car) {
    const bool flipped = (t.flips & 1) != 0;
    return car == kSections - 1 ? !flipped : flipped;
}

int Sim::doorSide(int i, int car) const {
    // A car's +x points to the right of the way it drives -- where the stop
    // is -- only while it faces backwards.
    return backwards(m_trams[static_cast<std::size_t>(i)], car) ? 1 : -1;
}

float Sim::doors(int i) const { return m_trams[static_cast<std::size_t>(i)].door; }

void Sim::holdDoors(int i) {
    Tram& t = m_trams[static_cast<std::size_t>(i)];
    if (t.dwell > 0.0f) t.dwell = std::max(t.dwell, kDoorTime + 0.4f);
}

bool Sim::lineOpen(int line) const {
    return line >= 0 && line < lineCount() && m_lines[static_cast<std::size_t>(line)].open;
}

std::vector<Box> Sim::boxes() const {
    std::vector<Box> out;
    out.reserve(m_trams.size() * kSections);
    for (int i = 0; i < tramCount(); ++i) {
        Section sec[kSections];
        sections(i, sec);
        const float v = speed(i);
        for (const Section& s : sec) {
            const glm::vec3 fwd = glm::normalize(glm::vec3(s.dir.x, 0.0f, s.dir.z) + glm::vec3(0.0f, 0.0f, 1e-6f));
            const glm::vec3 right(-fwd.z, 0.0f, fwd.x);
            Box b;
            b.center  = s.center + glm::vec3(0.0f, 1.7f, 0.0f);
            b.axes[0] = fwd * (kSectionLen * 0.5f);
            b.axes[1] = glm::vec3(0.0f, 1.7f, 0.0f);
            b.axes[2] = right * (kHalfWidth + 0.1f);
            b.vel     = fwd * v;
            out.push_back(b);
        }
        // Its right of way: the track ahead of its nose. Standing at a stop it
        // keeps only its front clear; moving, the stretch it covers in the next
        // three and a half seconds, and never less than a car's length.
        const Tram& t = m_trams[static_cast<std::size_t>(i)];
        const Half& h = m_lines[static_cast<std::size_t>(t.line)].halves[static_cast<std::size_t>(t.half)];
        const bool standing = t.dwell > 0.0f || t.hold > 0.0f;
        float ahead = standing ? 6.0f : std::clamp(v * 3.5f + 10.0f, 10.0f, 50.0f);
        if (!h.loop) ahead = std::min(ahead, std::max(h.len - t.x, 0.0f));
        for (float d = 0.0f; d < ahead; d += 6.0f) {
            const float seg = std::min(6.0f, ahead - d);
            const glm::vec3 a = h.at(t.x + d), c = h.at(t.x + d + seg);
            glm::vec3 dir(c.x - a.x, 0.0f, c.z - a.z);
            if (glm::dot(dir, dir) < 1e-6f) continue;
            dir = glm::normalize(dir);
            Box z;
            z.center  = 0.5f * (a + c) + glm::vec3(0.0f, 1.7f, 0.0f);
            z.axes[0] = dir * (seg * 0.5f);
            z.axes[1] = glm::vec3(0.0f, 1.7f, 0.0f);
            z.axes[2] = glm::vec3(-dir.z, 0.0f, dir.x) * (kHalfWidth + 0.1f);
            z.giveWay = true;
            out.push_back(z);
        }
    }
    return out;
}

Sim::Info Sim::info(int i) const {
    const Tram& t = m_trams[static_cast<std::size_t>(i)];
    Info in;
    in.line = t.line;  in.half = t.half;
    in.x = t.x;        in.v = t.v;          in.dwell = t.dwell;
    in.served = t.served;  in.flips = t.flips;
    in.next = t.next;  in.door = t.door;
    return in;
}

int Sim::halfCount(int line) const {
    return line >= 0 && line < lineCount()
               ? static_cast<int>(m_lines[static_cast<std::size_t>(line)].halves.size()) : 0;
}

float Sim::halfLength(int line, int half) const {
    if (half < 0 || half >= halfCount(line)) return 0.0f;
    return m_lines[static_cast<std::size_t>(line)].halves[static_cast<std::size_t>(half)].len;
}

glm::vec3 Sim::pointOn(int line, int half, float x) const {
    if (half < 0 || half >= halfCount(line)) return glm::vec3(0.0f);
    return m_lines[static_cast<std::size_t>(line)].halves[static_cast<std::size_t>(half)].at(x);
}

const std::vector<float>& Sim::stopsOn(int line, int half) const {
    static const std::vector<float> none;
    if (half < 0 || half >= halfCount(line)) return none;
    return m_lines[static_cast<std::size_t>(line)].halves[static_cast<std::size_t>(half)].stops;
}

} // namespace tramsim
