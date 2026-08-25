#include "RoadLoop.hpp"

#include <algorithm>
#include <cmath>

namespace roadloop {

namespace {

constexpr float kPi  = 3.14159265358979323846f;
constexpr float kTau = 2.0f * kPi;

// Frames around one turn. Enough that the ribbon reads as a circle rather than a
// polygon at a 10 m radius, and cheap either way -- a loop is one feature, not a
// field of them.
constexpr int kSamples = 96;

// Position and unit tangent of the flat centreline at arc length `q`.
void groundAt(const std::vector<glm::vec2>& center, const std::vector<float>& s,
              const std::vector<float>& prof, float q, glm::vec2& pos,
              glm::vec2& dir, float& y) {
    q = std::clamp(q, 0.0f, s.empty() ? 0.0f : s.back());
    std::size_t i = static_cast<std::size_t>(
        std::lower_bound(s.begin(), s.end(), q) - s.begin());
    if (i == 0) i = 1;
    if (i >= center.size()) i = center.size() - 1;
    const float seg = std::max(s[i] - s[i - 1], 1e-5f);
    const float t   = std::clamp((q - s[i - 1]) / seg, 0.0f, 1.0f);
    pos = center[i - 1] + (center[i] - center[i - 1]) * t;
    y   = prof[i - 1] + (prof[i] - prof[i - 1]) * t;
    const glm::vec2 d = center[i] - center[i - 1];
    const float     l = glm::length(d);
    dir = (l > 1e-5f) ? d / l : glm::vec2(0.0f, 1.0f);
}

} // namespace

std::vector<Loop> plan(const std::vector<glm::vec2>& center,
                       const std::vector<float>& prof,
                       const std::vector<int>& ptSample,
                       const std::vector<Spec>& specs, float roadWidth) {
    std::vector<Loop> out;
    if (center.size() < 2 || prof.size() != center.size() || specs.empty())
        return out;

    // Arc length along the flat centreline, so a loop can be positioned in metres.
    std::vector<float> s(center.size(), 0.0f);
    for (std::size_t i = 1; i < center.size(); ++i)
        s[i] = s[i - 1] + glm::length(center[i] - center[i - 1]);

    const int last = static_cast<int>(center.size()) - 1;
    for (const Spec& sp : specs) {
        const int p0 = std::min(sp.a, sp.b), p1 = std::max(sp.a, sp.b);
        if (p0 < 0 || p1 >= static_cast<int>(ptSample.size()) || p0 == p1) continue;
        const int sa = ptSample[p0], sb = ptSample[p1];
        if (sa < 0 || sb > last || sb <= sa) continue;

        const float s0 = s[sa], s1 = s[sb];
        const float L  = s1 - s0;                     // the stretch the loop is on
        const float R  = std::clamp(sp.radius, 3.0f, 120.0f);
        if (L < 2.0f) continue;                       // the points are on top of each other

        const float adv = L;          // the turn covers the stretch it was given
        const float a   = adv / kTau;  // forward metres per radian of turn

        // Where the rising branch runs into the falling one. Same height means
        // theta and 2*pi - theta; same place as well means
        //     a*theta + R*sin(theta) = pi*a,
        // and that has a root in (0, pi) exactly when R > a -- which is also
        // exactly when the turn inverts at all. Bisection, because the root is
        // bracketed by construction (the left end is -pi*a, the right end is
        // +(R - a)*epsilon) and forty halvings are nothing next to lofting the
        // ribbon it decides the shape of.
        float crossT = -1.0f, crossY = 0.0f;
        if (R > a) {
            auto f = [&](float t) { return a * t + R * std::sin(t) - kPi * a; };
            float lo = 1e-4f, hi = kPi - 1e-4f;
            for (int it = 0; it < 40; ++it) {
                const float mid = 0.5f * (lo + hi);
                (f(mid) < 0.0f ? lo : hi) = mid;
            }
            crossT = 0.5f * (lo + hi);
            crossY = R * (1.0f - std::cos(crossT));
        }

        // How far to lean the turn across the road so those two branches pass
        // beside each other instead of through each other. One carriageway plus a
        // metre is the clearance to buy; `g` is how much of the sway actually
        // shows up at the crossing, so a crossing near the crest (g ~ 1) is cheap
        // and one near the feet (g ~ 0) is not worth buying at all -- which is why
        // it fades out down there rather than being scaled up without limit.
        float sway = 0.0f;
        if (crossT > 0.0f && crossY > kCrossFree * R) {
            const float g    = std::sin(crossT) * std::fabs(std::sin(crossT));
            const float fade = std::clamp((crossY - kCrossFree * R) / (kCrossFade * R),
                                          0.0f, 1.0f);
            const float want = roadWidth + 1.0f;
            if (g > 1e-3f)
                sway = fade * std::min(want / (2.0f * g), kMaxSway * roadWidth);
        }

        Loop lp;
        lp.radius  = R;
        lp.sa = sa; lp.sb = sb;   // the ground this turn stands in place of
        lp.pa = p0; lp.pb = p1;
        lp.sway    = sway;
        lp.inverts = (R > a);
        lp.frames.reserve(kSamples + 1);
        lp.lo = glm::vec3(1e9f);
        lp.hi = glm::vec3(-1e9f);

        for (int k = 0; k <= kSamples; ++k) {
            const float th = kTau * static_cast<float>(k) / kSamples;
            // The ground station this turn has advanced to. Linear in theta, so
            // the loop's footprint is spread evenly along the road rather than
            // bunched where the circle happens to be steep.
            glm::vec2 gp, gd; float gy;
            groundAt(center, s, prof, s0 + adv * th / kTau, gp, gd, gy);
            const glm::vec3 fwd(gd.x, 0.0f, gd.y);
            const glm::vec3 up(0.0f, 1.0f, 0.0f);
            // Across the road, horizontal: the axis the turn sways along.
            const glm::vec3 flatSide = glm::normalize(glm::cross(up, fwd));

            // The sway and its rate. sin*|sin| rather than sin: both are odd about
            // the crest (which is what separates the branches at all), but this one
            // is quadratic at the ends, so the turn leaves and rejoins the road
            // running straight instead of setting off sideways.
            const float sn   = std::sin(th);
            const float lat  = sway * sn * std::fabs(sn);
            const float dlat = sway * 2.0f * std::fabs(sn) * std::cos(th);

            Frame fr;
            // Circle in the vertical plane containing the road direction, plus the
            // advance already baked into `gp`. R*sin(theta) is what makes the turn
            // leave and rejoin the ground TANGENTIALLY (dh/dtheta = 0 at both
            // ends) instead of with a lip.
            fr.ahead = fwd;
            fr.pos = glm::vec3(gp.x, gy, gp.y) + fwd * (R * std::sin(th)) +
                     up * (R * (1.0f - std::cos(th))) + flatSide * lat;
            // d(pos)/d(theta), ignoring how the road itself curves under the turn.
            const glm::vec3 dp = fwd * (R * std::cos(th) + a) +
                                 up * (R * std::sin(th)) + flatSide * dlat;
            fr.tangent = glm::normalize(dp);
            // The frame is built OFF THE TANGENT, not off the circle. The circle's
            // own normal (-sin, cos) is not perpendicular to this path: the advance
            // term leaves dot(tangent, circleNormal) = -(L/2pi)*sin(theta), which
            // is half a radian out at the quarter points -- enough to light the
            // ribbon wrongly and to tilt the gravity the craft is held on by.
            //
            // `side` starts as the horizontal across the road -- well defined even
            // where the tangent passes through vertical, which it does twice and
            // where cross(tangent, worldUp) would collapse -- and is then squared
            // up against the tangent, because a swaying turn's tangent is no longer
            // perpendicular to it. Straightening it here rather than using it raw
            // is what keeps the cross-section square to the track: the raw vector
            // would shear the ribbon by the sway's own angle. The lean it picks up
            // in exchange is real banking, and the craft rides it.
            const glm::vec3 sq = flatSide - fr.tangent * glm::dot(flatSide, fr.tangent);
            fr.side   = glm::normalize(sq);
            fr.normal = glm::normalize(glm::cross(fr.tangent, fr.side));
            // Nose elevation, unwrapped against the previous frame.
            float ang = std::atan2(glm::dot(fr.tangent, up), glm::dot(fr.tangent, fwd));
            if (!lp.frames.empty()) {
                const float prev = lp.frames.back().pitch;
                while (ang - prev >  kPi) ang -= kTau;
                while (ang - prev < -kPi) ang += kTau;
            }
            fr.pitch = ang;
            lp.frames.push_back(fr);
            lp.lo = glm::min(lp.lo, fr.pos);
            lp.hi = glm::max(lp.hi, fr.pos);
        }

        lp.s.assign(lp.frames.size(), 0.0f);
        for (std::size_t i = 1; i < lp.frames.size(); ++i)
            lp.s[i] = lp.s[i - 1] + glm::length(lp.frames[i].pos - lp.frames[i - 1].pos);
        lp.length = lp.s.back();
        // Pad the box by the carriageway, so the engage test can use it directly.
        lp.lo -= glm::vec3(8.0f);
        lp.hi += glm::vec3(8.0f);
        out.push_back(std::move(lp));
    }
    return out;
}

Frame at(const Loop& lp, float arc) {
    if (lp.frames.empty()) return {};
    if (lp.length <= 1e-4f) return lp.frames.front();
    arc = std::clamp(arc, 0.0f, lp.length);            // the ends are NOT the same place
    std::size_t i = static_cast<std::size_t>(
        std::lower_bound(lp.s.begin(), lp.s.end(), arc) - lp.s.begin());
    if (i == 0) i = 1;
    if (i >= lp.frames.size()) i = lp.frames.size() - 1;
    const float seg = std::max(lp.s[i] - lp.s[i - 1], 1e-5f);
    const float t   = std::clamp((arc - lp.s[i - 1]) / seg, 0.0f, 1.0f);
    const Frame& a = lp.frames[i - 1];
    const Frame& b = lp.frames[i];
    Frame f;
    f.pos     = glm::mix(a.pos, b.pos, t);
    f.tangent = glm::normalize(glm::mix(a.tangent, b.tangent, t));
    // Rebuilt from the interpolated tangent rather than interpolated itself, so
    // the frame stays orthonormal between samples too.
    f.side    = glm::normalize(glm::mix(a.side, b.side, t));
    f.normal  = glm::normalize(glm::cross(f.tangent, f.side));
    f.ahead   = glm::normalize(glm::mix(a.ahead, b.ahead, t));
    f.pitch   = glm::mix(a.pitch, b.pitch, t);
    return f;
}

bool locate(const Loop& lp, const glm::vec3& p, float halfWidth, float& outArc,
            float& outLateral, float& outHeight) {
    if (lp.frames.size() < 2) return false;
    if (p.x < lp.lo.x || p.x > lp.hi.x || p.y < lp.lo.y || p.y > lp.hi.y ||
        p.z < lp.lo.z || p.z > lp.hi.z)
        return false;
    // Nearest frame in 3D. A loop passes over its own entry, so anything measured
    // in plan view would answer with the wrong half of the turn -- which is the
    // same mistake the flat road's height query used to make under a flyover.
    std::size_t best = 0;
    float bestD2 = 1e30f;
    for (std::size_t i = 0; i < lp.frames.size(); ++i) {
        const glm::vec3 d = p - lp.frames[i].pos;
        const float d2 = glm::dot(d, d);
        if (d2 < bestD2) { bestD2 = d2; best = i; }
    }
    const Frame& fr = lp.frames[best];
    const glm::vec3 rel = p - fr.pos;
    outArc     = lp.s[best];
    outLateral = glm::dot(rel, fr.side);
    outHeight  = glm::dot(rel, fr.normal);
    // Generous across the carriageway (a craft leaning off the edge is still on
    // the loop) but tight along the normal, or the turn would claim a craft
    // flying past the outside of it.
    return std::fabs(outLateral) < halfWidth + 2.0f &&
           outHeight > -3.0f && outHeight < 8.0f;
}

void build(const std::vector<Loop>& loops, float roadWidth, float texTile,
           fitzel::MeshData& md) {
    const float half = roadWidth * 0.5f;
    const float tile = std::max(texTile, 0.1f);
    for (const Loop& lp : loops) {
        if (lp.frames.size() < 2) continue;
        const auto base = static_cast<std::uint32_t>(md.vertices.size());
        for (std::size_t i = 0; i < lp.frames.size(); ++i) {
            const Frame& fr = lp.frames[i];
            const float v = lp.s[i] / tile;
            md.vertices.push_back({fr.pos - fr.side * half, fr.normal, {0.0f, v}});
            md.vertices.push_back({fr.pos + fr.side * half, fr.normal,
                                   {roadWidth / tile, v}});
        }
        for (std::size_t k = 0; k + 1 < lp.frames.size(); ++k) {
            const auto a = base + static_cast<std::uint32_t>(2 * k);
            // Both windings. Half of a loop is seen from its underside -- from the
            // ground looking up at the top of the turn -- and a single-sided
            // ribbon simply vanishes there.
            md.indices.insert(md.indices.end(),
                              {a, a + 2, a + 1, a + 1, a + 2, a + 3,
                               a, a + 1, a + 2, a + 1, a + 3, a + 2});
        }
    }
}

} // namespace roadloop
