#include "TreeGen.hpp"

#include <algorithm>
#include <cmath>
#include <type_traits>

#include <nlohmann/json.hpp>

namespace treegen {

namespace {

constexpr float kPi  = 3.14159265358979f;
constexpr float kTau = 2.0f * kPi;
constexpr float kDeg = kPi / 180.0f;
const glm::vec3 kUp{0.0f, 1.0f, 0.0f};

// splitmix64. Every stem draws from its OWN stream, seeded from the tree seed
// and where the stem sits in the hierarchy -- so editing the leaves, or the
// twigs, never reshuffles the limbs: a tree keeps its character while it is
// being tuned, which one shared generator would not allow.
struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed) {}
    std::uint64_t next() {
        std::uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    float uni() { return static_cast<float>(next() >> 40) * (1.0f / 16777216.0f); } // [0,1)
    float sym() { return uni() * 2.0f - 1.0f; }                                     // [-1,1)
};

std::uint64_t hashMix(std::uint64_t a, std::uint64_t b) {
    Rng r(a * 0xD1B54A32D192ED03ull + b);
    r.next();
    return r.next();
}

glm::vec3 rotateAbout(const glm::vec3& v, const glm::vec3& axis, float ang) {
    const float c = std::cos(ang), s = std::sin(ang);
    return v * c + glm::cross(axis, v) * s + axis * glm::dot(axis, v) * (1.0f - c);
}

glm::vec3 anyPerp(const glm::vec3& d) {
    const glm::vec3 a = std::abs(d.y) < 0.9f ? kUp : glm::vec3(1.0f, 0.0f, 0.0f);
    return glm::normalize(glm::cross(d, a));
}

// `v` made perpendicular to the unit vector `d` -- or any perpendicular, if it
// lies along it.
glm::vec3 perpTo(const glm::vec3& v, const glm::vec3& d) {
    const glm::vec3 p = v - d * glm::dot(v, d);
    const float l = glm::length(p);
    return l > 1e-5f ? p / l : anyPerp(d);
}

float smooth01(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

std::size_t sz(int i) { return static_cast<std::size_t>(i); }

// A child (a branch, or the other half of a fork) leaving its stem at arc s.
struct Attach { float s; int stem; };

struct Stem {
    int   level   = 0;
    int   parent  = -1;       // the stem it grows from (-1: the trunk foot)
    float parentS = 0.0f;     // ...at this arc length along it
    bool  fork    = false;    // the other half of a fork: same level as its parent
    float fullLen = 1.0f;     // the whole stem, fork pieces included
    float t0      = 0.0f;     // where along that whole stem this piece begins
    std::uint64_t seed = 0;
    std::vector<glm::vec3> pts;    // axis
    std::vector<glm::vec3> side;   // parallel-transported cross axis, per point
    std::vector<float>     arc;    // arc length, per point
    std::vector<float>     r;      // radius, per point
    std::vector<Attach>    attach; // sorted by s once grown

    float length() const { return arc.empty() ? 0.0f : arc.back(); }
    // Fraction along the WHOLE stem at arc s of this piece.
    float tAt(float s) const { return t0 + s / std::max(fullLen, 1e-4f); }
};

struct Sample { glm::vec3 p, t, side; float r; };

class Grower {
public:
    explicit Grower(const Params& p) : P(p) {}

    void grow();
    void solveRadii(Mesh& m);
    void skin(Mesh& m) const;
    void leaves(Mesh& m) const;
    int  stemCount() const { return static_cast<int>(stems.size()); }

private:
    int  growStem(int level, int parent, float parentS, bool isFork, glm::vec3 base,
                  glm::vec3 dir, float len, float fullLen, float t0, std::uint64_t seed,
                  glm::vec3 sideRef, int forksDone);
    void placeChildren(int idx);
    Sample sampleAt(const Stem& s, float arcS) const;
    double pipeArea(double e, std::vector<double>& base) const;

    const Params& P;
    std::vector<Stem> stems;
    float H = 10.0f;                // this seed's height
    float refLen[4] = {1, 1, 1, 1}; // a full-length stem of each level
};

// --- 1. The skeleton ------------------------------------------------------------

void Grower::grow() {
    Rng rng(hashMix(P.seed, 0x7A11));
    H = std::max(0.5f, P.height * (1.0f + P.heightVar * rng.sym()));
    refLen[0] = H;
    for (int l = 1; l < 4; ++l)
        refLen[l] = std::max(0.05f, refLen[l - 1] * std::max(P.level[l].length, 0.01f));
    // The lean: a tilt about a random horizontal axis.
    const float az = rng.uni() * kTau;
    const glm::vec3 axis(std::cos(az), 0.0f, std::sin(az));
    const glm::vec3 dir = rotateAbout(kUp, axis, P.lean * kDeg * (0.5f + 0.5f * rng.uni()));
    stems.reserve(4096);
    growStem(0, -1, 0.0f, false, glm::vec3(0.0f), dir, H, H, 0.0f, hashMix(P.seed, 1),
             glm::vec3(1.0f, 0.0f, 0.0f), 0);
    for (Stem& s : stems)
        std::sort(s.attach.begin(), s.attach.end(),
                  [](const Attach& a, const Attach& b) { return a.s < b.s; });

    // The height asked for is the TREE's, not the trunk's: upswept limbs rise
    // past the leader, and a tree asked for 16 m came out 20. So the grown
    // skeleton is scaled to it -- positions and lengths only; the radii are
    // solved afterwards and keep the trunk and the twigs at what was asked.
    float top = 0.0f;
    for (const Stem& s : stems)
        for (const glm::vec3& q : s.pts) top = std::max(top, q.y);
    const float k = top > 0.1f ? std::clamp(H / top, 0.5f, 2.0f) : 1.0f;
    if (std::abs(k - 1.0f) > 1e-4f) {
        for (Stem& s : stems) {
            for (glm::vec3& q : s.pts) q *= k;
            for (float& a : s.arc) a *= k;
            for (Attach& a : s.attach) a.s *= k;
            s.parentS *= k;
            s.fullLen *= k;
        }
        for (float& l : refLen) l *= k;
    }
}

int Grower::growStem(int level, int parent, float parentS, bool isFork, glm::vec3 base,
                     glm::vec3 dir, float len, float fullLen, float t0, std::uint64_t seed,
                     glm::vec3 sideRef, int forksDone) {
    const Level& L = P.level[level];
    Rng rng(seed);
    const int idx = static_cast<int>(stems.size());
    stems.emplace_back();
    {
        Stem& s = stems[sz(idx)];
        s.level = level; s.parent = parent; s.parentS = parentS; s.fork = isFork;
        s.fullLen = fullLen; s.t0 = t0; s.seed = seed;
    }

    // Resolution: a whole stem gets its level's segments, a fork piece its
    // share, and a short stem of the level a few less (less to bend over).
    const float share = len / std::max(fullLen, 1e-4f);
    const float sizeK = std::clamp(std::sqrt(fullLen / refLen[level]), 0.4f, 1.4f);
    const int n = std::clamp(static_cast<int>(std::ceil(static_cast<float>(L.segments) * P.detail *
                                                        share * sizeK)), 2, 96);
    const float seg  = len / static_cast<float>(n);
    const float step = seg / std::max(fullLen, 1e-4f);   // of the whole stem, per step

    // The wiggle: two smooth waves about each cross axis. Per-step random turns
    // (the classic generator) integrate to a random walk and read as a zig-zag;
    // waves read as wood. Per unit of the whole stem, so the resolution does
    // not change the shape.
    const float f1 = 0.6f + 1.1f * rng.uni(), f2 = 1.7f + 1.9f * rng.uni();
    const float ph[4] = {rng.uni() * kTau, rng.uni() * kTau, rng.uni() * kTau, rng.uni() * kTau};
    auto wave = [&](float t, float a, float b) {
        return std::sin(kTau * f1 * t + a) + 0.45f * std::sin(kTau * f2 * t + b);
    };
    const float wig  = L.gnarl * 2.4f * step;
    const float kink = L.gnarl * 0.05f;
    const float bend = L.curve * kDeg * step;

    // Forks: evenly between these heights of the whole stem. A piece that
    // forked off keeps the schedule, so two forks make four pieces.
    const float fa = level == 0 ? P.forkStart : 0.25f;
    const float fb = level == 0 ? P.forkEnd   : 0.75f;
    Rng frng(hashMix(seed, 0xF0));
    struct Pending { float s; glm::vec3 p, dir, side; float t; int done; std::uint64_t seed; };
    std::vector<Pending> pending;
    auto forkT = [&](int k) {
        const float u = (static_cast<float>(k) + 0.5f) / static_cast<float>(std::max(L.forks, 1));
        return fa + (fb - fa) * u;
    };

    glm::vec3 p = base;
    glm::vec3 d = glm::normalize(dir);
    glm::vec3 sd = perpTo(sideRef, d);
    std::vector<glm::vec3> pts{p}, sides{sd};
    std::vector<float> arc{0.0f};
    float s = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float tw = t0 + (s + 0.5f * seg) / fullLen;   // along the whole stem
        // Fork at the point we stand on, before moving on.
        if (L.forks > 0 && forksDone < L.forks && i > 0 && tw >= forkT(forksDone)) {
            const float spin = frng.uni() * kTau + 2.39996f * static_cast<float>(forksDone);
            const glm::vec3 ax = rotateAbout(sd, d, spin);
            const float half = 0.5f * L.forkAngle * kDeg * (0.8f + 0.4f * frng.uni());
            const glm::vec3 other = glm::normalize(rotateAbout(d, ax, -half));
            d = glm::normalize(rotateAbout(d, ax, half));
            ++forksDone;
            pending.push_back({s, p, other, sd, t0 + s / fullLen, forksDone,
                               hashMix(seed, 0x100 + static_cast<std::uint64_t>(forksDone))});
        }
        // Wiggle, kinks, the bend towards the sky, gravity.
        const glm::vec3 b2 = glm::cross(d, sd);
        d = rotateAbout(d, sd, wig * wave(tw, ph[0], ph[1]) + kink * rng.sym());
        d = rotateAbout(d, b2, wig * wave(tw, ph[2], ph[3]) + kink * rng.sym());
        const glm::vec3 h = glm::cross(d, kUp);
        if (glm::length(h) > 1e-4f) d = rotateAbout(d, glm::normalize(h), bend);
        d = glm::normalize(d - kUp * (L.gravity * (0.4f + 1.2f * tw) * step));
        sd = perpTo(sd, d);
        const glm::vec3 next = p + d * seg;
        // Nothing grows into the ground: a hanging strand ends above it.
        if (level > 0 && next.y < 0.3f) break;
        p = next;
        s += seg;
        pts.push_back(p);
        sides.push_back(sd);
        arc.push_back(s);
    }
    {
        Stem& st = stems[sz(idx)];
        st.pts  = std::move(pts);
        st.side = std::move(sides);
        st.arc  = std::move(arc);
    }
    const float grown = stems[sz(idx)].length();

    // The other halves of the forks, then the children of the next level.
    for (const Pending& f : pending) {
        if (f.s >= grown) continue;
        const int c = growStem(level, idx, f.s, true, f.p, f.dir, len - f.s, fullLen, f.t,
                               f.seed, f.side, f.done);
        stems[sz(idx)].attach.push_back({f.s, c});
    }
    if (grown > 1e-3f && level < std::clamp(P.levels, 0, 3)) placeChildren(idx);
    return idx;
}

Sample Grower::sampleAt(const Stem& s, float arcS) const {
    const int n = static_cast<int>(s.pts.size());
    if (n < 2)
        return {s.pts.empty() ? glm::vec3(0.0f) : s.pts[0], kUp, glm::vec3(1.0f, 0.0f, 0.0f),
                s.r.empty() ? 0.0f : s.r[0]};
    arcS = std::clamp(arcS, 0.0f, s.arc.back());
    int j = static_cast<int>(std::upper_bound(s.arc.begin(), s.arc.end(), arcS) - s.arc.begin()) - 1;
    j = std::clamp(j, 0, n - 2);
    const std::size_t a = sz(j), b = a + 1;
    const float len = s.arc[b] - s.arc[a];
    const float f = len > 1e-6f ? (arcS - s.arc[a]) / len : 0.0f;
    auto tangent = [&](int k) {
        const glm::vec3 d = s.pts[sz(std::min(k + 1, n - 1))] - s.pts[sz(std::max(k - 1, 0))];
        const float l = glm::length(d);
        return l > 1e-6f ? d / l : kUp;
    };
    Sample o;
    o.p = glm::mix(s.pts[a], s.pts[b], f);
    const glm::vec3 t = glm::mix(tangent(j), tangent(j + 1), f);
    o.t = glm::length(t) > 1e-6f ? glm::normalize(t) : kUp;
    o.side = perpTo(glm::mix(s.side[a], s.side[b], f), o.t);
    o.r = s.r.empty() ? 0.0f : s.r[a] + (s.r[b] - s.r[a]) * f;
    return o;
}
void Grower::placeChildren(int idx) {
    const int cl = stems[sz(idx)].level + 1;
    const Level& C = P.level[cl];
    if (C.count <= 0) return;
    Rng rng(hashMix(stems[sz(idx)].seed, 0xC41D));
    const float pieceLen = stems[sz(idx)].length();
    const float fullLen  = stems[sz(idx)].fullLen;
    const float ta = stems[sz(idx)].t0;
    const float tb = stems[sz(idx)].tAt(pieceLen);
    const float lo = std::max(C.start, ta), hi = std::min(C.end, tb);
    if (hi <= lo) return;
    // How many: the level's count on a whole parent, fewer on a short one (a
    // young branch has had fewer years to put out side shoots).
    const float lengthK = cl == 1 ? 1.0f : std::clamp(fullLen / refLen[cl - 1], 0.12f, 1.5f);
    const float wantF = static_cast<float>(C.count) * lengthK * (hi - lo) /
                        std::max(C.end - C.start, 1e-3f);
    int want = static_cast<int>(wantF);
    if (rng.uni() < wantF - static_cast<float>(want)) ++want;
    if (want <= 0) return;
    const int whorl = std::max(1, C.whorl);
    const int nodes = std::max(1, (want + whorl - 1) / whorl);
    const float crownY0 = P.level[1].start * H;
    float phi = rng.uni() * kTau;
    for (int k = 0; k < nodes; ++k) {
        const float u = (static_cast<float>(k) + 0.5f + 0.35f * rng.sym()) / static_cast<float>(nodes);
        const float t = lo + (hi - lo) * std::clamp(u, 0.0f, 1.0f);
        const float arcS = (t - ta) * fullLen;
        const Sample sm = sampleAt(stems[sz(idx)], arcS);
        phi += C.rotate * kDeg + 0.12f * rng.sym();
        // The azimuth is measured from the HORIZONTAL cross axis where there is
        // one, so opposite pairs and flat sprays (spruce twigs) lie level.
        const glm::vec3 hz = glm::cross(sm.t, kUp);
        const glm::vec3 ref = glm::length(hz) > 0.15f ? glm::normalize(hz) : sm.side;
        const glm::vec3 ref2 = glm::cross(sm.t, ref);
        const float rel = std::clamp((t - C.start) / std::max(C.end - C.start, 1e-3f), 0.0f, 1.0f);
        for (int m = 0; m < whorl && want > 0; ++m, --want) {
            const float az = phi + kTau * static_cast<float>(m) / static_cast<float>(whorl) +
                             0.08f * rng.sym();
            const float ang = (C.angle + (C.angleTip - C.angle) * rel + C.angleVar * rng.sym()) * kDeg;
            const glm::vec3 radial = ref * std::cos(az) + ref2 * std::sin(az);
            const glm::vec3 dir = glm::normalize(sm.t * std::cos(ang) + radial * std::sin(ang));
            float len;
            if (cl == 1) {
                const float h = std::clamp((sm.p.y - crownY0) / std::max(H - crownY0, 0.1f), 0.0f, 1.0f);
                len = H * C.length * shapeRatio(P.shape, h);
            } else {
                len = fullLen * C.length * (1.0f - 0.6f * t);
            }
            len *= 1.0f + C.lengthVar * rng.sym();
            if (len < 0.03f) continue;
            const std::uint64_t cs = hashMix(stems[sz(idx)].seed,
                                             0x1000 + static_cast<std::uint64_t>(k * 16 + m));
            const int c = growStem(cl, idx, arcS, false, sm.p, dir, len, len, 0.0f, cs, sm.t, 0);
            stems[sz(idx)].attach.push_back({arcS, c});
        }
    }
}

// --- 2. The thickness: the pipe model ----------------------------------------------

// Cross-section "area" (r^e) at the foot of every stem for exponent e, children
// before parents (they always have larger indices). Returns the trunk foot's.
double Grower::pipeArea(double e, std::vector<double>& base) const {
    const double tip = std::pow(static_cast<double>(P.twigRadius), e);
    base.assign(stems.size(), 0.0);
    for (int i = static_cast<int>(stems.size()) - 1; i >= 0; --i) {
        double a = tip;
        for (const Attach& at : stems[sz(i)].attach) a += base[sz(at.stem)];
        base[sz(i)] = a;
    }
    return base.empty() ? 0.0 : base[0];
}

void Grower::solveRadii(Mesh& m) {
    if (stems.empty()) return;
    // The exponent that gives the trunk the radius asked for, with every tip at
    // the twig radius asked for. The foot's radius falls as e grows, so bisect.
    // But only between da Vinci's 2 and Murray's 3.2: outside that a limb is
    // no longer the thickness of wood that carries what it carries (below 2,
    // which a tree with too few twigs to fill its trunk would ask for, the
    // limbs come out as sticks on a post). There the tree is scaled instead,
    // and the twigs give way -- they are the part nobody sees.
    std::vector<double> base;
    const double want = std::max(1e-3, static_cast<double>(P.radius));
    auto rootR = [&](double e) { return std::pow(pipeArea(e, base), 1.0 / e); };
    double lo = 2.0, hi = 3.2, e = 2.5, scale = 1.0;
    if (rootR(lo) < want)      { e = lo; scale = want / rootR(lo); }
    else if (rootR(hi) > want) { e = hi; scale = want / rootR(hi); }
    else {
        for (int it = 0; it < 40; ++it) {
            e = 0.5 * (lo + hi);
            if (rootR(e) > want) lo = e; else hi = e;
        }
    }
    m.exponent = static_cast<float>(e);
    pipeArea(e, base);
    const double tip = std::pow(static_cast<double>(P.twigRadius), e);
    for (Stem& s : stems) {
        const int n = static_cast<int>(s.pts.size());
        s.r.assign(sz(n), 0.0f);
        double a = tip;
        int k = static_cast<int>(s.attach.size()) - 1;
        for (int j = n - 1; j >= 0; --j) {
            const float sj = s.arc[sz(j)];
            while (k >= 0 && s.attach[sz(k)].s >= sj - 1e-5f) {
                a += base[sz(s.attach[sz(k)].stem)];
                --k;
            }
            s.r[sz(j)] = static_cast<float>(std::pow(a, 1.0 / e) * scale);
        }
        // The taper on top: wood thins towards the tip between branches too.
        const float taper = std::clamp(P.level[s.level].taper, 0.0f, 0.95f);
        for (int j = 0; j < n; ++j) {
            const float t = std::clamp(s.tAt(s.arc[sz(j)]), 0.0f, 1.0f);
            float& r = s.r[sz(j)];
            r = std::max(r * (1.0f - taper * t * std::sqrt(t)), 0.0008f);
        }
    }
    // No branch is thicker than where it leaves its parent (the taper can
    // make it so). Parents first, so a clamp carries down the hierarchy.
    for (std::size_t i = 1; i < stems.size(); ++i) {
        Stem& s = stems[i];
        if (s.parent < 0 || s.r.empty()) continue;
        const float rp = sampleAt(stems[sz(s.parent)], s.parentS).r;
        const float cap = (s.fork ? 1.0f : 0.9f) * rp;
        if (s.r[0] > cap) {
            const float k = cap / s.r[0];
            for (float& r : s.r) r = std::max(r * k, 0.0008f);
        }
    }
}

// --- 3. The skin -----------------------------------------------------------------

void Grower::skin(Mesh& m) const {
    const float edge = 0.04f / std::max(P.detail, 0.1f);   // ring edge we aim for (m)
    for (std::size_t si = 0; si < stems.size(); ++si) {
        const Stem& s = stems[si];
        if (s.pts.size() < 2 || s.r.empty()) continue;
        const Level& L = P.level[s.level];
        const float r0 = s.r[0];
        if (r0 < 0.001f) continue;
        const int maxSides = std::max(3, static_cast<int>(std::round(static_cast<float>(L.sides) * P.detail)));
        const int minSides = r0 < 0.012f ? 3 : 4;
        const int sides = std::clamp(static_cast<int>(std::round(kTau * r0 / edge)), minSides,
                                     std::max(minSides, maxSides));
        Rng rng(hashMix(s.seed, 0xBA4C));

        // Rings on every axis point, plus extra ones in the flare, which bends
        // faster than the trunk axis is sampled.
        std::vector<float> ringS(s.arc.begin(), s.arc.end());
        const bool foot = si == 0 && P.flare > 0.0f;
        if (foot) {
            const float fh = std::max(P.flareHeight, 0.05f);
            for (float f : {0.03f, 0.08f, 0.15f, 0.24f, 0.35f, 0.5f, 0.7f, 1.0f})
                if (f * fh < s.length()) ringS.push_back(f * fh);
            std::sort(ringS.begin(), ringS.end());
            ringS.erase(std::unique(ringS.begin(), ringS.end(),
                                    [](float a, float b) { return std::abs(a - b) < 0.02f; }),
                        ringS.end());
        }
        const int rows = static_cast<int>(ringS.size());
        const int cols = sides + 1;                      // a seam vertex for the UVs

        // Bark at constant texel density: whole tiles round the stem, tiles as
        // tall as the image says along it.
        const int   uRep  = std::max(1, static_cast<int>(std::round(kTau * r0 / std::max(P.barkTile, 0.05f))));
        const float tileW = kTau * r0 / static_cast<float>(uRep);
        const float tileH = std::max(tileW * std::max(P.barkAspect, 0.1f), 1e-3f);
        const float v0    = rng.uni();

        // Relief (trunk and limbs) and buttresses (the foot).
        const float relief = s.level == 0 ? P.relief : (s.level == 1 ? 0.5f * P.relief : 0.0f);
        const float rp[4] = {rng.uni() * kTau, rng.uni() * kTau, rng.uni() * kTau, rng.uni() * kTau};
        const int lobes = std::max(0, P.buttress);

        std::vector<glm::vec3> pos(sz(rows * cols)), rad(pos.size());
        std::vector<glm::vec2> uv(pos.size());
        for (int j = 0; j < rows; ++j) {
            const float sArc = ringS[sz(j)];
            const Sample sm = sampleAt(s, sArc);
            const glm::vec3 N = sm.side, B = glm::cross(sm.t, N);
            float fl = 0.0f;
            if (foot && sArc < P.flareHeight) {
                const float g = 1.0f - sArc / std::max(P.flareHeight, 0.05f);
                fl = P.flare * g * g * g;
            }
            const float rr = (j == rows - 1) ? sm.r * 0.25f : sm.r;   // the tip closes in
            for (int k = 0; k < cols; ++k) {
                const float th = kTau * static_cast<float>(k % sides) / static_cast<float>(sides);
                const glm::vec3 dir = N * std::cos(th) + B * std::sin(th);
                float r = rr;
                if (fl > 0.0f) {
                    float lobe = 1.0f;
                    if (lobes > 0) {
                        const float w = th + 0.35f * std::sin(th * 2.0f + rp[3]);
                        const float c = 0.5f + 0.5f * std::cos(static_cast<float>(lobes) * w + rp[2]);
                        lobe = c * c;
                    }
                    r *= 1.0f + fl * (0.3f + 0.7f * lobe);
                }
                if (relief > 0.0f) {
                    const float z = sArc / std::max(r0, 0.05f);
                    r *= 1.0f + relief * (0.55f * std::sin(7.0f * th + 0.35f * z + rp[0]) +
                                          0.3f * std::sin(13.0f * th - 0.6f * z + rp[1]) +
                                          0.45f * std::sin(2.0f * th + 0.15f * z + rp[2]));
                }
                const std::size_t vi = sz(j * cols + k);
                pos[vi] = sm.p + dir * r;
                if (si == 0) pos[vi].y = std::max(pos[vi].y, 0.0f);   // stands ON the ground
                rad[vi] = dir;
                uv[vi]  = glm::vec2(static_cast<float>(k) / static_cast<float>(sides) *
                                        static_cast<float>(uRep),
                                    v0 + sArc / tileH);
            }
        }
        // Normals from the surface itself: the flare and the relief tilt them.
        const std::uint32_t first = static_cast<std::uint32_t>(m.bark.size() / 8);
        for (int j = 0; j < rows; ++j)
            for (int k = 0; k < cols; ++k) {
                const int kk = k % sides;
                const glm::vec3 a = pos[sz(j * cols + (kk + 1) % sides)];
                const glm::vec3 b = pos[sz(j * cols + (kk + sides - 1) % sides)];
                const glm::vec3 c = pos[sz(std::min(j + 1, rows - 1) * cols + kk)];
                const glm::vec3 d = pos[sz(std::max(j - 1, 0) * cols + kk)];
                const glm::vec3 radial = rad[sz(j * cols + k)];
                glm::vec3 n = glm::cross(a - b, c - d);
                const float l = glm::length(n);
                n = l > 1e-12f ? n / l : radial;
                if (glm::dot(n, radial) < 0.0f) n = -n;
                const glm::vec3& q = pos[sz(j * cols + k)];
                const glm::vec2& t = uv[sz(j * cols + k)];
                m.bark.insert(m.bark.end(), {q.x, q.y, q.z, n.x, n.y, n.z, t.x, t.y});
            }
        for (int j = 0; j + 1 < rows; ++j)
            for (int k = 0; k < sides; ++k) {
                const std::uint32_t a = first + static_cast<std::uint32_t>(j * cols + k);
                const std::uint32_t b = a + 1;
                const std::uint32_t d = a + static_cast<std::uint32_t>(cols);
                const std::uint32_t c = d + 1;
                m.barkIdx.insert(m.barkIdx.end(), {a, b, d, b, c, d});
            }
    }
}
// --- 4. The leaves -------------------------------------------------------------------

void Grower::leaves(Mesh& m) const {
    const Leaves& LF = P.leaves;
    if (!LF.enabled || stems.empty()) return;
    // The finest level that actually grew carries the leaves.
    int top = 0;
    for (const Stem& s : stems) top = std::max(top, s.level);
    if (top == 0) return;
    const float density = std::max(P.leafDensity, 0.02f);
    const float growK = 1.0f / std::sqrt(std::min(density, 1.0f)); // fewer cards, larger

    struct Spot { glm::vec3 p, t, side; float r, along, az; std::uint64_t seed; };
    std::vector<Spot> spots;
    for (const Stem& s : stems) {
        const bool finest = s.level == top;
        const bool outer  = LF.onParent && s.level == top - 1 && s.level >= 1;
        if ((!finest && !outer) || s.pts.size() < 2) continue;
        Rng rng(hashMix(s.seed, 0x1EAF));
        const float pieceLen = s.length();
        const float ta = s.t0, tb = s.tAt(pieceLen);
        const float start = finest ? LF.start : std::max(LF.start, 0.55f);
        const float lo = std::max(start, ta), hi = std::min(1.0f, tb);
        if (hi <= lo) continue;
        float wantF = LF.perStem * density * (hi - lo) / std::max(1.0f - start, 0.05f);
        if (!finest) wantF *= 0.5f;
        int want = static_cast<int>(wantF);
        if (rng.uni() < wantF - static_cast<float>(want)) ++want;
        float az = rng.uni() * kTau;
        for (int k = 0; k < want; ++k) {
            const float u = (static_cast<float>(k) + 0.5f + 0.4f * rng.sym()) / static_cast<float>(want);
            const float t = lo + (hi - lo) * std::clamp(u, 0.0f, 1.0f);
            const Sample sm = sampleAt(s, (t - ta) * s.fullLen);
            az += 137.5f * kDeg + 0.3f * rng.sym();
            spots.push_back({sm.p, sm.t, sm.side, sm.r, t, az,
                             hashMix(s.seed, 0x2000 + static_cast<std::uint64_t>(k))});
        }
    }
    if (spots.empty()) return;

    // The crown: an ellipsoid round everything that bears leaves. It decides
    // the shaded core (left bare) and the normals the cards are lit with.
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (const Spot& sp : spots) { lo = glm::min(lo, sp.p); hi = glm::max(hi, sp.p); }
    const glm::vec3 cc = 0.5f * (lo + hi);
    const glm::vec3 cr = glm::max(0.5f * (hi - lo), glm::vec3(0.3f));
    auto crownN = [&](const glm::vec3& q) {
        const glm::vec3 g = (q - cc) / (cr * cr);
        const float l = glm::length(g);
        return l > 1e-6f ? g / l : kUp;
    };
    const int cols = std::max(1, LF.cols), rows = std::max(1, LF.rows);
    const int cells = cols * rows;
    const float hollow = std::clamp(LF.hollow, 0.0f, 0.95f);
    const float fold   = std::clamp(LF.fold, 0.0f, 1.5f);
    const float bend   = std::clamp(LF.bend, 0.0f, 1.0f);
    const float ins    = 0.004f;   // keep the neighbouring atlas cell out of the filter

    auto put = [&](const glm::vec3& q, const glm::vec3& n0, float u, float v) {
        glm::vec3 n = glm::mix(n0, crownN(q), bend);
        const float l = glm::length(n);
        n = l > 1e-6f ? n / l : n0;
        m.leaves.insert(m.leaves.end(), {q.x, q.y, q.z, n.x, n.y, n.z, u, v});
    };
    const int clusterN = std::clamp(LF.cluster, 1, 12);
    for (const Spot& sp : spots) {
        Rng rng(sp.seed);
        if (hollow > 0.0f) {
            const float rho = glm::length((sp.p - cc) / cr);
            if (rng.uni() >= smooth01(hollow - 0.25f, hollow + 0.1f, rho)) continue;
        }
        // A cluster: leaves in a rosette round the twig (each its own azimuth),
        // or -- for sprays along the twig -- crossed cards turned about it, the
        // bottle brush of a pine shoot.
        for (int ci = 0; ci < clusterN; ++ci) {
            const float size = std::max(0.005f, LF.size * growK * (1.0f + LF.sizeVar * rng.sym()) *
                                                    (1.0f - 0.25f * sp.along));
            const float w = size * std::max(LF.aspect, 0.05f);
            const glm::vec3 T = sp.t;
            const float az = LF.alongTwig ? sp.az
                                          : sp.az + kTau * static_cast<float>(ci) / static_cast<float>(clusterN) +
                                                0.5f * rng.sym() / static_cast<float>(clusterN);
            const glm::vec3 radial = sp.side * std::cos(az) + glm::cross(T, sp.side) * std::sin(az);
            const float a = LF.alongTwig ? (6.0f + 10.0f * rng.uni()) * kDeg
                                         : (LF.angle + 15.0f * rng.sym()) * kDeg;
            glm::vec3 D = glm::normalize(T * std::cos(a) + radial * std::sin(a));
            D = glm::normalize(D - kUp * (LF.droop * (0.6f + 0.8f * rng.uni())));
            // The blade faces the sky as far as asked, the rest at random, and a
            // little twisted about its midrib either way.
            const glm::vec3 nUp  = perpTo(kUp, D);
            const glm::vec3 nRnd = rotateAbout(anyPerp(D), D, rng.uni() * kTau);
            const float up = std::clamp(LF.up, 0.0f, 1.0f);
            glm::vec3 N = perpTo(nRnd * (1.0f - up) + nUp * up, D);
            N = rotateAbout(N, D, 0.3f * rng.sym());
            if (LF.alongTwig && clusterN > 1)
                N = rotateAbout(N, D, kPi * static_cast<float>(ci) / static_cast<float>(clusterN));
            // Front face out of the crown: the side the sun sees, and the one the
            // shadow pass keeps (it culls back faces).
            if (glm::dot(N, crownN(sp.p)) < 0.0f) N = -N;
            const glm::vec3 W = glm::cross(D, N);
            const glm::vec3 P0 = LF.alongTwig ? sp.p : sp.p + radial * sp.r;

            const int cell = static_cast<int>(rng.next() % static_cast<std::uint64_t>(cells));
            const float cx = static_cast<float>(cell % cols), cy = static_cast<float>(cell / cols);
            const float u0 = (cx + ins) / static_cast<float>(cols);
            const float u1 = (cx + 1.0f - ins) / static_cast<float>(cols);
            const float vt = (cy + ins) / static_cast<float>(rows);          // the leaf tip: cell top
            const float vb = (cy + 1.0f - ins) / static_cast<float>(rows);   // its stalk: cell bottom
            const glm::vec3 tip  = D * size;
            const glm::vec3 half = W * (0.5f * w);
            // A card that would hang into the ground is left off: it would drag
            // the whole tree up when the model is stood on its lowest point.
            if (std::min({(P0 - half).y, (P0 + half).y, (P0 + tip - half).y, (P0 + tip + half).y}) < 0.02f)
                continue;
            const std::uint32_t f = static_cast<std::uint32_t>(m.leaves.size() / 8);
            if (fold <= 0.01f) {
                put(P0 - half, N, u0, vb);
                put(P0 + half, N, u1, vb);
                put(P0 + tip + half, N, u1, vt);
                put(P0 + tip - half, N, u0, vt);
                m.leafIdx.insert(m.leafIdx.end(), {f, f + 1, f + 2, f, f + 2, f + 3});
            } else {
                // Folded along the midrib: the halves rise towards the edges.
                const glm::vec3 lift = N * (0.25f * fold * w);
                const glm::vec3 nl = glm::normalize(N + W * (0.5f * fold));
                const glm::vec3 nr = glm::normalize(N - W * (0.5f * fold));
                const float um = 0.5f * (u0 + u1);
                put(P0 - half + lift, nl, u0, vb);
                put(P0, N, um, vb);
                put(P0 + half + lift, nr, u1, vb);
                put(P0 + tip - half + lift, nl, u0, vt);
                put(P0 + tip, N, um, vt);
                put(P0 + tip + half + lift, nr, u1, vt);
                m.leafIdx.insert(m.leafIdx.end(),
                                 {f, f + 1, f + 4, f, f + 4, f + 3, f + 1, f + 2, f + 5, f + 1, f + 5, f + 4});
            }
            ++m.leafCards;
        }
    }
}

} // namespace

