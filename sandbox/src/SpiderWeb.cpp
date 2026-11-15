#include "SpiderWeb.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <numeric>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "SolidBuild.hpp"

using fitzel::AssetId;

namespace spiderweb {

namespace {

using solidbuild::Builder;
using solidbuild::DV2;
using solidbuild::DV3;
using solidbuild::kPi;

constexpr double kPla  = 1.24;    // g/cm3
constexpr double kFilR = 0.875;   // 1.75 mm filament's radius
constexpr double kDeg  = kPi / 180.0;

// --- Chance, the same on every machine ------------------------------------------------

struct Rng {
    std::uint64_t s;
    explicit Rng(int seed) : s(0x9E3779B97F4A7C15ull * static_cast<std::uint64_t>(seed + 1) + 0x632BE59BD9B4E019ull) {}
    double next() {   // [0, 1), splitmix64
        std::uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        return static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0);
    }
    double pm() { return 2.0 * next() - 1.0; }   // [-1, 1)
};

DV2 dirOf(double a) { return {std::cos(a), std::sin(a)}; }
double cross(DV2 a, DV2 b) { return a.x * b.y - a.y * b.x; }
DV2 leftOf(DV2 d) { return {-d.y, d.x}; }
DV2 rightOf(DV2 d) { return {d.y, -d.x}; }

// --- The web as a graph -----------------------------------------------------------------

// A knot where threads meet or end. A disc (the hub, a pad) is round of its
// own radius, the threads running into its edge; any other knot is as big as
// its threads make it.
struct Knot {
    DV2 p;
    double disc = 0.0;
};

// A thread from knot a to knot b along its middle line (pts: a's place first,
// b's last), straight or drooping; a == b is a loop.
struct Strand {
    std::vector<DV2> pts;
    double w = 1.0;
    int a = -1, b = -1;
};

struct Graph {
    std::vector<Knot>   knots;
    std::vector<Strand> strands;
    std::vector<Disc>   discs;
    int spokes = 0, turns = 0;
    double hub = 0.0;

