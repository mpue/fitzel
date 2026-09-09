#include "RoadJunction.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

#ifndef FITZEL_PLAYER
#include <imgui.h>
#endif

#include <fitzel/graphics/Mesh.hpp>

namespace roadjunction {
namespace {

// Side of the grid the segments are bucketed into. Four times the centreline
// sample spacing, so a bucket holds a handful of segments per road rather than
// one -- the map lookups cost more than the pair tests do at this size.
constexpr float kCell = 8.0f;

// An arm shorter than this is not a mouth, it is a road that happens to stop
// here. Dropping it is what turns a four-way plate into a T.
constexpr float kMinArm = 0.5f;

// How far the apron sits above the shared height. The ribbon is lofted at
// prof + 0.06 (see RoadSystem::build), so this wins the depth test wherever the
// two overlap -- and they DO overlap, deliberately: the ribbon is cut back a
// quarter metre less than the apron reaches, so the seam is a hidden joint
// rather than a butt joint that shows as a hairline of terrain.
constexpr float kPlateLift = 0.07f;

// The apron reaches this much less than the ribbon is cut back, in metres.
constexpr float kCutInset = 0.25f;

float cross2(const glm::vec2& a, const glm::vec2& b) { return a.x * b.y - a.y * b.x; }

// Squared distance from p to segment [a,b], plus the projection parameter. The
// same primitive RoadSystem's corridor grading uses; kept local because it is
// four lines and exporting it would put a geometry helper in a road header.
float distToSeg(glm::vec2 p, glm::vec2 a, glm::vec2 b, float& t) {
    const glm::vec2 ab = b - a;
    const float len2 = glm::dot(ab, ab);
    t = (len2 > 1e-8f) ? glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    const glm::vec2 proj = a + ab * t;
    return glm::dot(p - proj, p - proj);
}

// Distance between two stations along one road, the short way round when the
// road is a ring. Measured in arc length rather than in samples on purpose:
// sampleSpline gives a short span at least six subdivisions whatever its length
// (see SandboxMath.cpp), so a gap counted in samples is not a gap in metres.
float arcGap(float a, float b, bool closed, float total) {
    float d = std::fabs(a - b);
    if (closed && total > 1e-4f) d = std::min(d, total - d);
    return d;
}

std::int64_t cellKey(int ix, int iz) {
    return (static_cast<std::int64_t>(ix) << 32) ^ static_cast<std::uint32_t>(iz);
}

// One segment of one road, flattened so the grid can hold every road's at once.
struct Seg {
    int       road;
    int       i;      // segment i runs center[i] -> center[i+1]
    glm::vec2 a, b;
};

// Convex hull, monotone chain. There is no hull helper in the sandbox and this
// is the only caller, so it lives here. Returns fewer than 3 points for a
// degenerate input, which the caller reads as "no plate".
std::vector<glm::vec2> hull(std::vector<glm::vec2> p) {
    if (p.size() < 3) return {};
    std::sort(p.begin(), p.end(), [](const glm::vec2& l, const glm::vec2& r) {
        return l.x < r.x || (l.x == r.x && l.y < r.y);
    });
    p.erase(std::unique(p.begin(), p.end(),
                        [](const glm::vec2& l, const glm::vec2& r) {
                            return glm::distance(l, r) < 1e-4f;
                        }),
            p.end());
    if (p.size() < 3) return {};
    std::vector<glm::vec2> h(2 * p.size());
    std::size_t k = 0;
    for (std::size_t i = 0; i < p.size(); ++i) {
        while (k >= 2 && cross2(h[k - 1] - h[k - 2], p[i] - h[k - 2]) <= 0.0f) --k;
        h[k++] = p[i];
    }
    const std::size_t lower = k + 1;
    for (std::size_t i = p.size() - 1; i > 0; --i) {
        while (k >= lower && cross2(h[k - 1] - h[k - 2], p[i - 1] - h[k - 2]) <= 0.0f) --k;
        h[k++] = p[i - 1];
    }
    h.resize(k - 1);
    if (h.size() < 3) return {};
    // Wind it so the triangles face UP. In the XZ plane a face whose normal is
    // +y runs CLOCKWISE as (x,z) is usually drawn -- the same handedness the
    // ribbon's quads come out with (see RoadSystem::loft's index order), which
    // is what the whole engine takes as the front face.
    float area = 0.0f;
    for (std::size_t i = 0; i < h.size(); ++i)
        area += cross2(h[i], h[(i + 1) % h.size()]);
    if (area > 0.0f) std::reverse(h.begin(), h.end());
    return h;
}

// One mouth of a junction: a direction out of it, how wide the road is there and
// how far the apron reaches that way.
struct Arm {
    glm::vec2 dir{0.0f};
    float     half = 0.0f;
    float     len  = 0.0f;
};

// The apron as the convex hull of every mouth's far corners. Building it from
// arms rather than from a pair of bands is what makes a T fall out of the same
// code as an X -- a T is an X with one arm dropped -- and would make a
// three-road meeting fall out of it too.
std::vector<glm::vec2> plateOf(const glm::vec2& at, const std::vector<Arm>& arms) {
    std::vector<glm::vec2> pts;
    pts.reserve(arms.size() * 2);
    for (const Arm& arm : arms) {
        const glm::vec2 tip(at + arm.dir * arm.len);
        const glm::vec2 n(arm.dir.y, -arm.dir.x);   // the road's right, as everywhere else
        pts.push_back(tip + n * arm.half);
        pts.push_back(tip - n * arm.half);
    }
    return hull(std::move(pts));
}
} // namespace

bool insidePlate(const std::vector<glm::vec2>& plate, const glm::vec2& p) {
    if (plate.size() < 3) return false;
    // Sign-agnostic, so it cannot be broken by a later change of winding: a point
    // is inside a convex polygon exactly when every edge turns the same way
    // toward it.
    bool pos = false, neg = false;
    for (std::size_t i = 0; i < plate.size(); ++i) {
        const glm::vec2& a = plate[i];
        const glm::vec2& b = plate[(i + 1) % plate.size()];
        const float c = cross2(b - a, p - a);
        if (c >  1e-5f) pos = true;
        if (c < -1e-5f) neg = true;
        if (pos && neg) return false;
    }
    return true;
}

std::vector<Crossing> find(const std::vector<Trace>& traces) {
    std::vector<Crossing> out;

    // --- Bucket every segment of every road ----------------------------------
    std::vector<Seg> segs;
    for (int r = 0; r < static_cast<int>(traces.size()); ++r) {
        const Trace& t = traces[r];
        if (!t.enabled || !t.params.enabled) continue;
        if (t.center.size() < 2 || t.prof.size() != t.center.size()) continue;
        for (std::size_t i = 0; i + 1 < t.center.size(); ++i)
            segs.push_back({r, static_cast<int>(i), t.center[i], t.center[i + 1]});
    }
    if (segs.empty()) return out;

    std::unordered_map<std::int64_t, std::vector<int>> grid;
    for (int s = 0; s < static_cast<int>(segs.size()); ++s) {
        const glm::vec2 lo = glm::min(segs[s].a, segs[s].b);
        const glm::vec2 hi = glm::max(segs[s].a, segs[s].b);
        const int ix0 = static_cast<int>(std::floor(lo.x / kCell));
        const int ix1 = static_cast<int>(std::floor(hi.x / kCell));
        const int iz0 = static_cast<int>(std::floor(lo.y / kCell));
        const int iz1 = static_cast<int>(std::floor(hi.y / kCell));
        for (int iz = iz0; iz <= iz1; ++iz)
            for (int ix = ix0; ix <= ix1; ++ix) grid[cellKey(ix, iz)].push_back(s);
    }

    // Everything the two tests below agree on: is this pair of stations allowed
    // to meet at all? Height, structure and -- when both stations are on the same
    // road -- distance along it.
    auto admissible = [&](int ra, int rb, int ia, int ib, float arcA, float arcB,
                          float ya, float yb) {
        const Trace& A = traces[ra];
        const Trace& B = traces[rb];
        const float clearance = std::min(A.params.clearance, B.params.clearance);
        if (std::fabs(ya - yb) > clearance) return false;   // an over/under; leave it be
        // Not on a deck, in a bore or on an abutment ramp: those stretches have
        // asked the terrain to leave them alone, and an apron graded into a
        // bridge deck is nonsense.
        auto onGround = [](const Trace& t, int i) {
            if (i + 1 >= static_cast<int>(t.center.size())) return false;
            if (!t.gradeW.empty() &&
                (t.gradeW[i] < 0.999f || t.gradeW[i + 1] < 0.999f)) return false;
            if (!t.standing.empty() && (t.standing[i] || t.standing[i + 1])) return false;
            return true;
        };
        if (!onGround(A, ia) || !onGround(B, ib)) return false;
        if (ra != rb) return true;
        // Two stations on ONE road are only a crossing if they are genuinely far
        // apart along it. Near ones are the road being sampled, not the road
        // meeting itself -- and on a ring the first and last samples are the SAME
        // place (sampleSpline closes it by landing back on point 0), so without
        // this every oval would report a junction at its start line.
        const float total = A.arc.empty() ? 0.0f : A.arc.back();
        const float need  = 4.0f * A.half + A.params.margin;
        return arcGap(arcA, arcB, A.closed, total) >= need;
    };

    auto arcAt = [](const Trace& t, int i, float u) {
        if (t.arc.size() < 2) return 0.0f;
        const int j = std::min(i + 1, static_cast<int>(t.arc.size()) - 1);
        return glm::mix(t.arc[i], t.arc[j], u);
    };

    // --- X: two centrelines that actually cross ------------------------------
    for (const auto& bucket : grid) {
        const std::vector<int>& list = bucket.second;
        for (std::size_t u = 0; u + 1 < list.size(); ++u) {
            for (std::size_t v = u + 1; v < list.size(); ++v) {
                const Seg& sa = segs[list[u]];
                const Seg& sb = segs[list[v]];
                if (sa.road == sb.road && std::abs(sa.i - sb.i) <= 1) continue;
                const glm::vec2 ra = sa.b - sa.a, rb = sb.b - sb.a;
                const float den = cross2(ra, rb);
                if (std::fabs(den) < 1e-9f) continue;             // parallel
                const glm::vec2 d = sb.a - sa.a;
                const float ta = cross2(d, rb) / den;
                const float tb = cross2(d, ra) / den;
                if (ta < 0.0f || ta > 1.0f || tb < 0.0f || tb > 1.0f) continue;
                const glm::vec2 at = sa.a + ra * ta;
                // Whichever bucket the meeting point falls in owns it. Both
                // segments cover that point, so both are registered there, and
                // this is what stops a pair that shares two buckets reporting the
                // same crossing twice.
                if (cellKey(static_cast<int>(std::floor(at.x / kCell)),
                            static_cast<int>(std::floor(at.y / kCell))) != bucket.first)
                    continue;
                const Trace& A = traces[sa.road];
                const Trace& B = traces[sb.road];
                const float aA = arcAt(A, sa.i, ta), aB = arcAt(B, sb.i, tb);
                const float yA = glm::mix(A.prof[sa.i], A.prof[sa.i + 1], ta);
                const float yB = glm::mix(B.prof[sb.i], B.prof[sb.i + 1], tb);
                if (!admissible(sa.road, sb.road, sa.i, sb.i, aA, aB, yA, yB)) continue;
                const float la = glm::length(ra), lb = glm::length(rb);
                if (la < 1e-5f || lb < 1e-5f) continue;
                const float sinT = std::fabs(den) / (la * lb);
                const float minSin = std::sin(glm::radians(
                    std::max(A.params.minAngleDeg, B.params.minAngleDeg)));
                if (sinT < minSin) continue;    // converging, not crossing
                Crossing x;
                x.roadA = sa.road; x.roadB = sb.road;
                x.segA = sa.i; x.segB = sb.i; x.tA = ta; x.tB = tb;
                x.at = at; x.arcA = aA; x.arcB = aB;
                x.yA = yA; x.yB = yB; x.sinAngle = sinT;
                out.push_back(x);
            }
        }
    }

    // --- T: one road ENDS on another's carriageway ---------------------------
    // The plain "the endpoint is on the carriageway" test is not enough on its
    // own: it fires on every service road that runs beside a main road and stops
    // there. Four more conditions have to hold, and the one that carries the rule
    // is the angle -- a road running parallel arrives at no angle at all.
    float reach = 0.0f;
    for (const Trace& t : traces) reach = std::max(reach, t.half + t.params.margin);
    const int rings = std::max(1, static_cast<int>(std::ceil(reach / kCell)));
    for (int rb = 0; rb < static_cast<int>(traces.size()); ++rb) {
        const Trace& B = traces[rb];
        if (!B.enabled || !B.params.enabled || B.closed) continue;
        if (B.center.size() < 2 || B.prof.size() != B.center.size()) continue;
        const int last = static_cast<int>(B.center.size()) - 1;
        for (int e = 0; e < 2; ++e) {
            const int  end   = e == 0 ? 0 : last;
            const int  inner = e == 0 ? 1 : last - 1;
            const glm::vec2 pe = B.center[end];
            glm::vec2 outDir = pe - B.center[inner];
            if (glm::length(outDir) < 1e-5f) continue;
            outDir = glm::normalize(outDir);
            const int segB = e == 0 ? 0 : last - 1;
            const float arcB = B.arc.empty() ? 0.0f : B.arc[end];
            const int cx = static_cast<int>(std::floor(pe.x / kCell));
            const int cz = static_cast<int>(std::floor(pe.y / kCell));
            for (int dz = -rings; dz <= rings; ++dz) {
                for (int dx = -rings; dx <= rings; ++dx) {
                    const auto it = grid.find(cellKey(cx + dx, cz + dz));
                    if (it == grid.end()) continue;
                    for (int si : it->second) {
                        const Seg& sa = segs[si];
                        if (sa.road == rb && std::abs(sa.i - segB) <= 1) continue;
                        const Trace& A = traces[sa.road];
                        // 2) on the carriageway, not merely near the road
                        float t = 0.0f;
                        const float lim = A.half + A.params.margin;
                        const float d2 = distToSeg(pe, sa.a, sa.b, t);
                        if (d2 > lim * lim) continue;
                        const glm::vec2 at = sa.a + (sa.b - sa.a) * t;
                        // 4) B ARRIVES at A rather than skimming past it: its last
                        //    few metres point into A, not along it.
                        if (glm::dot(outDir, at - pe) <= 0.0f) continue;
                        const glm::vec2 ra = sa.b - sa.a;
                        const float la = glm::length(ra);
                        if (la < 1e-5f) continue;
                        // 3) a real approach angle -- the parallel test
                        const float sinT = std::fabs(cross2(ra / la, outDir));
                        const float minSin = std::sin(glm::radians(
                            std::max(A.params.minAngleDeg, B.params.minAngleDeg)));
                        if (sinT < minSin) continue;
                        const float aA = arcAt(A, sa.i, t);
                        const float yA = glm::mix(A.prof[sa.i], A.prof[sa.i + 1], t);
                        const float yB = B.prof[end];
                        if (!admissible(sa.road, rb, sa.i, segB, aA, arcB, yA, yB))
                            continue;
                        glm::vec2 dB = B.center[segB + 1] - B.center[segB];
                        if (glm::length(dB) < 1e-5f) continue;
                        dB = glm::normalize(dB);
                        Crossing x;
                        x.roadA = sa.road; x.roadB = rb;
                        x.segA = sa.i; x.segB = segB; x.tA = t;
                        x.tB = e == 0 ? 0.0f : 1.0f;
                        x.at = at; x.arcA = aA; x.arcB = arcB;
                        x.yA = yA; x.yB = yB; x.sinAngle = sinT;
                        // Signed along B's own arc, so the two ends of a road do
                        // not need separate arithmetic: at the far end `at` lies
                        // beyond the last sample (positive), at the near end
                        // before the first (negative).
                        x.offB = glm::dot(at - pe, dB);
                        x.tee = true;
                        out.push_back(x);
                    }
                }
            }
        }
    }

    // --- One meeting, one crossing -------------------------------------------
    // At two-metre sampling a single X reports from one to four neighbouring
    // segment pairs, because neither polyline is exactly straight there. Left
    // alone that stacks aprons on top of each other and pulls the profile twice.
    // The most perpendicular hit is the best estimate of where the centrelines
    // actually meet, so that is the one kept.
    std::sort(out.begin(), out.end(), [](const Crossing& l, const Crossing& r) {
        const int la = std::min(l.roadA, l.roadB), lb = std::max(l.roadA, l.roadB);
        const int ra = std::min(r.roadA, r.roadB), rb = std::max(r.roadA, r.roadB);
        if (la != ra) return la < ra;
        if (lb != rb) return lb < rb;
        if (l.at.x != r.at.x) return l.at.x < r.at.x;
        if (l.at.y != r.at.y) return l.at.y < r.at.y;
        return l.sinAngle > r.sinAngle;
    });
    std::vector<Crossing> kept;
    for (const Crossing& x : out) {
        const float span = std::max(traces[x.roadA].half, traces[x.roadB].half) * 2.0f;
        bool merged = false;
        for (Crossing& k : kept) {
            const bool samePair =
                (std::min(k.roadA, k.roadB) == std::min(x.roadA, x.roadB) &&
                 std::max(k.roadA, k.roadB) == std::max(x.roadA, x.roadB));
            if (!samePair || glm::distance(k.at, x.at) > span) continue;
            // A tee wins a tie: it means a road really does stop here, and the
            // arm an X would have drawn past it covers ground no ribbon reaches.
            if (x.sinAngle > k.sinAngle || (x.tee && !k.tee)) {
                const bool tee = k.tee || x.tee;
                k = x;
                k.tee = tee;
            }
            merged = true;
            break;
        }
        if (!merged) kept.push_back(x);
    }

    // --- The apron ------------------------------------------------------------
    std::vector<Crossing> done;
    for (Crossing& x : kept) {
        const Trace& A = traces[x.roadA];
        const Trace& B = traces[x.roadB];
        const float margin = std::max(A.params.margin, B.params.margin);
        const float minSin = std::sin(glm::radians(
            std::max(A.params.minAngleDeg, B.params.minAngleDeg)));
        const float sinT = std::max(x.sinAngle, minSin);

        glm::vec2 dA = A.center[x.segA + 1] - A.center[x.segA];
        glm::vec2 dB = B.center[x.segB + 1] - B.center[x.segB];
        if (glm::length(dA) < 1e-5f || glm::length(dB) < 1e-5f) continue;
        dA = glm::normalize(dA);
        dB = glm::normalize(dB);

        // How far each road has to reach to get clear of the other. At a square
        // crossing that is simply the other road's half width; the 1/sin is the
        // oblique case, where the same width takes longer to cross. Capped, so a
        // near-tangent pair that slipped through the angle test can never produce
        // a hundred-metre apron.
        const float capA = 4.0f * (A.half + B.half);
        const float LA = std::min(B.half / sinT + margin, capA);
        const float LB = std::min(A.half / sinT + margin, capA);

        // An arm never reaches past the end of its own road: a road that stops
        // just past the crossing has no mouth there, and an apron drawn over the
        // ground beyond it would be asphalt no ribbon ever meets. This is also
        // the whole of the T case -- the ending road's forward arm comes out at
        // zero length and is dropped.
        const float totA = A.arc.empty() ? 0.0f : A.arc.back();
        const float totB = B.arc.empty() ? 0.0f : B.arc.back();
        // Where the meeting point sits in each road's OWN arc coordinate. The
        // same thing as the crossing for an X. At a T it is not: the stem starts
        // on A's centreline, which is `offB` further along B's line than B itself
        // reaches -- so B's forward arm comes out NEGATIVE and is dropped, which
        // is the whole of what makes a T a T.
        const float qA = x.arcA;
        const float qB = x.arcB + x.offB;
        const float aFwd = A.closed ? LA : std::min(LA, totA - qA);
        const float aBck = A.closed ? LA : std::min(LA, qA);
        const float bFwd = B.closed ? LB : std::min(LB, totB - qB);
        const float bBck = B.closed ? LB : std::min(LB, qB);

        x.dirA = dA;
        std::vector<Arm> arms;
        if (aFwd >= kMinArm) arms.push_back({ dA, A.half, aFwd});
        if (aBck >= kMinArm) arms.push_back({-dA, A.half, aBck});
        if (bFwd >= kMinArm) arms.push_back({ dB, B.half, bFwd});
        if (bBck >= kMinArm) arms.push_back({-dB, B.half, bBck});
        if (arms.size() < 3) continue;   // two roads ending nose to nose: a joint
        x.tee = arms.size() < 4;

        x.plate = plateOf(x.at, arms);
        if (x.plate.size() < 3) continue;

        // The shared height. A road that ENDS here comes to the one that carries
        // on, rather than bending it: a side road meeting a main road should not
        // move the main road.
        const bool aEnds = (aFwd < kMinArm || aBck < kMinArm);
        const bool bEnds = (bFwd < kMinArm || bBck < kMinArm);
        if      (aEnds && !bEnds) x.y = x.yB;
        else if (bEnds && !aEnds) x.y = x.yA;
        else                      x.y = 0.5f * (x.yA + x.yB);

        // The hole in each ribbon, as a window in that road's own arc. It stops
        // a quarter metre INSIDE the apron on both sides, so the last quad ends
        // underneath it: the apron sits a centimetre higher and wins the depth
        // test, whereas cutting past its edge would leave a gap with the terrain
        // showing through. Asymmetric wherever the arms are, which is why it is a
        // window and not a radius.
        auto window = [](float q, float fwd, float bck, float& mid, float& half) {
            const float lo = q - bck + kCutInset;
            const float hi = q + fwd - kCutInset;
            if (hi <= lo) { mid = q; half = 0.0f; return; }
            mid  = 0.5f * (lo + hi);
            half = 0.5f * (hi - lo);
        };
        window(qA, aFwd, aBck, x.cutAtA, x.cutA);
        window(qB, bFwd, bBck, x.cutAtB, x.cutB);
        done.push_back(x);
    }

    std::sort(done.begin(), done.end(), [](const Crossing& l, const Crossing& r) {
        if (l.roadA != r.roadA) return l.roadA < r.roadA;
        if (l.roadB != r.roadB) return l.roadB < r.roadB;
        if (l.arcA  != r.arcA)  return l.arcA  < r.arcA;
        return l.arcB < r.arcB;
    });
    return done;
}

std::vector<Plan> assign(const std::vector<Crossing>& xs,
                         const std::vector<Trace>& traces) {
    std::vector<Plan> out(traces.size());
    for (const Crossing& x : xs) {
        if (x.roadA < 0 || x.roadA >= static_cast<int>(out.size())) continue;
        if (x.roadB < 0 || x.roadB >= static_cast<int>(out.size())) continue;
        const float blend = std::max(traces[x.roadA].params.blend,
                                     traces[x.roadB].params.blend);
        // The flat span is the apron's own reach along this road -- the cut
        // window -- widened to take in the crossing station itself, which at a T
        // sits outside it. Measured in arc length, which on a bend runs LONGER
        // than the straight the apron was cut from: erring that way leaves the
        // road flat a little past the plate, and the other way would leave a step
        // at its edge.
        auto pull = [&](float at, float cutAt, float cut) {
            Pull p;
            p.target = x.y;
            const float lo = std::min(cutAt - cut, at);
            const float hi = std::max(cutAt + cut, at);
            p.flatAt = 0.5f * (lo + hi);
            p.flat   = 0.5f * (hi - lo);
            p.blend  = blend;
            return p;
        };
        out[x.roadA].pulls.push_back(pull(x.arcA, x.cutAtA, x.cutA));
        out[x.roadA].grade.push_back(x);
        if (x.roadB != x.roadA) {
            out[x.roadB].pulls.push_back(pull(x.arcB, x.cutAtB, x.cutB));
            out[x.roadB].grade.push_back(x);
        } else {
            // A road crossing itself takes part twice and grades once.
            out[x.roadA].pulls.push_back(pull(x.arcB, x.cutAtB, x.cutB));
        }
        out[std::min(x.roadA, x.roadB)].plates.push_back(x);
    }
    return out;
}

void profile(const Plan& jp, const std::vector<glm::vec2>& center,
             const std::vector<float>& prof, const std::vector<float>& arc,
             float half, bool closed, std::vector<float>& outOffset,
             std::vector<float>& outBankFade, std::vector<char>& outCut) {
    outOffset.clear();
    outBankFade.clear();
    outCut.clear();
    const std::size_t n = arc.size();
    if (jp.empty() || n < 2 || prof.size() != n || center.size() != n) return;

    const float total = closed ? arc.back() : 0.0f;
    outOffset.assign(n, 0.0f);
    outBankFade.assign(n, 1.0f);
    outCut.assign(n, 0);

    // --- The height -----------------------------------------------------------
    for (std::size_t i = 0; i < n; ++i) {
        float bestW = 0.0f, target = 0.0f;
        for (const Pull& p : jp.pulls) {
            const float d = std::max(
                0.0f, arcGap(arc[i], p.flatAt, closed, total) - p.flat);
            float w = 1.0f;
            if (d > 0.0f) {
                if (p.blend <= 1e-4f) continue;
                const float e = glm::clamp(1.0f - d / p.blend, 0.0f, 1.0f);
                w = e * e * (3.0f - 2.0f * e);
            }
            // The strongest pull wins; they are NOT summed. Two junctions twenty
            // metres apart would otherwise raise the road between them twice and
            // launch anything driving over it.
            if (w > bestW) { bestW = w; target = p.target; }
        }
        if (bestW <= 0.0f) continue;
        outOffset[i]   = bestW * (target - prof[i]);
        outBankFade[i] = 1.0f - bestW;
    }

    // --- The hole -------------------------------------------------------------
    // Per sample first: is the road's WHOLE WIDTH under an apron here? Then a
    // quad goes only where both of its rungs are, which is the difference between
    // a hole that ends under the plate and one that ends a sample past it.
    std::vector<char> covered(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        glm::vec2 fwd = (i == 0)       ? center[1] - center[0]
                      : (i + 1 == n)   ? center[i] - center[i - 1]
                                       : center[i + 1] - center[i - 1];
        if (glm::length(fwd) < 1e-4f) fwd = glm::vec2(0.0f, 1.0f);
        fwd = glm::normalize(fwd);
        const glm::vec2 side(fwd.y, -fwd.x);   // the road's right, as everywhere
        for (const Crossing& x : jp.grade) {
            if (!insidePlate(x.plate, center[i] + side * half) ||
                !insidePlate(x.plate, center[i] - side * half))
                continue;
            covered[i] = 1;
            break;
        }
    }
    for (std::size_t i = 0; i + 1 < n; ++i)
        outCut[i] = static_cast<char>(covered[i] && covered[i + 1]);
    if (closed && n >= 2) outCut[n - 1] = static_cast<char>(covered[n - 1] && covered[0]);
}

void build(const std::vector<Crossing>& plates, float texTile, bool fitted,
           fitzel::MeshData& md) {
    const float tile = (texTile > 1e-4f) ? texTile : 1.0f;
    for (const Crossing& x : plates) {
        if (x.plate.size() < 3) continue;
        glm::vec2 c(0.0f);
        for (const glm::vec2& p : x.plate) c += p;
        c /= static_cast<float>(x.plate.size());
        const float y = x.y + kPlateLift;

        // A fitted image is squared up with road A and sized by the plate's
        // LARGEST extent in that frame, the same on both axes. Fitting each axis
        // separately would stretch a junction image into a lozenge at every
        // oblique crossing -- and a stretched zebra crossing is worse than one
        // that reaches a little past the asphalt, where nothing can see it.
        const glm::vec2 u = x.dirA;
        const glm::vec2 v(u.y, -u.x);
        float ext = 1e-4f;
        for (const glm::vec2& p : x.plate) {
            const glm::vec2 d = p - c;
            ext = std::max(ext, std::max(std::fabs(glm::dot(d, u)),
                                         std::fabs(glm::dot(d, v))));
        }
        const float inv = 0.5f / ext;
        auto push = [&](const glm::vec2& p) {
            const glm::vec2 d = p - c;
            const glm::vec2 uv = fitted
                ? glm::vec2(0.5f + glm::dot(d, u) * inv, 0.5f + glm::dot(d, v) * inv)
                : glm::vec2(p.x / tile, p.y / tile);
            md.vertices.push_back({glm::vec3(p.x, y, p.y), glm::vec3(0.0f, 1.0f, 0.0f),
                                   uv});
        };
        // Fanned from the centroid rather than from a corner: at an oblique
        // crossing the apron is a long thin octagon, and a fan from one of its
        // ends is a sheaf of slivers -- bad triangles to raycast a collider
        // against, and bad ones to shade.
        const auto base = static_cast<std::uint32_t>(md.vertices.size());
        push(c);
        for (const glm::vec2& p : x.plate) push(p);
        const auto n = static_cast<std::uint32_t>(x.plate.size());
        for (std::uint32_t i = 0; i < n; ++i)
            md.indices.insert(md.indices.end(),
                              {base, base + 1 + i, base + 1 + (i + 1) % n});
    }
}

#ifndef FITZEL_PLAYER
bool panel(Params& p) {
    bool rc = ImGui::Checkbox("Junctions", &p.enabled);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Let this road meet others -- and itself -- on the level.\n"
                          "Off, every crossing stays an over/under, whatever the heights.");
    ImGui::BeginDisabled(!p.enabled);
    rc |= ImGui::SliderFloat("Clearance", &p.clearance, 0.5f, 12.0f, "%.1f m");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Above this much height difference the two roads are an\n"
                          "over/under and no junction is made. Raising one road's\n"
                          "control points past this is how you ask for a flyover.");
    rc |= ImGui::SliderFloat("Blend", &p.blend, 4.0f, 80.0f, "%.0f m");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How far along each road the profile is ramped to reach\n"
                          "the shared height. Raise it on steep ground.");
    rc |= ImGui::SliderFloat("Apron margin", &p.margin, 0.0f, 6.0f, "%.1f m");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How far the junction surface reaches past the far\n"
                          "carriageway.");
    rc |= ImGui::SliderFloat("Min angle", &p.minAngleDeg, 5.0f, 60.0f, "%.0f\xC2\xB0");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Shallower than this, two roads are converging rather\n"
                          "than crossing, and no junction is made. This is what\n"
                          "keeps a road running close alongside another from\n"
                          "being read as meeting it.");
    ImGui::EndDisabled();
    return rc;
}
#endif

} // namespace roadjunction