// --- Public ----------------------------------------------------------------------------

const char* shapeName(Shape s) {
    switch (s) {
    case Shape::Conical:            return "Conical";
    case Shape::Spherical:          return "Spherical";
    case Shape::Hemispherical:      return "Hemispherical";
    case Shape::Cylindrical:        return "Cylindrical";
    case Shape::TaperedCylindrical: return "Tapered cylinder";
    case Shape::Flame:              return "Flame";
    case Shape::InverseConical:     return "Inverse cone";
    case Shape::TendFlame:          return "Soft flame";
    default:                        return "?";
    }
}

float shapeRatio(Shape s, float h) {
    const float r = 1.0f - std::clamp(h, 0.0f, 1.0f);   // Weber & Penn: 1 at the crown base
    float v = 1.0f;
    switch (s) {
    case Shape::Conical:            v = 0.2f + 0.8f * r; break;
    case Shape::Spherical:          v = 0.2f + 0.8f * std::sin(kPi * r); break;
    case Shape::Hemispherical:      v = 0.2f + 0.8f * std::sin(0.5f * kPi * r); break;
    case Shape::Cylindrical:        v = 1.0f; break;
    case Shape::TaperedCylindrical: v = 0.5f + 0.5f * r; break;
    case Shape::Flame:              v = r <= 0.7f ? r / 0.7f : (1.0f - r) / 0.3f; break;
    case Shape::InverseConical:     v = 1.0f - 0.8f * r; break;
    case Shape::TendFlame:          v = r <= 0.7f ? 0.5f + 0.5f * r / 0.7f
                                                  : 0.5f + 0.5f * (1.0f - r) / 0.3f; break;
    default: break;
    }
    return std::max(v, 0.05f);
}