    int knot(DV2 p, double disc = 0.0) {
        knots.push_back({p, disc});
        if (disc > 0.0) discs.push_back({glm::vec2(p), static_cast<float>(disc)});
        return static_cast<int>(knots.size()) - 1;
    }
    void strand(int a, int b, double w, const std::vector<DV2>& between = {}) {
        Strand s;
        s.a = a;
        s.b = b;
        s.w = w;
        s.pts.push_back(knots[static_cast<std::size_t>(a)].p);
        s.pts.insert(s.pts.end(), between.begin(), between.end());
        s.pts.push_back(knots[static_cast<std::size_t>(b)].p);
        strands.push_back(std::move(s));
    }
    // A thread drooping by `sag` towards `towards` in its middle: a parabola,
    // in pieces of about a millimetre.
    void drooping(int a, int b, double w, double sag, DV2 towards) {
        const DV2 P = knots[static_cast<std::size_t>(a)].p, Q = knots[static_cast<std::size_t>(b)].p;
        const double len = glm::length(Q - P);
        if (sag < 0.02 * len || len < 1e-9) { strand(a, b, w); return; }
        const DV2 mid = 0.5 * (P + Q);
        DV2 u = towards - mid;
        const double lu = glm::length(u);
        if (lu < 1e-9) { strand(a, b, w); return; }
        u /= lu;
        const int m = std::clamp(static_cast<int>(std::ceil(len / 1.2)), 2, 10);
        std::vector<DV2> between;
        for (int i = 1; i < m; ++i) {
            const double t = static_cast<double>(i) / m;
            between.push_back(P + (Q - P) * t + u * (4.0 * t * (1.0 - t) * sag));
        }
        strand(a, b, w, between);
    }
    // A loop of radius r round `centre`, starting and ending at knot a.
    void loop(int a, DV2 centre, double r, double w) {
        const DV2 P = knots[static_cast<std::size_t>(a)].p;
        const double a0 = std::atan2(P.y - centre.y, P.x - centre.x);
        const int n = std::max(16, static_cast<int>(std::ceil(2.0 * kPi * r / 1.0)));
        std::vector<DV2> between;
        for (int i = 1; i < n; ++i) between.push_back(centre + r * dirOf(a0 + 2.0 * kPi * i / n));
        strand(a, a, w, between);
    }
};

// Where the left edge of a thread leaving C along d1 (half width h1) meets the
// right edge of the next one counter-clockwise, along d2 (half width h2).
DV2 miter(DV2 C, DV2 d1, double h1, DV2 d2, double h2) {
    const DV2 P1 = C + h1 * leftOf(d1), P2 = C + h2 * rightOf(d2);
    const double den = cross(d1, d2);
    if (std::abs(den) < 1e-12) return C + 0.5 * (h1 + h2) * leftOf(d1);
    return P1 + d1 * (cross(P2 - P1, d2) / den);
}

// The radius at which threads `gap` apart in angle are `chord` apart.
double radiusForChord(double chord, double gap) { return chord / (2.0 * std::sin(0.5 * gap)); }

// The largest angle between two neighbouring directions of `angles` (sorted,
// all round) or between neighbours of an open fan.
double smallestGap(const std::vector<double>& a, bool round) {
    double g = 1e30;
    for (std::size_t i = 0; i + 1 < a.size(); ++i) g = std::min(g, a[i + 1] - a[i]);
    if (round && !a.empty()) g = std::min(g, a.front() + 2.0 * kPi - a.back());
    return g;
}

double widestGap(const std::vector<double>& a, bool round) {
    double g = 0.0;
    for (std::size_t i = 0; i + 1 < a.size(); ++i) g = std::max(g, a[i + 1] - a[i]);
    if (round && !a.empty()) g = std::max(g, a.front() + 2.0 * kPi - a.back());
    return g;
}

// How unevenly the knots lie along a radius, and how unevenly the threads
// across droop, as shares of the spacing; both eat into the holes between two
// turns, together with the droop itself: a turn's threads are longer than the
// ones inside it, so they droop further. What is left between two turns, as a
// share of the spacing, where the gap between two radii is `gap`.
constexpr double kKnotJitter = 0.08, kDroopJitter = 0.12;
double leftBetween(double gap, double sag, double irr) {
    return std::cos(0.5 * gap) - 2.0 * sag * std::sin(0.5 * gap) - 2.0 * (kKnotJitter + kDroopJitter) * irr;
}
// The fan of radii as uneven as `irr` makes it: gaps between (1 -+ irr / 2) of even.
double gapSpread(double even, double irr, bool widest) { return even * (widest ? 1.0 + 0.5 * irr : 1.0 - 0.5 * irr); }

// The hub's disc: big enough that the radii running into it stay apart.
double hubDisc(double hS, double minGap) { return std::max(2.0, hS / std::sin(0.3 * minGap)); }

// --- An orb web: in a hoop, or on a frame -----------------------------------------------

void orbWeb(const Params& p, Graph& g) {
    Rng rng(p.seed);
    const int N = p.spokes;
    const double hS = 0.5 * p.spokeWidth, hC = 0.5 * p.spiralWidth, hF = 0.5 * p.frameWidth;
    const double irr = p.irregular, step = 2.0 * kPi / N;
    const bool round = p.shape == Shape::Round;
    g.spokes = N;

    // The radii: the first straight up (where the hoop's loop is), the others a
    // little off their even places.
    std::vector<double> ang(static_cast<std::size_t>(N));
    for (int j = 0; j < N; ++j) ang[static_cast<std::size_t>(j)] = 0.5 * kPi + j * step + (j ? 0.25 * irr * step * rng.pm() : 0.0);
    const double minGap = smallestGap(ang, true), maxGap = widestGap(ang, true);

    // The frame, and how far a point lies inside it.
    const double R = 0.5 * p.size - hF;   // the hoop's or the frame corners' middle line
    std::vector<DV2> corner;
    if (!round) {
        const int K = p.corners;
        for (int k = 0; k < K; ++k) {
            const int j = k * N / K;
            const double a0 = ang[static_cast<std::size_t>(j)];
            const double a1 = j + 1 < N ? ang[static_cast<std::size_t>(j + 1)] : ang[0] + 2.0 * kPi;
            const double r = R * (1.0 - 0.2 * irr * rng.next());
            corner.push_back(r * dirOf(0.5 * (a0 + a1)));   // between two radii, never on one
        }
    }
    auto inside = [&](DV2 q) {   // negative outside
        if (round) return R - glm::length(q);
        double d = 1e30;
        bool in = false;
        for (std::size_t k = 0; k < corner.size(); ++k) {
            const DV2 A = corner[k], B = corner[(k + 1) % corner.size()];
            const double t = std::clamp(glm::dot(q - A, B - A) / glm::dot(B - A, B - A), 0.0, 1.0);
            d = std::min(d, glm::length(q - (A + t * (B - A))));
            if ((A.y > q.y) != (B.y > q.y) && q.x < A.x + (B.x - A.x) * (q.y - A.y) / (B.y - A.y)) in = !in;
        }
        return in ? d : -d;
    };
    // Where radius j meets the frame.
    std::vector<DV2> rim(static_cast<std::size_t>(N));
    std::vector<int> rimEdge(static_cast<std::size_t>(N), -1);
    for (int j = 0; j < N; ++j) {
        const DV2 d = dirOf(ang[static_cast<std::size_t>(j)]);
        if (round) {
            rim[static_cast<std::size_t>(j)] = R * d;
            continue;
        }
        for (std::size_t k = 0; k < corner.size(); ++k) {
            const DV2 A = corner[k], B = corner[(k + 1) % corner.size()];
            const double den = cross(d, B - A);
            if (std::abs(den) < 1e-12) continue;
            const double t = cross(A, B - A) / den, u = cross(A, d) / den;
            if (t > 0.0 && u >= 0.0 && u < 1.0) {
                rim[static_cast<std::size_t>(j)] = t * d;
                rimEdge[static_cast<std::size_t>(j)] = static_cast<int>(k);
            }
        }
    }

    // The hub: a disc the radii run into, its tight turns, then the free zone.
    const double disc = hubDisc(hS, minGap);
    g.hub = disc;
    const int centre = g.knot({0.0, 0.0}, disc);
    // A thread across two radii runs inside the knots it joins, the more so the
    // wider apart they are: the hub's first turn (straight) clear of the disc,
    // the capture spiral's first (drooping) clear of the hub.
    const double inward = std::max(0.2, std::cos(0.5 * maxGap));
    const double hubStep = (p.spokeWidth + 1.2) / std::max(0.3, inward - 0.1 * irr);
    const double rh0 = std::max((disc + 2.0 * hS + 1.5) / inward, radiusForChord(p.spokeWidth + 1.2, minGap));
    const double droopIn = std::max(0.2, inward - 2.0 * p.sag * std::sin(0.5 * maxGap));
    // A small web has no room for all of the hub and the free zone.
    const double room = 0.55 * (round ? R : R * std::cos(kPi / std::max(3, p.corners)) * (1.0 - 0.2 * irr));
    int hubTurns = p.hubTurns;
    double freeZone = p.freeZone, rc0 = 0.0;
    for (;;) {
        // The hub's last turn reaches a whole step out from where it starts.
        const double hubEdge = hubTurns > 0 ? rh0 + hubStep * hubTurns : disc + hS;
        rc0 = std::max((hubEdge + freeZone + hS + hC + 1.2 + 2.0 * (kKnotJitter + kDroopJitter) * irr * p.spacing) / droopIn,
                       radiusForChord(p.spokeWidth + 2.0, minGap));
        if (rc0 <= room || (hubTurns == 0 && freeZone <= 0.0)) break;
        if (hubTurns > 0) --hubTurns;
        else freeZone = std::max(0.0, freeZone - 2.0);
    }
    const double clearC = hF + hC + std::max(1.2, 0.35 * p.spacing);
    const double clearH = hF + hS + 1.0;

    // On every radius the knots along it, by their distance from the middle.
    std::vector<std::vector<std::pair<double, int>>> along(static_cast<std::size_t>(N));
    auto cum = [&](int j) { return ang[static_cast<std::size_t>(j)] - ang[0]; };

    // A spiral of `turns` (or as many as fit) from r0 out, `gapR` from turn to
    // turn; `jit` its unevenness, `sag` its droop, `torn` the share missing.
    auto spiral = [&](double r0, double gapR, int maxTurns, double w, double clear, double jit, double sag,
                      double droopJit, double torn) {
        struct Spot { DV2 p; bool fits; int id = -1; };
        std::vector<std::vector<Spot>> spot;
        for (int k = 0; k < maxTurns; ++k) {
            std::vector<Spot> turn;
            bool any = false;
            for (int j = 0; j < N; ++j) {
                const double r = r0 + gapR * (k + cum(j) / (2.0 * kPi)) + jit * gapR * rng.pm();
                const DV2 q = r * dirOf(ang[static_cast<std::size_t>(j)]);
                const bool fits = inside(q) >= clear;
                any |= fits;
                turn.push_back({q, fits});
            }
            if (!any) break;
            spot.push_back(std::move(turn));
        }
        auto id = [&](int k, int j) {
            Spot& s = spot[static_cast<std::size_t>(k)][static_cast<std::size_t>(j)];
            if (s.id < 0) {
                s.id = g.knot(s.p);
                along[static_cast<std::size_t>(j)].push_back({glm::length(s.p), s.id});
            }
            return s.id;
        };
        int laid = 0;
        for (int k = 0; k < static_cast<int>(spot.size()); ++k)
            for (int j = 0; j < N; ++j) {
                const int k2 = j + 1 < N ? k : k + 1, j2 = j + 1 < N ? j + 1 : 0;
                const bool gone = rng.next() < torn;   // drawn for every thread, so a seed tears the same ones
                if (k2 >= static_cast<int>(spot.size())) continue;
                if (!spot[static_cast<std::size_t>(k)][static_cast<std::size_t>(j)].fits ||
                    !spot[static_cast<std::size_t>(k2)][static_cast<std::size_t>(j2)].fits || gone)
                    continue;
                const int a = id(k, j), b = id(k2, j2);
                const double len = glm::length(g.knots[static_cast<std::size_t>(b)].p - g.knots[static_cast<std::size_t>(a)].p);
                g.drooping(a, b, w, sag > 0.0 ? sag * len + droopJit * gapR * rng.pm() : 0.0, DV2(0.0));
                ++laid;
            }
        return std::pair<int, int>(static_cast<int>(spot.size()), laid);
    };
    if (hubTurns > 0) spiral(rh0, hubStep, hubTurns, p.spokeWidth, clearH, 0.05 * irr, 0.0, 0.0, 0.0);
    g.turns = spiral(rc0, p.spacing, 400, p.spiralWidth, clearC, kKnotJitter * irr, p.sag, kDroopJitter * irr, p.torn).first;

    // The frame's knots: where the radii meet it, and its corners.
    std::vector<int> rimKnot(static_cast<std::size_t>(N));
    for (int j = 0; j < N; ++j) rimKnot[static_cast<std::size_t>(j)] = g.knot(rim[static_cast<std::size_t>(j)]);
    // The radii, from the hub out to the frame.
    for (int j = 0; j < N; ++j) {
        auto& ks = along[static_cast<std::size_t>(j)];
        std::sort(ks.begin(), ks.end());
        int from = centre;
        for (const auto& [r, k] : ks) {
            g.strand(from, k, p.spokeWidth);
            from = k;
        }
        g.strand(from, rimKnot[static_cast<std::size_t>(j)], p.spokeWidth);
    }
    if (round) {
        // The hoop: arcs from radius to radius, a few degrees a piece.
        for (int j = 0; j < N; ++j) {
            const double a0 = ang[static_cast<std::size_t>(j)];
            const double a1 = j + 1 < N ? ang[static_cast<std::size_t>(j + 1)] : ang[0] + 2.0 * kPi;
            const int n = std::max(2, static_cast<int>(std::ceil((a1 - a0) / (3.0 * kDeg))));
            std::vector<DV2> between;
            for (int i = 1; i < n; ++i) between.push_back(R * dirOf(a0 + (a1 - a0) * i / n));
            g.strand(rimKnot[static_cast<std::size_t>(j)], rimKnot[static_cast<std::size_t>((j + 1) % N)], p.frameWidth,
                     between);
        }
        if (p.eyelet) {
            // A short stem straight up from the top radius, and a loop on it.
            const double loopR = std::max(3.5, hF + 2.5);
            const DV2 top = rim[0];
            const int stem = g.knot(top + DV2(0.0, 2.0 * hF + 2.0));
            g.strand(rimKnot[0], stem, p.frameWidth);
            g.loop(stem, g.knots[static_cast<std::size_t>(stem)].p + DV2(0.0, loopR), loopR, p.frameWidth);
        }
    } else {
        // The frame's straight threads, cut where the radii meet them; from each
        // corner an anchor thread out.
        std::vector<int> cornerKnot;
        for (const DV2& c : corner) cornerKnot.push_back(g.knot(c));
        const int K = static_cast<int>(corner.size());
        for (int k = 0; k < K; ++k) {
            std::vector<std::pair<double, int>> on;
            const DV2 A = corner[static_cast<std::size_t>(k)], B = corner[static_cast<std::size_t>((k + 1) % K)];
            for (int j = 0; j < N; ++j)
                if (rimEdge[static_cast<std::size_t>(j)] == k)
                    on.push_back({glm::dot(rim[static_cast<std::size_t>(j)] - A, B - A), rimKnot[static_cast<std::size_t>(j)]});
            std::sort(on.begin(), on.end());
            int from = cornerKnot[static_cast<std::size_t>(k)];
            for (const auto& [t, kn] : on) {
                g.strand(from, kn, p.frameWidth);
                from = kn;
            }
            g.strand(from, cornerKnot[static_cast<std::size_t>((k + 1) % K)], p.frameWidth);
        }
        for (int k = 0; k < K; ++k) {
            const DV2 c = corner[static_cast<std::size_t>(k)], out = glm::normalize(c);
            const DV2 end = c + out * static_cast<double>(p.anchor);
            if (p.ends == Ends::Pads) {
                g.strand(cornerKnot[static_cast<std::size_t>(k)], g.knot(end, std::max(4.0, 2.0 * hF + 2.0)),
                         p.frameWidth);
            } else {
                const int e = g.knot(end);
                g.strand(cornerKnot[static_cast<std::size_t>(k)], e, p.frameWidth);
                if (p.ends == Ends::Loops) {
                    const double loopR = std::max(3.5, hF + 2.5);
                    g.loop(e, end + out * loopR, loopR, p.frameWidth);
                }
            }
        }
    }
}

// --- A corner web ----------------------------------------------------------------------

void cornerWeb(const Params& p, Graph& g) {
    Rng rng(p.seed);
    const int N = p.spokes;
    const double hS = 0.5 * p.spokeWidth, hC = 0.5 * p.spiralWidth, hF = 0.5 * p.frameWidth;
    const double irr = p.irregular, L = p.size, step = 0.5 * kPi / (N + 1);
    g.spokes = N;
    // The walls along x and y, the radii fanning out between them.
    std::vector<double> ang{0.0};
    for (int j = 0; j < N; ++j) ang.push_back((j + 1) * step + 0.25 * irr * step * rng.pm());
    ang.push_back(0.5 * kPi);
    std::vector<double> half(ang.size(), hS);
    half.front() = half.back() = hF;
    // How far out from the corner the radii run together.
    double joined = 0.0;
    for (std::size_t i = 0; i + 1 < ang.size(); ++i)
        joined = std::max(joined, glm::length(miter(DV2(0.0), dirOf(ang[i]), half[i], dirOf(ang[i + 1]), half[i + 1])));
    const double minGap = smallestGap(ang, false), maxGap = widestGap(ang, false);
    // The first thread across clear of where the radii run together, drooping
    // and all, and far enough out that the wedges between the radii inside it
    // hold a round of 1.2 mm.
    const double droopIn = std::max(0.2, std::cos(0.5 * maxGap) - 2.0 * p.sag * std::sin(0.5 * maxGap));
    const double sinHalf = std::sin(0.5 * minGap);
    const double r1 = std::max({(joined + std::max(hS, hF) + hC + 1.2 + 2.0 * (kKnotJitter + kDroopJitter) * irr * p.spacing) /
                                    droopIn,
                                (joined + 0.6 * (1.0 + sinHalf) / sinHalf + hC +
                                 2.0 * (kKnotJitter + kDroopJitter) * irr * p.spacing) / droopIn,
                                radiusForChord(p.spokeWidth + 2.0, minGap)});
    const double last = L - 2.0 * hF - std::max(4.0, 0.6 * p.spacing);
    const int K = std::max(1, static_cast<int>(std::floor((last - r1) / p.spacing)) + 1);
    g.turns = K;

    const int corner = g.knot({0.0, 0.0});
    std::vector<std::vector<int>> ring(static_cast<std::size_t>(K));
    for (int k = 0; k < K; ++k) {
        const double r = std::min(last, r1 + k * p.spacing);
        for (std::size_t i = 0; i < ang.size(); ++i) {
            const bool wall = i == 0 || i + 1 == ang.size();
            const double rr = r + (k + 1 < K || !wall ? kKnotJitter * irr * p.spacing * rng.pm() : 0.0);
            ring[static_cast<std::size_t>(k)].push_back(g.knot(rr * dirOf(ang[i])));
        }
    }
    // The threads across, from wall to wall, the outermost one as strong as the
    // walls; some of the others torn.
    for (int k = 0; k < K; ++k) {
        const bool outer = k + 1 == K;
        for (std::size_t i = 0; i + 1 < ang.size(); ++i) {
            const bool gone = rng.next() < p.torn;
            if (gone && !outer) continue;
            const int a = ring[static_cast<std::size_t>(k)][i], b = ring[static_cast<std::size_t>(k)][i + 1];
            const double len = glm::length(g.knots[static_cast<std::size_t>(b)].p - g.knots[static_cast<std::size_t>(a)].p);
            g.drooping(a, b, outer ? p.frameWidth : p.spiralWidth,
                       (outer ? 0.5 : 1.0) * p.sag * len + (p.sag > 0.0f ? kDroopJitter * irr * p.spacing * rng.pm() : 0.0),
                       DV2(0.0));
        }
    }
    // The radii out to the outermost thread, the walls on past it.
    std::vector<int> used(g.knots.size(), 0);
    for (const Strand& s : g.strands) {
        ++used[static_cast<std::size_t>(s.a)];
        ++used[static_cast<std::size_t>(s.b)];
    }
    for (std::size_t i = 0; i < ang.size(); ++i) {
        const bool wall = i == 0 || i + 1 == ang.size();
        int from = corner;
        for (int k = 0; k < K; ++k) {
            const int kn = ring[static_cast<std::size_t>(k)][i];
            if (!used[static_cast<std::size_t>(kn)]) continue;   // both threads across it torn
            g.strand(from, kn, wall ? p.frameWidth : p.spokeWidth);
            from = kn;
        }
        // The wall's rounded end and the corner's rounded outside each reach
        // half a wall further: L from end to end.
        if (wall) g.strand(from, g.knot((L - 2.0 * hF) * dirOf(ang[i])), p.frameWidth);
    }
}

// --- Threads to a flat plate -------------------------------------------------------------

struct Plate {
    std::vector<DV2> pts;
    std::vector<glm::ivec3> tris;   // counter-clockwise from above
    std::string error;
};

void thicken(const Graph& g, Plate& out) {
    auto add = [&](DV2 q) {
        out.pts.push_back(q);
        return static_cast<int>(out.pts.size()) - 1;
    };
    auto tri = [&](int a, int b, int c) {
        const double s = cross(out.pts[static_cast<std::size_t>(b)] - out.pts[static_cast<std::size_t>(a)],
                               out.pts[static_cast<std::size_t>(c)] - out.pts[static_cast<std::size_t>(a)]);
        if (s <= 1e-7 && out.error.empty()) {
            const DV2 q = out.pts[static_cast<std::size_t>(a)], r = out.pts[static_cast<std::size_t>(b)],
                      u = out.pts[static_cast<std::size_t>(c)];
            char b2[200];
            std::snprintf(b2, sizeof b2, "threads too close to join at %.1f, %.1f (%.2f, %.2f; %.2f, %.2f)", q.x, q.y,
                          r.x, r.y, u.x, u.y);
            out.error = b2;
        }
        out.tris.push_back({a, b, c});
    };

    // Each strand's corners where it leaves a knot: right and left of it,
    // looking along it out of the knot.
    const std::size_t S = g.strands.size();
    std::vector<std::array<int, 2>> cR(S), cL(S);
    struct Inc {
        int s, end;
        double ang, h;
        DV2 d;
    };
    std::vector<std::vector<Inc>> at(g.knots.size());
    for (std::size_t i = 0; i < S; ++i) {
        const Strand& s = g.strands[i];
        const std::size_t n = s.pts.size();
        const DV2 d0 = glm::normalize(s.pts[1] - s.pts[0]), d1 = glm::normalize(s.pts[n - 2] - s.pts[n - 1]);
        at[static_cast<std::size_t>(s.a)].push_back({static_cast<int>(i), 0, std::atan2(d0.y, d0.x), 0.5 * s.w, d0});
        at[static_cast<std::size_t>(s.b)].push_back({static_cast<int>(i), 1, std::atan2(d1.y, d1.x), 0.5 * s.w, d1});
    }
    for (std::size_t n = 0; n < g.knots.size(); ++n) {
        std::vector<Inc>& inc = at[n];
        if (inc.empty()) continue;
        std::sort(inc.begin(), inc.end(), [](const Inc& a, const Inc& b) { return a.ang < b.ang; });
        const DV2 C = g.knots[n].p;
        const double rho = g.knots[n].disc;
        const std::size_t k = inc.size();
        std::vector<int> poly;
        auto push = [&](int v) {
            if (poly.empty() || poly.back() != v) poly.push_back(v);
        };
        auto arc = [&](double a0, double a1, double r0, double r1, double stepDeg) {
            const int m = std::max(1, static_cast<int>(std::ceil((a1 - a0) / (stepDeg * kDeg))));
            for (int i = 1; i < m; ++i) {
                const double t = static_cast<double>(i) / m;
                push(add(C + (r0 + (r1 - r0) * t) * dirOf(a0 + (a1 - a0) * t)));
            }
        };
        auto gapTo = [&](std::size_t i) {
            const std::size_t j = (i + 1) % k;
            double th = inc[j].ang - inc[i].ang;
            if (k == 1) th = 2.0 * kPi;
            else if (th <= 0.0) th += 2.0 * kPi;
            return th;
        };
        if (rho > 0.0) {
            // A disc: every thread runs into its edge.
            std::vector<double> ah(k);
            for (std::size_t i = 0; i < k; ++i) {
                ah[i] = std::asin(std::min(1.0, inc[i].h / rho));
                cR[static_cast<std::size_t>(inc[i].s)][static_cast<std::size_t>(inc[i].end)] = add(C + rho * dirOf(inc[i].ang - ah[i]));
                cL[static_cast<std::size_t>(inc[i].s)][static_cast<std::size_t>(inc[i].end)] = add(C + rho * dirOf(inc[i].ang + ah[i]));
            }
            for (std::size_t i = 0; i < k; ++i) {
                const std::size_t j = (i + 1) % k;
                push(cR[static_cast<std::size_t>(inc[i].s)][static_cast<std::size_t>(inc[i].end)]);
                push(cL[static_cast<std::size_t>(inc[i].s)][static_cast<std::size_t>(inc[i].end)]);
                const double a0 = inc[i].ang + ah[i], a1 = inc[i].ang + gapTo(i) - ah[j];
                if (a1 - a0 < 0.2 * kDeg && out.error.empty()) out.error = "threads too close together on a disc";
                arc(a0, a1, rho, rho, 10.0);
            }
        } else {
            // Neighbouring threads share the corner between them where their
            // edges meet; where the gap between them opens wider than a straight
            // line (a thread ending, the outside of a bend), it is rounded off.
            std::vector<int> L(k, -1), Rr(k, -1);
            std::vector<std::vector<int>> between(k);
            for (std::size_t i = 0; i < k; ++i) {
                const std::size_t j = (i + 1) % k;
                const double th = gapTo(i);
                if (k >= 2 && th <= 190.0 * kDeg) {
                    const int m = add(miter(C, inc[i].d, inc[i].h, inc[j].d, inc[j].h));
                    L[i] = m;
                    Rr[j] = m;
                } else {
                    L[i] = add(C + inc[i].h * leftOf(inc[i].d));
                    Rr[j] = add(C + inc[j].h * rightOf(inc[j].d));
                    std::swap(poly, between[i]);
                    arc(inc[i].ang + 0.5 * kPi, inc[i].ang + th - 0.5 * kPi, inc[i].h, inc[j].h, 15.0);
                    std::swap(poly, between[i]);
                }
            }
            for (std::size_t i = 0; i < k; ++i) {
                cR[static_cast<std::size_t>(inc[i].s)][static_cast<std::size_t>(inc[i].end)] = Rr[i];
                cL[static_cast<std::size_t>(inc[i].s)][static_cast<std::size_t>(inc[i].end)] = L[i];
                push(Rr[i]);
                push(L[i]);
                for (int v : between[i]) push(v);
            }
        }
        while (poly.size() > 1 && poly.front() == poly.back()) poly.pop_back();
        if (k == 1) {
            // An end, or a pad: the cap round the far side, a fan from one corner.
            for (std::size_t i = 1; i + 1 < poly.size(); ++i) tri(poly[0], poly[i], poly[i + 1]);
        } else if (rho > 0.0) {
            const int c = add(C);
            for (std::size_t i = 0; i < poly.size(); ++i) tri(c, poly[i], poly[(i + 1) % poly.size()]);
        } else {
            // The knot's own outline need not be round its middle: a thread
            // drooping back leaves the next one's corners behind the knot. So
            // ears are cut off it, the roundest first.
            std::vector<int> left = poly;
            auto P = [&](int v) { return out.pts[static_cast<std::size_t>(v)]; };
            while (left.size() > 3) {
                const std::size_t m = left.size();
                double best = -1.0;
                std::size_t at = m;
                for (std::size_t i = 0; i < m; ++i) {
                    const DV2 a = P(left[(i + m - 1) % m]), b = P(left[i]), c = P(left[(i + 1) % m]);
                    const double ar = cross(b - a, c - a);
                    if (ar <= 1e-9) continue;
                    bool empty = true;
                    for (std::size_t j = 0; j < m && empty; ++j) {
                        if (j == i || j == (i + 1) % m || j == (i + m - 1) % m) continue;
                        const DV2 q = P(left[j]);
                        empty = !(cross(b - a, q - a) > 0.0 && cross(c - b, q - b) > 0.0 && cross(a - c, q - c) > 0.0);
                    }
                    if (!empty) continue;
                    const double l = glm::length(b - a) + glm::length(c - b) + glm::length(a - c);
                    if (ar / (l * l) > best) {
                        best = ar / (l * l);
                        at = i;
                    }
                }
                if (at == m) {
                    if (out.error.empty()) {
                        char b2[120];
                        std::snprintf(b2, sizeof b2, "threads crossing at the knot at %.1f, %.1f", C.x, C.y);
                        out.error = b2;
                    }
                    break;
                }
                tri(left[(at + m - 1) % m], left[at], left[(at + 1) % m]);
                left.erase(left.begin() + static_cast<std::ptrdiff_t>(at));
            }
            if (left.size() == 3) tri(left[0], left[1], left[2]);
        }
    }

    // The threads between the knots: ribbons along their middle lines.
    for (std::size_t i = 0; i < S; ++i) {
        const Strand& s = g.strands[i];
        const std::size_t n = s.pts.size();
        const double h = 0.5 * s.w;
        const DV2 d0 = glm::normalize(s.pts[1] - s.pts[0]), d1 = glm::normalize(s.pts[n - 2] - s.pts[n - 1]);
        auto setback = [&](int a, int b, DV2 p0, DV2 d) {
            return std::max(glm::dot(out.pts[static_cast<std::size_t>(a)] - p0, d),
                            glm::dot(out.pts[static_cast<std::size_t>(b)] - p0, d));
        };
        const double sbA = setback(cR[i][0], cL[i][0], s.pts[0], d0);
        const double sbB = setback(cR[i][1], cL[i][1], s.pts[n - 1], d1);
        std::vector<double> cum(n, 0.0);
        for (std::size_t q = 1; q < n; ++q) cum[q] = cum[q - 1] + glm::length(s.pts[q] - s.pts[q - 1]);
        std::vector<int> right{cR[i][0]}, left{cL[i][0]};
        for (std::size_t q = 1; q + 1 < n; ++q) {
            if (cum[q] <= sbA + 0.3 * h || cum[n - 1] - cum[q] <= sbB + 0.3 * h) continue;
            const DV2 n1 = rightOf(glm::normalize(s.pts[q] - s.pts[q - 1]));
            const DV2 n2 = rightOf(glm::normalize(s.pts[q + 1] - s.pts[q]));
            const DV2 m = (n1 + n2) / (1.0 + glm::dot(n1, n2));
            right.push_back(add(s.pts[q] + h * m));
            left.push_back(add(s.pts[q] - h * m));
        }
        right.push_back(cL[i][1]);
        left.push_back(cR[i][1]);
        for (std::size_t q = 0; q + 1 < right.size(); ++q) {
            const int a = right[q], b = right[q + 1], c = left[q + 1], d = left[q];
            auto area = [&](int u, int v, int w) {
                return cross(out.pts[static_cast<std::size_t>(v)] - out.pts[static_cast<std::size_t>(u)],
                             out.pts[static_cast<std::size_t>(w)] - out.pts[static_cast<std::size_t>(u)]);
            };
            if (std::min(area(a, b, c), area(a, c, d)) >= std::min(area(a, b, d), area(b, c, d))) {
                tri(a, b, c);
                tri(a, c, d);
            } else {
                tri(a, b, d);
                tri(b, c, d);
            }
        }
    }
}

// How wide a hole is: the widest round that fits in it, found on a grid over
// it and refined round the best twice. The loop starts at `u0`.
double holeWidth(const std::vector<DV2>& pts, const std::unordered_map<int, int>& next, int u0, DV2& where) {
    std::vector<DV2> loop;
    for (int u = u0;;) {
        loop.push_back(pts[static_cast<std::size_t>(u)]);
        u = next.at(u);
        if (u == u0 || loop.size() > 100000) break;
    }
    DV2 lo(1e30), hi(-1e30);
    for (const DV2& q : loop) {
        lo = glm::min(lo, q);
        hi = glm::max(hi, q);
    }
    auto clearance = [&](DV2 q) {   // to the outline, negative outside the hole
        double d = 1e30;
        bool in = false;
        for (std::size_t i = 0; i < loop.size(); ++i) {
            const DV2 A = loop[i], B = loop[(i + 1) % loop.size()];
            const double t = std::clamp(glm::dot(q - A, B - A) / std::max(1e-18, glm::dot(B - A, B - A)), 0.0, 1.0);
            d = std::min(d, glm::length(q - (A + t * (B - A))));
            if ((A.y > q.y) != (B.y > q.y) && q.x < A.x + (B.x - A.x) * (q.y - A.y) / (B.y - A.y)) in = !in;
        }
        return in ? d : -d;
    };
    DV2 best = 0.5 * (lo + hi), span = hi - lo;
    double r = clearance(best);
    for (int round = 0; round < 3; ++round) {
        const DV2 c = best;
        for (int i = 0; i <= 12; ++i)
            for (int j = 0; j <= 12; ++j) {
                const DV2 q = c + span * DV2(i / 12.0 - 0.5, j / 12.0 - 0.5);
                const double d = clearance(q);
                if (d > r) {
                    r = d;
                    best = q;
                }
            }
        span *= 0.25;
    }
    where = best;
    return 2.0 * r;
}

std::uint32_t toByte(float v) {
    return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

} // namespace

const char* shapeName(Shape s) {
    switch (s) {
    case Shape::Round:  return "Round";
    case Shape::Frame:  return "Framed";
    case Shape::Corner: return "Corner";
    default:            return "?";
    }
}

const char* endsName(Ends e) {
    switch (e) {
    case Ends::Pads:  return "Pads";
    case Ends::Loops: return "Loops";
    case Ends::Free:  return "Free";
    default:          return "?";
    }
}

Params sane(const Params& in) {
    Params p = in;
    p.shape = static_cast<Shape>(std::clamp(static_cast<int>(in.shape), 0, static_cast<int>(Shape::Count) - 1));
    p.ends  = static_cast<Ends>(std::clamp(static_cast<int>(in.ends), 0, static_cast<int>(Ends::Count) - 1));
    // Two lines of a 0.4 mm nozzle at the least, every thread; two layers.
    p.spiralWidth = std::clamp(in.spiralWidth, 0.8f, 4.0f);
    p.spokeWidth  = std::clamp(in.spokeWidth, 0.8f, 5.0f);
    p.frameWidth  = std::clamp(in.frameWidth, 0.8f, 8.0f);
    p.thickness   = std::clamp(in.thickness, 0.4f, 5.0f);
    p.size        = std::clamp(in.size, 40.0f, 250.0f);
    p.irregular   = std::clamp(in.irregular, 0.0f, 1.0f);
    p.sag         = std::clamp(in.sag, 0.0f, 0.3f);
    p.torn        = std::clamp(in.torn, 0.0f, 0.5f);
    p.hubTurns    = std::clamp(in.hubTurns, 0, 6);
    p.freeZone    = std::clamp(in.freeZone, 0.0f, 40.0f);
    p.corners     = std::clamp(in.corners, 3, 8);
    p.anchor      = std::clamp(in.anchor, 2.0f * p.frameWidth + 3.0f, 40.0f);
    // As many radii as leave the hub small next to the web: its disc grows as
    // the radii crowd round it (an uneven web has narrower gaps).
    const float frameR = 0.5f * p.size - p.frameWidth;
    if (p.shape == Shape::Corner) {
        p.spokes = std::clamp(in.spokes, 2, 12);
        // The radii run together out from the corner: no further than a
        // quarter of the walls.
        while (p.spokes > 2) {
            const double gap = gapSpread(0.5 * kPi / (p.spokes + 1), p.irregular, false);
            const double h = 0.5 * std::max(p.spokeWidth, p.frameWidth);
            if (h / std::sin(0.5 * gap) < 0.25 * p.size) break;
            --p.spokes;
        }
    } else {
        p.spokes = std::clamp(in.spokes, 5, 48);
        while (p.spokes > 5) {
            const double gap = gapSpread(2.0 * kPi / p.spokes, p.irregular, false);
            if (hubDisc(0.5 * p.spokeWidth, gap) < 0.2 * frameR) break;
            --p.spokes;
        }
        p.corners = std::min(p.corners, p.spokes);
    }
    // Between two turns a hole 1.2 mm wide at least, where the droop and the
    // unevenness have taken their share: the widest gap between two radii
    // droops furthest. The droop kept to what leaves a fifth of the spacing.
    const double gap = p.shape == Shape::Corner ? gapSpread(0.5 * kPi / (p.spokes + 1), p.irregular, true)
                                                : gapSpread(2.0 * kPi / p.spokes, p.irregular, true);
    const double sagMax = (std::cos(0.5 * gap) - 2.0 * (kKnotJitter + kDroopJitter) * p.irregular - 0.2) /
                          (2.0 * std::sin(0.5 * gap));
    p.sag = std::clamp(p.sag, 0.0f, static_cast<float>(std::max(0.0, sagMax)));
    const double left = std::max(0.12, leftBetween(gap, p.sag, p.irregular));
    p.spacing = std::max(static_cast<float>((p.spiralWidth + 1.2) / left), std::min(in.spacing, 30.0f));
    return p;
}

Model build(const Params& in) {
    const Params p = sane(in);
    Model out;
    Graph g;
    if (p.shape == Shape::Corner) cornerWeb(p, g);
    else orbWeb(p, g);
    Plate plate;
    thicken(g, plate);
    if (!plate.error.empty()) {
        out.ok = false;
        out.error = plate.error;
    }

    // The top's edges: each inside one crossed once each way, the rest the
    // outline and the holes' -- where the walls stand.
    std::map<std::pair<int, int>, int> edges;
    for (const glm::ivec3& t : plate.tris)
        for (int k = 0; k < 3; ++k) ++edges[{t[k], t[(k + 1) % 3]}];
    std::vector<std::pair<int, int>> rim;
    for (const auto& [e, n] : edges) {
        if (n > 1 && out.ok) {
            out.ok = false;
            out.error = "two threads overlap";
        }
        if (!edges.count({e.second, e.first})) rim.push_back(e);
    }
    // The holes between the threads: the outlines going round clockwise.
    {
        std::unordered_map<int, int> next;
        for (const auto& [u, v] : rim) next[u] = v;
        std::unordered_map<int, bool> seen;
        out.smallestHole = 1e30;
        for (const auto& [u0, v0] : rim) {
            if (seen[u0]) continue;
            double a = 0.0;
            int u = u0;
            for (std::size_t guard = 0; guard <= rim.size(); ++guard) {
                seen[u] = true;
                const auto it = next.find(u);
                if (it == next.end()) break;
                const DV2 P = plate.pts[static_cast<std::size_t>(u)], Q = plate.pts[static_cast<std::size_t>(it->second)];
                a += 0.5 * cross(P, Q);
                u = it->second;
                if (u == u0) break;
            }
            if (a < 0.0) {
                ++out.holes;
                DV2 where;
                const double w = holeWidth(plate.pts, next, u0, where);
                if (w < out.smallestHole) {
                    out.smallestHole = w;
                    out.smallestHoleAt = glm::vec2(where);
                }
            }
        }
        if (out.holes == 0) out.smallestHole = 0.0;
    }

    // Centred, lying on the bed.
    DV2 lo(1e30), hi(-1e30);
    for (const glm::ivec3& t : plate.tris)
        for (int k = 0; k < 3; ++k) {
            lo = glm::min(lo, plate.pts[static_cast<std::size_t>(t[k])]);
            hi = glm::max(hi, plate.pts[static_cast<std::size_t>(t[k])]);
        }
    const DV2 mid = 0.5 * (lo + hi);
    const double T = p.thickness;
    Builder b;
    std::vector<int> top(plate.pts.size(), -1), bot(plate.pts.size(), -1);
    auto up = [&](int i) {
        if (top[static_cast<std::size_t>(i)] < 0) {
            const DV2 q = plate.pts[static_cast<std::size_t>(i)] - mid;
            top[static_cast<std::size_t>(i)] = b.vid({q.x, q.y, T});
            bot[static_cast<std::size_t>(i)] = b.vid({q.x, q.y, 0.0});
        }
    };
    for (const glm::ivec3& t : plate.tris) {
        for (int k = 0; k < 3; ++k) up(t[k]);
        b.face({top[static_cast<std::size_t>(t[0])], top[static_cast<std::size_t>(t[1])], top[static_cast<std::size_t>(t[2])]});
        b.face({bot[static_cast<std::size_t>(t[0])], bot[static_cast<std::size_t>(t[2])], bot[static_cast<std::size_t>(t[1])]});
        out.area += 0.5 * cross(plate.pts[static_cast<std::size_t>(t[1])] - plate.pts[static_cast<std::size_t>(t[0])],
                                plate.pts[static_cast<std::size_t>(t[2])] - plate.pts[static_cast<std::size_t>(t[0])]);
    }
    for (const auto& [u, v] : rim)
        b.face({bot[static_cast<std::size_t>(u)], bot[static_cast<std::size_t>(v)], top[static_cast<std::size_t>(v)],
                top[static_cast<std::size_t>(u)]});

    out.size   = glm::vec3(static_cast<float>(hi.x - lo.x), static_cast<float>(hi.y - lo.y), static_cast<float>(T));
    out.volume = solidbuild::volume(b);
    out.grams  = out.volume / 1000.0 * kPla;
    out.metres = out.volume / (kPi * kFilR * kFilR) / 1000.0;
    out.spokes = g.spokes;
    out.turns  = g.turns;
    out.threads = static_cast<int>(g.strands.size());
    {
        std::vector<char> used(g.knots.size(), 0);
        for (const Strand& st : g.strands) used[static_cast<std::size_t>(st.a)] = used[static_cast<std::size_t>(st.b)] = 1;
        out.knots = static_cast<int>(std::count(used.begin(), used.end(), 1));
    }
    out.hubRadius = g.hub;
    out.body   = solidbuild::toEditMesh(b);
    for (const Strand& s : g.strands) {
        Thread t;
        for (const DV2& q : s.pts) t.pts.emplace_back(q - mid);
        t.width = static_cast<float>(s.w);
        out.threadsDrawn.push_back(std::move(t));
    }
    out.smallestHoleAt -= glm::vec2(mid);
    for (Disc d : g.discs) {
        d.centre -= glm::vec2(mid);
        out.discs.push_back(d);
    }

    const float thin = std::min({p.spiralWidth, p.spokeWidth, p.frameWidth});
    char hint[320];
    std::snprintf(hint, sizeof hint,
                  "Prints flat as it lies, without support: one plate %.2g mm thick (%d layers of 0.2 mm). The "
                  "thinnest thread is %.2g mm, %d lines of a 0.4 mm nozzle -- slice with walls only and a slow "
                  "first layer.",
                  p.thickness, std::max(1, static_cast<int>(std::lround(p.thickness / 0.2f))), thin,
                  std::max(1, static_cast<int>(std::floor(thin / 0.42f))));
    out.printHint = hint;
    return out;
}

AssetId ensureMaterial(std::vector<MaterialDef>& mats, const Params& p) {
    char name[64];
    std::snprintf(name, sizeof name, "Spider web#%02X%02X%02X", toByte(p.color.r), toByte(p.color.g), toByte(p.color.b));
    for (MaterialDef& m : mats) {
        if (m.name != name) continue;
        m.albedo = p.color;
        if (!m.assetId.valid()) m.assetId = AssetId::generate();
        return m.assetId;
    }
    MaterialDef md;
    md.assetId      = AssetId::generate();
    md.name         = name;
    md.albedo       = p.color;
    md.reflectivity = 0.0f;
    md.roughness    = 0.55f;   // printed plastic: a dull sheen
    mats.push_back(md);
    return md.assetId;
}

std::string autoName(const Params& in) {
    const Params p = sane(in);
    char b[96];
    if (p.shape == Shape::Corner) std::snprintf(b, sizeof b, "Corner spider web %.4g mm", p.size);
    else std::snprintf(b, sizeof b, "Spider web %s %.4g mm", p.shape == Shape::Round ? "round" : "framed", p.size);
    return b;
}

std::vector<Entity> entities(const Params& in, const AssetId& material, int& counter, const glm::vec3& at) {
    const Params p = sane(in);
    Model mdl = build(p);
    if (mdl.body.faces.empty()) return {};
    std::vector<Entity> out;
    Entity root;
    root.type = EntityType::Empty;
    root.name = autoName(p);
    root.id = counter++;
    root.parent = -1;
    // The root in the middle of the web, so its pick box is the web's: `at` is
    // where its bottom lies.
    glm::vec3 lo(1e9f), hi(-1e9f);
    for (const glm::vec3& v : mdl.body.verts) {
        lo = glm::min(lo, v);
        hi = glm::max(hi, v);
    }
    const glm::vec3 mid = 0.5f * (lo + hi);
    root.half = glm::max(0.5f * (hi - lo), glm::vec3(1e-4f));
    root.localCenter = root.center = at + glm::vec3(0.0f, root.half.y, 0.0f);
    {
        auto wc = std::make_unique<SpiderWebComponent>();
        wc->params = p;
        root.components.items.push_back(std::move(wc));
    }
    out.push_back(std::move(root));
    const int rootId = out.front().id;
    const glm::vec3 rootC = out.front().center;

    EditMesh m = std::move(mdl.body);
    for (glm::vec3& v : m.verts) v -= mid;
    const glm::vec3 shift = editmesh::recenter(m);
    glm::vec3 mn, mx;
    m.bounds(mn, mx);
    Entity e;
    e.type = EntityType::Box;
    e.name = "Web";
    e.id = counter++;
    e.parent = rootId;
    e.half = glm::max(0.5f * (mx - mn), glm::vec3(1e-4f));
    e.localCenter = shift;
    e.center = rootC + shift;
    auto mc = std::make_unique<MeshComponent>();
    mc->mesh = std::move(m);
    mc->touch();
    e.components.items.push_back(std::move(mc));
    auto mt = std::make_unique<MaterialComponent>();
    mt->material = material;
    e.components.items.push_back(std::move(mt));
    auto ph = std::make_unique<PhysicsComponent>();
    ph->dynamic = false;
    e.components.items.push_back(std::move(ph));
    out.push_back(std::move(e));
    return out;
}

// --- Persistence -------------------------------------------------------------------

void saveParams(nlohmann::json& j, const Params& p) {
    j["shape"]       = static_cast<int>(p.shape);
    j["size"]        = p.size;
    j["spokes"]      = p.spokes;
    j["spacing"]     = p.spacing;
    j["hubTurns"]    = p.hubTurns;
    j["freeZone"]    = p.freeZone;
    j["sag"]         = p.sag;
    j["irregular"]   = p.irregular;
    j["seed"]        = p.seed;
    j["torn"]        = p.torn;
    j["spiralWidth"] = p.spiralWidth;
    j["spokeWidth"]  = p.spokeWidth;
    j["frameWidth"]  = p.frameWidth;
    j["thickness"]   = p.thickness;
    j["eyelet"]      = p.eyelet;
    j["corners"]     = p.corners;
    j["anchor"]      = p.anchor;
    j["ends"]        = static_cast<int>(p.ends);
    j["color"]       = nlohmann::json::array({p.color.r, p.color.g, p.color.b});
}

void loadParams(const nlohmann::json& j, Params& p) {
    const Params d;
    p.shape       = static_cast<Shape>(j.value("shape", static_cast<int>(d.shape)));
    p.size        = j.value("size", d.size);
    p.spokes      = j.value("spokes", d.spokes);
    p.spacing     = j.value("spacing", d.spacing);
    p.hubTurns    = j.value("hubTurns", d.hubTurns);
    p.freeZone    = j.value("freeZone", d.freeZone);
    p.sag         = j.value("sag", d.sag);
    p.irregular   = j.value("irregular", d.irregular);
    p.seed        = j.value("seed", d.seed);
    p.torn        = j.value("torn", d.torn);
    p.spiralWidth = j.value("spiralWidth", d.spiralWidth);
    p.spokeWidth  = j.value("spokeWidth", d.spokeWidth);
    p.frameWidth  = j.value("frameWidth", d.frameWidth);
    p.thickness   = j.value("thickness", d.thickness);
    p.eyelet      = j.value("eyelet", d.eyelet);
    p.corners     = j.value("corners", d.corners);
    p.anchor      = j.value("anchor", d.anchor);
    p.ends        = static_cast<Ends>(j.value("ends", static_cast<int>(d.ends)));
    p.color       = d.color;
    if (const auto c = j.find("color"); c != j.end() && c->is_array() && c->size() == 3)
        p.color = {(*c)[0].get<float>(), (*c)[1].get<float>(), (*c)[2].get<float>()};
    p = sane(p);
}

namespace {
// Scene files name components by type id; this lets a saved web come back as
// one, parameters and all.
struct RegisterSpiderWeb {
    RegisterSpiderWeb() {
        components::registerType({"spiderWeb", "Spider web",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<SpiderWebComponent>()); },
            /*addable=*/false});
    }
} g_registerSpiderWeb;
} // namespace

} // namespace spiderweb
