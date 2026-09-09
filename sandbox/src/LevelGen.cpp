#include "LevelGen.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>

#include "SandboxMath.hpp"   // sampleSpline() -- the same one RoadSystem samples with

namespace levelgen {
namespace {

constexpr float kPi  = 3.14159265358979323846f;
constexpr float kTau = 2.0f * kPi;

// Roughly the spacing sampleSpline aims for. Mirrored from RoadSystem, and what
// lets passesForSigma turn a distance into a number of filter passes.
constexpr float kSampleStep = 2.0f;

// Arc length either side of a declared crossing that is held to the analytic
// curve: the perturbation fades out over it and both relaxations leave it alone.
// Without it the crossing wanders off the place the algebra put it, and every
// decision that names "the crossing station" is naming somewhere else.
constexpr float kCrossFade = 60.0f;

// How far the road has to be off the ground before flying it is better than
// filling under it -- and how far into a hill before boring is better than
// milling a cutting.
constexpr float kBridgeRise = 6.0f;
constexpr float kTunnelSink = 8.0f;
// A span shorter than its own abutments is a lump, not a structure.
constexpr float kMinBridge = 40.0f;
constexpr float kMinTunnel = 60.0f;

// How far apart the control points end up, once the SHAPE has been settled.
//
// `corners` says how many corners the circuit has; it does not say how many
// numbers the profile gets to work with, and those are two different questions.
// ptLift is one value per control point ramped linearly between them, so the
// road between two of them is the smoothed ground plus a straight line: whatever
// slope the land has in there survives untouched. Twelve points over three
// kilometres leaves two hundred metres of hillside per point, and a gradient
// limit asked of that is a limit on the AVERAGE of a slope nobody drives the
// average of. (The first version did exactly that and produced 27% climbs while
// reporting a 9% limit.)
//
// Re-spacing along the curve the shape phases settled costs nothing in shape --
// the points lie ON that curve -- and buys the profile a correction every forty
// metres. It is also what makes a figure of eight come out as one: a lemniscate
// through twelve points is not a lemniscate anywhere near its crossing.
constexpr float kCtrlStep = 20.0f;
// A ceiling only against absurdity, not a budget. Control points cost the scene
// file a few bytes each and the editor a handle each; they do NOT cost the
// corridor grading, whose price is set by the sampled centreline (one station
// every two metres however many points describe it) and by the swept rectangle.
// Capping them lower than the lap needs is how a long circuit ends up with
// thirty-three metres between corrections and a gradient it cannot hold.
constexpr int   kMaxCtrl  = 600;

// Five seeded streams, one per phase, so adding a feature to a later phase does
// not reshuffle the earlier ones -- the same decorrelation discipline as the
// golden-ratio constants in CityGen. A generator whose every knob reshuffles
// everything is one nobody can tune.
constexpr unsigned kPhaseLand    = 0x9e3779b9u;
constexpr unsigned kPhaseLayout  = 0x85ebca6bu;
constexpr unsigned kPhaseFeature = 0x27d4eb2fu;
constexpr unsigned kPhaseDress   = 0x165667b1u;

float cross2(const glm::vec2& a, const glm::vec2& b) { return a.x * b.y - a.y * b.x; }

struct Rng {
    std::mt19937 g;
    explicit Rng(unsigned s) : g(s) {}
    // Deterministic across standard libraries: mt19937 is specified, and this
    // maps its top 24 bits by hand rather than going through
    // uniform_real_distribution, whose implementation is not. A generator that
    // produces a different circuit on someone else's machine is not a
    // generator, it is a surprise.
    float uni(float a, float b) {
        return a + (b - a) * static_cast<float>(g() >> 8) /
                             static_cast<float>((1u << 24) - 1u);
    }
    float sym(float m) { return uni(-m, m); }
};

// The sampled centreline and everything measured along it. `line.front()` and
// `line.back()` are the same place on a closed road (sampleSpline lands back on
// point 0), so the RING has `line.size() - 1` distinct stations.
struct Path {
    std::vector<glm::vec2> line;
    std::vector<int>       pt;     // sample index of each control point (+ closer)
    std::vector<float>     arc;    // arc length at each sample
    float total = 0.0f;
    int   ring  = 0;
    const glm::vec2& at(int k) const { return line[((k % ring) + ring) % ring]; }
    float arcOf(int k) const { return arc[((k % ring) + ring) % ring]; }
};

Path samplePath(const std::vector<glm::vec2>& pts) {
    Path P;
    P.line = sampleSpline(pts, /*closed=*/true, &P.pt);
    if (P.line.size() < 4) return P;
    P.ring = static_cast<int>(P.line.size()) - 1;
    P.arc.assign(P.line.size(), 0.0f);
    for (std::size_t i = 1; i < P.line.size(); ++i)
        P.arc[i] = P.arc[i - 1] + glm::length(P.line[i] - P.line[i - 1]);
    P.total = P.arc.back();
    return P;
}

// The radius of the circle through three consecutive stations. A collinear
// triple comes back "very large" rather than infinite, so callers can compare it
// without a special case.
float radiusAt(const Path& P, int j) {
    const glm::vec2 a = P.at(j - 1), b = P.at(j), c = P.at(j + 1);
    const float area2 = std::fabs(cross2(b - a, c - a));
    if (area2 < 1e-6f) return 1.0e9f;
    return glm::length(b - a) * glm::length(c - b) * glm::length(a - c) / (2.0f * area2);
}

// +1 when the road turns RIGHT here, which is the direction positive bank drops
// the edge in: RoadSystem's `side` is (dir.y, -dir.x), and a right turn puts the
// next heading on that side.
float turnSign(const Path& P, int j) {
    const float x = cross2(P.at(j) - P.at(j - 1), P.at(j + 1) - P.at(j));
    return (x < 0.0f) ? 1.0f : (x > 0.0f ? -1.0f : 0.0f);
}

glm::vec2 dirAt(const Path& P, int j) {
    glm::vec2 d = P.at(j + 1) - P.at(j - 1);
    if (glm::length(d) < 1e-5f) d = glm::vec2(0.0f, 1.0f);
    return glm::normalize(d);
}

// Heading in the convention every craft here uses: dir = (sin H, 0, cos H).
float headingDeg(const glm::vec2& dir) {
    return glm::degrees(std::atan2(dir.x, dir.y));
}

// The shorter way round between two stations on a ring.
float arcGap(float a, float b, float total) {
    const float d = std::fabs(a - b);
    return (total > 1e-4f) ? std::min(d, total - d) : d;
}

// The point at an arc length, INTERPOLATED between the two samples either side
// of it. Snapping to the nearer one instead puts up to half a sample spacing of
// lateral wobble into whatever is built from it -- which at a metre of wobble
// every twenty is a fifty-metre corner radius that was never in the shape, and
// is exactly how the first re-spacing pass produced circuits it then rejected as
// undrivable.
glm::vec2 posAtArc(const Path& P, float s);

int sampleAtArc(const Path& P, float s) {
    if (P.total < 1e-4f || P.ring < 2) return 0;
    s = std::fmod(std::fmod(s, P.total) + P.total, P.total);
    const auto it = std::lower_bound(P.arc.begin(), P.arc.end(), s);
    return std::clamp(static_cast<int>(it - P.arc.begin()), 0, P.ring - 1);
}

glm::vec2 posAtArc(const Path& P, float s) {
    if (P.ring < 2) return glm::vec2(0.0f);
    s = std::fmod(std::fmod(s, P.total) + P.total, P.total);
    const int j = sampleAtArc(P, s);
    const int i = (j > 0) ? j - 1 : 0;
    const float a = P.arc[i], b = P.arc[j];
    if (b - a < 1e-5f) return P.at(j);
    return glm::mix(P.at(i), P.at(j), glm::clamp((s - a) / (b - a), 0.0f, 1.0f));
}

// The control point whose own station is nearest this sample, the short way
// round. It has to ask rather than divide: relaxation moves control points, so
// they are not evenly spaced along the finished curve.
int nearestControl(const Path& P, int sample, int n) {
    int best = 0;
    float bestD = 1.0e9f;
    for (int i = 0; i < n && i + 1 < static_cast<int>(P.pt.size()); ++i) {
        const float d = arcGap(P.arc[P.pt[i]], P.arcOf(sample), P.total);
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

// One Laplacian pass over a closed polygon. Monotonically reduces curvature,
// which is what makes the corner relaxation converge instead of bouncing two
// tight corners off each other for forty iterations.
void relaxRing(std::vector<glm::vec2>& p, const std::vector<char>& pinned, float k) {
    const int n = static_cast<int>(p.size());
    if (n < 3) return;
    std::vector<glm::vec2> out = p;
    for (int i = 0; i < n; ++i) {
        if (!pinned.empty() && pinned[i]) continue;
        const glm::vec2 mid = 0.5f * (p[(i + n - 1) % n] + p[(i + 1) % n]);
        out[i] = p[i] + k * (mid - p[i]);
    }
    p.swap(out);
}

// Rescale a closed ring about its centroid so its sampled length comes back to
// target. The corner pushes lengthen it, and a circuit that grows by a quarter
// every time it is straightened is not the circuit that was asked for.
void rescaleTo(std::vector<glm::vec2>& p, float target) {
    if (p.size() < 3 || target < 1.0f) return;
    const Path P = samplePath(p);
    if (P.total < 1.0f) return;
    glm::vec2 c(0.0f);
    for (const glm::vec2& q : p) c += q;
    c /= static_cast<float>(p.size());
    const float k = target / P.total;
    for (glm::vec2& q : p) q = c + (q - c) * k;
}

// Ramp a per-control-point value out to every sample: linear between the points,
// then lightly smoothed. The two steps RoadSystem::pointRamp takes, because this
// has to predict what that will do rather than something like it.
std::vector<float> pointRamp(const std::vector<float>& perPoint, const Path& P) {
    std::vector<float> out(P.line.size(), 0.0f);
    const int n = static_cast<int>(perPoint.size());
    if (n == 0 || P.pt.size() < 2) return out;
    bool any = false;
    for (float v : perPoint) if (v != 0.0f) { any = true; break; }
    if (!any) return out;
    for (std::size_t k = 0; k + 1 < P.pt.size(); ++k) {
        const int a = P.pt[k], b = P.pt[k + 1];
        if (b <= a) continue;
        const float va = perPoint[k % n], vb = perPoint[(k + 1) % n];
        for (int i = a; i <= b && i < static_cast<int>(out.size()); ++i)
            out[i] = glm::mix(va, vb, static_cast<float>(i - a) /
                                      static_cast<float>(b - a));
    }
    lowPass(out, 5);
    return out;
}

// A run of stations, half-open [a, b). `a` may be negative, meaning the run
// straddles the seam -- a dip that happens to sit under the start line is still
// one dip.
struct Run { int a = 0, b = 0; };

template <typename F>
std::vector<Run> runsWhere(int ring, F test) {
    std::vector<Run> out;
    int start = -1;
    for (int j = 0; j < ring; ++j) {
        if (test(j)) { if (start < 0) start = j; }
        else if (start >= 0) { out.push_back({start, j}); start = -1; }
    }
    if (start >= 0) out.push_back({start, ring});
    if (out.size() >= 2 && out.front().a == 0 && out.back().b == ring) {
        out.front().a = out.back().a - ring;
        out.pop_back();
    }
    return out;
}

// Re-space the control points evenly along the curve they already describe, and
// mark the ones a declared crossing needs held. The shape is unchanged: every
// new point is sampled from the old spline.
std::vector<glm::vec2> densify(const Path& P, const std::vector<float>& declared,
                               std::vector<char>& outPinned) {
    std::vector<glm::vec2> pts;
    if (P.ring < 6 || P.total < 1.0f) return pts;
    const int n = std::clamp(static_cast<int>(std::lround(P.total / kCtrlStep)),
                             8, kMaxCtrl);
    pts.reserve(n);
    outPinned.assign(n, 0);
    for (int i = 0; i < n; ++i) {
        const float s = P.total * static_cast<float>(i) / static_cast<float>(n);
        pts.push_back(posAtArc(P, s));
        for (float cs : declared)
            if (arcGap(s, cs, P.total) < kCrossFade) outPinned[i] = 1;
    }
    return pts;
}

// -----------------------------------------------------------------------------
// Phase A -- the plan view, as a parametrised closed curve.
//
// Not relaxation from noise: the invariants have to hold by CONSTRUCTION, or the
// ring can start life knotted and every later phase inherits the knot.
// -----------------------------------------------------------------------------
struct Seeded {
    std::vector<glm::vec2> pts;
    std::vector<char>      pinned;   // held through both relaxations
    // The self-crossings the SHAPE has, as pairs of arc stations on the base
    // curve. Found on the curve rather than declared per shape, so a new shape
    // is a formula and not a special case -- and so a shape that crosses itself
    // three times needs no more code than one that crosses once.
    std::vector<std::pair<float, float>> meets;
};

// The base curve, at unit scale, for t in [0, 2pi). Everything else about a
// shape -- how long it is, how uneven, where its corners end up -- is done to
// this by the phases that follow.
glm::vec2 baseCurve(Shape s, float t, float aspect, float squareness) {
    switch (s) {
        case Shape::Speedway:
        case Shape::Street: {
            // A superellipse: |x/a|^n + |z|^n = 1. n = 2 is an ellipse; raising
            // it flattens the sides into straights and pushes the turning into
            // the corners, which is the whole difference between a circuit that
            // is one long bend and one you can be flat out on.
            const float n = squareness;
            const float c = std::cos(t), sn = std::sin(t);
            const float e = 2.0f / n;
            return {aspect * (c < 0.0f ? -1.0f : 1.0f) * std::pow(std::fabs(c), e),
                             (sn < 0.0f ? -1.0f : 1.0f) * std::pow(std::fabs(sn), e)};
        }
        case Shape::Eight:
            // Gerono's lemniscate. It crosses itself at t = 0 and t = pi, both
            // at the origin, and the branches meet square.
            return {std::sin(t), 0.5f * std::sin(2.0f * t)};
        case Shape::Knot:
            // The standard planar trefoil. Three crossings, none of them at the
            // same place, which is what makes it the shape junctions were built
            // for -- and what makes it the hardest thing in here to get right.
            return {(std::sin(t) + 2.0f * std::sin(2.0f * t)) / 3.0f,
                    (std::cos(t) - 2.0f * std::cos(2.0f * t)) / 3.0f};
        case Shape::Ring:
        default:
            return {std::cos(t), std::sin(t)};
    }
}

// Every place the base curve crosses itself, as pairs of arc stations.
//
// Measured rather than looked up. The alternative is a table of parameter values
// per shape, which is a table that is right until someone changes a formula by a
// constant and does not think to look at it.
std::vector<std::pair<float, float>> baseMeets(const std::vector<glm::vec2>& base,
                                               const std::vector<float>& s) {
    std::vector<std::pair<float, float>> out;
    const int K = static_cast<int>(base.size()) - 1;
    const float total = s[K];
    // Coarse: a crossing is a place two stretches come together, and at this
    // resolution the pair that is nearest is within a stride of the real one.
    // The relaxations move everything by metres afterwards anyway; what has to
    // survive is WHICH pair of stretches meet, not where to the centimetre.
    const int stride = std::max(1, K / 384);
    for (int i = 0; i < K; i += stride) {
        for (int j = i + stride; j < K; j += stride) {
            // Adjacent stretches are the curve, not a crossing. An eighth of the
            // lap keeps the two apart on every shape here.
            if (arcGap(s[i], s[j], total) < total * 0.125f) continue;
            const glm::vec2 r1 = base[i + stride < K ? i + stride : K] - base[i];
            const glm::vec2 r2 = base[j + stride < K ? j + stride : K] - base[j];
            const float den = cross2(r1, r2);
            if (std::fabs(den) < 1e-9f) continue;
            const glm::vec2 d = base[j] - base[i];
            const float ta = cross2(d, r2) / den, tb = cross2(d, r1) / den;
            if (ta < 0.0f || ta > 1.0f || tb < 0.0f || tb > 1.0f) continue;
            const float sa = s[i] + ta * (s[std::min(i + stride, K)] - s[i]);
            const float sb = s[j] + tb * (s[std::min(j + stride, K)] - s[j]);
            // The SWAPPED pair is the same crossing seen from the other
            // branch, and every crossing is found both ways round: the scan
            // walks i < j, but a ring has no first station, so the stretch that
            // is "later" wraps back to being "earlier". Counting it twice would
            // lift BOTH branches of a figure of eight over each other, which
            // leaves them exactly where they started -- a flyover with no
            // clearance, and no obvious reason why.
            bool dup = false;
            for (const auto& m : out) {
                const bool same = arcGap(m.first, sa, total) < total * 0.02f &&
                                  arcGap(m.second, sb, total) < total * 0.02f;
                const bool swapped = arcGap(m.first, sb, total) < total * 0.02f &&
                                     arcGap(m.second, sa, total) < total * 0.02f;
                if (same || swapped) dup = true;
            }
            if (!dup) out.emplace_back(sa, sb);
        }
    }
    return out;
}

Seeded seedShape(const Params& p, Rng& rng) {
    Seeded S;
    const Shape shape = p.shape;
    const bool  square = (shape == Shape::Speedway || shape == Shape::Street);

    // Shape character, seeded a little so two speedways are not the same oval.
    const float aspect = (shape == Shape::Speedway) ? rng.uni(1.7f, 2.3f)
                       : (shape == Shape::Street)   ? rng.uni(1.1f, 1.5f)
                                                    : 1.0f;
    // How square. Two is an ellipse -- one long bend, which is what a speedway
    // is NOT. Past about five the sides are flat enough to be flat out on and
    // the turning is all in the corners, which is the whole distinction being
    // drawn here.
    const float squareness = (shape == Shape::Speedway) ? rng.uni(5.0f, 7.0f)
                           : (shape == Shape::Street)   ? rng.uni(5.5f, 8.0f)
                                                        : 2.0f;
    const float rot = rng.uni(0.0f, kTau);
    const float ph2 = rng.uni(0.0f, kTau), ph3 = rng.uni(0.0f, kTau),
                ph5 = rng.uni(0.0f, kTau);

    // The base curve, densely, with its arc length -- so control points can be
    // placed evenly along it rather than evenly in a parameter nobody drives.
    constexpr int K = 2048;
    std::vector<glm::vec2> base(K + 1);
    std::vector<float>     s(K + 1, 0.0f);
    for (int k = 0; k <= K; ++k) {
        const float t = kTau * static_cast<float>(k) / static_cast<float>(K);
        base[k] = baseCurve(shape, t, aspect, squareness);
        if (k) s[k] = s[k - 1] + glm::length(base[k] - base[k - 1]);
    }
    // Under target: perturbing a curve lengthens it, and the relaxation phases
    // rescale back to the number that was asked for.
    const float A = (s[K] > 1e-4f) ? p.length / (s[K] * 1.10f) : 1.0f;
    const float total = s[K] * A;

    const std::vector<std::pair<float, float>> meets = baseMeets(base, s);
    std::vector<float> stations;
    for (const auto& m : meets) {
        S.meets.emplace_back(m.first * A, m.second * A);
        stations.push_back(m.first * A);
        stations.push_back(m.second * A);
    }

    // A crossing needs control points near it to hold it in place, so a shape
    // that has one is described densely from the start. Twelve points over three
    // kilometres puts nothing within reach of the crossing to pin, the corner
    // relaxation moves what is either side of it, and the figure of eight quietly
    // stops crossing itself. A plain ring has nothing to hold and is described by
    // its corners, which is what `corners` means.
    const int n = stations.empty()
        ? p.corners
        : std::clamp(static_cast<int>(std::lround(p.length / kCtrlStep)), 24, kMaxCtrl);

    const float irr = std::clamp(p.irregular, 0.0f, 1.0f);
    S.pts.reserve(n);
    S.pinned.assign(n, 0);
    for (int i = 0; i < n; ++i) {
        // Half a step off, so no control point lands ON a crossing.
        const float want = (static_cast<float>(i) + 0.5f) / static_cast<float>(n) * s[K];
        const int k0 = std::clamp(
            static_cast<int>(std::lower_bound(s.begin(), s.end(), want) - s.begin()),
            1, K - 1);
        const glm::vec2 q  = base[k0] * A;
        const glm::vec2 tg = glm::normalize(base[k0 + 1] - base[k0 - 1]);
        const glm::vec2 nrm(tg.y, -tg.x);

        // Faded to nothing near any crossing, so the branches still pass through
        // the point the base curve put them through.
        // Twice the fade the rest of the code uses, because this one has to
        // survive TWO relaxations that are each allowed to move a point by
        // metres. A window that only just covers the crossing leaves the points
        // either side of it free, and the figure of eight comes out as a ring
        // with a kink -- silently, on some seeds and not others.
        float w = 1.0f;
        const float st = s[k0] * A;
        for (float cs : stations) {
            const float e = std::clamp(arcGap(st, cs, total) / (kCrossFade * 2.0f),
                                       0.0f, 1.0f);
            w = std::min(w, e * e * (3.0f - 2.0f * e));
        }
        if (w < 0.999f) S.pinned[i] = 1;

        // Cyclic by construction, and in the CURVE's parameter rather than in the
        // point index -- a noise lookup would leave a step at the seam, and the
        // seam is exactly where the start line goes.
        const float th = kTau * (static_cast<float>(i) + 0.5f) / static_cast<float>(n) + rot;
        const float harm = 0.50f * std::cos(2.0f * th + ph2) +
                           0.33f * std::cos(3.0f * th + ph3) +
                           0.17f * std::cos(5.0f * th + ph5);
        // A speedway is an oval on purpose: unevenness is most of what it is NOT.
        // A street circuit keeps its straights for the same reason, so both take
        // the perturbation at a fraction.
        const float amp = A * (square ? 0.05f : (stations.empty() ? 0.16f : 0.09f)) *
                          irr;
        S.pts.push_back(q + nrm * (amp * harm * w));
    }
    // The whole ring turned, so two seeds do not lay their start lines on top of
    // each other -- and so the shapes above, which are all axis-aligned, are not.
    if (rot != 0.0f) {
        const float c = std::cos(rot), sn = std::sin(rot);
        for (glm::vec2& q : S.pts) q = {q.x * c - q.y * sn, q.x * sn + q.y * c};
        // The crossings turn with it; their ARC stations do not change.
    }
    return S;
}
// Phase B -- push every corner out to the minimum radius, measured on the curve
// that is actually driven rather than on the control polygon.
bool widenCorners(std::vector<glm::vec2>& pts, const std::vector<char>& pinned,
                  const Params& p, float& outMinR) {
    // Aimed a little ABOVE the requirement. The relaxation stops the moment it
    // is met, and everything after it -- the separation pushes, the re-spacing
    // onto even stations -- costs a percent or two of radius. Landing exactly on
    // the number means reporting a circuit as undrivable by a metre, which is
    // both true and useless.
    const float Rmin = minRadius(p) * 1.30f;
    const int   n    = static_cast<int>(pts.size());
    const float cap  = 0.15f * p.length / kTau;
    outMinR = 0.0f;
    for (int iter = 0; iter < 40; ++iter) {
        const Path P = samplePath(pts);
        if (P.ring < 6) return false;
        float worst = 1.0e9f;
        std::vector<glm::vec2> push(n, glm::vec2(0.0f));
        for (int j = 0; j < P.ring; ++j) {
            const float R = radiusAt(P, j);
            worst = std::min(worst, R);
            if (R >= Rmin) continue;
            const int i = nearestControl(P, j, n);
            if (pinned[i]) continue;
            // Away from the local centre of curvature -- the centre is on the
            // side the road turns toward, so pushing the other way is exactly
            // "make this corner rounder".
            const glm::vec2 d = dirAt(P, j);
            push[i] += glm::vec2(d.y, -d.x) * turnSign(P, j) *
                       std::min(0.30f * (Rmin - R), cap);
        }
        outMinR = worst;
        if (worst >= Rmin) return true;
        for (int i = 0; i < n; ++i) pts[i] += push[i];
        relaxRing(pts, pinned, 0.10f);
        rescaleTo(pts, p.length);
    }
    const Path P = samplePath(pts);
    outMinR = 1.0e9f;
    for (int j = 0; j < P.ring; ++j) outMinR = std::min(outMinR, radiusAt(P, j));
    return outMinR >= Rmin;
}

// Phase C -- keep the ring off itself.
//
// roadjunction detects crossings from the drawing, so a circuit that merely
// BRUSHES itself grows aprons, profile pulls and twice-graded ground that
// nobody asked for and no panel mentions.
void separate(std::vector<glm::vec2>& pts, const std::vector<char>& pinned,
              const Params& p, const std::vector<float>& declared, float& outMinD) {
    const int   n       = static_cast<int>(pts.size());
    const float sepDist = 2.0f * (surfaceHalf(p) + 8.0f) + 8.0f;
    const float sepArc  = 4.0f * (surfaceHalf(p) + 8.0f);
    outMinD = 1.0e9f;
    for (int iter = 0; iter < 20; ++iter) {
        const Path P = samplePath(pts);
        if (P.ring < 6) return;
        std::vector<glm::vec2> push(n, glm::vec2(0.0f));
        float worst = 1.0e9f;
        // Every second station: two stretches cannot pass this close without one
        // of the pairs in between seeing it, and the quadratic scan is the one
        // thing here that would be felt in a harness running hundreds of seeds.
        for (int j = 0; j < P.ring; j += 2) {
            for (int k = j + 2; k < P.ring; k += 2) {
                if (arcGap(P.arc[j], P.arc[k], P.total) < sepArc) continue;
                bool skip = false;
                for (float cs : declared)
                    if (arcGap(P.arc[j], cs, P.total) < kCrossFade * 1.5f ||
                        arcGap(P.arc[k], cs, P.total) < kCrossFade * 1.5f) skip = true;
                if (skip) continue;
                const glm::vec2 ab = P.at(k) - P.at(j);
                const float d = glm::length(ab);
                worst = std::min(worst, d);
                if (d >= sepDist || d < 1e-4f) continue;
                const glm::vec2 dir = ab / d;
                const float ex = 0.4f * (sepDist - d);
                push[nearestControl(P, j, n)] -= dir * ex;
                push[nearestControl(P, k, n)] += dir * ex;
            }
        }
        outMinD = worst;
        if (worst >= sepDist) return;
        for (int i = 0; i < n; ++i) if (!pinned[i]) pts[i] += push[i];
        relaxRing(pts, pinned, 0.06f);
        rescaleTo(pts, p.length);
    }
}

// Phase D -- the profile, and the gradient.
//
// ptLift is NOT the profile: RoadSystem::layout already starts from the base
// terrain and low-passes it, and adds ptLift AFTER that. All that is wanted here
// is the correction the smoothing could not make.
struct Profile {
    std::vector<float> ground;    // per sample, straight from the sampler
    std::vector<float> smoothed;  // per sample, what layout() will settle on
    std::vector<float> lift;      // per CONTROL POINT
    std::vector<float> road;      // per sample, smoothed + the ramped lift
    float maxGradient = 0.0f;
    bool  clamped     = false;
};

void buildProfile(Profile& pr, const Path& P, const Params& p, float grade,
                  const std::function<float(float, float)>& groundAt,
                  const std::vector<float>& bias,
                  const std::vector<std::pair<int, int>>& equalities) {
    pr.ground.resize(P.line.size());
    for (std::size_t i = 0; i < P.line.size(); ++i)
        pr.ground[i] = groundAt(P.line[i].x, P.line[i].y);
    pr.smoothed = pr.ground;
    lowPass(pr.smoothed, passesForSigma(3.0f + grade * 15.0f));

    const int n = static_cast<int>(P.pt.size()) - 1;
    pr.lift.assign(std::max(n, 0), 0.0f);
    if (n < 3) return;

    std::vector<float> G(n), L(n);
    for (int i = 0; i < n; ++i) {
        G[i] = pr.smoothed[P.pt[i]] + (bias.empty() ? 0.0f : bias[i]);
        L[i] = std::max(P.arc[P.pt[i + 1]] - P.arc[P.pt[i]], 1.0f);
    }
    // The chain is held tighter than the limit that was asked for, and how much
    // tighter is MEASURED rather than guessed (see the retry at the end of this
    // function). The constraint the solver can express is on the average slope
    // between two control points; what the panel promises is the steepest slope
    // over kGradientWindow of the road that gets driven. Those are different
    // quantities, and the ground inside an interval has a shape of its own.
    float lim0 = p.maxGradient * 0.70f;

    // One cyclic smoothing pass. Cyclic, not lowPass: this is a ring, and a
    // filter that holds its ends would pin the start line to the raw ground and
    // bend the whole lap around it.
    auto smoothRing = [&](std::vector<float>& y, int passes) {
        std::vector<float> t(y.size());
        for (int q = 0; q < passes; ++q) {
            for (int i = 0; i < n; ++i)
                t[i] = 0.5f * y[i] + 0.25f * (y[(i + n - 1) % n] + y[(i + 1) % n]);
            y.swap(t);
        }
    };
    auto worstSlope = [&](const std::vector<float>& y) {
        float w = 0.0f;
        for (int i = 0; i < n; ++i)
            w = std::max(w, std::fabs(y[(i + 1) % n] - y[i]) / L[i]);
        return w;
    };

    auto solve = [&]() {
    // Start from a profile that is already feasible, found by smoothing the
    // ground until it is. A projection onto adjacent-difference constraints
    // propagates like diffusion -- one edge per sweep -- so from a cold start it
    // needs O(n^2) sweeps to carry a correction round the lap, and the first
    // version of this ran 240 of them for seventy points and lifted the road by
    // two metres where it needed fifteen. Smoothing puts it a sweep or two away
    // instead of thousands.
    std::vector<float> y = G;
    for (int passes = 4; passes <= 16384 && worstSlope(y) > lim0; passes *= 2) {
        y = G;
        smoothRing(y, passes);
    }

    auto project = [&](int sweeps) {
        for (int s = 0; s < sweeps; ++s) {
            for (int i = 0; i < n; ++i) {
                const int j = (i + 1) % n;
                const float d = y[j] - y[i];
                const float lim = lim0 * L[i];
                if (std::fabs(d) <= lim) continue;
                const float ex = (std::fabs(d) - lim) * 0.5f * (d > 0.0f ? 1.0f : -1.0f);
                y[i] += ex;
                y[j] -= ex;
            }
            // A level crossing is an equality between two arbitrary stations of
            // that same chain. One more projection edge, with no allowance.
            for (const auto& e : equalities) {
                const float d = y[e.second] - y[e.first];
                y[e.first]  += d * 0.5f;
                y[e.second] -= d * 0.5f;
            }
        }
    };
    // ...then let it back down onto the ground as far as the constraints allow.
    // Smoothing alone gives a feasible profile, but a needlessly flat one: the
    // road would fly over every rise on the lap with an embankment under it.
    for (int iter = 0; iter < 60; ++iter) {
        for (int i = 0; i < n; ++i) y[i] += 0.35f * (G[i] - y[i]);
        project(30);
    }
    project(60);   // end feasible, whatever the last attraction step did

    for (int i = 0; i < n; ++i) {
        const float v = (y[i] - G[i]) + (bias.empty() ? 0.0f : bias[i]);
        if (std::fabs(v) > p.maxLift) pr.clamped = true;
        pr.lift[i] = std::clamp(v, -p.maxLift, p.maxLift);
    }

    const std::vector<float> ramp = pointRamp(pr.lift, P);
    pr.road = pr.smoothed;
    for (std::size_t i = 0; i < pr.road.size() && i < ramp.size(); ++i)
        pr.road[i] += ramp[i];

    pr.maxGradient = worstGradient(P.arc, pr.road);
    };  // solve

    // Solve, then LOOK at the road it produced, and tighten if the answer is not
    // the promise. Four attempts is plenty: each one buys back a third of the
    // overshoot, and a seed that still misses after them has a report saying so
    // rather than a track that quietly breaks its own limit.
    for (int attempt = 0; attempt < 6; ++attempt) {
        pr.clamped = false;
        solve();
        if (pr.maxGradient <= p.maxGradient) break;
        lim0 *= 0.65f;
    }
}

// -----------------------------------------------------------------------------
// Phase F -- bridges, tunnels and loops, and the order they give way in.
// -----------------------------------------------------------------------------

// The control-point span a run of stations sits in, widened by one either side
// so the abutments land on natural ground. Returns false for a run that
// straddles the seam or that no control point falls inside: RoadSystem takes a
// span the LOW-to-HIGH way round, so a wrapped one would bridge the long way.
bool spanOfRun(const Path& P, int n, const Run& r, Span& out) {
    if (r.a < 0) return false;
    int lo = -1, hi = -1;
    for (int i = 0; i < n; ++i) {
        const int s = P.pt[i];
        if (s < r.a || s >= r.b) continue;
        if (lo < 0) lo = i;
        hi = i;
    }
    if (lo < 0) return false;
    out.a = std::max(lo - 1, 0);
    out.b = std::min(hi + 1, n - 1);
    return out.b > out.a;
}

bool spansOverlap(const Span& x, const Span& y) {
    return x.a <= y.b && y.a <= x.b;
}

bool spanCovers(const Span& x, int ctrl) { return ctrl >= x.a && ctrl <= x.b; }

void planFeatures(Level& lv, const Params& p, const Path& P, const Profile& pr,
                  const std::vector<int>& crossCtrl,
                  const std::vector<Span>& flyoverSpans) {
    const int n = static_cast<int>(lv.track.points.size());
    Rng rng(p.seed ^ kPhaseFeature);

    auto arcOfRun = [&](const Run& r) {
        return P.arcOf(std::max(r.b - 1, 0)) - P.arcOf(std::max(r.a, 0));
    };
    auto height = [&](int j) { return pr.road[j] - pr.ground[j]; };

    std::vector<Span> bridges, tunnels;
    // Placed first and never dropped: a lift with no span under it is a dam.
    for (const Span& f : flyoverSpans) bridges.push_back(f);

    // Candidates, longest first: where the road flies, and where it is buried.
    auto collect = [&](bool up, float thresh, float minLen, std::vector<Span>& into,
                       int budget) {
        std::vector<Run> rs = runsWhere(P.ring, [&](int j) {
            return up ? (height(j) > thresh) : (height(j) < -thresh);
        });
        std::sort(rs.begin(), rs.end(), [&](const Run& a, const Run& b) {
            return arcOfRun(a) > arcOfRun(b);
        });
        for (const Run& r : rs) {
            const int already = up ? static_cast<int>(flyoverSpans.size()) : 0;
            if (static_cast<int>(into.size()) >= budget + already) break;
            if (arcOfRun(r) < minLen) continue;
            Span s;
            if (!spanOfRun(P, n, r, s)) { ++lv.report.droppedFeatures; continue; }
            bool clash = false;
            for (const Span& o : bridges) if (spansOverlap(s, o)) clash = true;
            for (const Span& o : tunnels) if (spansOverlap(s, o)) clash = true;
            if (clash) { ++lv.report.droppedFeatures; continue; }
            into.push_back(s);
        }
    };
    collect(true,  kBridgeRise, kMinBridge, bridges, std::max(p.bridges, 0));
    collect(false, kTunnelSink, kMinTunnel, tunnels, std::max(p.tunnels, 0));

    // Loops. A loop must stand on a STRAIGHT: outside a narrow band of advance
    // per radius, roadloop either sways the turn hard across the road or stops
    // inverting altogether and builds a hump.
    std::vector<roadloop::Spec> loops;
    if (p.loops > 0) {
        const float R = std::clamp(p.width * 0.6f, 10.0f, 16.0f);
        const float Rmin = minRadius(p);
        for (int i = 0; i < n && static_cast<int>(loops.size()) < p.loops; ++i) {
            for (int k = 1; k <= 3 && i + k < n; ++k) {
                const int a = i, b = i + k;
                const float len = P.arc[P.pt[b]] - P.arc[P.pt[a]];
                if (len < 2.2f * R || len > 6.0f * R) continue;
                bool straight = true;
                for (int j = P.pt[a]; j <= P.pt[b]; ++j)
                    if (radiusAt(P, j) < 4.0f * Rmin) { straight = false; break; }
                if (!straight) continue;
                // A loop and a span cannot share ground, and a loop over a level
                // crossing would stand the road on end where an apron goes.
                bool clash = false;
                for (int c : crossCtrl) if (c >= 0 && a <= c && c <= b) clash = true;
                for (const Span& s : bridges) if (spansOverlap(s, {a, b})) clash = true;
                for (const Span& s : tunnels) if (spansOverlap(s, {a, b})) clash = true;
                for (const roadloop::Spec& s : loops)
                    if (spansOverlap({s.a, s.b}, {a, b})) clash = true;
                if (clash) continue;
                roadloop::Spec sp;
                sp.a = a; sp.b = b; sp.radius = R;
                loops.push_back(sp);
                break;
            }
        }
        lv.report.droppedFeatures += p.loops - static_cast<int>(loops.size());
    }

    // A span may not cover a LEVEL crossing station: roadjunction refuses a
    // crossing on a deck or in a bore while it is still detecting, so the result
    // would be two ribbons overlapping with no apron and no pull -- which looks
    // like the junction feature being broken rather than like a rule.
    auto dropOverCrossing = [&](std::vector<Span>& v) {
        v.erase(std::remove_if(v.begin(), v.end(), [&](const Span& s) {
            for (const Span& f : flyoverSpans)
                if (s.a == f.a && s.b == f.b) return false;   // that IS the crossing
            bool bad = false;
            for (int c : crossCtrl) if (c >= 0 && spanCovers(s, c)) bad = true;
            if (bad) ++lv.report.droppedFeatures;
            return bad;
        }), v.end());
    };
    if (flyoverSpans.empty() && !crossCtrl.empty()) {
        dropOverCrossing(bridges);
        dropOverCrossing(tunnels);
    }
    for (const roadloop::Spec& s : loops) {
        auto over = [&](std::vector<Span>& v) {
            v.erase(std::remove_if(v.begin(), v.end(), [&](const Span& x) {
                const bool bad = spansOverlap(x, {s.a, s.b});
                if (bad) ++lv.report.droppedFeatures;
                return bad;
            }), v.end());
        };
        over(bridges);
        over(tunnels);
    }

    lv.track.bridges = std::move(bridges);
    lv.track.tunnels = std::move(tunnels);
    lv.track.loops   = std::move(loops);
    (void)rng;
}

// -----------------------------------------------------------------------------
// The race
// -----------------------------------------------------------------------------

// Is this station clear of everything a gate or a grid may not stand on?
bool clearGround(const Level& lv, const Path& P, int ctrl, int sample,
                 float station, const std::vector<float>& crossings, float guard) {
    for (const Span& s : lv.track.bridges) if (spanCovers(s, ctrl)) return false;
    for (const Span& s : lv.track.tunnels) if (spanCovers(s, ctrl)) return false;
    for (const roadloop::Spec& s : lv.track.loops)
        if (ctrl >= s.a && ctrl <= s.b) return false;
    for (float cs : crossings)
        if (arcGap(station, cs, P.total) < guard) return false;
    (void)sample;
    return true;
}

void placeRace(Level& lv, const Params& p, const Path& P, const Profile& pr,
               const std::vector<float>& crossings) {
    const int   n     = static_cast<int>(lv.track.points.size());
    const float Rmin  = minRadius(p);
    const float half  = surfaceHalf(p);
    // Wide enough that the AI's own racing line always crosses it. The opponents
    // run out to width/2 - 1.2 and only count a gate within halfW + 1 of their
    // lane, so a gate sized to the prefab's default would be one they never
    // satisfy -- a field that never completes a lap, which reads as bad AI.
    const float gateW = 2.0f * half + 4.0f;
    const float gateH = 12.0f;
    const float gateD = 6.0f;
    const float gridLane = std::min(2.6f, p.width * 0.18f);
    const float needBehind = 14.0f + 8.0f * std::ceil(p.gridSlots * 0.5f) + 20.0f;

    auto surfaceAt = [&](int j) { return pr.road[j]; };
    auto place = [&](float station, glm::vec3& pos, float& hdg) {
        const int j = sampleAtArc(P, station);
        const glm::vec2 c = P.at(j);
        pos = glm::vec3(c.x, surfaceAt(j), c.y);
        hdg = headingDeg(dirAt(P, j));
        return j;
    };

    // --- The finish line, on the longest straight -----------------------------
    std::vector<Run> straights = runsWhere(P.ring, [&](int j) {
        return radiusAt(P, j) >= 6.0f * Rmin;
    });
    float bestLen = -1.0f, finishStation = 0.0f;
    for (const Run& r : straights) {
        const int a = std::max(r.a, 0);
        const float len = P.arcOf(std::max(r.b - 1, 0)) - P.arcOf(a);
        if (len <= bestLen) continue;
        // 40 percent along, not the middle: the grid goes BEHIND the line and
        // needs straight road of its own.
        const float at = P.arcOf(a) + len * 0.40f;
        const int ctrl = nearestControl(P, sampleAtArc(P, at), n);
        if (!clearGround(lv, P, ctrl, sampleAtArc(P, at), at, crossings,
                         2.0f * half + 20.0f))
            continue;
        bestLen = len;
        finishStation = at;
    }
    if (bestLen < 0.0f) {
        // No straight at all is still a track; the line goes at station 0 and the
        // report says the grid will curve, which is a thing to look at rather
        // than a thing to hide.
        finishStation = 0.0f;
        lv.report.why = "no straight long enough for the grid; it will curve";
    } else if (bestLen < needBehind) {
        lv.report.why = "the start straight is short; the back of the grid curves";
    }

    {
        Marker m;
        m.kind = Marker::Kind::Finish;
        place(finishStation, m.pos, m.headingDeg);
        m.gateW = gateW; m.gateH = gateH; m.gateD = gateD;
        m.station = finishStation;
        m.prefab = p.finishPrefab;
        lv.markers.push_back(m);
    }

    // --- Checkpoints ----------------------------------------------------------
    // Evenly by arc length, then nudged to the nearest straighter station: a gate
    // on a straight is easier to see and easier to fly through. Always ON the
    // centreline -- a gate whose centre is off the road is dropped from the
    // opponents' list entirely while the player still has to fly it, which is a
    // lap nobody can complete.
    const int k = p.checkpoints;
    for (int i = 0; i < k; ++i) {
        float st = finishStation + P.total * static_cast<float>(i + 1) /
                                   static_cast<float>(k + 1);
        const float window = P.total / static_cast<float>(4 * k);
        float bestR = -1.0f, bestSt = st;
        for (float d = -window; d <= window; d += kSampleStep) {
            const float cand = st + d;
            const int j = sampleAtArc(P, cand);
            const int c = nearestControl(P, j, n);
            if (!clearGround(lv, P, c, j, cand, crossings, gateD * 2.0f)) continue;
            if (arcGap(cand, finishStation, P.total) < gateD * 2.0f) continue;
            const float R = radiusAt(P, j);
            if (R > bestR) { bestR = R; bestSt = cand; }
        }
        Marker m;
        m.kind = Marker::Kind::Checkpoint;
        const int j = place(bestSt, m.pos, m.headingDeg);
        m.gateW = gateW; m.gateH = gateH; m.gateD = gateD;
        m.station = std::fmod(std::fmod(bestSt, P.total) + P.total, P.total);
        m.prefab = p.checkPrefab;
        // Which stretch between two crossings this gate is on. Crude, and only
        // ever used to check that a shape which crosses itself has gates on more
        // than one of its lobes -- otherwise the crossing is a shortcut that arc
        // length cannot see.
        m.lobe = 0;
        for (float cs : crossings)
            if (m.station > cs) ++m.lobe;
        (void)j;
        lv.markers.push_back(m);
    }

    // --- The grid -------------------------------------------------------------
    // Rows of two behind the line, laid out exactly as racegrid computes them, so
    // an authored grid and a drawn one look the same.
    for (int s = 0; s < p.gridSlots; ++s) {
        const float back = 14.0f + 8.0f * static_cast<float>(s / 2);
        const float lane = (s % 2 == 0 ? -1.0f : 1.0f) * gridLane;
        const float st = finishStation - back;
        const int j = sampleAtArc(P, st);
        const glm::vec2 c = P.at(j);
        const glm::vec2 d = dirAt(P, j);
        const glm::vec2 side(d.y, -d.x);
        const glm::vec2 q = c + side * lane;
        Marker m;
        m.kind = Marker::Kind::Grid;
        m.pos = glm::vec3(q.x, surfaceAt(j), q.y);
        m.headingDeg = headingDeg(d);
        m.slot = s;
        m.station = std::fmod(std::fmod(st, P.total) + P.total, P.total);
        m.player = (s == 0) && p.pinPlayerPole;
        m.prefab = (s == 0) ? p.playerPrefab : p.rivalPrefab;
        lv.markers.push_back(m);
    }
}

// -----------------------------------------------------------------------------
// The dressing
// -----------------------------------------------------------------------------
void dress(Level& lv, const Params& p, const Path& P) {
    Rng rng(p.seed ^ kPhaseDress);

    if (p.sideObjects) {
        // The rule, not the object: a line with no model draws nothing, and the
        // author fills the model in from the Roads panel. Better than no rule at
        // all, which is a thing they have to know to add.
        roadside::Line rail = roadside::preset(roadside::Kind::GuardRail);
        rail.side  = roadside::Side::Both;
        rail.model = p.railModel;
        lv.sideLines.push_back(rail);
    }

    if (!p.startDecal.empty()) {
        // The start line, on the start line. Placed by typing where it goes,
        // which is the only way to land it there without a steady hand.
        float finish = 0.0f;
        for (const Marker& m : lv.markers)
            if (m.kind == Marker::Kind::Finish) finish = m.station;
        roaddecal::Decal d;
        d.texture = p.startDecal;
        d.dist    = finish;
        d.width   = p.width;
        d.length   = 6.0f;
        d.blend    = roaddecal::Blend::Cutout;
        lv.decals.push_back(d);
    }

    const float amount = std::clamp(p.cityAmount, 0.0f, 1.0f);
    if (amount > 0.02f) {
        // One district, on a stretch of the lap. Canyon when there is a lot of it
        // and Outskirts when there is little: a sparse handful of towers hard
        // against the kerb reads as a mistake, and a canyon that goes all the way
        // round costs the whole frame budget.
        city::Biome b = city::preset(amount > 0.55f ? city::Preset::Canyon
                                                    : city::Preset::Outskirts);
        b.from = P.total * 0.12f;
        b.to   = b.from + P.total * amount * 0.8f;
        b.seed = (p.seed * 2654435761u + 17u) | 1u;
        lv.biomes.push_back(b);
    }
    (void)rng;
}

} // namespace

// --- The two numbers everything else is measured against ----------------------

float surfaceHalf(const Params& p) {
    return p.width * 0.5f + kEdgeWidth * std::cos(glm::radians(kEdgeAngle));
}

float minRadius(const Params& p) {
    // Two floors, and which one binds changes with the parameters. The first is
    // geometric -- a corner tighter than a few road widths is a hairpin the
    // ribbon folds over. The second is the AI's: it corners at
    // sqrt(grip / curvature), so asking for a pace is asking for a radius.
    const float pace = std::max(p.cornerPace, 1.0f);
    return std::max(4.0f * surfaceHalf(p), pace * pace / kAiGrip);
}

float worstGradient(const std::vector<float>& arc, const std::vector<float>& profile) {
    const int m = static_cast<int>(std::min(arc.size(), profile.size())) - 1;
    if (m < 2) return 0.0f;
    const float total = arc[m];
    float worst = 0.0f;
    int k = 0;
    for (int j = 0; j < m; ++j) {
        // Walk a second cursor a window ahead, wrapping the seam: a ring has no
        // last station, and a climb that straddles the start line is one climb.
        while (arc[(k % m)] + (k >= m ? total : 0.0f) - arc[j] < kGradientWindow &&
               k < j + m)
            ++k;
        const float ds = arc[k % m] + (k >= m ? total : 0.0f) - arc[j];
        if (ds < 1.0f) continue;
        worst = std::max(worst, std::fabs(profile[k % m] - profile[j]) / ds);
    }
    return worst;
}

int passesForSigma(float sigmaMetres) {
    const float p = 2.0f * (sigmaMetres / kSampleStep) * (sigmaMetres / kSampleStep);
    return std::clamp(static_cast<int>(std::lround(p)), 1, 600);
}

void lowPass(std::vector<float>& prof, int passes) {
    if (prof.size() < 3) return;
    std::vector<float> tmp = prof;
    for (int p = 0; p < passes; ++p) {
        for (std::size_t i = 1; i + 1 < prof.size(); ++i)
            tmp[i] = 0.5f * prof[i] + 0.25f * (prof[i - 1] + prof[i + 1]);
        std::swap(prof, tmp);
    }
}

const char* shapeName(Shape s) {
    switch (s) {
        case Shape::Ring:     return "Ring";
        case Shape::Speedway: return "Speedway";
        case Shape::Street:   return "Street circuit";
        case Shape::Eight:    return "Figure of eight";
        case Shape::Knot:     return "Trefoil";
        default:              return "Ring";
    }
}

// --- The land ------------------------------------------------------------------

fitzel::TerrainSettings terrainFor(const Params& p) {
    fitzel::TerrainSettings s;   // the engine's own defaults are the baseline
    Rng rng(p.seed ^ kPhaseLand);

    // A noise-coordinate OFFSET, so it has to move far. Stepping it by one gives
    // the same landscape a metre to the side, which reads as the seed doing
    // nothing at all.
    s.seed = static_cast<float>((p.seed * 7919u) % 65536u) * 11.0f;

    const float r = std::clamp(p.relief, 0.05f, 3.0f);
    s.heightScale   = 14.0f * r;
    s.ridgeScale    = 24.0f * r;
    s.continentAmp  = 1.5f * r;
    s.warpStrength  = 14.0f * (0.5f + 0.5f * r);
    s.frequency     = 0.012f * rng.uni(0.8f, 1.25f);
    s.terrace       = rng.uni(0.15f, 0.50f);
    s.peakSharpness = rng.uni(0.85f, 1.35f);
    s.valleyDepth   = rng.uni(0.0f, 10.0f) * r;
    return s;
}

// --- The whole thing -----------------------------------------------------------

namespace {

// One attempt on one landscape. `generate` wraps it in the softening retry.
Level generateOn(const Params& params, const fitzel::TerrainSettings& terrain,
                 const std::function<float(float, float)>& groundAt) {
    Level lv;
    Params p = params;
    p.corners     = std::clamp(p.corners, 6, 24);
    p.length      = std::clamp(p.length, 600.0f, 12000.0f);
    p.width       = std::clamp(p.width, 6.0f, 60.0f);
    p.maxBank     = std::clamp(p.maxBank, 0.0f, 18.0f);
    p.maxGradient = std::clamp(p.maxGradient, 0.01f, 0.30f);
    p.checkpoints = std::clamp(p.checkpoints, 3, 24);
    p.gridSlots   = std::clamp(p.gridSlots, 1, 24);

    lv.terrain = terrain;

    // --- A, B, C: the plan view ----------------------------------------------
    // Laid out, measured, and laid out rounder if it did not come out drivable.
    // A short lap and a wide road are two demands on the same corner: eight
    // hundred metres of circuit at twenty metres wide has room for the radius
    // the AI needs, but not for much unevenness on top of it. Taking the
    // unevenness back is a shape the author can see and judge; shipping a corner
    // nobody can hold is one they find at speed.
    Seeded S;
    std::vector<float> declared;   // every crossing station, both branches
    float minR = 0.0f, minD = 1.0e9f;
    bool  radiusMet = false;
    float irr = p.irregular;
    int   rounded = 0;
    constexpr int kLayoutTries = 6;
    for (int attempt = 0; attempt < kLayoutTries; ++attempt) {
        Params q = p;
        // The last attempt is a plain oval (or a plain lemniscate): no
        // unevenness at all. That one cannot fail on radius unless the lap is
        // too short for the road to be that wide at that pace, which is a thing
        // to report rather than to keep rolling dice against.
        q.irregular = (attempt + 1 == kLayoutTries) ? 0.0f : irr;
        Rng rng(p.seed ^ kPhaseLayout);   // re-seeded, so an attempt is repeatable
        S = seedShape(q, rng);
        declared.clear();
        for (const auto& m : S.meets) { declared.push_back(m.first);
                                       declared.push_back(m.second); }
        widenCorners(S.pts, S.pinned, q, minR);
        separate(S.pts, S.pinned, q, declared, minD);
        // The separation pushes can tighten a corner again, so B runs once more.
        radiusMet = widenCorners(S.pts, S.pinned, q, minR);
        if (radiusMet) break;
        irr *= 0.5f;
        ++rounded;
    }

    {   // The shape is settled; now give the profile something to work with.
        const Path shape = samplePath(S.pts);
        if (shape.ring < 6) {
            lv.report.ok = false;
            lv.report.why = "the circuit collapsed: too few stations survived";
            return lv;
        }
        std::vector<char> pinned;
        std::vector<glm::vec2> dense = densify(shape, declared, pinned);
        if (dense.size() >= 8) { S.pts.swap(dense); S.pinned.swap(pinned); }
    }

    const Path P = samplePath(S.pts);
    if (P.ring < 6) {
        lv.report.ok = false;
        lv.report.why = "the circuit collapsed: too few stations survived";
        return lv;
    }
    const int n = static_cast<int>(S.pts.size());

    // Measured on the curve that will actually be driven, after the re-spacing.
    minR = 1.0e9f;
    for (int j = 0; j < P.ring; ++j) minR = std::min(minR, radiusAt(P, j));
    const bool radiusOk = minR >= minRadius(p) * 0.98f;
    (void)radiusMet;

    lv.track.points   = S.pts;
    lv.track.closed   = true;
    lv.track.width    = p.width;
    lv.track.grade    = 0.90f;
    lv.track.shoulder = 8.0f;

    // --- Where the crossings actually are ------------------------------------
    // The base curve puts them at known stations; after two relaxations the
    // spline passes NEAR those places rather than exactly through them. Each is
    // therefore searched for, but only inside a window around the station the
    // shape declared -- which is what keeps a three-crossing circuit as cheap to
    // lay out as a one-crossing one.
    struct Meet { float arcA = 0.0f, arcB = 0.0f; int ctrlA = -1, ctrlB = -1; };
    std::vector<Meet> meets;
    std::vector<float> stations;   // both branches of every meet, for the guards
    {
        const int span = std::max(8, static_cast<int>(120.0f / kSampleStep));
        for (const auto& d : S.meets) {
            const int wa = sampleAtArc(P, d.first), wb = sampleAtArc(P, d.second);
            float best = 1.0e9f;
            Meet m;
            for (int u = wa - span; u <= wa + span; ++u)
                for (int v = wb - span; v <= wb + span; ++v) {
                    const float dist = glm::length(P.at(u) - P.at(v));
                    if (dist >= best) continue;
                    best = dist;
                    m.arcA = P.arcOf(u);
                    m.arcB = P.arcOf(v);
                }
            // Only a pair that really did come together counts. A shape whose
            // crossing the relaxation pulled apart has no crossing, and saying
            // so is better than lifting a branch over nothing.
            if (best > surfaceHalf(p) * 0.5f) continue;
            m.ctrlA = nearestControl(P, sampleAtArc(P, m.arcA), n);
            m.ctrlB = nearestControl(P, sampleAtArc(P, m.arcB), n);
            meets.push_back(m);
            stations.push_back(m.arcA);
            stations.push_back(m.arcB);
        }
        lv.report.crossings = static_cast<int>(meets.size());
    }

    // --- D: the profile -------------------------------------------------------
    std::vector<float> bias;
    std::vector<std::pair<int, int>> equalities;
    const bool flying = !meets.empty() && p.crossing == Crossing::Flyover;
    if (!meets.empty()) {
        if (p.crossing == Crossing::Level) {
            for (const Meet& m : meets) equalities.emplace_back(m.ctrlA, m.ctrlB);
        } else {
            // Lift ONE branch of each crossing, ramped over its neighbours.
            // Applied as a bias on the ground the projection sees rather than as
            // a value it must preserve: that way the gradient limit shapes the
            // ramp instead of fighting it.
            //
            // The strongest lift wins where two of them overlap, never the sum:
            // on a trefoil the crossings are a third of a lap apart, but a
            // shorter circuit can bring two ramps into each other, and adding
            // them would launch the road between them.
            bias.assign(n, 0.0f);
            for (const Meet& m : meets)
                for (int i = 0; i < n; ++i) {
                    const float d = arcGap(P.arc[P.pt[i]], m.arcB, P.total);
                    const float e = std::clamp(1.0f - d / (kCrossFade * 2.0f),
                                               0.0f, 1.0f);
                    bias[i] = std::max(bias[i],
                                       p.flyover * e * e * (3.0f - 2.0f * e));
                }
        }
    }

    Profile pr;
    buildProfile(pr, P, p, lv.track.grade, groundAt, bias, equalities);
    // The tightest of them: a report that averaged would hide the one crossing
    // that does not clear.
    auto clearance = [&]() {
        float worst = 0.0f;
        bool  first = true;
        for (const Meet& m : meets) {
            const float d = std::fabs(pr.road[sampleAtArc(P, m.arcB)] -
                                      pr.road[sampleAtArc(P, m.arcA)]);
            if (first || d < worst) { worst = d; first = false; }
        }
        return worst;
    };
    // The lift is a bias, not a value the projection must preserve -- which is
    // what lets the limit shape the ramp, and also means the projection flattens
    // some of the lift away. How much is a fact about this seed's hillside, so it
    // is MEASURED and the bias raised until the flyover really does clear what
    // passes under it.
    for (int attempt = 0; flying && attempt < 4 && clearance() < 4.5f; ++attempt) {
        const float k = std::min(2.2f, 5.5f / std::max(clearance(), 0.6f));
        for (float& b : bias) b *= k;
        buildProfile(pr, P, p, lv.track.grade, groundAt, bias, equalities);
    }
    lv.track.lift = pr.lift;
    lv.report.crossingClearance = clearance();

    // --- E: banking -----------------------------------------------------------
    const float Rmin = minRadius(p);
    std::vector<float> bank(n, 0.0f);
    for (int i = 0; i < n; ++i) {
        const int j = P.pt[i];
        // sqrt, not the ratio itself. A craft at its limit needs the same bank
        // whatever the radius -- the required angle goes as v^2/R, and v^2 IS
        // grip*R -- so falling off linearly with radius leaves a fast sweeper
        // dead flat, which is the corner banking exists for.
        const float k = std::clamp(Rmin / std::max(radiusAt(P, j), 1.0f), 0.0f, 1.0f);
        bank[i] = p.maxBank * std::sqrt(k) * turnSign(P, j);
    }
    {   // ease it, so a per-point sawtooth does not survive pointRamp
        std::vector<float> sm = bank;
        for (int i = 0; i < n; ++i)
            sm[i] = 0.5f * bank[i] +
                    0.25f * (bank[(i + n - 1) % n] + bank[(i + 1) % n]);
        bank.swap(sm);
    }
    if (p.crossing == Crossing::Level && !stations.empty()) {
        // The apron is one flat polygon at one height; a branch still banked
        // under it grades a tilted bed and pokes out of its low side.
        for (int i = 0; i < n; ++i) {
            float d = 1.0e9f;
            for (float cs : stations)
                d = std::min(d, arcGap(P.arc[P.pt[i]], cs, P.total));
            const float e = std::clamp(d / kCrossFade, 0.0f, 1.0f);
            bank[i] *= e * e * (3.0f - 2.0f * e);
        }
    }
    lv.track.bank = bank;

    // --- F: the structures ----------------------------------------------------
    // A lift with no span under it is a DAM: the corridor grading would pull the
    // terrain up to the raised profile and bury the branch passing underneath.
    std::vector<Span> flySpans;
    std::vector<int>  crossCtrl;
    for (const Meet& m : meets) {
        crossCtrl.push_back(m.ctrlA);
        crossCtrl.push_back(m.ctrlB);
        if (!flying) continue;
        Span f;
        f.a = std::max(m.ctrlB - 1, 0);
        f.b = std::min(m.ctrlB + 1, n - 1);
        bool clash = false;
        for (const Span& o : flySpans) if (o.a <= f.b && f.a <= o.b) clash = true;
        if (f.b > f.a && !clash) flySpans.push_back(f);
    }
    planFeatures(lv, p, P, pr, crossCtrl, flySpans);

    // --- The race and the dressing -------------------------------------------
    placeRace(lv, p, P, pr, stations);
    dress(lv, p, P);

    // --- The report -----------------------------------------------------------
    Report& R = lv.report;
    R.length          = P.total;
    R.minRadius       = minR;
    R.maxGradient     = pr.maxGradient;
    R.minSelfDistance = minD;
    for (float b : lv.track.bank) R.maxBank = std::max(R.maxBank, std::fabs(b));
    R.bridges = static_cast<int>(lv.track.bridges.size());
    R.tunnels = static_cast<int>(lv.track.tunnels.size());
    R.loops   = static_cast<int>(lv.track.loops.size());
    {
        glm::vec2 lo = P.line[0], hi = P.line[0];
        for (const glm::vec2& q : P.line) { lo = glm::min(lo, q); hi = glm::max(hi, q); }
        const glm::vec2 e = (hi - lo) +
                            glm::vec2(2.0f * (surfaceHalf(p) + lv.track.shoulder));
        R.corridorCells = static_cast<int>(e.x * e.y);
    }
    if (!radiusOk) {
        R.ok  = false;
        R.why = "the tightest corner is too small for this width and pace";
    } else if (rounded) {
        R.why = "the circuit was rounded off to fit the corner radius its width "
                "and pace need";
    }
    if (R.ok && pr.maxGradient > p.maxGradient * 1.15f) {
        R.ok  = false;
        R.why = "the profile is steeper than the gradient limit allows";
    } else if (pr.clamped) {
        R.ok  = false;
        R.why = "the road needed more lift than allowed; soften the relief or "
                "raise the lift limit";
    } else if (flying && R.crossingClearance < 3.5f) {
        R.ok  = false;
        R.why = "the flyover does not clear its own underpass; raise it";
    }
    return lv;
}

} // namespace

Level generate(const Params& p, const GroundFn& groundAt) {
    // Lay the circuit; if the land turns out to be steeper than the gradient
    // limit can survive, soften the land and lay it again. Alpine relief and a
    // nine percent limit are two demands on the same hillside, and the answer an
    // author can use is the circuit they asked for on a gentler landscape --
    // said out loud in the report -- rather than a refusal.
    //
    // Four attempts, each two thirds of the last: past that the land is flat
    // enough that the limit was never the problem, and whatever is left is
    // reported honestly instead of being smoothed at forever.
    fitzel::TerrainSettings ts = terrainFor(p);
    Level lv;
    for (int attempt = 0; attempt < 4; ++attempt) {
        lv = generateOn(p, ts, [&](float x, float z) { return groundAt(ts, x, z); });
        if (lv.report.ok) {
            if (attempt) {
                if (!lv.report.why.empty()) lv.report.why += "; ";
                lv.report.why += "the land was softened to hold the gradient limit";
            }
            return lv;
        }
        // Only a profile fault is worth softening for. A corner that is too
        // tight, or a flyover that does not clear, is not a fact about the
        // hillside, and grinding the mountains down would not fix it.
        const bool profileFault = lv.report.why.rfind("the profile", 0) == 0 ||
                                  lv.report.why.rfind("the road needed", 0) == 0;
        if (!profileFault) return lv;
        ts.heightScale  *= 0.66f;
        ts.ridgeScale   *= 0.66f;
        ts.continentAmp *= 0.66f;
        ts.valleyDepth  *= 0.66f;
    }
    return lv;
}

} // namespace levelgen