Mesh generate(const Params& p) {
    Mesh m;
    Grower g(p);
    g.grow();
    g.solveRadii(m);
    g.skin(m);
    g.leaves(m);
    m.stems = g.stemCount();
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (const std::vector<float>* v : {&m.bark, &m.leaves})
        for (std::size_t i = 0; i + 7 < v->size(); i += 8) {
            const glm::vec3 q((*v)[i], (*v)[i + 1], (*v)[i + 2]);
            lo = glm::min(lo, q);
            hi = glm::max(hi, q);
        }
    if (lo.x <= hi.x) { m.lo = lo; m.hi = hi; }
    return m;
}
// --- Presets ------------------------------------------------------------------------

namespace {

Level lv(int count, float start, float length, float angle, float angleTip, float gnarl,
         float gravity, float curve, int segments, int sides) {
    Level l;
    l.count = count; l.start = start; l.length = length;
    l.angle = angle; l.angleTip = angleTip;
    l.gnarl = gnarl; l.gravity = gravity; l.curve = curve;
    l.segments = segments; l.sides = sides;
    return l;
}

Params oak() {
    Params p;
    p.name = "Oak"; p.seed = 7;
    p.height = 17.0f; p.radius = 0.5f; p.twigRadius = 0.005f;
    p.flare = 0.7f; p.flareHeight = 1.4f; p.buttress = 6; p.lean = 4.0f;
    p.forkStart = 0.3f; p.forkEnd = 0.5f; p.shape = Shape::Spherical; p.relief = 0.04f;
    p.levels = 3;
    p.level[0] = lv(0, 0, 1, 0, 0, 0.35f, -0.4f, 0, 18, 20);
    p.level[0].taper = 0.3f; p.level[0].forks = 2; p.level[0].forkAngle = 42.0f;
    p.level[1] = lv(14, 0.28f, 0.45f, 72, 38, 0.7f, 0.25f, 5, 12, 10);
    p.level[1].angleVar = 12.0f; p.level[1].lengthVar = 0.2f; p.level[1].taper = 0.3f;
    p.level[1].forks = 1; p.level[1].forkAngle = 35.0f;
    p.level[2] = lv(13, 0.1f, 0.45f, 55, 35, 0.6f, 0.1f, 10, 6, 5);
    p.level[2].angleVar = 15.0f; p.level[2].lengthVar = 0.25f;
    p.level[3] = lv(7, 0.15f, 0.4f, 45, 35, 0.4f, 0.05f, 0, 2, 3);
    p.level[3].angleVar = 15.0f; p.level[3].taper = 0.5f;
    Leaves& l = p.leaves;
    l.perStem = 4; l.cluster = 3; l.size = 0.2f; l.start = 0.2f; l.angle = 55; l.up = 0.55f;
    l.droop = 0.15f; l.bend = 0.7f; l.hollow = 0.45f; l.fold = 0.0f;
    return p;
}

Params beech() {
    Params p;
    p.name = "Beech"; p.seed = 3;
    p.height = 24.0f; p.radius = 0.42f; p.twigRadius = 0.004f;
    p.flare = 0.45f; p.flareHeight = 1.0f; p.buttress = 4; p.lean = 2.0f;
    p.forkStart = 0.5f; p.forkEnd = 0.62f; p.shape = Shape::TaperedCylindrical; p.relief = 0.015f;
    p.levels = 3;
    p.level[0] = lv(0, 0, 1, 0, 0, 0.12f, -0.6f, 0, 20, 20);
    p.level[0].taper = 0.35f; p.level[0].forks = 1; p.level[0].forkAngle = 25.0f;
    p.level[1] = lv(34, 0.3f, 0.36f, 58, 28, 0.35f, -0.1f, 18, 12, 10);
    p.level[1].angleVar = 10.0f; p.level[1].lengthVar = 0.2f;
    p.level[2] = lv(16, 0.1f, 0.5f, 55, 40, 0.35f, 0.35f, 0, 6, 5);
    p.level[2].angleVar = 12.0f; p.level[2].rotate = 180.0f;
    p.level[3] = lv(8, 0.15f, 0.35f, 45, 35, 0.3f, 0.1f, 0, 2, 3);
    p.level[3].rotate = 180.0f;
    Leaves& l = p.leaves;
    l.perStem = 4; l.cluster = 3; l.size = 0.16f; l.start = 0.15f; l.angle = 60; l.up = 0.8f;
    l.droop = 0.05f; l.bend = 0.7f; l.hollow = 0.4f; l.fold = 0.0f;
    return p;
}

Params birch() {
    Params p;
    p.name = "Birch"; p.seed = 11;
    p.height = 19.0f; p.radius = 0.22f; p.twigRadius = 0.0025f;
    p.flare = 0.35f; p.flareHeight = 0.7f; p.buttress = 3; p.lean = 5.0f;
    p.shape = Shape::TendFlame; p.relief = 0.01f;
    p.levels = 3;
    p.level[0] = lv(0, 0, 1, 0, 0, 0.18f, -0.3f, 0, 18, 16);
    p.level[0].taper = 0.5f;
    p.level[1] = lv(50, 0.3f, 0.3f, 48, 25, 0.35f, 0.2f, 18, 10, 8);
    p.level[1].angleVar = 12.0f; p.level[1].lengthVar = 0.25f;
    p.level[2] = lv(12, 0.15f, 0.6f, 40, 30, 0.2f, 1.8f, 0, 8, 4);
    p.level[3] = lv(7, 0.2f, 0.45f, 32, 25, 0.15f, 2.5f, 0, 3, 3);
    Leaves& l = p.leaves;
    l.perStem = 7; l.cluster = 2; l.size = 0.1f; l.start = 0.1f; l.angle = 50; l.up = 0.3f;
    l.droop = 0.4f; l.bend = 0.65f; l.hollow = 0.3f; l.fold = 0.0f;
    return p;
}

Params spruce() {
    Params p;
    p.name = "Spruce"; p.seed = 5;
    p.height = 24.0f; p.radius = 0.33f; p.twigRadius = 0.004f;
    p.flare = 0.5f; p.flareHeight = 0.9f; p.buttress = 5; p.lean = 1.0f;
    p.shape = Shape::Conical; p.relief = 0.02f;
    p.levels = 2;
    p.level[0] = lv(0, 0, 1, 0, 0, 0.05f, -1.0f, 0, 22, 16);
    p.level[0].taper = 0.6f;
    p.level[1] = lv(150, 0.06f, 0.3f, 100, 65, 0.1f, 0.6f, 28, 8, 6);
    p.level[1].whorl = 5; p.level[1].rotate = 37.0f; p.level[1].angleVar = 6.0f;
    p.level[1].lengthVar = 0.12f; p.level[1].taper = 0.5f;
    p.level[2] = lv(30, 0.1f, 0.35f, 62, 50, 0.1f, 1.2f, 0, 3, 3);
    p.level[2].whorl = 2; p.level[2].rotate = 0.0f; p.level[2].angleVar = 8.0f;
    Leaves& l = p.leaves;
    l.alongTwig = true; l.perStem = 6; l.cluster = 2; l.size = 0.6f; l.start = 0.05f; l.up = 0.75f;
    l.droop = 0.05f; l.bend = 0.55f; l.hollow = 0.25f; l.fold = 0.0f;
    return p;
}

Params pine() {
    Params p;
    p.name = "Scots pine"; p.seed = 9;
    p.height = 21.0f; p.radius = 0.3f; p.twigRadius = 0.005f;
    p.flare = 0.4f; p.flareHeight = 0.8f; p.buttress = 4; p.lean = 5.0f;
    p.forkStart = 0.78f; p.forkEnd = 0.85f; p.shape = Shape::Hemispherical; p.relief = 0.05f;
    p.levels = 2;
    p.level[0] = lv(0, 0, 1, 0, 0, 0.3f, -0.5f, 0, 20, 16);
    p.level[0].taper = 0.5f; p.level[0].forks = 1; p.level[0].forkAngle = 30.0f;
    p.level[1] = lv(70, 0.6f, 0.26f, 80, 50, 0.6f, 0.15f, 22, 10, 8);
    p.level[1].whorl = 4; p.level[1].rotate = 40.0f; p.level[1].angleVar = 12.0f;
    p.level[1].lengthVar = 0.25f;
    p.level[2] = lv(18, 0.2f, 0.45f, 45, 35, 0.4f, 0.0f, 25, 4, 4);
    Leaves& l = p.leaves;
    l.alongTwig = true; l.perStem = 4; l.cluster = 3; l.size = 0.4f; l.start = 0.5f; l.up = 0.3f;
    l.droop = 0.0f; l.bend = 0.55f; l.hollow = 0.2f; l.fold = 0.0f;
    return p;
}

Params willow() {
    Params p;
    p.name = "Weeping willow"; p.seed = 4;
    p.height = 13.0f; p.radius = 0.45f; p.twigRadius = 0.003f;
    p.flare = 0.6f; p.flareHeight = 1.0f; p.buttress = 5; p.lean = 6.0f;
    p.forkStart = 0.2f; p.forkEnd = 0.38f; p.shape = Shape::Hemispherical; p.relief = 0.05f;
    p.levels = 2;
    p.level[0] = lv(0, 0, 1, 0, 0, 0.4f, -0.2f, 0, 16, 18);
    p.level[0].taper = 0.3f; p.level[0].forks = 2; p.level[0].forkAngle = 45.0f;
    p.level[1] = lv(14, 0.3f, 0.55f, 50, 30, 0.45f, 0.35f, -25, 12, 10);
    p.level[1].angleVar = 12.0f; p.level[1].lengthVar = 0.2f;
    p.level[2] = lv(22, 0.1f, 1.0f, 35, 25, 0.1f, 4.5f, 0, 8, 3);
    p.level[2].lengthVar = 0.3f;
    Leaves& l = p.leaves;
    l.perStem = 14; l.cluster = 2; l.size = 0.14f; l.start = 0.05f; l.angle = 25; l.up = 0.2f;
    l.droop = 0.3f; l.bend = 0.55f; l.hollow = 0.2f; l.fold = 0.0f; l.onParent = false;
    return p;
}

Params poplar() {
    Params p;
    p.name = "Lombardy poplar"; p.seed = 2;
    p.height = 26.0f; p.radius = 0.4f; p.twigRadius = 0.004f;
    p.flare = 0.5f; p.flareHeight = 1.0f; p.buttress = 5; p.lean = 1.5f;
    p.shape = Shape::Flame; p.relief = 0.05f;
    p.levels = 3;
    p.level[0] = lv(0, 0, 1, 0, 0, 0.1f, -0.8f, 0, 22, 18);
    p.level[0].taper = 0.55f;
    p.level[1] = lv(80, 0.08f, 0.19f, 28, 15, 0.3f, -0.5f, 10, 10, 8);
    p.level[1].angleVar = 6.0f;
    p.level[2] = lv(12, 0.15f, 0.45f, 30, 20, 0.3f, -0.3f, 0, 5, 4);
    p.level[3] = lv(6, 0.2f, 0.4f, 35, 25, 0.25f, 0.0f, 0, 2, 3);
    Leaves& l = p.leaves;
    l.perStem = 5; l.cluster = 3; l.size = 0.13f; l.start = 0.15f; l.angle = 45; l.up = 0.4f;
    l.droop = 0.15f; l.bend = 0.65f; l.hollow = 0.3f; l.fold = 0.0f;
    return p;
}

Params maple() {
    Params p;
    p.name = "Maple"; p.seed = 6;
    p.height = 16.0f; p.radius = 0.4f; p.twigRadius = 0.0045f;
    p.flare = 0.5f; p.flareHeight = 1.0f; p.buttress = 5; p.lean = 3.0f;
    p.forkStart = 0.4f; p.forkEnd = 0.5f; p.shape = Shape::Spherical; p.relief = 0.035f;
    p.levels = 3;
    p.level[0] = lv(0, 0, 1, 0, 0, 0.2f, -0.5f, 0, 18, 18);
    p.level[0].taper = 0.35f; p.level[0].forks = 1; p.level[0].forkAngle = 35.0f;
    p.level[1] = lv(20, 0.3f, 0.42f, 55, 30, 0.45f, 0.05f, 12, 12, 10);
    p.level[1].whorl = 2; p.level[1].rotate = 90.0f; p.level[1].angleVar = 10.0f;
    p.level[2] = lv(14, 0.1f, 0.45f, 50, 35, 0.4f, 0.1f, 10, 6, 5);
    p.level[2].whorl = 2; p.level[2].rotate = 90.0f;
    p.level[3] = lv(7, 0.2f, 0.4f, 45, 35, 0.3f, 0.05f, 0, 2, 3);
    p.level[3].whorl = 2; p.level[3].rotate = 90.0f;
    Leaves& l = p.leaves;
    l.perStem = 3; l.cluster = 3; l.size = 0.24f; l.start = 0.2f; l.angle = 55; l.up = 0.75f;
    l.droop = 0.1f; l.bend = 0.7f; l.hollow = 0.45f; l.fold = 0.0f;
    return p;
}

Params shrub() {
    Params p;
    p.name = "Shrub"; p.seed = 8;
    p.height = 2.6f; p.heightVar = 0.15f; p.radius = 0.06f; p.twigRadius = 0.0025f;
    p.flare = 0.0f; p.buttress = 0; p.lean = 2.0f;
    p.forkStart = 0.02f; p.forkEnd = 0.2f; p.shape = Shape::Hemispherical; p.relief = 0.0f;
    p.levels = 2;
    p.level[0] = lv(0, 0, 1, 0, 0, 0.5f, 0.3f, 0, 10, 6);
    p.level[0].taper = 0.4f; p.level[0].forks = 3; p.level[0].forkAngle = 38.0f;
    p.level[1] = lv(16, 0.2f, 0.45f, 50, 35, 0.5f, 0.3f, 0, 6, 5);
    p.level[1].angleVar = 15.0f; p.level[1].lengthVar = 0.3f;
    p.level[2] = lv(7, 0.15f, 0.45f, 45, 35, 0.4f, 0.1f, 0, 2, 3);
    Leaves& l = p.leaves;
    l.perStem = 5; l.cluster = 2; l.size = 0.1f; l.start = 0.1f; l.angle = 50; l.up = 0.5f;
    l.droop = 0.1f; l.bend = 0.7f; l.hollow = 0.2f; l.fold = 0.0f;
    return p;
}
} // namespace

const std::vector<Preset>& presets() {
    static const std::vector<Preset> list = {
        {"Oak",             "Short trunk forking into gnarled limbs, broad round crown.", oak()},
        {"Beech",           "Tall smooth bole, dense layered ovoid crown.", beech()},
        {"Birch",           "Slender, light crown with hanging twigs.", birch()},
        {"Maple",           "Opposite branching, round dense crown.", maple()},
        {"Spruce",          "Conical, whorled limbs with hanging needle combs.", spruce()},
        {"Scots pine",      "Long bare trunk, flat-topped crown, needle tufts.", pine()},
        {"Weeping willow",  "Arching limbs with long hanging strands.", willow()},
        {"Lombardy poplar", "Narrow column of steep branches.", poplar()},
        {"Shrub",           "No trunk: many stems from the ground.", shrub()},
    };
    return list;
}

// --- JSON ---------------------------------------------------------------------------

namespace {

template <class F> void visitLevel(Level& l, F&& f) {
    f("count", l.count);       f("start", l.start);       f("end", l.end);
    f("length", l.length);     f("lengthVar", l.lengthVar);
    f("angle", l.angle);       f("angleTip", l.angleTip); f("angleVar", l.angleVar);
    f("whorl", l.whorl);       f("rotate", l.rotate);
    f("curve", l.curve);       f("gnarl", l.gnarl);       f("gravity", l.gravity);
    f("taper", l.taper);       f("segments", l.segments); f("sides", l.sides);
    f("forks", l.forks);       f("forkAngle", l.forkAngle);
}

template <class F> void visitLeaves(Leaves& l, F&& f) {
    f("enabled", l.enabled);   f("perStem", l.perStem);   f("cluster", l.cluster);
    f("size", l.size);
    f("sizeVar", l.sizeVar);   f("start", l.start);       f("angle", l.angle);
    f("up", l.up);             f("droop", l.droop);       f("bend", l.bend);
    f("hollow", l.hollow);     f("fold", l.fold);         f("alongTwig", l.alongTwig);
    f("onParent", l.onParent); f("cols", l.cols);         f("rows", l.rows);
    f("aspect", l.aspect);
}

template <class F> void visitParams(Params& p, F&& f) {
    f("name", p.name);             f("seed", p.seed);
    f("height", p.height);         f("heightVar", p.heightVar);
    f("radius", p.radius);         f("twigRadius", p.twigRadius);
    f("flare", p.flare);           f("flareHeight", p.flareHeight);
    f("buttress", p.buttress);     f("lean", p.lean);
    f("forkStart", p.forkStart);   f("forkEnd", p.forkEnd);
    f("relief", p.relief);         f("levels", p.levels);
    f("barkTexture", p.barkTexture); f("leafTexture", p.leafTexture);
    f("barkNormal", p.barkNormal); f("barkNormalStrength", p.barkNormalStrength);
    f("barkNormalDX", p.barkNormalDX);
    f("barkTile", p.barkTile);     f("barkAspect", p.barkAspect);
    f("detail", p.detail);         f("leafDensity", p.leafDensity);
}

} // namespace

void toJson(const Params& in, nlohmann::json& j) {
    Params p = in;
    j = nlohmann::json::object();
    j["version"] = 1;
    auto put = [](nlohmann::json& o) { return [&o](const char* k, const auto& v) { o[k] = v; }; };
    visitParams(p, put(j));
    j["shape"] = static_cast<int>(p.shape);
    nlohmann::json lvls = nlohmann::json::array();
    for (Level& l : p.level) {
        nlohmann::json o = nlohmann::json::object();
        visitLevel(l, put(o));
        lvls.push_back(o);
    }
    j["level"] = lvls;
    nlohmann::json lf = nlohmann::json::object();
    visitLeaves(p.leaves, put(lf));
    j["leaves"] = lf;
}

Params fromJson(const nlohmann::json& j) {
    Params p;
    if (!j.is_object()) return p;
    auto get = [](const nlohmann::json& o) {
        return [&o](const char* k, auto& v) {
            const auto it = o.find(k);
            if (it == o.end()) return;
            try { v = it->template get<std::decay_t<decltype(v)>>(); } catch (...) {}
        };
    };
    visitParams(p, get(j));
    if (j.contains("shape") && j["shape"].is_number_integer())
        p.shape = static_cast<Shape>(std::clamp(j["shape"].get<int>(), 0,
                                                static_cast<int>(Shape::Count) - 1));
    if (j.contains("level") && j["level"].is_array())
        for (std::size_t i = 0; i < 4 && i < j["level"].size(); ++i)
            visitLevel(p.level[i], get(j["level"][i]));
    if (j.contains("leaves") && j["leaves"].is_object()) visitLeaves(p.leaves, get(j["leaves"]));
    p.levels = std::clamp(p.levels, 1, 3);
    return p;
}

} // namespace treegen