#include "PathTrace.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace pathtrace {
namespace {

constexpr float kPi     = 3.14159265358979323846f;
constexpr float kInf    = std::numeric_limits<float>::infinity();
// Offset applied to a bounced ray's origin, in metres. Big enough that a
// surface does not shadow itself out of floating-point noise, small enough that
// contact shadows still touch the ground.
constexpr float kRayEps = 1e-3f;

float luminance(const glm::vec3& c) {
    return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
}

// --- Random numbers ---------------------------------------------------------
// PCG32. Small, fast, and -- the reason it is here rather than std::mt19937 --
// cheap to seed per tile and per pass, which is what makes a render
// reproducible: the same seed gives the same picture no matter how many threads
// happened to be free or which order they took the tiles in.
struct Rng {
    std::uint64_t state = 0, inc = 1;

    Rng(std::uint64_t seq, std::uint64_t seed) {
        inc   = (seq << 1u) | 1u;
        state = 0;
        next();
        state += seed;
        next();
    }
    std::uint32_t next() {
        const std::uint64_t old = state;
        state = old * 6364136223846793005ULL + inc;
        const std::uint32_t xorshifted = static_cast<std::uint32_t>(((old >> 18u) ^ old) >> 27u);
        const std::uint32_t rot        = static_cast<std::uint32_t>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31u));
    }
    // [0, 1). The shift keeps the top 24 bits, which is exactly a float's
    // mantissa -- so the value can never round up to 1.0 and break a sampler.
    float uniform() { return static_cast<float>(next() >> 8) * 0x1p-24f; }
};

// --- Sampling ---------------------------------------------------------------
// An orthonormal basis around `n` (Duff et al., branchless). Used everywhere a
// direction is sampled in tangent space and pushed back into the world.
void basisFrom(const glm::vec3& n, glm::vec3& t, glm::vec3& b) {
    const float sign = std::copysign(1.0f, n.z);
    const float a    = -1.0f / (sign + n.z);
    const float c    = n.x * n.y * a;
    t = glm::vec3(1.0f + sign * n.x * n.x * a, sign * c, -sign * n.x);
    b = glm::vec3(c, sign + n.y * n.y * a, -n.y);
}

glm::vec3 cosineHemisphere(const glm::vec3& n, Rng& rng) {
    const float u1 = rng.uniform(), u2 = rng.uniform();
    const float r  = std::sqrt(u1);
    const float phi = 2.0f * kPi * u2;
    glm::vec3 t, b;
    basisFrom(n, t, b);
    return glm::normalize(t * (r * std::cos(phi)) + b * (r * std::sin(phi)) +
                          n * std::sqrt(std::max(0.0f, 1.0f - u1)));
}

// A direction inside a cone of half-angle `cosMax` around `axis`. This is the
// sun's disc and a lamp's bulb: sampling it rather than aiming at a point is
// the whole of what makes a shadow edge soft.
glm::vec3 sampleCone(const glm::vec3& axis, float cosMax, Rng& rng) {
    const float u1  = rng.uniform(), u2 = rng.uniform();
    const float cosT = 1.0f - u1 * (1.0f - cosMax);
    const float sinT = std::sqrt(std::max(0.0f, 1.0f - cosT * cosT));
    const float phi  = 2.0f * kPi * u2;
    glm::vec3 t, b;
    basisFrom(axis, t, b);
    return glm::normalize(t * (sinT * std::cos(phi)) + b * (sinT * std::sin(phi)) +
                          axis * cosT);
}

glm::vec2 sampleDisk(Rng& rng) {
    const float r   = std::sqrt(rng.uniform());
    const float phi = 2.0f * kPi * rng.uniform();
    return {r * std::cos(phi), r * std::sin(phi)};
}

// --- Microfacet terms -------------------------------------------------------
// Trowbridge-Reitz (GGX) with height-correlated Smith visibility, i.e. the same
// specular model lit.frag approximates -- written out here because an offline
// renderer can afford the exact form the shader had to fit into a Karis
// approximation.
float ggxD(float NoH, float a) {
    const float a2 = a * a;
    const float d  = NoH * NoH * (a2 - 1.0f) + 1.0f;
    return a2 / std::max(kPi * d * d, 1e-9f);
}
float smithG1(float NoX, float a) {
    const float a2 = a * a;
    const float d  = NoX + std::sqrt(a2 + (1.0f - a2) * NoX * NoX);
    return 2.0f * NoX / std::max(d, 1e-9f);
}
float smithVis(float NoV, float NoL, float a) {
    const float a2 = a * a;
    const float v  = NoL * std::sqrt(a2 + (1.0f - a2) * NoV * NoV);
    const float l  = NoV * std::sqrt(a2 + (1.0f - a2) * NoL * NoL);
    return 0.5f / std::max(v + l, 1e-9f);
}
glm::vec3 fresnel(const glm::vec3& F0, float VoH) {
    const float f = std::pow(std::max(0.0f, 1.0f - VoH), 5.0f);
    return F0 + (glm::vec3(1.0f) - F0) * f;
}

// Heitz' visible-normal sampling. Draws half-vectors the eye can actually see,
// which at grazing angles is the difference between a clean highlight and a
// field of fireflies.
glm::vec3 sampleGGXVNDF(const glm::vec3& Ve, float a, float u1, float u2) {
    const glm::vec3 Vh = glm::normalize(glm::vec3(a * Ve.x, a * Ve.y, Ve.z));
    const float lensq  = Vh.x * Vh.x + Vh.y * Vh.y;
    const glm::vec3 T1 = lensq > 0.0f
                       ? glm::vec3(-Vh.y, Vh.x, 0.0f) * (1.0f / std::sqrt(lensq))
                       : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 T2 = glm::cross(Vh, T1);

    const float r   = std::sqrt(u1);
    const float phi = 2.0f * kPi * u2;
    float t1 = r * std::cos(phi);
    float t2 = r * std::sin(phi);
    const float s = 0.5f * (1.0f + Vh.z);
    t2 = (1.0f - s) * std::sqrt(std::max(0.0f, 1.0f - t1 * t1)) + s * t2;

    const glm::vec3 Nh = T1 * t1 + T2 * t2 +
                         Vh * std::sqrt(std::max(0.0f, 1.0f - t1 * t1 - t2 * t2));
    return glm::normalize(glm::vec3(a * Nh.x, a * Nh.y, std::max(1e-6f, Nh.z)));
}

// --- Terrain shading --------------------------------------------------------
// lit.frag's terrainSurface(), transcribed. A terrain has no UVs worth the name
// -- it is a heightfield, and a texture laid on it by its vertices stretches
// wherever the ground gets steep -- so the ground is coloured by WHERE IT IS
// instead: each painted layer claims the surfaces whose height and slope fall
// inside its band, and the layers cross-fade where the bands overlap.
//
// Transcribed rather than approximated, for the same reason as the tonemap: a
// terrain that is nearly the viewport's is a terrain somebody has to decide
// about, every time they look at a render.
float hash21(const glm::vec2& p0) {
    glm::vec2 p = glm::fract(p0 * glm::vec2(123.34f, 345.45f));
    p += glm::dot(p, p + 34.345f);
    return glm::fract(p.x * p.y);
}

float vnoise(const glm::vec2& p) {
    const glm::vec2 i = glm::floor(p);
    const glm::vec2 f = glm::fract(p);
    const glm::vec2 u = f * f * (3.0f - 2.0f * f);
    const float a = hash21(i);
    const float b = hash21(i + glm::vec2(1, 0));
    const float c = hash21(i + glm::vec2(0, 1));
    const float d = hash21(i + glm::vec2(1, 1));
    return glm::mix(glm::mix(a, b, u.x), glm::mix(c, d, u.x), u.y);
}

float detailFbm(glm::vec2 p) {
    float sum = 0.0f, amp = 0.5f;
    for (int i = 0; i < 4; ++i) {
        sum += amp * vnoise(p);
        p   *= 2.0f;
        amp *= 0.5f;
    }
    return sum;
}

// A soft window: fully inside between start and end, feathered at both edges.
float band(float x, float start, float end, float feather) {
    const auto ss = [](float e0, float e1, float v) {
        const float t = glm::clamp((v - e0) / std::max(e1 - e0, 1e-6f), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    return ss(start - feather, start + feather, x) *
           (1.0f - ss(end - feather, end + feather, x));
}

// One texture projected down all three axes and blended by the normal, so a
// cliff face gets the same material as the flat beside it without a seam.
glm::vec3 triplanar(const Image& tex, const glm::vec3& wp, const glm::vec3& n,
                    float scale) {
    glm::vec3 bw = glm::abs(n);
    bw = bw * bw * bw * bw;                       // pow(|n|, 4): a tight blend
    const float t = bw.x + bw.y + bw.z;
    bw /= std::max(t, 1e-6f);
    const glm::vec4 cx = tex.sample(wp.z * scale, wp.y * scale);
    const glm::vec4 cy = tex.sample(wp.x * scale, wp.z * scale);
    const glm::vec4 cz = tex.sample(wp.x * scale, wp.y * scale);
    return glm::vec3(cx) * bw.x + glm::vec3(cy) * bw.y + glm::vec3(cz) * bw.z;
}

// --- The surface at a hit ---------------------------------------------------
// The material with its texture already resolved, so the shading code below
// never has to know whether a colour came from a map or a field.
struct Surface {
    glm::vec3 diffuse{0.0f};  // albedo * (1 - reflectivity)
    glm::vec3 F0{0.04f};
    float     alpha = 0.16f;  // GGX roughness squared
    glm::vec3 emission{0.0f};
    float     coverage = 1.0f; // texture alpha * opacity
    bool      glass = false;
    // The share of the diffuse lobe that leaves through the far side. See
    // Material::translucency -- the diffuse lobe is split, never duplicated.
    float     translucency = 0.0f;
    glm::vec3 baseColor{1.0f};
};

// --- Ray/triangle -----------------------------------------------------------
struct Ray {
    glm::vec3 o{0.0f}, d{0.0f, 0.0f, -1.0f};
    glm::vec3 invD{0.0f};
    void prepare() {
        // Division by zero is deliberate: an axis-parallel ray gets +/-inf here
        // and the slab test's min/max then behaves exactly right, which the
        // guarded version does not.
        invD = glm::vec3(1.0f / d.x, 1.0f / d.y, 1.0f / d.z);
    }
};

// Where a ray stopped. `inst` is -1 for a world triangle and otherwise the
// instance whose mesh `tri` indexes into.
struct Hit {
    float t  = kInf;
    float u  = 0.0f, v = 0.0f;
    int   tri  = -1;
    int   inst = -1;
};

// A triangle as the intersection wants it and nothing else: one corner and the
// two edges from it. Thirty-six bytes against the hundred and eight of a whole
// Triangle, stored in the order the tree's leaves visit them -- so a leaf is
// one run of memory rather than four jumps through an index into a structure
// two thirds of which the test never reads. The walk is bandwidth-bound, and
// this is the bandwidth.
struct TriPos {
    glm::vec3 p0, e1, e2;
};

// Moeller-Trumbore, two-sided. Two-sided on purpose: a fair amount of scene
// geometry (a road ribbon, a decal patch, an imported model with a flipped
// winding) is single-sided in the raster path only because backface culling
// hides the problem, and a tracer that honoured the winding would put holes
// where the viewport shows a surface.
inline bool intersectPos(const TriPos& tp, const Ray& r, float tMax, float& t,
                         float& u, float& v) {
    const glm::vec3 p  = glm::cross(r.d, tp.e2);
    const float det = glm::dot(tp.e1, p);
    if (std::fabs(det) < 1e-12f) return false;
    const float inv = 1.0f / det;
    const glm::vec3 s = r.o - tp.p0;
    u = glm::dot(s, p) * inv;
    if (u < -1e-6f || u > 1.0f + 1e-6f) return false;
    const glm::vec3 q = glm::cross(s, tp.e1);
    v = glm::dot(r.d, q) * inv;
    if (v < -1e-6f || u + v > 1.0f + 1e-6f) return false;
    t = glm::dot(tp.e2, q) * inv;
    return t > kRayEps && t < tMax;
}

inline TriPos posOf(const Triangle& tri) {
    return {tri.p0, tri.p1 - tri.p0, tri.p2 - tri.p0};
}

bool intersectTri(const Triangle& tri, const Ray& r, float tMax, float& t,
                  float& u, float& v) {
    return intersectPos(posOf(tri), r, tMax, t, u, v);
}

// --- The accelerator build --------------------------------------------------
// A binned-SAH BVH. Chosen over a median split because scene geometry here is
// wildly non-uniform -- a 200-metre terrain chunk sits in the same list as a
// wing mirror -- and a median split over that produces nodes that overlap
// everything, which costs far more at trace time than the better build costs
// once.
//
// One builder for both levels. It knows primitives only as boxes, so the same
// code splits a mesh's triangles and the top level's instances -- and the two
// levels cannot drift into two ideas of what a good split is.
float surfaceArea(const glm::vec3& lo, const glm::vec3& hi) {
    const glm::vec3 e = glm::max(hi - lo, glm::vec3(0.0f));
    return 2.0f * (e.x * e.y + e.y * e.z + e.z * e.x);
}

struct TreeBuilder {
    // Per primitive, computed ONCE. The old build recomputed a triangle's box
    // from its three corners every time a bin or a child touched it -- several
    // times per level, on every level -- and that, not the SAH, was what the
    // build spent its time on.
    const std::vector<glm::vec3>& lo;
    const std::vector<glm::vec3>& hi;
    const std::vector<glm::vec3>& cen;
    std::vector<int>&             index;
    int                           leafSize;

    static constexpr int kBins = 16;

    void bound(BvhNode& n) const {
        n.lo = glm::vec3(kInf);
        n.hi = glm::vec3(-kInf);
        for (int i = 0; i < n.count; ++i) {
            const int p = index[n.leftFirst + i];
            n.lo = glm::min(n.lo, lo[p]);
            n.hi = glm::max(n.hi, hi[p]);
        }
    }

    // Split node `ni` in two, appending the children to `nodes`. False when it
    // is better left a leaf. Splitting has to beat leaving the node alone, or a
    // leaf is the right answer -- this is the termination rule, not the depth.
    bool split(std::vector<BvhNode>& nodes, int ni) const {
        const int first = nodes[ni].leftFirst;
        const int count = nodes[ni].count;
        if (count <= leafSize) return false;

        // Bin over the centroid bounds, not the node bounds: a handful of huge
        // triangles otherwise stretch the range so far that every centroid
        // lands in bin 0 and no split is ever found.
        glm::vec3 cLo(kInf), cHi(-kInf);
        for (int i = 0; i < count; ++i) {
            const glm::vec3& c = cen[index[first + i]];
            cLo = glm::min(cLo, c);
            cHi = glm::max(cHi, c);
        }

        // All three axes binned in ONE pass over the primitives rather than
        // three: the bins are small and hot, the primitives are not.
        glm::vec3 binLo[3][kBins], binHi[3][kBins];
        int       binN[3][kBins];
        glm::vec3 scale(0.0f);
        for (int a = 0; a < 3; ++a) {
            const float ext = cHi[a] - cLo[a];
            scale[a] = ext > 1e-6f ? static_cast<float>(kBins) / ext : 0.0f;
            for (int b = 0; b < kBins; ++b) {
                binLo[a][b] = glm::vec3(kInf);
                binHi[a][b] = glm::vec3(-kInf);
                binN[a][b]  = 0;
            }
        }
        for (int i = 0; i < count; ++i) {
            const int p = index[first + i];
            for (int a = 0; a < 3; ++a) {
                if (scale[a] == 0.0f) continue;
                int b = static_cast<int>((cen[p][a] - cLo[a]) * scale[a]);
                b = b < 0 ? 0 : (b >= kBins ? kBins - 1 : b);
                ++binN[a][b];
                binLo[a][b] = glm::min(binLo[a][b], lo[p]);
                binHi[a][b] = glm::max(binHi[a][b], hi[p]);
            }
        }

        float bestCost = kInf;
        int   bestAxis = -1;
        float bestSplit = 0.0f;
        for (int a = 0; a < 3; ++a) {
            if (scale[a] == 0.0f) continue;
            // Sweep from both ends so each candidate plane knows the cost of
            // both sides in one pass.
            float leftArea[kBins], rightArea[kBins];
            int   leftN[kBins],    rightN[kBins];
            glm::vec3 acLo(kInf), acHi(-kInf);
            int acN = 0;
            for (int b = 0; b < kBins; ++b) {
                acLo = glm::min(acLo, binLo[a][b]);
                acHi = glm::max(acHi, binHi[a][b]);
                acN += binN[a][b];
                leftN[b]    = acN;
                leftArea[b] = acN ? surfaceArea(acLo, acHi) : 0.0f;
            }
            acLo = glm::vec3(kInf); acHi = glm::vec3(-kInf); acN = 0;
            for (int b = kBins - 1; b >= 0; --b) {
                acLo = glm::min(acLo, binLo[a][b]);
                acHi = glm::max(acHi, binHi[a][b]);
                acN += binN[a][b];
                rightN[b]    = acN;
                rightArea[b] = acN ? surfaceArea(acLo, acHi) : 0.0f;
            }
            for (int b = 0; b < kBins - 1; ++b) {
                if (leftN[b] == 0 || rightN[b + 1] == 0) continue;
                const float cost = leftArea[b] * static_cast<float>(leftN[b]) +
                                   rightArea[b + 1] * static_cast<float>(rightN[b + 1]);
                if (cost < bestCost) {
                    bestCost  = cost;
                    bestAxis  = a;
                    bestSplit = cLo[a] + (static_cast<float>(b) + 1.0f) / scale[a];
                }
            }
        }

        const float leafCost = surfaceArea(nodes[ni].lo, nodes[ni].hi) *
                               static_cast<float>(count);
        if (bestAxis < 0 || bestCost >= leafCost) return false;

        const auto mid = std::partition(
            index.begin() + first, index.begin() + first + count,
            [&](int p) { return cen[p][bestAxis] < bestSplit; });
        const int leftCount = static_cast<int>(mid - (index.begin() + first));
        if (leftCount == 0 || leftCount == count) return false;

        const int leftIdx = static_cast<int>(nodes.size());
        BvhNode left{}, right{};
        left.leftFirst  = first;
        left.count      = leftCount;
        right.leftFirst = first + leftCount;
        right.count     = count - leftCount;
        bound(left);
        bound(right);
        nodes.push_back(left);
        nodes.push_back(right);
        nodes[ni].leftFirst = leftIdx;
        nodes[ni].count     = 0;
        return true;
    }

    // Everything under `root`, depth first, into `nodes`.
    void buildAll(std::vector<BvhNode>& nodes, int root) const {
        std::vector<int> stack{root};
        while (!stack.empty()) {
            const int ni = stack.back();
            stack.pop_back();
            if (split(nodes, ni)) {
                const int l = nodes[ni].leftFirst;
                stack.push_back(l);
                stack.push_back(l + 1);
            }
        }
    }
};

// Primitives below this many are built on one thread. Above it, the top of the
// tree is split serially until every open node is under it, and those subtrees
// are then built side by side and spliced in. A meadow is millions of blade
// triangles, and a single-threaded build of that was most of the wait before
// the first sample.
constexpr int kParallelGrain = 1 << 15;

// The tree over a set of boxes. Deterministic whatever the thread count: each
// subtree is a pure function of its primitive range, and they are spliced in a
// fixed order, so a render is reproducible to the bit on any machine.
void buildTree(const std::vector<glm::vec3>& lo, const std::vector<glm::vec3>& hi,
               const std::vector<glm::vec3>& cen, int leafSize,
               std::vector<BvhNode>& nodes, std::vector<int>& index) {
    nodes.clear();
    index.clear();
    const int n = static_cast<int>(lo.size());
    if (n == 0) return;
    index.resize(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) index[static_cast<std::size_t>(i)] = i;

    TreeBuilder tb{lo, hi, cen, index, std::max(1, leafSize)};
    nodes.reserve(static_cast<std::size_t>(n) * 2 / static_cast<std::size_t>(tb.leafSize) + 1);
    BvhNode root{};
    root.leftFirst = 0;
    root.count     = n;
    tb.bound(root);
    nodes.push_back(root);

    const int hw = static_cast<int>(std::thread::hardware_concurrency());
    if (n < kParallelGrain * 2 || hw < 2) {
        tb.buildAll(nodes, 0);
        return;
    }

    // The top, serially, parking every node that is small enough to be a job.
    std::vector<int> deferred;
    {
        std::vector<int> stack{0};
        while (!stack.empty()) {
            const int ni = stack.back();
            stack.pop_back();
            if (nodes[ni].count <= kParallelGrain) {
                if (nodes[ni].count > tb.leafSize) deferred.push_back(ni);
                continue;
            }
            if (tb.split(nodes, ni)) {
                const int l = nodes[ni].leftFirst;
                stack.push_back(l);
                stack.push_back(l + 1);
            }
        }
    }
    if (deferred.empty()) return;

    // Each parked node becomes the root of a private tree. The primitive
    // ranges are disjoint, so the partitions inside them never touch.
    std::vector<std::vector<BvhNode>> sub(deferred.size());
    std::atomic<std::size_t> next{0};
    auto work = [&] {
        for (;;) {
            const std::size_t k = next.fetch_add(1);
            if (k >= deferred.size()) return;
            sub[k].reserve(static_cast<std::size_t>(nodes[deferred[k]].count) * 2 /
                           static_cast<std::size_t>(tb.leafSize) + 1);
            sub[k].push_back(nodes[deferred[k]]);
            tb.buildAll(sub[k], 0);
        }
    };
    const int threads = std::clamp(hw, 1, std::min<int>(64, static_cast<int>(deferred.size())));
    std::vector<std::thread> pool;
    for (int t = 1; t < threads; ++t) pool.emplace_back(work);
    work();
    for (std::thread& t : pool) t.join();

    // Splice. A private tree's node j > 0 lands at offset + j - 1; its root
    // replaces the parked node in place, so the parent's pointer still holds.
    for (std::size_t k = 0; k < deferred.size(); ++k) {
        std::vector<BvhNode>& s = sub[k];
        const int offset = static_cast<int>(nodes.size());
        auto remap = [offset](BvhNode& nd) {
            if (nd.count == 0) nd.leftFirst = offset + nd.leftFirst - 1;
        };
        for (std::size_t j = 1; j < s.size(); ++j) {
            BvhNode nd = s[j];
            remap(nd);
            nodes.push_back(nd);
        }
        BvhNode rootNode = s[0];
        remap(rootNode);
        nodes[deferred[k]] = rootNode;
    }
}

// Slab test. Returns the near distance so the traversal can visit the closer
// child first and cull the far one against a hit it already has.
inline bool slab(const BvhNode& n, const Ray& r, float tMax, float& tNear) {
    const glm::vec3 t0 = (n.lo - r.o) * r.invD;
    const glm::vec3 t1 = (n.hi - r.o) * r.invD;
    const glm::vec3 lo = glm::min(t0, t1);
    const glm::vec3 hi = glm::max(t0, t1);
    const float a = std::max(std::max(lo.x, lo.y), std::max(lo.z, 0.0f));
    const float b = std::min(std::min(hi.x, hi.y), std::min(hi.z, tMax));
    tNear = a;
    return a <= b;
}

// --- One level: a tree over triangles ---------------------------------------
struct Blas {
    std::vector<BvhNode> nodes;
    std::vector<int>     index;   // leaf slot -> triangle
    std::vector<TriPos>  pos;     // leaf slot order: what the walk reads

    static constexpr int kLeafSize = 4;
    static constexpr int kStack    = 128;

    void build(const std::vector<Triangle>& tris) {
        const std::size_t n = tris.size();
        std::vector<glm::vec3> lo(n), hi(n), cen(n);
        for (std::size_t i = 0; i < n; ++i) {
            const Triangle& t = tris[i];
            // Centroids are computed once. Recomputing them inside the binning
            // loop is the single easiest way to make a build take minutes.
            lo[i]  = glm::min(t.p0, glm::min(t.p1, t.p2));
            hi[i]  = glm::max(t.p0, glm::max(t.p1, t.p2));
            cen[i] = (t.p0 + t.p1 + t.p2) * (1.0f / 3.0f);
        }
        buildTree(lo, hi, cen, kLeafSize, nodes, index);
        pos.resize(index.size());
        for (std::size_t s = 0; s < index.size(); ++s)
            pos[s] = posOf(tris[static_cast<std::size_t>(index[s])]);
    }

    bool empty() const { return nodes.empty(); }

    // Nearest hit closer than hit.t; on success hit.t/u/v/tri are updated and
    // the caller stamps whose triangle it was.
    bool closest(const Ray& r, Hit& hit) const {
        if (nodes.empty()) return false;
        int   stack[kStack];
        float stackT[kStack];
        int   sp = 0;
        int   node = 0;
        float nodeT = 0.0f;
        if (!slab(nodes[0], r, hit.t, nodeT)) return false;

        int slotHit = -1;
        for (;;) {
            const BvhNode& n = nodes[node];
            if (n.count > 0) {
                const TriPos* tp = &pos[static_cast<std::size_t>(n.leftFirst)];
                for (int i = 0; i < n.count; ++i) {
                    float t, u, v;
                    if (intersectPos(tp[i], r, hit.t, t, u, v)) {
                        hit.t = t; hit.u = u; hit.v = v;
                        slotHit = n.leftFirst + i;
                    }
                }
            } else {
                const int  l = n.leftFirst, rr = n.leftFirst + 1;
                float tl, tr;
                const bool hl = slab(nodes[l],  r, hit.t, tl);
                const bool hr = slab(nodes[rr], r, hit.t, tr);
                if (hl && hr) {
                    // Nearer child first: the far one is then very often already
                    // behind a confirmed hit and never opened at all.
                    const int  nearI = tl <= tr ? l : rr;
                    const int  farI  = tl <= tr ? rr : l;
                    const float farT = tl <= tr ? tr : tl;
                    if (sp < kStack) { stack[sp] = farI; stackT[sp] = farT; ++sp; }
                    node = nearI;
                    continue;
                }
                if (hl) { node = l;  continue; }
                if (hr) { node = rr; continue; }
            }
            // Pop, discarding anything the closest hit has already overtaken.
            bool popped = false;
            while (sp > 0) {
                --sp;
                if (stackT[sp] > hit.t) continue;
                node = stack[sp];
                popped = true;
                break;
            }
            if (!popped) break;
        }
        if (slotHit < 0) return false;
        hit.tri = index[static_cast<std::size_t>(slotHit)];
        return true;
    }

    // Every hit along the ray, multiplied into `tr`. False once nothing gets
    // through, so the caller can stop walking the other levels too. Not a
    // boolean at heart: the scene has glass and cutout foliage in it, and a
    // shadow ray that stopped at the first triangle would put a solid black
    // shadow under a windscreen.
    template <typename AlphaFn>
    bool transmittance(const Ray& r, float tMax, glm::vec3& tr,
                       const AlphaFn& alphaAt) const {
        if (nodes.empty()) return true;
        int stack[kStack];
        int sp = 0;
        int node = 0;
        float dummy;
        if (!slab(nodes[0], r, tMax, dummy)) return true;

        for (;;) {
            const BvhNode& n = nodes[node];
            if (n.count > 0) {
                const TriPos* tp = &pos[static_cast<std::size_t>(n.leftFirst)];
                for (int i = 0; i < n.count; ++i) {
                    float t, u, v;
                    if (!intersectPos(tp[i], r, tMax, t, u, v)) continue;
                    tr *= alphaAt(index[static_cast<std::size_t>(n.leftFirst + i)], u, v, t);
                    // Fully blocked: nothing further along the ray can matter.
                    if (luminance(tr) < 1e-4f) { tr = glm::vec3(0.0f); return false; }
                }
            } else {
                const int l = n.leftFirst, rr = n.leftFirst + 1;
                float tl, trr;
                const bool hl = slab(nodes[l],  r, tMax, tl);
                const bool hr = slab(nodes[rr], r, tMax, trr);
                if (hl && hr) {
                    if (sp < kStack) stack[sp++] = rr;
                    node = l;
                    continue;
                }
                if (hl) { node = l;  continue; }
                if (hr) { node = rr; continue; }
            }
            if (sp == 0) return true;
            node = stack[--sp];
        }
    }
};

// --- Two levels: the world, and the instances over it ------------------------
// An instance is a transform and a mesh. A ray meets it by being carried INTO
// the mesh's space rather than the mesh being carried out to the ray's -- the
// direction is transformed but not normalised, so the distance along it stays
// the world distance and a hit in one mesh compares directly with a hit in any
// other.
struct InstXf {
    glm::mat4 toObject{1.0f};
    glm::mat3 normalToWorld{1.0f};  // inverse transpose of the object->world part
    int       mesh     = -1;        // -1: unusable (degenerate transform, no mesh)
    int       material = -1;
};

struct Accel {
    Blas                 world;
    std::vector<Blas>    meshes;
    std::vector<BvhNode> top;        // over the instances' world boxes
    std::vector<int>     topIndex;   // leaf slot -> instance
    std::vector<InstXf>  xf;         // per instance, in Scene::instances order

    static constexpr int kStack = 64;

    void build(const Scene& sc) {
        world.build(sc.triangles);

        // The meshes side by side: a forest is a handful of species of a
        // hundred thousand triangles each, and nothing ties one build to the
        // next. (Each build parallelises itself too when it is big enough.)
        meshes.assign(sc.meshes.size(), Blas{});
        {
            std::atomic<std::size_t> next{0};
            auto work = [&] {
                for (;;) {
                    const std::size_t k = next.fetch_add(1);
                    if (k >= sc.meshes.size()) return;
                    meshes[k].build(sc.meshes[k].triangles);
                }
            };
            const int threads = std::clamp(
                static_cast<int>(std::thread::hardware_concurrency()), 1,
                std::max(1, static_cast<int>(sc.meshes.size())));
            std::vector<std::thread> pool;
            for (int t = 1; t < threads; ++t) pool.emplace_back(work);
            work();
            for (std::thread& t : pool) t.join();
        }

        const std::size_t n = sc.instances.size();
        xf.assign(n, InstXf{});
        std::vector<glm::vec3> lo, hi, cen;
        std::vector<int>       ids;   // box k -> instance
        lo.reserve(n); hi.reserve(n); cen.reserve(n); ids.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            const Instance& in = sc.instances[i];
            if (in.mesh < 0 || in.mesh >= static_cast<int>(meshes.size())) continue;
            const Blas& b = meshes[static_cast<std::size_t>(in.mesh)];
            if (b.empty()) continue;
            const glm::mat3 m3(in.transform);
            // A transform that flattens its mesh to nothing has no inverse, and
            // the NaNs an inverse would hand out poison every ray near it.
            if (std::fabs(glm::determinant(m3)) < 1e-12f) continue;
            InstXf& x = xf[i];
            x.toObject      = glm::inverse(in.transform);
            x.normalToWorld = glm::transpose(glm::inverse(m3));
            x.mesh          = in.mesh;
            x.material      = in.material;

            // The mesh's box, carried to the world by its eight corners.
            const BvhNode& r = b.nodes[0];
            glm::vec3 wl(kInf), wh(-kInf);
            for (int c = 0; c < 8; ++c) {
                const glm::vec3 p((c & 1) ? r.hi.x : r.lo.x,
                                  (c & 2) ? r.hi.y : r.lo.y,
                                  (c & 4) ? r.hi.z : r.lo.z);
                const glm::vec3 w = glm::vec3(in.transform * glm::vec4(p, 1.0f));
                wl = glm::min(wl, w);
                wh = glm::max(wh, w);
            }
            lo.push_back(wl);
            hi.push_back(wh);
            cen.push_back((wl + wh) * 0.5f);
            ids.push_back(static_cast<int>(i));
        }
        // One instance per leaf: two trees whose boxes overlap are better told
        // apart by the tree than by walking both of them.
        buildTree(lo, hi, cen, 1, top, topIndex);
        for (int& k : topIndex) k = ids[static_cast<std::size_t>(k)];
    }

    // A world-space ray, carried into instance `i`'s mesh space.
    Ray toObject(const Ray& r, int i) const {
        const InstXf& x = xf[static_cast<std::size_t>(i)];
        Ray o;
        o.o = glm::vec3(x.toObject * glm::vec4(r.o, 1.0f));
        o.d = glm::mat3(x.toObject) * r.d;
        o.prepare();
        return o;
    }

    bool closest(const Ray& r, float tMax, Hit& hit) const {
        hit = Hit{};
        hit.t = tMax;
        if (world.closest(r, hit)) hit.inst = -1;
        if (top.empty()) return hit.tri >= 0;

        int   stack[kStack];
        float stackT[kStack];
        int   sp = 0, node = 0;
        float nodeT = 0.0f;
        if (!slab(top[0], r, hit.t, nodeT)) return hit.tri >= 0;
        for (;;) {
            const BvhNode& n = top[node];
            if (n.count > 0) {
                for (int i = 0; i < n.count; ++i) {
                    const int inst = topIndex[static_cast<std::size_t>(n.leftFirst + i)];
                    const Ray lr = toObject(r, inst);
                    if (meshes[static_cast<std::size_t>(xf[static_cast<std::size_t>(inst)].mesh)]
                            .closest(lr, hit))
                        hit.inst = inst;
                }
            } else {
                const int l = n.leftFirst, rr = n.leftFirst + 1;
                float tl, tr;
                const bool hl = slab(top[l],  r, hit.t, tl);
                const bool hr = slab(top[rr], r, hit.t, tr);
                if (hl && hr) {
                    const int  nearI = tl <= tr ? l : rr;
                    const int  farI  = tl <= tr ? rr : l;
                    const float farT = tl <= tr ? tr : tl;
                    if (sp < kStack) { stack[sp] = farI; stackT[sp] = farT; ++sp; }
                    node = nearI;
                    continue;
                }
                if (hl) { node = l;  continue; }
                if (hr) { node = rr; continue; }
            }
            bool popped = false;
            while (sp > 0) {
                --sp;
                if (stackT[sp] > hit.t) continue;
                node = stack[sp];
                popped = true;
                break;
            }
            if (!popped) break;
        }
        return hit.tri >= 0;
    }

    // alphaAt(inst, tri, u, v, t): the colour one crossing, t along the ray,
    // lets through.
    template <typename AlphaFn>
    glm::vec3 transmittance(const Ray& r, float tMax, const AlphaFn& alphaAt) const {
        glm::vec3 tr(1.0f);
        if (!world.transmittance(r, tMax, tr, [&](int tri, float u, float v, float t) {
                return alphaAt(-1, tri, u, v, t);
            }))
            return glm::vec3(0.0f);
        if (top.empty()) return tr;

        int stack[kStack];
        int sp = 0, node = 0;
        float dummy;
        if (!slab(top[0], r, tMax, dummy)) return tr;
        for (;;) {
            const BvhNode& n = top[node];
            if (n.count > 0) {
                for (int i = 0; i < n.count; ++i) {
                    const int inst = topIndex[static_cast<std::size_t>(n.leftFirst + i)];
                    const Ray lr = toObject(r, inst);
                    const Blas& b =
                        meshes[static_cast<std::size_t>(xf[static_cast<std::size_t>(inst)].mesh)];
                    if (!b.transmittance(lr, tMax, tr, [&](int tri, float u, float v, float t) {
                            return alphaAt(inst, tri, u, v, t);
                        }))
                        return glm::vec3(0.0f);
                }
            } else {
                const int l = n.leftFirst, rr = n.leftFirst + 1;
                float tl, trr;
                const bool hl = slab(top[l],  r, tMax, tl);
                const bool hr = slab(top[rr], r, tMax, trr);
                if (hl && hr) {
                    if (sp < kStack) stack[sp++] = rr;
                    node = l;
                    continue;
                }
                if (hl) { node = l;  continue; }
                if (hr) { node = rr; continue; }
            }
            if (sp == 0) return tr;
            node = stack[--sp];
        }
    }
};

// Holds one tile while its sums and its sample count are brought into step.
// A spin lock rather than a mutex: the critical section is a thousand adds, and
// a thread that finds a tile busy is a preview refresh that can wait a
// microsecond.
struct TileGuard {
    std::atomic<bool>& flag;
    explicit TileGuard(std::atomic<bool>& f) : flag(f) {
        bool expected = false;
        while (!flag.compare_exchange_weak(expected, true,
                                           std::memory_order_acquire)) {
            expected = false;
            std::this_thread::yield();
        }
    }
    ~TileGuard() { flag.store(false, std::memory_order_release); }
    TileGuard(const TileGuard&)            = delete;
    TileGuard& operator=(const TileGuard&) = delete;
};

// --- Importance sampling the environment ------------------------------------
// Why this is not optional once a scene lights from a panorama. An HDRI is
// almost all of its light concentrated in almost none of its area: a barn with
// a bright roof opening, a sun disc, a window. A diffuse bounce drawn from the
// cosine lobe finds that opening once in a few thousand tries and comes back
// carrying thousands of times the average -- which is one white speck per
// unlucky pixel and no amount of samples to average them out inside a render
// anybody will wait for. Clamping hides it by throwing the light away, and the
// picture then comes out dark for a reason nothing on screen explains.
//
// So the panorama is turned into a distribution and sampled by BRIGHTNESS: a
// row is chosen against the marginal, a texel within it against that row's
// conditional, and the estimator divides by the density it used. The bright
// opening is then found on nearly every sample and contributes its true amount.
//
// The grid is deliberately coarser than the panorama (a few hundred texels
// across). It only has to steer the sampling; the RADIANCE is still read from
// the full-resolution map. A finer grid costs memory quadratically and buys
// nothing, because a sampler does not need to resolve what it is aiming at.
struct EnvSampler {
    int w = 0, h = 0;
    std::vector<float> func;     // w*h: luminance weighted by solid angle
    std::vector<float> condCdf;  // h * (w + 1)
    std::vector<float> margCdf;  // h + 1
    float total = 0.0f;

    bool valid() const { return total > 1e-12f && w > 0 && h > 0; }

    void build(const Environment& env, int maxWidth = 1024) {
        if (!env.hasMap()) return;
        w = std::min(env.width,  maxWidth);
        h = std::min(env.height, std::max(1, maxWidth / 2));
        if (w < 2 || h < 2) { w = h = 0; return; }

        func.assign(static_cast<std::size_t>(w) * h, 0.0f);
        // Block AVERAGE, not a point sample: a sun disc a few texels across
        // would fall between point samples on the coarse grid, and a sampler
        // blind to the brightest thing in the sky is worse than none.
        for (int y = 0; y < h; ++y) {
            const int sy0 = y * env.height / h;
            const int sy1 = std::max(sy0 + 1, (y + 1) * env.height / h);
            // The solid angle a row covers shrinks towards the poles.
            const float sinT = std::sin(kPi * (static_cast<float>(y) + 0.5f) /
                                        static_cast<float>(h));
            for (int x = 0; x < w; ++x) {
                const int sx0 = x * env.width / w;
                const int sx1 = std::max(sx0 + 1, (x + 1) * env.width / w);
                double sum = 0.0;
                double peak = 0.0;
                int n = 0;
                for (int sy = sy0; sy < sy1; ++sy)
                    for (int sx = sx0; sx < sx1; ++sx) {
                        const std::size_t o =
                            (static_cast<std::size_t>(sy) * env.width + sx) * 3;
                        const double l = 0.2126 * env.pixels[o] +
                                         0.7152 * env.pixels[o + 1] +
                                         0.0722 * env.pixels[o + 2];
                        sum += l;
                        peak = std::max(peak, l);
                        ++n;
                    }
                // Half the block's mean, half its PEAK. The mean alone is the
                // textbook answer and it is the one that hurts: a cell holding
                // a sun a few texels across averages out to something dim, the
                // sampler gives it a correspondingly tiny probability, and the
                // estimator then divides the sun's full radiance by that tiny
                // number. The result is a value large enough that multiplying
                // it by a colour channel which happens to be exactly zero
                // produces 0 * infinity -- a NaN in that one channel, which is
                // why the brightest parts of a sky came out yellow, cyan and
                // black rather than white. Weighting the peak in keeps the
                // probability in proportion to what is actually there.
                const double mean = sum / std::max(n, 1);
                func[static_cast<std::size_t>(y) * w + x] =
                    static_cast<float>(0.5 * mean + 0.5 * peak) * sinT;
            }
        }

        condCdf.assign(static_cast<std::size_t>(h) * (w + 1), 0.0f);
        margCdf.assign(static_cast<std::size_t>(h) + 1, 0.0f);
        for (int y = 0; y < h; ++y) {
            float* row = &condCdf[static_cast<std::size_t>(y) * (w + 1)];
            row[0] = 0.0f;
            for (int x = 0; x < w; ++x)
                row[x + 1] = row[x] + func[static_cast<std::size_t>(y) * w + x];
            const float rowSum = row[w];
            if (rowSum > 0.0f)
                for (int x = 1; x <= w; ++x) row[x] /= rowSum;
            margCdf[y + 1] = margCdf[y] + rowSum;
        }
        total = margCdf[h];
        if (total > 0.0f)
            for (int y = 1; y <= h; ++y) margCdf[y] /= total;
        else
            w = h = 0;
    }

    // The density this sampler puts on `dir`, per unit solid angle. Needed on
    // its own so a BSDF-sampled ray that lands on the sky can be weighted
    // against what the light sampler would have done.
    float pdfFor(const glm::vec3& dir) const {
        if (!valid()) return 0.0f;
        const float u = std::atan2(dir.z, dir.x) / (2.0f * kPi) + 0.5f;
        const float v = std::asin(glm::clamp(dir.y, -1.0f, 1.0f)) / kPi + 0.5f;
        const int x = std::clamp(static_cast<int>(u * w), 0, w - 1);
        const int y = std::clamp(static_cast<int>(v * h), 0, h - 1);
        const float sinT = std::sin(kPi * (static_cast<float>(y) + 0.5f) /
                                    static_cast<float>(h));
        if (sinT < 1e-6f) return 0.0f;
        // Density over (u, v), converted to solid angle: one unit square of
        // (u, v) covers 2*pi^2*sin(theta) steradians.
        const float pdfUv = func[static_cast<std::size_t>(y) * w + x] / total *
                            static_cast<float>(w * h);
        return pdfUv / (2.0f * kPi * kPi * sinT);
    }

    glm::vec3 sampleDir(float u1, float u2, float& pdfSolid) const {
        pdfSolid = 0.0f;
        if (!valid()) return glm::vec3(0.0f, 1.0f, 0.0f);

        // Row against the marginal, then texel against that row's conditional.
        const auto rowIt = std::upper_bound(margCdf.begin(), margCdf.end(), u1);
        int y = static_cast<int>(rowIt - margCdf.begin()) - 1;
        y = std::clamp(y, 0, h - 1);
        const float y0 = margCdf[y], y1 = margCdf[y + 1];
        const float dv = y1 > y0 ? (u1 - y0) / (y1 - y0) : 0.5f;

        const float* row = &condCdf[static_cast<std::size_t>(y) * (w + 1)];
        const auto colIt = std::upper_bound(row, row + w + 1, u2);
        int x = static_cast<int>(colIt - row) - 1;
        x = std::clamp(x, 0, w - 1);
        const float x0 = row[x], x1 = row[x + 1];
        const float du = x1 > x0 ? (u2 - x0) / (x1 - x0) : 0.5f;

        const float u = (static_cast<float>(x) + du) / static_cast<float>(w);
        const float v = (static_cast<float>(y) + dv) / static_cast<float>(h);

        const float elev = kPi * (v - 0.5f);
        const float phi  = 2.0f * kPi * (u - 0.5f);
        const float cosE = std::cos(elev);
        const glm::vec3 dir(cosE * std::cos(phi), std::sin(elev), cosE * std::sin(phi));

        const float sinT = std::sin(kPi * v);
        if (sinT < 1e-6f) return dir;
        const float pdfUv = func[static_cast<std::size_t>(y) * w + x] / total *
                            static_cast<float>(w * h);
        pdfSolid = pdfUv / (2.0f * kPi * kPi * sinT);
        return dir;
    }
};

// --- The integrator ---------------------------------------------------------
// The power heuristic (beta = 2). Two ways of finding the same light -- aiming
// at it, and bouncing into it -- each with a pdf; this decides how much to
// believe each one. Without it a near-mirror lit by a small sun gets a shadow
// ray whose BRDF value is enormous and whose probability is tiny, which is
// precisely a firefly: one white pixel that no number of neighbours averages
// away.
float misWeight(float a, float b) {
    const float a2 = a * a, b2 = b * b;
    return a2 / std::max(a2 + b2, 1e-9f);
}

// What a ray hit, resolved into the world: the triangle it was, the material
// it is drawn with, and its normals carried out of the instance's space. Every
// consumer of a hit goes through this, so no part of the integrator has to know
// whether the surface was drawn once or ten thousand times.
struct HitInfo {
    const Triangle* tri = nullptr;
    int       material = 0;
    glm::vec3 N{0.0f, 1.0f, 0.0f};   // interpolated shading normal, world
    glm::vec3 faceN{0.0f, 1.0f, 0.0f}; // geometric normal (winding), world, unnormalised
    glm::vec2 uv{0.0f};
};

struct Tracer {
    const Scene&      sc;
    const Accel&      bvh;
    const EnvSampler& envDist;
    int               maxBounces;
    float             clampIndirect;
    // Whether a ray that came straight from the camera, or off a delta lobe,
    // sees the captured backdrop (Environment::backdrop) rather than the
    // lighting environment. On for a picture; off for the light-probe bake,
    // whose rays are not an eye's and must only ever see the light.
    bool              useBackdrop = true;
    // What a lit water body glows with per unit of its colour (mediumLight()).
    glm::vec3         mediumGlow{0.0f};

    // The sun as a disc rather than a direction. Both estimators need it: the
    // light sampler needs its solid angle, and a ray that escapes needs to be
    // able to HIT it -- which is also what puts a sun in a chrome reflection,
    // where the raster path only ever had a bright blob from the probe.
    glm::vec3 sunAxis{0.0f, 1.0f, 0.0f};
    float     sunCosMax = 1.0f;
    float     sunPdf    = 0.0f;  // solid-angle density; 0 marks a hard-edged sun
    glm::vec3 sunRadiance{0.0f};
    // Held separately from the scene's own switch so a caller can trace the
    // scene WITHOUT its sun while leaving the scene untouched. The light-probe
    // bake needs exactly that: what it stores has to stay true as the sun moves
    // across the sky, so the sun is the one light it must not include.
    bool      sunOn = true;

    Tracer(const Scene& scene, const Accel& accel, const EnvSampler& env,
           int bounces, float clamp)
        : sc(scene), bvh(accel), envDist(env), maxBounces(bounces),
          clampIndirect(clamp) {
        sunOn     = sc.sun.enabled;
        sunAxis   = glm::normalize(sc.sun.direction);
        sunCosMax = std::cos(glm::radians(std::max(0.0f, sc.sun.angularRadiusDeg)));
        const float solid = 2.0f * kPi * (1.0f - sunCosMax);
        if (solid > 1e-7f) {
            sunPdf      = 1.0f / solid;
            // Radiance, from the irradiance the directional light was authored
            // as: spreading `color` over the disc keeps a render at the same
            // brightness as the viewport whatever angle the disc is given.
            sunRadiance = sc.sun.color / solid;
        }
        mediumGlow = mediumLight(sc);
    }

    // What `dist` metres of a water body do to a path inside it: the light
    // behind is attenuated, and the water's own lit colour fills in what was
    // lost. Nothing at all outside a medium.
    void throughMedium(const glm::vec3& sigma, const glm::vec3& glow, float dist,
                       glm::vec3& beta, glm::vec3& L, int bounce) const {
        if (sigma.x <= 0.0f && sigma.y <= 0.0f && sigma.z <= 0.0f) return;
        const glm::vec3 T = dist < kInf ? glm::exp(-sigma * dist) : glm::vec3(0.0f);
        L    += clamped(beta * glow * (glm::vec3(1.0f) - T), bounce);
        beta *= T;
    }

    // Ceiling on one indirect sample. See Settings::clampIndirect for why this
    // is here and why the primary hit is exempt.
    glm::vec3 clamped(const glm::vec3& c, int bounce) const {
        if (clampIndirect <= 0.0f || bounce == 0) return c;
        const float m = std::max(c.r, std::max(c.g, c.b));
        return m > clampIndirect ? c * (clampIndirect / m) : c;
    }

    // The texture's alpha at a hit, or 1 where there is no map.
    float texAlphaAt(const Material& m, const glm::vec2& uv) const {
        if (m.texture < 0 || m.texture >= static_cast<int>(sc.textures.size()))
            return 1.0f;
        return sc.textures[m.texture].sample(uv.x, uv.y).a;
    }

    // How much of this surface a ray actually meets, following AlphaMode
    // exactly as SceneTypes.hpp defines it and lit.frag implements it:
    //
    //   Opaque - the map's alpha is IGNORED. Not "usually 1, so it does not
    //            matter": ignored. Plenty of opaque materials carry an atlas
    //            with an alpha channel that means nothing, and reading it as
    //            transparency puts holes through solid paintwork.
    //   Cutout - a texel below the cutoff is a hole, the rest is solid. The
    //            comparison is against the TEXTURE's alpha, not against the
    //            texture's alpha times the scalar opacity, or a half-faded
    //            material would lose its cutout shape as well as its opacity.
    //   Blend  - texture alpha and scalar opacity multiply, as a blend does.
    //
    // One function for it because the camera walk and the shadow walk have to
    // agree: a leaf transparent to the eye and opaque to the sun is a bug that
    // only ever appears as a shadow with no object above it.
    float coverageAt(const Material& m, const glm::vec2& uv) const {
        if (m.alphaMode == 0) return m.opacity;
        const float texA = texAlphaAt(m, uv);
        if (m.alphaMode == 1) return texA < m.alphaCutoff ? 0.0f : m.opacity;
        return texA * m.opacity;
    }

    const Triangle& triOf(int inst, int tri) const {
        if (inst < 0) return sc.triangles[static_cast<std::size_t>(tri)];
        const Instance& in = sc.instances[static_cast<std::size_t>(inst)];
        return sc.meshes[static_cast<std::size_t>(in.mesh)]
                   .triangles[static_cast<std::size_t>(tri)];
    }

    int materialOf(int inst, const Triangle& t) const {
        int m = t.material;
        if (inst >= 0) {
            const int o = sc.instances[static_cast<std::size_t>(inst)].material;
            if (o >= 0) m = o;
        }
        return (m >= 0 && m < static_cast<int>(sc.materials.size())) ? m : 0;
    }

    static glm::vec2 uvOf(const Triangle& t, float u, float v) {
        const float w = 1.0f - u - v;
        return t.uv0 * w + t.uv1 * u + t.uv2 * v;
    }

    // Everything about a hit that is not the light: which triangle, which
    // material, and its normals in WORLD space. An instanced triangle's normals
    // are carried out through the inverse transpose, so a tree scaled to twice
    // its height is still lit as the tree and not as a sheared one.
    HitInfo resolve(const Hit& h) const {
        HitInfo r;
        const Triangle& t = triOf(h.inst, h.tri);
        r.tri      = &t;
        r.material = materialOf(h.inst, t);
        r.uv       = uvOf(t, h.u, h.v);
        const float w = 1.0f - h.u - h.v;
        glm::vec3 N  = t.n0 * w + t.n1 * h.u + t.n2 * h.v;
        glm::vec3 fN = glm::cross(t.p1 - t.p0, t.p2 - t.p0);
        if (h.inst >= 0) {
            const glm::mat3& nm = bvh.xf[static_cast<std::size_t>(h.inst)].normalToWorld;
            N  = nm * N;
            fN = nm * fN;
        }
        r.faceN = fN;
        // A triangle with no area AND no usable vertex normal has no side to
        // shade; up is as good an answer as any and, unlike normalize(0), is a
        // number -- a NaN here would be every direction the path takes next.
        if (glm::dot(N, N) >= 1e-12f)       r.N = glm::normalize(N);
        else if (glm::dot(fN, fN) >= 1e-24f) r.N = glm::normalize(fN);
        else                                r.N = glm::vec3(0.0f, 1.0f, 0.0f);
        return r;
    }

    // The terrain paint at a hit. Zero when the scene carries none, which is
    // the same answer an unpainted terrain gives -- so nothing downstream has
    // to know whether the side table exists. Instanced geometry is never
    // painted terrain.
    glm::vec4 paintAt(const Hit& h) const {
        if (h.inst >= 0 || sc.vertexPaint.empty()) return glm::vec4(0.0f);
        const std::size_t o = static_cast<std::size_t>(h.tri) * 3;
        if (o + 2 >= sc.vertexPaint.size()) return glm::vec4(0.0f);
        const float w = 1.0f - h.u - h.v;
        return sc.vertexPaint[o] * w + sc.vertexPaint[o + 1] * h.u +
               sc.vertexPaint[o + 2] * h.v;
    }

    // A surface's own colour at a UV, sRGB, before any light: the map times its
    // tint where there is one, the flat albedo where there is not.
    glm::vec3 baseAt(const Material& m, const glm::vec2& uv) const {
        if (m.texture >= 0 && m.texture < static_cast<int>(sc.textures.size()))
            return glm::vec3(sc.textures[static_cast<std::size_t>(m.texture)]
                                 .sample(uv.x, uv.y)) * m.tint;
        return m.albedo;
    }

    // How much light survives one crossing of this triangle, as a colour. Glass
    // tints what passes through it, which is what stops a green-tinted
    // windscreen from casting a grey shadow.
    glm::vec3 shadowFactor(int inst, int tri, float u, float v) const {
        const Triangle& t = triOf(inst, tri);
        const Material& m = sc.materials[static_cast<std::size_t>(materialOf(inst, t))];
        // A glow blocks nothing: light goes straight through a spark.
        if (m.additive) return glm::vec3(1.0f);
        const glm::vec2 uv = uvOf(t, u, v);
        const float a = coverageAt(m, uv);
        if (m.translucency > 0.0f && !m.glass) {
            // A leaf between a point and the sun does not black it out; it
            // dims and tints what gets past, and a canopy is mostly this. The
            // tint is the leaf's own colour -- its map where it has one, which
            // is every tree leaf -- linearised as everywhere else, which is why
            // light under grass is green rather than merely less.
            const glm::vec3 tint = glm::pow(glm::max(baseAt(m, uv), glm::vec3(0.0f)),
                                            glm::vec3(2.2f));
            const float tl = glm::clamp(m.translucency, 0.0f, 1.0f);
            return glm::mix(glm::vec3(1.0f - a), tint * a + glm::vec3(1.0f - a), tl);
        }
        if (a >= 0.999f && !m.glass) return glm::vec3(0.0f);
        if (m.glass) {
            // A pane refracts rather than blocks; treating it as a partial
            // blocker is the cheap stand-in for caustics we are not tracing.
            const glm::vec3 tint = glm::pow(glm::max(m.albedo, glm::vec3(0.0f)),
                                            glm::vec3(2.2f));
            return glm::mix(glm::vec3(1.0f), tint, 0.35f) * 0.85f;
        }
        return glm::vec3(1.0f - a);
    }

    // The terrain's colour at a point, from its layers. Returns false when no
    // layer covers it, which is the shader's "gap between bands" case and falls
    // back to the flat base colour exactly as lit.frag does.
    bool terrainColorAt(const Material& m, const glm::vec3& wp, const glm::vec3& n,
                        const glm::vec4& paint, glm::vec3& out) const {
        if (m.layers.empty()) return false;
        const GroundLook& g = sc.ground;
        // The woods: this point's share of a stand (ecology.glsl's .y).
        const float woods = g.woodsOn() ? woodsAt(g, glm::vec2(wp.x, wp.z), n.y) : 0.0f;
        glm::vec3 col;
        if (!layeredColour(m, wp, n, paint, woods, col)) return false;
        const float dist = glm::distance(wp, sc.camera.position);
        // Past the traced forest the floor goes the canopy's colour -- the
        // shader's stand-in for crowns it no longer draws.
        if (woods > 0.0f && g.canopyFrom < 1e29f) {
            const float t = woods * glm::smoothstep(g.canopyFrom, g.canopyTo, dist);
            col = glm::mix(col, glm::pow(g.canopy * 0.55f, glm::vec3(1.0f / 2.2f)), 0.7f * t);
        }
        // Past the traced blades, the field's colour.
        if (g.meadowOn()) {
            const float t = glm::smoothstep(g.meadowNear, g.meadowFar, dist) * g.meadowAmount;
            if (t > 0.0f) {
                const float lush = g.lushAt(wp.x, wp.z);
                const float thin = lush < 0.22f ? g.grassDry * glm::mix(0.35f, 0.75f, lush / 0.22f)
                                                : 1.0f;
                const float cover = t * meadowCover(glm::vec2(wp.x, wp.z), wp.y, n.y,
                                                    g.waterLevel, g.grassTop) *
                                    (1.0f - 0.9f * woods) * glm::clamp(thin * 1.4f, 0.0f, 1.0f);
                const glm::vec3 mc = glm::pow(
                    glm::pow(meadowColour(glm::vec2(wp.x, wp.z), lush), glm::vec3(2.2f)) *
                        g.grassTint, glm::vec3(1.0f / 2.2f));
                col = glm::mix(col, mc, cover);
            }
        }
        out = col;
        return true;
    }

    // The layers alone: height and slope bands, the forest floor in the woods,
    // and the hand-painted weights over both.
    bool layeredColour(const Material& m, const glm::vec3& wp, const glm::vec3& n,
                       const glm::vec4& paint, float woods, glm::vec3& out) const {
        const int forestLayer = sc.ground.forestLayer;

        // The height-edge jitter. The shader fades this noise out where a pixel
        // covers more than about one period of it, because it has one sample
        // per pixel and would otherwise alias. Here it is taken at full
        // strength everywhere: a path tracer already takes tens of samples per
        // pixel with the ray jittered inside it, so the noise is AVERAGED
        // rather than pointed at -- the one place the offline renderer gets a
        // better answer than the shader for free rather than for effort.
        const float detail = m.detailScale > 0.0f
                           ? detailFbm(glm::vec2(wp.x, wp.z) * m.detailScale)
                           : 0.5f;
        const float h        = wp.y + (detail - 0.5f) * 3.0f;
        const float slopeDeg = glm::degrees(std::acos(glm::clamp(n.y, -1.0f, 1.0f)));

        // Hand-painted layers override the automatic height/slope blend where
        // they cover, and leave it untouched where they do not.
        const glm::vec4 p = glm::clamp(paint, glm::vec4(0.0f), glm::vec4(1.0f));
        const float cover = glm::clamp(p.x + p.y + p.z + p.w, 0.0f, 1.0f);

        constexpr std::size_t kMax = 8;
        glm::vec3 cols[kMax];
        float     ws[kMax] = {};
        float wsum = 0.0f;
        const std::size_t count = std::min(m.layers.size(), kMax);
        for (std::size_t i = 0; i < count; ++i) {
            const TerrainLayer& L = m.layers[i];
            float autoW = band(h, L.band.x, L.band.y, 1.5f) *
                          band(slopeDeg, L.band.z, L.band.w, 6.0f);
            // In the woods the forest floor layer wins over whatever the
            // height and slope bands would have laid there.
            if (forestLayer >= 0 && woods > 0.0f)
                autoW = static_cast<int>(i) == forestLayer ? std::max(autoW, woods * 1.3f)
                                                           : autoW * (1.0f - 0.85f * woods);
            const float pw = i < 4 ? p[static_cast<int>(i)] : 0.0f;
            const float w  = autoW * (1.0f - cover) + pw;
            if (w <= 0.0f) continue;
            if (L.texture < 0 || L.texture >= static_cast<int>(sc.textures.size()))
                continue;
            cols[i] = triplanar(sc.textures[L.texture], wp, n, L.scale);
            ws[i]   = w;
            wsum   += w;
        }
        if (wsum < 1e-4f) return false;
        // lit.frag's height blend, transcribed: brightness stands in for
        // height, and within a transition the higher texel wins.
        if (m.heightBlend > 0.0f) {
            constexpr float depth = 0.2f;
            const glm::vec3 lw(0.299f, 0.587f, 0.114f);
            float top = -1e9f;
            for (std::size_t i = 0; i < count; ++i)
                if (ws[i] > 0.0f) top = std::max(top, ws[i] / wsum + glm::dot(cols[i], lw));
            float nsum = 0.0f;
            const float s = glm::clamp(m.heightBlend, 0.0f, 1.0f);
            for (std::size_t i = 0; i < count; ++i) {
                if (ws[i] <= 0.0f) continue;
                const float lin = ws[i] / wsum;
                const float hb  = std::max(lin + glm::dot(cols[i], lw) - (top - depth), 0.0f);
                ws[i] = lin + (hb - lin) * s;
                nsum += ws[i];
            }
            wsum = std::max(nsum, 1e-6f);
        }
        glm::vec3 acc(0.0f);
        for (std::size_t i = 0; i < count; ++i)
            if (ws[i] > 0.0f) acc += cols[i] * ws[i];
        out = acc / wsum;
        return true;
    }

    Surface surfaceAt(const Material& m, const glm::vec2& uv, const glm::vec3& wp,
                      const glm::vec3& n, const glm::vec4& paint,
                      float& outAlpha) const {
        Surface s;
        glm::vec3 base = m.albedo;
        float     texA = 1.0f;
        glm::vec3 terrain;
        if (terrainColorAt(m, wp, n, paint, terrain)) {
            base = terrain;
        } else if (m.texture >= 0 &&
                   m.texture < static_cast<int>(sc.textures.size())) {
            const glm::vec4 t = sc.textures[m.texture].sample(uv.x, uv.y);
            base = glm::vec3(t) * m.tint;
            texA = t.a;
        }
        outAlpha = texA;
        // sRGB -> linear, the same conversion and at the same point as
        // lit.frag's `albedo = pow(albedo, vec3(2.2))`. Skipping it is the
        // classic way to end up with a render that is washed out next to the
        // viewport and no obvious reason why.
        base = glm::pow(glm::max(base, glm::vec3(0.0f)), glm::vec3(2.2f));
        const float refl = glm::clamp(m.reflectivity, 0.0f, 1.0f);
        s.baseColor = base;
        s.diffuse   = base * (1.0f - refl);
        // F0 rises from a dielectric's 4% towards the base colour, which is the
        // same trade lit.frag makes with uReflectivity -- one dial standing in
        // for metalness.
        s.F0        = glm::mix(glm::vec3(0.04f), base, refl);
        const float r = glm::clamp(m.roughness, 0.03f, 1.0f);
        s.alpha     = r * r;
        s.emission  = glm::pow(glm::max(m.emission, glm::vec3(0.0f)),
                               glm::vec3(2.2f)) * m.emissionStrength;
        if (m.farGround) {
            // Already linear: farterrain.frag works in linear throughout.
            base = farGroundAlbedo(sc.ground, wp, n, uv.x);
            s.baseColor = base;
            s.diffuse   = base;
            s.F0        = glm::vec3(0.04f);
        }
        s.coverage  = coverageAt(m, uv);
        s.glass     = m.glass;
        s.translucency = glm::clamp(m.translucency, 0.0f, 1.0f);
        return s;
    }

    // The BRDF, without the cosine. Diffuse plus a GGX lobe. `specScale` is the
    // energy correction that comes with widening the lobe for a light of finite
    // size -- 1 everywhere else.
    glm::vec3 evalBsdf(const Surface& s, const glm::vec3& N, const glm::vec3& V,
                       const glm::vec3& L, float specScale = 1.0f) const {
        const float NoL = glm::dot(N, L);
        const float NoV = glm::dot(N, V);
        if (NoV <= 0.0f) return glm::vec3(0.0f);
        if (NoL <= 0.0f) {
            // The far side. Diffuse only: light that went through a leaf has
            // forgotten which way it came in, and a specular highlight seen
            // through a surface is not a thing a thin sheet does.
            if (s.translucency <= 0.0f) return glm::vec3(0.0f);
            return s.diffuse * (s.translucency * (1.0f / kPi));
        }
        const glm::vec3 H = glm::normalize(V + L);
        const float NoH = std::max(0.0f, glm::dot(N, H));
        const float VoH = std::max(0.0f, glm::dot(V, H));
        const glm::vec3 spec = fresnel(s.F0, VoH) *
                               (ggxD(NoH, s.alpha) * smithVis(NoV, NoL, s.alpha));
        // The near side keeps whatever the far side did not take.
        return s.diffuse * ((1.0f - s.translucency) * (1.0f / kPi))
             + spec * specScale;
    }

    // The pdf the sampler below would have used for this direction. Needed
    // separately because the sampler picks between two lobes and the weight has
    // to divide by the combined density, not by the one that happened to fire.
    float pdfBsdf(const Surface& s, const glm::vec3& N, const glm::vec3& V,
                  const glm::vec3& L, float pSpec) const {
        const float NoL = glm::dot(N, L);
        const float NoV = glm::dot(N, V);
        if (NoV <= 0.0f) return 0.0f;
        if (NoL <= 0.0f) {
            // The far-side lobe, chosen inside the diffuse branch with
            // probability `translucency` and cosine-distributed about -N.
            if (s.translucency <= 0.0f) return 0.0f;
            return (1.0f - pSpec) * s.translucency * (-NoL) * (1.0f / kPi);
        }
        const glm::vec3 H = glm::normalize(V + L);
        const float NoH = std::max(0.0f, glm::dot(N, H));
        const float pdfSpec = ggxD(NoH, s.alpha) * smithG1(NoV, s.alpha) /
                              std::max(4.0f * NoV, 1e-6f);
        const float pdfDiff = (1.0f - s.translucency) * NoL * (1.0f / kPi);
        return pSpec * pdfSpec + (1.0f - pSpec) * pdfDiff;
    }

    // How often to try the specular lobe. A heuristic, not a law -- the weight
    // divides by the true combined pdf either way, so a poor guess costs noise
    // and never correctness.
    static float specProbability(const Surface& s) {
        return glm::clamp(0.25f + 0.6f * luminance(s.F0), 0.05f, 0.95f);
    }

    // Light arriving directly at a point. Everything soft-edged in a render
    // comes from here: the sun is sampled across its disc and a lamp across its
    // bulb, so a shadow gains a penumbra that widens with distance the way a
    // real one does.
    // `sigma` is the medium the point sits in (zero outside one): a lake bed's
    // shadow ray is attenuated by the water column up to the surface it leaves
    // through, the nearest glass crossing along it.
    glm::vec3 directLight(const glm::vec3& P, const glm::vec3& N, const glm::vec3& V,
                          const Surface& s, float pSpec, Rng& rng,
                          const glm::vec3& sigma = glm::vec3(0.0f)) const {
        glm::vec3 L(0.0f);
        const bool inMedium = sigma.x > 0.0f || sigma.y > 0.0f || sigma.z > 0.0f;
        float exitT = kInf;
        const auto alphaFn = [&](int inst, int tri, float u, float v, float t) {
            if (inMedium && t < exitT) {
                const Triangle& tt = triOf(inst, tri);
                if (sc.materials[static_cast<std::size_t>(materialOf(inst, tt))].glass)
                    exitT = t;
            }
            return shadowFactor(inst, tri, u, v);
        };
        // The column of water between the point and where its shadow ray
        // leaves it. A ray that never leaves (the medium is all there is in
        // that direction) is swallowed.
        const auto column = [&](float tMax) {
            if (!inMedium) return glm::vec3(1.0f);
            const float d = std::min(exitT, tMax);
            return d < kInf ? glm::exp(-sigma * d) : glm::vec3(0.0f);
        };
        // One shadow ray: the walk, and THEN the water column it measured. Two
        // statements on purpose -- written as one product, the order the two
        // operands are evaluated in is the compiler's choice, and MSVC chose to
        // measure the column before the walk had found the surface: every lake
        // bed in the picture lost its sun.
        const auto shadowRay = [&](const Ray& sr, float tMax) {
            exitT = kInf;
            glm::vec3 tr = bvh.transmittance(sr, tMax, alphaFn);
            tr *= column(tMax);
            return tr;
        };

        if (sunOn && luminance(sc.sun.color) > 1e-5f) {
            const glm::vec3 dir = sunPdf > 0.0f ? sampleCone(sunAxis, sunCosMax, rng)
                                                : sunAxis;
            const float NoL = glm::dot(N, dir);
            // A light behind a translucent surface reaches it through the far
            // side -- which for a meadow is most of the light there is. The ray
            // then has to leave from the OTHER face, or it starts inside the
            // leaf it is trying to see past.
            const bool behind = NoL < 0.0f && s.translucency > 0.0f;
            if (NoL > 0.0f || behind) {
                Ray sr;
                sr.o = P + (behind ? -N : N) * kRayEps;
                sr.d = dir;
                sr.prepare();
                // The clouds first: a point in their shadow needs no shadow
                // ray to know it is dark.
                const float clouds = sc.sunMask.valid() ? sc.sunMask.at(P) : 1.0f;
                const glm::vec3 tr = clouds > 1e-4f ? shadowRay(sr, kInf) * clouds
                                                    : glm::vec3(0.0f);
                if (luminance(tr) > 1e-4f) {
                    // sun.color is already radiance divided by this sampler's
                    // own pdf, which is why it appears here unscaled.
                    glm::vec3 c = sc.sun.color * tr * evalBsdf(s, N, V, dir)
                                * std::fabs(NoL);
                    // A disc can also be found by bouncing into it, so this
                    // strategy only claims its share. A hard-edged sun cannot
                    // be hit at all, and keeps the lot.
                    if (sunPdf > 0.0f)
                        c *= misWeight(sunPdf, pdfBsdf(s, N, V, dir, pSpec));
                    L += c;
                }
            }
        }

        // The sky, aimed at rather than stumbled into. Same shape as the sun
        // above -- sample, shadow-test, weigh against the BSDF's own chance of
        // having found the same direction.
        if (envDist.valid()) {
            float pdfE = 0.0f;
            const glm::vec3 dir = envDist.sampleDir(rng.uniform(), rng.uniform(), pdfE);
            const float NoL = glm::dot(N, dir);
            // 1e-4, not 1e-9. The guard is not there to avoid dividing by
            // zero -- it is there to stop the quotient reaching a size where
            // the arithmetic after it stops being arithmetic. A direction the
            // sampler considers this unlikely contributes nothing worth the
            // risk.
            const bool envBehind = NoL < 0.0f && s.translucency > 0.0f;
            if (pdfE > 1e-4f && (NoL > 0.0f || envBehind)) {
                Ray sr;
                sr.o = P + (envBehind ? -N : N) * kRayEps;
                sr.d = dir;
                sr.prepare();
                const glm::vec3 tr = shadowRay(sr, kInf);
                if (luminance(tr) > 1e-4f) {
                    const glm::vec3 Le = sc.env.sample(dir) * sc.env.intensity;
                    const float wgt = misWeight(pdfE, pdfBsdf(s, N, V, dir, pSpec));
                    const glm::vec3 c = Le * tr * evalBsdf(s, N, V, dir) *
                                        (std::fabs(NoL) * wgt / pdfE);
                    // Checked rather than trusted. Every other term here is
                    // bounded by construction; this one is a quotient, and a
                    // quotient is where a renderer stops being able to promise
                    // anything about its own numbers.
                    if (c.x == c.x && c.y == c.y && c.z == c.z)
                        L += glm::min(c, glm::vec3(50000.0f));
                }
            }
        }

        for (const Lamp& lamp : sc.lamps) {
            glm::vec3 d = lamp.position - P;
            const float dist = glm::length(d);
            if (dist < 1e-4f) continue;
            d /= dist;

            // The same range-limited falloff lit.frag uses. Not inverse-square
            // on purpose: matching the raster path's brightness matters more
            // here than being right about a light the author already tuned by
            // eye against the other one.
            float att = glm::clamp(1.0f - dist / std::max(lamp.range, 1e-3f), 0.0f, 1.0f);
            att *= att;
            if (att <= 0.0f) continue;

            if (lamp.isSpot()) {
                const float cosA = glm::dot(-d, glm::normalize(lamp.direction));
                float cone = glm::clamp((cosA - lamp.cosOuter) /
                                        std::max(lamp.cosInner - lamp.cosOuter, 1e-3f),
                                        0.0f, 1.0f);
                cone *= cone;
                if (cone <= 0.0f) continue;
                att *= cone;
            }

            const float NoL = glm::dot(N, d);
            if (NoL <= 0.0f) continue;

            // The bulb has a size, and that size does two separate things. It
            // softens the SHADOW, which needs a jittered ray to find out about;
            // and it softens the HIGHLIGHT, which does not -- a bulb cannot
            // produce a reflection narrower than the angle it subtends. Doing
            // the second by jittering as well was the expensive mistake: the
            // BRDF value then swings by orders of magnitude between samples on
            // a glossy surface, and every one of those swings is a white speck
            // that takes thousands of samples to average out. Widening the lobe
            // instead gives the same picture with none of the variance.
            const float sphereAngle = std::min(1.0f, lamp.radius / std::max(dist, 1e-3f));
            Surface wide = s;
            wide.alpha = glm::clamp(s.alpha + 0.5f * sphereAngle, s.alpha, 1.0f);
            const float ratio     = s.alpha / std::max(wide.alpha, 1e-6f);
            const float specScale = ratio * ratio; // the same energy, spread wider

            // The shadow ray, and only the shadow ray, aims at a point on the bulb.
            glm::vec3 target = lamp.position;
            if (lamp.radius > 1e-4f) {
                glm::vec3 t, b;
                basisFrom(d, t, b);
                const glm::vec2 disk = sampleDisk(rng) * lamp.radius;
                target += t * disk.x + b * disk.y;
            }
            glm::vec3 sd = target - P;
            const float sdist = glm::length(sd);
            if (sdist < 1e-4f) continue;
            sd /= sdist;

            Ray sr;
            sr.o = P + N * kRayEps;
            sr.d = sd;
            sr.prepare();
            const glm::vec3 tr = shadowRay(sr, sdist - kRayEps);
            if (luminance(tr) < 1e-4f) continue;
            L += lamp.color * att * tr * evalBsdf(wide, N, V, d, specScale) * NoL;
        }
        return L;
    }

    // lit.frag's applyFog(), on the primary ray. Same reasoning as the tonemap:
    // a render of a foggy scene that came back clear would not read as the same
    // place, however correct the light in it was.
    glm::vec3 applyFog(const glm::vec3& color, const glm::vec3& eye,
                       const glm::vec3& dir, float dist) const {
        const FogDesc& f = sc.fog;
        if (f.density <= 0.0f) return color;
        const float b = f.heightFalloff;
        const float c = f.density * std::exp(-(eye.y - f.height) * b);
        float od;
        if (std::fabs(dir.y) > 1e-4f) od = c * (1.0f - std::exp(-b * dir.y * dist)) / (b * dir.y);
        else                          od = c * dist;
        const float fog = 1.0f - std::exp(-std::max(od, 0.0f));
        const float sunAmt = std::pow(std::max(0.0f, glm::dot(dir, glm::normalize(sc.sun.direction))), 4.0f);
        const glm::vec3 fogCol = glm::mix(f.color, f.sunColor, sunAmt);
        return glm::mix(color, fogCol, glm::clamp(fog, 0.0f, 1.0f));
    }

    // The diagnostic modes. One camera ray, no lighting, no tonemap: whatever
    // comes back is the raw value of one stage of the pipeline, shown as colour.
    glm::vec3 probe(Ray ray, Show mode) const {
        // Through what the eye would see through -- a leaf card's holes, a glow
        // -- or every cut-out tree reads as a stack of white quads.
        const glm::vec3 eye0 = ray.o;
        Hit hit;
        HitInfo hi;
        const Material* found = nullptr;
        for (int pass = 0; pass < 64; ++pass) {
            ray.prepare();
            if (!bvh.closest(ray, kInf, hit)) return glm::vec3(0.0f);
            hi = resolve(hit);
            const Material& m = sc.materials[static_cast<std::size_t>(hi.material)];
            const bool hole = m.additive ||
                              (m.alphaMode == 1 && texAlphaAt(m, hi.uv) < m.alphaCutoff);
            if (!hole) { found = &m; break; }
            ray.o = ray.o + ray.d * (hit.t + kRayEps);
        }
        if (!found) return glm::vec3(0.0f);
        const Material& mat = *found;

        if (mode == Show::BaseColor) {
            // As AUTHORED, before the sRGB->linear step, so the image is
            // literally the colour the texture holds and the inspector shows.
            // Linearising here would make every diagnostic look too dark and
            // start a second hunt.
            glm::vec3 terrain;
            if (terrainColorAt(mat, ray.o + ray.d * hit.t, hi.N, paintAt(hit), terrain))
                return terrain;
            if (mat.farGround)   // worked out in linear; shown as authored colours are
                return glm::pow(farGroundAlbedo(sc.ground, ray.o + ray.d * hit.t, hi.N, hi.uv.x),
                                glm::vec3(1.0f / 2.2f));
            return baseAt(mat, hi.uv);
        }
        if (mode == Show::Normal) return hi.N * 0.5f + 0.5f;
        // Depth, on a soft ramp so both a wing mirror and a distant hill land
        // somewhere readable rather than at the ends.
        const float t = glm::distance(eye0, ray.o + ray.d * hit.t);
        const float d = t / (t + 20.0f);
        return glm::vec3(d);
    }

    glm::vec3 radiance(Ray ray, Rng& rng) const {
        glm::vec3 L(0.0f), beta(1.0f);
        // Whether the first surface is the far terrain's ground, which takes
        // the air's colour with distance as farterrain.frag's applyAir gives it.
        bool primaryFar = false;
        glm::vec3 primaryPos(0.0f);
        // Distance to the first SURFACE. It stays negative when the camera ray
        // reaches the sky, because the sky must not be fogged: lit.frag hazes a
        // fragment by its own depth, and the sky is drawn by a different shader
        // that never sees the fog at all. Fogging it here turned every horizon
        // into a flat wall of fog colour.
        float primaryDist = -1.0f;
        const glm::vec3 eye = ray.o;
        const glm::vec3 eyeDir = ray.d;

        // Counts crossings of transparent surfaces separately from real
        // bounces: a ray passing through five panes of glass has scattered
        // once, and charging it five bounces would darken every window.
        int passthrough = 0;

        // The density this ray was sampled with, and whether that sampler was a
        // delta (the camera, a mirror, a pane of glass). Both are here for the
        // sun: an escaping ray that lands on the disc has to know how likely it
        // was to do so, or it double-counts what the shadow ray already found.
        float lastPdf   = 0.0f;
        bool  lastDelta = true;

        // The water body the path is inside, if any: its extinction and the
        // lit colour it fills in with. Set when a ray refracts INTO a glass
        // surface that has a body, cleared when it refracts back out.
        glm::vec3 sigma(0.0f), glow(0.0f);

        for (int bounce = 0; bounce <= maxBounces; ++bounce) {
            ray.prepare();
            Hit hit;
            const bool found = bvh.closest(ray, kInf, hit);
            throughMedium(sigma, glow, found ? hit.t : kInf, beta, L, bounce);
            if (!found) {
                // Straight from the camera, or off a mirror or through glass,
                // a ray sees what the viewport shows there: the captured sky
                // where there is one (see Environment::backdrop). Everything
                // that scattered sees the light the scene is lit by.
                glm::vec3 sky = lastDelta && useBackdrop
                              ? sc.env.sampleSeen(ray.d)
                              : sc.env.sample(ray.d) * sc.env.intensity;
                // The sky is now also aimed at by the light sampler, so a ray
                // that arrives here by scattering only claims its share --
                // except off a delta lobe or straight from the camera, where
                // the light sampler never had a chance.
                if (!lastDelta && envDist.valid())
                    sky *= misWeight(lastPdf, envDist.pdfFor(ray.d));
                if (sunOn && sunPdf > 0.0f &&
                    glm::dot(ray.d, sunAxis) > sunCosMax) {
                    // Straight from the camera, or off a mirror, the disc is
                    // simply what is there. Off a rough surface it shares the
                    // find with the shadow ray.
                    sky += sunRadiance * (lastDelta ? 1.0f
                                                    : misWeight(lastPdf, sunPdf));
                }
                L += clamped(beta * sky, bounce);
                break;
            }
            const HitInfo hi = resolve(hit);
            const Material& mat = sc.materials[static_cast<std::size_t>(hi.material)];
            const glm::vec3 P = ray.o + ray.d * hit.t;

            glm::vec3 N = hi.N;
            const glm::vec3 gN = N;
            if (glm::dot(N, -ray.d) < 0.0f) N = -N; // shade the side we can see

            float texA = 1.0f;
            // gN, not N: the slope a layer band tests is the ground's, not the
            // one turned to face the eye.
            const Surface s = surfaceAt(mat, hi.uv, P, gN, paintAt(hit), texA);

            // A glow: its light is added and the ray carries on through it, as
            // the additive blend the viewport draws it with does. Not a bounce.
            //
            // SEEN, never lit by -- only a ray straight from the eye (or off a
            // mirror, through glass) collects it, like the backdrop. The
            // viewport's sparks light nothing around them, and a cloud of tiny
            // bright emitters found by chance from every diffuse bounce is the
            // purest firefly generator a scene can contain.
            if (mat.additive) {
                if (lastDelta) L += clamped(beta * s.emission * s.coverage, bounce);
                if (++passthrough > 64) break;
                ray.o = P + ray.d * kRayEps;
                --bounce;
                continue;
            }

            // Cutout and blended surfaces: decide whether this ray sees the
            // surface at all before doing any shading work.
            const bool cutoutMiss = mat.alphaMode == 1 && texA < mat.alphaCutoff;
            // Only a BLEND surface is passed through, and only by its own
            // coverage. An opaque one is solid whatever its map's alpha
            // channel happens to hold.
            const bool blendMiss  = !mat.glass && mat.alphaMode == 2 &&
                                    s.coverage < 0.999f &&
                                    rng.uniform() > s.coverage;
            if (cutoutMiss || blendMiss) {
                if (++passthrough > 64) break;
                ray.o = P + ray.d * kRayEps;
                --bounce; // a pane is not a bounce
                continue;
            }
            // Measured from the eye, not from wherever the ray was last
            // restarted: a leaf hole or a spark in front of a surface must not
            // make that surface read as nearer, and so less fogged, than it is.
            if (primaryDist < 0.0f) {
                primaryDist = glm::distance(eye, P);
                primaryFar  = mat.farGround;
                primaryPos  = P;
            }

            L += clamped(beta * s.emission, bounce);

            const glm::vec3 V = -ray.d;

            if (mat.glass) {
                // A dielectric: reflect or refract, chosen by Fresnel. No next
                // event estimation -- the lobe is a delta and a shadow ray at a
                // mirror direction would contribute nothing but noise.
                const bool  entering = glm::dot(gN, ray.d) < 0.0f;
                const float n   = std::max(1.0f, mat.ior);
                const float ior = entering ? (1.0f / n) : n;
                const float cosI = std::min(1.0f, glm::dot(N, V));
                const float sinT2 = ior * ior * (1.0f - cosI * cosI);
                float F;
                if (sinT2 > 1.0f) {
                    F = 1.0f; // total internal reflection
                } else {
                    const float cosT = std::sqrt(1.0f - sinT2);
                    const float rs = (ior * cosI - cosT) / (ior * cosI + cosT);
                    const float rp = (cosI - ior * cosT) / (cosI + ior * cosT);
                    F = 0.5f * (rs * rs + rp * rp);
                }
                if (rng.uniform() < F) {
                    ray.d = glm::reflect(ray.d, N);
                } else {
                    const glm::vec3 refr = glm::refract(ray.d, N, ior);
                    if (glm::dot(refr, refr) < 1e-8f) {
                        ray.d = glm::reflect(ray.d, N);
                    } else {
                        ray.d = refr;
                        beta *= s.baseColor; // the tint the pane carries
                        // Through the surface: into its body, or back out.
                        if (entering) {
                            sigma = glm::max(mat.absorption, glm::vec3(0.0f));
                            glow  = glm::pow(glm::max(mat.mediumColor, glm::vec3(0.0f)),
                                             glm::vec3(2.2f)) * mediumGlow;
                        } else {
                            sigma = glow = glm::vec3(0.0f);
                        }
                    }
                }
                ray.o = P + ray.d * kRayEps;
                lastDelta = true;
                continue;
            }

            // Chosen before the light sampling, not after: next event
            // estimation has to weigh itself against the SAME densities the
            // scatter below will use, or the two strategies do not add up to one.
            const float pSpec = specProbability(s);
            L += clamped(beta * directLight(P, N, V, s, pSpec, rng, sigma), bounce);

            if (bounce == maxBounces) break;

            // Scatter. One lobe is chosen, but the weight divides by the pdf of
            // BOTH, which keeps the estimate unbiased whichever fired.
            glm::vec3 nextDir;
            if (rng.uniform() < pSpec) {
                glm::vec3 t, b;
                basisFrom(N, t, b);
                const glm::vec3 Vl(glm::dot(V, t), glm::dot(V, b), glm::dot(V, N));
                if (Vl.z <= 0.0f) break;
                const glm::vec3 Hl = sampleGGXVNDF(Vl, s.alpha, rng.uniform(), rng.uniform());
                const glm::vec3 H  = glm::normalize(t * Hl.x + b * Hl.y + N * Hl.z);
                nextDir = glm::reflect(ray.d, H);
            } else {
                // The diffuse lobe covers the whole sphere on a translucent
                // surface: `translucency` of the time the path carries on out
                // the far side, which is how light gets through a canopy at all.
                const bool through = s.translucency > 0.0f &&
                                     rng.uniform() < s.translucency;
                nextDir = cosineHemisphere(through ? -N : N, rng);
            }
            const float NoL = glm::dot(N, nextDir);
            if (std::fabs(NoL) <= 0.0f) break;

            const float pdf = pdfBsdf(s, N, V, nextDir, pSpec);
            if (pdf < 1e-6f) break;
            beta *= evalBsdf(s, N, V, nextDir) * (std::fabs(NoL) / pdf);
            if (luminance(beta) < 1e-5f) break;
            lastPdf   = pdf;
            lastDelta = false;

            // Russian roulette, once the path has earned the right to be cut.
            // Started at bounce 2 rather than 0 so that the first bounces --
            // the ones carrying nearly all the visible light -- are never the
            // ones thrown away.
            if (bounce >= 2) {
                const float q = std::min(0.95f, std::max(beta.r, std::max(beta.g, beta.b)));
                if (rng.uniform() > q) break;
                beta /= q;
            }

            ray.o = P + (glm::dot(N, nextDir) < 0.0f ? -N : N) * kRayEps;
            ray.d = nextDir;
        }

        // The same guard lit.frag ends with, and for the same reason. A single
        // non-finite sample is not one bad pixel: it is added into a tile's
        // running sum, and from then on every sample that tile ever takes is
        // averaged with a NaN. One firefly becomes a permanent hole.
        if (!(L.x == L.x) || !(L.y == L.y) || !(L.z == L.z)) return glm::vec3(0.0f);
        L = glm::min(L, glm::vec3(50000.0f));
        if (primaryDist > 0.0f) L = applyFog(L, eye, eyeDir, primaryDist);
        if (primaryFar) L = applyAir(L, eye, primaryPos);
        if (!(L.x == L.x) || !(L.y == L.y) || !(L.z == L.z)) return glm::vec3(0.0f);
        return L;
    }

    // farterrain.frag's applyAir: the far ground fades towards the colour of the
    // air in front of it, by an exponential atmosphere 1.6 km deep with 17 km to
    // 1/e at sea level. The air's colour is the sky's own gradient just above
    // that bearing (skyGradient, as the far terrain's skyAir), plus the sun's
    // forward scatter.
    glm::vec3 applyAir(const glm::vec3& color, const glm::vec3& eye, const glm::vec3& p) const {
        const glm::vec3 to = p - eye;
        const float dist = glm::length(to);
        if (dist < 1e-3f) return color;
        const glm::vec3 rd = to / dist;
        constexpr float Hs = 1600.0f, Lair = 17000.0f;
        const float y0 = std::max(eye.y, 0.0f), y1 = std::max(p.y, 0.0f);
        const float avgD = std::fabs(y1 - y0) > 1.0f
                         ? Hs * (std::exp(-y0 / Hs) - std::exp(-y1 / Hs)) / (y1 - y0)
                         : std::exp(-y0 / Hs);
        const float a = 1.0f - std::exp(-dist / Lair * avgD);
        glm::vec3 air = skyGradient(glm::normalize(glm::vec3(rd.x, std::max(rd.y, 0.015f), rd.z)),
                                    sc.sun.direction);
        air += sc.ground.airSun * std::pow(std::max(glm::dot(rd, sunAxis), 0.0f), 8.0f) * 0.18f;
        return glm::mix(color, air, a);
    }
};

} // namespace

// --- Image / environment / tonemap ------------------------------------------

glm::vec4 Image::sample(float u, float v) const {
    if (!valid()) return glm::vec4(1.0f);

    // Wrap, then bilinear. Wrapping rather than clamping because that is what
    // the GL textures do, and a tiled road or terrain map read with clamped
    // edges shows a stretched border exactly where the tile repeats.
    float x = u * static_cast<float>(width)  - 0.5f;
    float y = v * static_cast<float>(height) - 0.5f;
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);

    auto wrap = [](int a, int n) {
        const int m = a % n;
        return std::clamp(m < 0 ? m + n : m, 0, n - 1);
    };
    const int xs[2] = {wrap(x0, width),  wrap(x0 + 1, width)};
    const int ys[2] = {wrap(y0, height), wrap(y0 + 1, height)};

    glm::vec4 c(0.0f);
    const float wx[2] = {1.0f - fx, fx};
    const float wy[2] = {1.0f - fy, fy};
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            const std::size_t o = (static_cast<std::size_t>(ys[j]) * width + xs[i]) * 4;
            c += glm::vec4(pixels[o], pixels[o + 1], pixels[o + 2], pixels[o + 3]) *
                 (wx[i] * wy[j]);
        }
    return c * (1.0f / 255.0f);
}

namespace {

// One equirectangular panorama, looked up. Shared by the lighting map and the
// backdrop so the two can never disagree about which way is up.
glm::vec3 samplePanorama(const std::vector<float>& pixels, int width, int height,
                         const glm::vec3& dir) {
    if (!std::isfinite(dir.x) || !std::isfinite(dir.y) || !std::isfinite(dir.z))
        return glm::vec3(0.0f);
    // Exactly EnvironmentIBL's sampleSpherical(): atan2(z, x) across,
    // asin(y) down, both scaled and biased by a half.
    //
    // asin and not acos, and this is not a nicety. The loader hands the
    // panorama over BOTTOM-UP (it flips .hdr on load and flips .exr by
    // hand), so row 0 is the ground and the last row is the zenith, which
    // is what the GL mapping's v = asin(y)/pi + 0.5 expects. acos gives
    // exactly the mirror of that -- a sky rendered with the ground's colours
    // above the horizon and the sky's below it.
    const float u = std::atan2(dir.z, dir.x) / (2.0f * kPi) + 0.5f;
    const float v = std::asin(glm::clamp(dir.y, -1.0f, 1.0f)) / kPi + 0.5f;

    // Bilinear, and not a refinement: a 4K panorama shown as the background
    // of a 1080p still is being minified about four to one, and picking one
    // texel out of every sixteen turns a corrugated roof or a row of railings
    // into moire. It reads as the map having the wrong colours -- which is
    // exactly how it was described -- because a stripe pattern sampled off
    // its own frequency comes back as a flat wrong shade rather than as
    // something recognisably aliased.
    //
    // Wrapped across, clamped down: a panorama joins itself at the seam
    // behind the camera, and does not join itself at the poles.
    const float fx = u * static_cast<float>(width)  - 0.5f;
    const float fy = v * static_cast<float>(height) - 0.5f;
    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);

    // Clamped after the wrap: a non-finite direction arrives here as INT_MIN,
    // and the remainder of that is not something to index with.
    auto wrapX = [width](int a) {
        const int m = a % width;
        return std::clamp(m < 0 ? m + width : m, 0, width - 1);
    };
    const int xs[2] = {wrapX(x0), wrapX(x0 + 1)};
    const int ys[2] = {std::clamp(y0,     0, height - 1),
                       std::clamp(y0 + 1, 0, height - 1)};
    const float wx[2] = {1.0f - tx, tx};
    const float wy[2] = {1.0f - ty, ty};

    glm::vec3 c(0.0f);
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            const std::size_t o =
                (static_cast<std::size_t>(ys[j]) * width + xs[i]) * 3;
            c += glm::vec3(pixels[o], pixels[o + 1], pixels[o + 2]) *
                 (wx[i] * wy[j]);
        }
    return c;
}

} // namespace

glm::vec3 Environment::sampleSeen(const glm::vec3& dir) const {
    if (hasBackdrop()) return samplePanorama(backdrop, backdropWidth, backdropHeight, dir);
    return sample(dir) * intensity;
}

glm::vec3 Environment::sample(const glm::vec3& dir) const {
    if (hasMap()) return samplePanorama(pixels, width, height, dir);
    // No panorama: the flat ambient the raster path would have used, spread
    // over a horizon so that a surface facing up and one facing down are not
    // lit identically. Not a sky model, and not pretending to be one.
    const float t = glm::clamp(dir.y, -1.0f, 1.0f);
    if (t >= 0.0f) return glm::mix(horizon, zenith, std::sqrt(t));
    return glm::mix(horizon, ground, std::sqrt(-t));
}

std::vector<ProbeSh> bakeProbes(const Scene& scene,
                                const std::vector<glm::vec3>& points,
                                const BakeSettings& settings,
                                const std::function<bool(float)>& progress) {
    std::vector<ProbeSh> out(points.size());
    if (points.empty()) return out;

    Accel bvh;
    bvh.build(scene);
    EnvSampler envDist;
    envDist.build(scene.env);

    const int rays = std::max(8, settings.rays);
    int threads = settings.threads > 0
                ? settings.threads
                : static_cast<int>(std::thread::hardware_concurrency()) - 1;
    threads = std::clamp(threads, 1, 64);

    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> done{0};
    std::atomic<bool>        stop{false};

    auto worker = [&]() {
        Tracer tracer(scene, bvh, envDist, settings.maxBounces, 0.0f);
        tracer.sunOn       = settings.includeSun;
        tracer.useBackdrop = false;  // a probe is not an eye
        for (;;) {
            const std::size_t i = next.fetch_add(1);
            if (i >= points.size() || stop.load()) return;

            Rng rng(i, static_cast<std::uint64_t>(settings.seed) * 6151u + 17u);
            const glm::vec3 P = points[i];

            // The projection integral, taken by Monte Carlo over the whole
            // sphere. Uniform directions rather than cosine-weighted ones
            // because a probe has no normal -- it has to answer for every
            // normal a surface near it might have.
            glm::vec3 c00(0.0f), c1m1(0.0f), c10(0.0f), c11(0.0f);
            int backfaces = 0;
            for (int r = 0; r < rays; ++r) {
                const float u1 = rng.uniform(), u2 = rng.uniform();
                const float z   = 1.0f - 2.0f * u1;
                const float rad = std::sqrt(std::max(0.0f, 1.0f - z * z));
                const float phi = 2.0f * kPi * u2;
                const glm::vec3 d(rad * std::cos(phi), z, rad * std::sin(phi));

                // Is this probe buried? A ray leaving a solid object meets the
                // INSIDE of its shell, and a probe whose rays mostly do that
                // has no sky to record.
                Ray probe;
                probe.o = P;
                probe.d = d;
                probe.prepare();
                Hit hit;
                if (bvh.closest(probe, kInf, hit)) {
                    if (glm::dot(tracer.resolve(hit).faceN, d) > 0.0f) ++backfaces;
                }

                Ray ray;
                ray.o = P;
                ray.d = d;
                const glm::vec3 L = tracer.radiance(ray, rng);

                c00  += L * 0.282095f;
                c1m1 += L * (0.488603f * d.y);
                c10  += L * (0.488603f * d.z);
                c11  += L * (0.488603f * d.x);
            }

            // 4*pi/N is the solid angle each sample stands for.
            const float norm = 4.0f * kPi / static_cast<float>(rays);
            c00 *= norm; c1m1 *= norm; c10 *= norm; c11 *= norm;

            // Convolve with the cosine lobe and divide by pi, so what comes out
            // is what a Lambertian surface reflects per unit albedo -- the same
            // quantity the flat ambient colour is. The band-1 factor is
            // (2*pi/3)/pi = 2/3; the band-0 factor is pi/pi = 1.
            ProbeSh sh;
            sh.sh0 = c00 * 0.282095f;
            const float k1 = (2.0f / 3.0f) * 0.488603f;
            sh.shX = c11  * k1;
            sh.shY = c1m1 * k1;
            sh.shZ = c10  * k1;
            sh.valid = static_cast<float>(backfaces) /
                       static_cast<float>(rays) < 0.6f;

            // Baked-only lamps: their DIRECT light as well, since nothing else
            // will draw it. Falloff as the lit shader's point lights, (1 - d/r)^2,
            // and what a surface facing the lamp gets matches that shader's
            // colour * falloff * cos. A light from one direction in the L1 basis
            // is the clamped cosine's projection: 1/4 constant + 1/2 along the
            // direction (full facing it, a quarter side-on, none behind).
            for (const Lamp& lamp : scene.lamps) {
                if (!lamp.bakeDirect) continue;
                const glm::vec3 toL = lamp.position - P;
                const float dist = glm::length(toL);
                if (dist < 1e-3f || dist >= lamp.range) continue;
                float att = 1.0f - dist / lamp.range;
                att *= att;
                const glm::vec3 w = toL / dist;
                Ray shadow;
                shadow.o = P;
                shadow.d = w;
                shadow.prepare();
                Hit blocker;
                if (bvh.closest(shadow, dist - 0.05f, blocker)) continue;
                const glm::vec3 V = lamp.color * att;
                sh.sh0 += V * 0.25f;
                sh.shX += V * (0.5f * w.x);
                sh.shY += V * (0.5f * w.y);
                sh.shZ += V * (0.5f * w.z);
            }
            out[i] = sh;

            done.fetch_add(1);
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(threads));
    for (int t = 0; t < threads; ++t) pool.emplace_back(worker);

    if (progress) {
        while (done.load() < points.size() && !stop.load()) {
            const float p = static_cast<float>(done.load()) /
                            static_cast<float>(points.size());
            if (!progress(p)) stop.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
    }
    for (std::thread& t : pool) t.join();
    if (progress) progress(1.0f);
    return out;
}

float firstHitDistance(const Scene& scene, const glm::vec3& origin,
                       const glm::vec3& dir) {
    Ray r;
    r.o = origin;
    r.d = glm::normalize(dir);
    float best = kInf;
    for (const Triangle& tri : scene.triangles) {
        float t, u, v;
        if (intersectTri(tri, r, best, t, u, v)) best = t;
    }
    for (const Instance& in : scene.instances) {
        if (in.mesh < 0 || in.mesh >= static_cast<int>(scene.meshes.size())) continue;
        if (std::fabs(glm::determinant(glm::mat3(in.transform))) < 1e-12f) continue;
        const glm::mat4 inv = glm::inverse(in.transform);
        Ray lr;
        lr.o = glm::vec3(inv * glm::vec4(r.o, 1.0f));
        lr.d = glm::mat3(inv) * r.d;
        for (const Triangle& tri : scene.meshes[static_cast<std::size_t>(in.mesh)].triangles) {
            float t, u, v;
            if (intersectTri(tri, lr, best, t, u, v)) best = t;
        }
    }
    return best < kInf ? best : 0.0f;
}

namespace {
// meadow.glsl's value noise (a sin hash, not ecology's integer one).
float meadowHash(glm::vec2 p) {
    const float v = std::sin(glm::dot(p, glm::vec2(127.1f, 311.7f))) * 43758.5453f;
    return v - std::floor(v);
}
float meadowNoise(glm::vec2 p) {
    const glm::vec2 i = glm::floor(p);
    glm::vec2 f = p - i;
    f = f * f * (3.0f - 2.0f * f);
    const float a = meadowHash(i), b = meadowHash(i + glm::vec2(1, 0));
    const float c = meadowHash(i + glm::vec2(0, 1)), d = meadowHash(i + glm::vec2(1, 1));
    return glm::mix(glm::mix(a, b, f.x), glm::mix(c, d, f.x), f.y);
}
// ecology.glsl's integer-hash noise -- uint arithmetic wraps the same on both
// sides, so the stands land exactly where the forest field put the trees.
std::uint32_t ecoHash(std::int32_t x, std::int32_t z, std::uint32_t seed) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 0x8da6b343u ^
                      static_cast<std::uint32_t>(z) * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 13u;
    h *= 0x5bd1e995u;
    h ^= h >> 15u;
    return h;
}
float ecoNoise(glm::vec2 p, std::uint32_t seed) {
    const float fx = std::floor(p.x), fz = std::floor(p.y);
    const std::int32_t ix = static_cast<std::int32_t>(fx), iz = static_cast<std::int32_t>(fz);
    float tx = p.x - fx, tz = p.y - fz;
    tx = tx * tx * (3.0f - 2.0f * tx);
    tz = tz * tz * (3.0f - 2.0f * tz);
    const float k = 1.0f / 16777216.0f;
    const float a = static_cast<float>(ecoHash(ix, iz, seed) & 0xffffffu) * k;
    const float b = static_cast<float>(ecoHash(ix + 1, iz, seed) & 0xffffffu) * k;
    const float c = static_cast<float>(ecoHash(ix, iz + 1, seed) & 0xffffffu) * k;
    const float d = static_cast<float>(ecoHash(ix + 1, iz + 1, seed) & 0xffffffu) * k;
    const float ab = a + (b - a) * tx, cd = c + (d - c) * tx;
    return ab + (cd - ab) * tz;
}
float ecoFbm(glm::vec2 p, int octaves, std::uint32_t seed) {
    float sum = 0.0f, amp = 0.5f, norm = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        sum  += amp * ecoNoise(p, seed + static_cast<std::uint32_t>(i) * 101u);
        norm += amp;
        p     = p * 2.03f + glm::vec2(17.3f, -9.1f);
        amp  *= 0.5f;
    }
    return sum / norm;
}
} // namespace

glm::vec3 meadowColour(glm::vec2 xz, float lush) {
    // Taken at full detail: the tracer supersamples every pixel, so the noise
    // the shader fades out past a pixel's footprint is averaged here instead.
    const float meadow = meadowNoise(xz * 0.05f);
    const float hueN   = meadowNoise(xz * 0.11f + 31.0f);
    const float shade  = meadowNoise(xz * 0.035f + 5.0f);
    const float green  = glm::clamp(lush - (1.0f - meadow) * 0.55f - 0.125f - 0.04f, 0.0f, 1.0f);
    const glm::vec3 dryBase(0.16f, 0.14f, 0.06f), dryTip(0.50f, 0.45f, 0.22f);
    const glm::vec3 lushBase(0.05f, 0.13f, 0.04f);
    const glm::vec3 lushTip = glm::mix(glm::vec3(0.14f, 0.36f, 0.13f),
                                       glm::vec3(0.44f, 0.54f, 0.16f), hueN * hueN);
    const float bright = (0.55f + 0.55f * shade) * 0.98f * glm::mix(0.85f, 1.05f, lush);
    const glm::vec3 base = glm::mix(dryBase, lushBase, green) * bright;
    const glm::vec3 tip  = glm::mix(dryTip, lushTip, green) * bright;
    return glm::mix(base, tip, 0.6f);
}

float meadowCover(glm::vec2 xz, float h, float ny, float waterLevel, float top) {
    const float c = glm::smoothstep(0.80f, 0.86f, ny) *
                    glm::smoothstep(waterLevel + 0.3f, waterLevel + 0.9f, h) *
                    (1.0f - glm::smoothstep(top - 3.0f, top, h));
    const float bare = meadowNoise(xz * 0.13f + glm::vec2(19.0f, 7.0f));
    return c * glm::mix(0.5f, 1.0f, glm::smoothstep(0.20f, 0.32f, bare));
}

float woodsAt(const GroundLook& g, glm::vec2 xz, float ny) {
    // ecology.glsl's ecoSample(...).y: the stand, before the height and water
    // cuts -- which is what lit.frag's forest floor keys off.
    const float stands = ecoFbm(xz / g.ecoStandSize, 4, 0x51edu);
    const float ragged = ecoFbm(xz / (g.ecoStandSize * 0.18f), 2, 0x3a7cu);
    const float f   = stands + 0.22f * (ragged - 0.5f) + g.ecoSlopeLove * (1.0f - ny);
    const float cut = 0.5f + (0.5f - g.ecoCover) * 0.45f;
    return glm::smoothstep(cut - 0.035f, cut + 0.035f, f);
}

namespace {
// farterrain.frag's own value noise (its hash is not lit.frag's).
float farHash21(glm::vec2 p) {
    p = glm::fract(p * glm::vec2(123.34f, 456.21f));
    p += glm::dot(p, p + 45.32f);
    return glm::fract(p.x * p.y);
}
float farNoise(glm::vec2 p) {
    const glm::vec2 i = glm::floor(p);
    glm::vec2 f = p - i;
    f = f * f * (3.0f - 2.0f * f);
    const float a = farHash21(i), b = farHash21(i + glm::vec2(1, 0));
    const float c = farHash21(i + glm::vec2(0, 1)), d = farHash21(i + glm::vec2(1, 1));
    return glm::mix(glm::mix(a, b, f.x), glm::mix(c, d, f.x), f.y);
}
// fbmPx at a footprint of zero: every octave, as a supersampled pixel wants.
float farFbm(glm::vec2 p, float scale) {
    float s = 0.0f, a = 0.5f, f = 1.0f / scale;
    for (int i = 0; i < 5; ++i) {
        s += a * (farNoise(p * f) - 0.5f);
        f *= 2.07f;
        a *= 0.5f;
    }
    return s;
}
glm::vec3 lin(float r, float g, float b) {
    return glm::pow(glm::vec3(r, g, b), glm::vec3(2.2f));
}
// ecology.glsl's ecoSample(...).x: trees per cell, with the slope, tree-line and
// water cuts -- what the far terrain paints its canopy by.
float ecoDensity(const GroundLook& g, glm::vec2 xz, float h, float ny) {
    const float stands = ecoFbm(xz / g.ecoStandSize, 4, 0x51edu);
    const float ragged = ecoFbm(xz / (g.ecoStandSize * 0.18f), 2, 0x3a7cu);
    const float f      = stands + 0.22f * (ragged - 0.5f) + g.ecoSlopeLove * (1.0f - ny);
    const float cut    = 0.5f + (0.5f - g.ecoCover) * 0.45f;
    const float forest = glm::smoothstep(cut - 0.035f, cut + 0.035f, f);
    float d = std::max(forest, g.ecoSolitary);
    d *= glm::smoothstep(0.62f, 0.72f, ny);
    const float tl = g.ecoTreeLine + (ragged - 0.5f) * 180.0f;
    d *= 1.0f - glm::smoothstep(tl - 140.0f, tl + 30.0f, h);
    d *= glm::smoothstep(g.ecoWater + 0.6f, g.ecoWater + 1.6f, h);
    return d;
}
} // namespace

glm::vec3 farGroundAlbedo(const GroundLook& g, const glm::vec3& wp, const glm::vec3& n,
                          float moist) {
    const glm::vec2 xz(wp.x, wp.z);
    const float h = wp.y;
    const float slope = glm::degrees(std::acos(glm::clamp(n.y, -1.0f, 1.0f)));
    const float n1 = farFbm(xz, 190.0f) + 0.5f;
    const float n2 = farFbm(xz + 71.0f, 45.0f) + 0.5f;
    const float n3 = farFbm(xz - 233.0f, 900.0f) + 0.5f;

    const glm::vec3 meadow = glm::pow(meadowColour(xz, moist), glm::vec3(2.2f)) * g.grassTint;
    const glm::vec3 alpine = glm::mix(lin(0.33f, 0.40f, 0.19f), lin(0.45f, 0.43f, 0.24f), n2);
    glm::vec3 forest = glm::mix(lin(0.085f, 0.13f, 0.07f), lin(0.12f, 0.15f, 0.08f), n2) *
                       (0.8f + 0.4f * (farFbm(xz, 14.0f) + 0.5f));
    const glm::vec3 rockCool = glm::mix(lin(0.36f, 0.36f, 0.37f), lin(0.50f, 0.50f, 0.50f), n1);
    const glm::vec3 rockWarm = glm::mix(lin(0.44f, 0.39f, 0.33f), lin(0.58f, 0.53f, 0.45f), n1);
    glm::vec3 rock = glm::mix(rockCool, rockWarm, glm::smoothstep(0.35f, 0.65f, n3));
    const float bed = h * 0.028f + n2 * 5.0f + n1 * 3.0f + glm::dot(xz, glm::vec2(0.0011f, -0.0007f));
    rock *= 0.9f + 0.14f * glm::smoothstep(0.35f, 0.65f, farNoise(glm::vec2(bed, n3 * 3.0f)));
    const glm::vec3 scree = glm::mix(lin(0.52f, 0.50f, 0.46f), lin(0.62f, 0.59f, 0.54f), n2);
    const glm::vec3 snow  = lin(0.93f, 0.95f, 0.98f);

    glm::vec3 albedo = meadow;
    const float alpineAmt = glm::smoothstep(g.farTreeLine - 60.0f, g.farTreeLine + 120.0f,
                                            h + (n1 - 0.5f) * 220.0f);
    albedo = glm::mix(albedo, alpine, alpineAmt);
    float forestAmt;
    if (g.ecoOn) {
        forestAmt = glm::clamp(ecoDensity(g, xz, h, n.y) * 1.3f, 0.0f, 1.0f);
        forest = g.canopy * 0.75f * (0.75f + 0.5f * (farFbm(xz, 14.0f) + 0.5f)) *
                 glm::mix(0.85f, 1.1f, n2);
    } else {
        const float woods = glm::smoothstep(0.28f, 0.5f, moist + (n2 - 0.5f) * 0.35f);
        const float valleyPatch = glm::mix(glm::smoothstep(0.42f, 0.62f, n1 * 0.6f + n3 * 0.6f),
                                           1.0f, glm::smoothstep(40.0f, 180.0f, h));
        forestAmt = woods * valleyPatch * (1.0f - alpineAmt) *
                    (1.0f - glm::smoothstep(30.0f, 40.0f, slope));
    }
    albedo = glm::mix(albedo, forest, forestAmt);
    const float screeAmt = glm::smoothstep(24.0f, 32.0f, slope + (n1 - 0.5f) * 10.0f) *
                           (1.0f - forestAmt) *
                           glm::smoothstep(g.farTreeLine - 200.0f, g.farTreeLine + 100.0f, h);
    albedo = glm::mix(albedo, scree, screeAmt * 0.8f);
    const float rockSlope = glm::mix(44.0f, 32.0f, alpineAmt);
    float rockAmt = glm::smoothstep(rockSlope, rockSlope + 12.0f, slope + (n2 - 0.5f) * 16.0f);
    rockAmt = std::max(rockAmt, alpineAmt * glm::smoothstep(0.55f, 0.8f, n1) *
                                    glm::smoothstep(g.farTreeLine + 150.0f, g.farTreeLine + 500.0f, h));
    albedo = glm::mix(albedo, rock, rockAmt);
    const float snowLine = g.farSnowLevel + (n3 - 0.5f) * 320.0f + (n1 - 0.5f) * 140.0f;
    float snowAmt = glm::smoothstep(snowLine - 50.0f, snowLine + 50.0f, h) *
                    (1.0f - glm::smoothstep(36.0f, 50.0f, slope + (n2 - 0.5f) * 12.0f));
    snowAmt = std::max(snowAmt, glm::smoothstep(snowLine + 350.0f, snowLine + 700.0f, h) *
                                    (1.0f - glm::smoothstep(55.0f, 68.0f, slope)));
    albedo = glm::mix(albedo, snow, snowAmt);
    albedo *= glm::mix(1.0f, 0.55f, 1.0f - glm::smoothstep(g.waterLevel, g.waterLevel + 1.5f, h));
    return albedo;
}

glm::vec3 skyGradient(const glm::vec3& dir, const glm::vec3& sunDirIn) {
    const glm::vec3 sunDir = glm::normalize(sunDirIn);
    const float day = glm::smoothstep(-0.12f, 0.18f, sunDir.y);
    const float h = glm::clamp(dir.y, 0.0f, 1.0f);
    glm::vec3 zenith  = glm::mix(glm::vec3(0.01f, 0.02f, 0.06f), glm::vec3(0.20f, 0.42f, 0.80f), day);
    glm::vec3 horizon = glm::mix(glm::vec3(0.04f, 0.06f, 0.12f), glm::vec3(0.70f, 0.82f, 0.95f), day);
    const float lowSun = (1.0f - glm::smoothstep(0.0f, 0.35f, sunDir.y)) * day;
    const float gold   = (1.0f - glm::smoothstep(0.0f, 0.35f, sunDir.y)) *
                         glm::smoothstep(-0.10f, 0.0f, sunDir.y);
    const glm::vec3 flatDir = glm::normalize(glm::vec3(dir.x, 0.0f, dir.z) + 1e-5f);
    const glm::vec3 flatSun = glm::normalize(glm::vec3(sunDir.x, 0.0f, sunDir.z) + 1e-5f);
    const float sunSide = std::pow(0.5f + 0.5f * glm::dot(flatDir, flatSun), 5.0f);
    zenith  = glm::mix(zenith, glm::mix(glm::vec3(0.26f, 0.27f, 0.38f),
                                        glm::vec3(0.32f, 0.29f, 0.36f), sunSide), gold * 0.60f);
    horizon = glm::mix(horizon, glm::mix(glm::vec3(0.42f, 0.42f, 0.50f),
                                         glm::vec3(0.52f, 0.44f, 0.42f), sunSide), gold * 0.60f);
    glm::vec3 col = glm::mix(horizon, zenith, std::pow(h, 0.5f));
    const float toSun = std::max(glm::dot(flatDir, flatSun), 0.0f);
    col += glm::vec3(0.85f, 0.35f, 0.10f) * glm::mix(1.0f, 0.6f, gold) * lowSun *
           std::pow(toSun, 3.0f) * (1.0f - h);
    const float sd   = std::max(glm::dot(dir, sunDir), 0.0f);
    const float core = std::pow(sd, 24.0f);
    const float wide = std::pow(sd, 4.0f) * (1.0f - 0.5f * h);
    col += (glm::vec3(1.00f, 0.76f, 0.38f) * core * 0.55f +
            glm::vec3(0.95f, 0.55f, 0.30f) * wide * 0.06f) * gold;
    return glm::pow(glm::max(col, glm::vec3(0.0f)), glm::vec3(2.2f));
}

float GroundLook::lushAt(float x, float z) const {
    if (moistSamples <= 0 || moisture.size() < static_cast<std::size_t>(moistSamples) * moistSamples)
        return meadowLush;
    // lit.frag: muv = ((xz - origin) / cell + 0.5) / samples, sampled linearly.
    const float u = ((x - moistOrigin.x) / moistCell + 0.5f) / static_cast<float>(moistSamples);
    const float v = ((z - moistOrigin.y) / moistCell + 0.5f) / static_cast<float>(moistSamples);
    if (!(u > 0.0f && u < 1.0f && v > 0.0f && v < 1.0f)) return meadowLush;
    const float fx = u * moistSamples - 0.5f, fy = v * moistSamples - 0.5f;
    const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, moistSamples - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, moistSamples - 1);
    const int x1 = std::min(x0 + 1, moistSamples - 1), y1 = std::min(y0 + 1, moistSamples - 1);
    const float tx = glm::clamp(fx - static_cast<float>(x0), 0.0f, 1.0f);
    const float ty = glm::clamp(fy - static_cast<float>(y0), 0.0f, 1.0f);
    auto at = [&](int xx, int yy) {
        return moisture[static_cast<std::size_t>(yy) * moistSamples + xx];
    };
    return glm::mix(glm::mix(at(x0, y0), at(x1, y0), tx), glm::mix(at(x0, y1), at(x1, y1), tx), ty);
}

float SunMask::at(const glm::vec3& p) const {
    if (!valid() || sunDir.y < 0.03f) return 1.0f;
    // Along the sun to the map's plane, then into the map.
    const glm::vec2 q(p.x - sunDir.x * ((p.y - refY) / sunDir.y),
                      p.z - sunDir.z * ((p.y - refY) / sunDir.y));
    const glm::vec2 uv = (q - origin) / size;
    const glm::vec2 e  = glm::min(uv, glm::vec2(1.0f) - uv);
    const float t    = glm::clamp(std::min(e.x, e.y) / 0.12f, 0.0f, 1.0f);
    const float edge = t * t * (3.0f - 2.0f * t);
    if (edge <= 0.0f) return 1.0f;
    // Bilinear, clamped: GL_LINEAR with CLAMP_TO_EDGE, as the map is sampled.
    const float fx = glm::clamp(uv.x, 0.0f, 1.0f) * static_cast<float>(width)  - 0.5f;
    const float fy = glm::clamp(uv.y, 0.0f, 1.0f) * static_cast<float>(height) - 0.5f;
    const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0);
    auto v = [&](int x, int y) {
        x = std::clamp(x, 0, width - 1);
        y = std::clamp(y, 0, height - 1);
        return values[static_cast<std::size_t>(y) * width + x];
    };
    const float m = (v(x0, y0) * (1.0f - tx) + v(x0 + 1, y0) * tx) * (1.0f - ty) +
                    (v(x0, y0 + 1) * (1.0f - tx) + v(x0 + 1, y0 + 1) * tx) * ty;
    return 1.0f + (m - 1.0f) * edge;
}

glm::vec3 mediumLight(const Scene& scene) {
    // The sky's irradiance on a horizontal surface, integrated over a fixed
    // set of directions -- deterministic, so the CPU and the GPU are handed the
    // same number -- plus the sun's, all over pi: the radiance a white
    // Lambertian surface lying in the open would reflect.
    glm::vec3 sky(0.0f);
    constexpr int kTheta = 16, kPhi = 32;
    for (int i = 0; i < kTheta; ++i) {
        // Cosine-weighted rings: cos(theta) = sqrt(1 - u).
        const float u  = (static_cast<float>(i) + 0.5f) / kTheta;
        const float ct = std::sqrt(1.0f - u);
        const float st = std::sqrt(std::max(0.0f, 1.0f - ct * ct));
        for (int j = 0; j < kPhi; ++j) {
            const float phi = 2.0f * kPi * (static_cast<float>(j) + 0.5f) / kPhi;
            sky += scene.env.sample(glm::vec3(st * std::cos(phi), ct, st * std::sin(phi)));
        }
    }
    // With cosine-weighted directions the mean IS irradiance / pi.
    glm::vec3 e = sky / static_cast<float>(kTheta * kPhi) * scene.env.intensity;
    if (scene.sun.enabled) {
        const glm::vec3 d = glm::normalize(scene.sun.direction);
        e += scene.sun.color * std::max(d.y, 0.0f) / kPi;
    }
    return e;
}

long long Scene::triangleCount() const {
    long long n = static_cast<long long>(triangles.size());
    for (const Instance& in : instances)
        if (in.mesh >= 0 && in.mesh < static_cast<int>(meshes.size()))
            n += static_cast<long long>(meshes[static_cast<std::size_t>(in.mesh)].triangles.size());
    return n;
}

bool Scene::bounds(glm::vec3& lo, glm::vec3& hi) const {
    lo = glm::vec3(kInf);
    hi = glm::vec3(-kInf);
    for (const Triangle& t : triangles) {
        lo = glm::min(lo, glm::min(t.p0, glm::min(t.p1, t.p2)));
        hi = glm::max(hi, glm::max(t.p0, glm::max(t.p1, t.p2)));
    }
    // A mesh's own box once, then its eight corners per placement: walking
    // every triangle of every tree would cost more than the question is worth.
    std::vector<glm::vec3> mlo(meshes.size(), glm::vec3(kInf)), mhi(meshes.size(), glm::vec3(-kInf));
    for (std::size_t m = 0; m < meshes.size(); ++m)
        for (const Triangle& t : meshes[m].triangles) {
            mlo[m] = glm::min(mlo[m], glm::min(t.p0, glm::min(t.p1, t.p2)));
            mhi[m] = glm::max(mhi[m], glm::max(t.p0, glm::max(t.p1, t.p2)));
        }
    for (const Instance& in : instances) {
        if (in.mesh < 0 || in.mesh >= static_cast<int>(meshes.size())) continue;
        const std::size_t m = static_cast<std::size_t>(in.mesh);
        if (mlo[m].x > mhi[m].x) continue;
        for (int c = 0; c < 8; ++c) {
            const glm::vec3 p((c & 1) ? mhi[m].x : mlo[m].x,
                              (c & 2) ? mhi[m].y : mlo[m].y,
                              (c & 4) ? mhi[m].z : mlo[m].z);
            const glm::vec3 w = glm::vec3(in.transform * glm::vec4(p, 1.0f));
            lo = glm::min(lo, w);
            hi = glm::max(hi, w);
        }
    }
    return lo.x <= hi.x;
}

namespace {

// composite.frag's rgb2hsv / hsv2rgb, transcribed rather than reimplemented.
// The grade has to be the SAME function, not an equivalent one: a hue wheel
// that rounds differently at the grey axis is a render that disagrees with the
// viewport by a hair on every desaturated surface, which is the hardest kind of
// difference to attribute to anything.
glm::vec3 rgb2hsv(const glm::vec3& c) {
    const float e = 1.0e-10f;
    glm::vec4 K(0.0f, -1.0f / 3.0f, 2.0f / 3.0f, -1.0f);
    glm::vec4 p = c.b > c.g ? glm::vec4(c.b, c.g, K.w, K.z)
                            : glm::vec4(c.g, c.b, K.x, K.y);
    glm::vec4 q = p.x > c.r ? glm::vec4(p.x, p.y, p.w, c.r)
                            : glm::vec4(c.r, p.y, p.z, p.x);
    const float d = q.x - std::min(q.w, q.y);
    return {std::fabs(q.z + (q.w - q.y) / (6.0f * d + e)), d / (q.x + e), q.x};
}

glm::vec3 hsv2rgb(const glm::vec3& c) {
    const glm::vec3 K(1.0f, 2.0f / 3.0f, 1.0f / 3.0f);
    auto fract = [](float v) { return v - std::floor(v); };
    const glm::vec3 p(std::fabs(fract(c.x + K.x) * 6.0f - 3.0f),
                      std::fabs(fract(c.x + K.y) * 6.0f - 3.0f),
                      std::fabs(fract(c.x + K.z) * 6.0f - 3.0f));
    return c.z * glm::mix(glm::vec3(1.0f),
                          glm::clamp(p - glm::vec3(1.0f), glm::vec3(0.0f),
                                     glm::vec3(1.0f)),
                          c.y);
}

// composite.frag's three curves, transcribed. Linear, exposed radiance in;
// display-encoded [0,1] out. See the shader for what each is for.
glm::vec3 agx(glm::vec3 v) {
    // Column-major like GLSL's mat3(...): the same numbers in the same order.
    const glm::mat3 inset(0.842479062253094f, 0.0423282422610123f, 0.0423756549057051f,
                          0.0784335999999992f, 0.878468636469772f, 0.0784336f,
                          0.0792237451477643f, 0.0791661274605434f, 0.879142973793104f);
    constexpr float minEv = -12.47393f, maxEv = 4.026069f;
    v = inset * (v * 2.2409f);
    for (int i = 0; i < 3; ++i) {
        float t = std::log2(std::max(v[i], 1e-10f));
        t = (std::clamp(t, minEv, maxEv) - minEv) / (maxEv - minEv);
        const float t2 = t * t, t4 = t2 * t2;
        v[i] = std::pow(std::max(15.5f * t4 * t2 - 40.14f * t4 * t + 31.96f * t4 -
                                 6.868f * t2 * t + 0.4298f * t2 + 0.1191f * t - 0.00232f,
                                 0.0f),
                        1.35f);                                  // the punchy look
    }
    const float lu = glm::dot(v, glm::vec3(0.2126f, 0.7152f, 0.0722f));
    return glm::clamp(glm::vec3(lu) + 1.4f * (v - glm::vec3(lu)),
                      glm::vec3(0.0f), glm::vec3(1.0f));
}

glm::vec3 pbrNeutral(glm::vec3 c) {
    c *= 1.7050f;
    constexpr float start = 0.76f, desat = 0.15f;
    const float x = std::min(c.r, std::min(c.g, c.b));
    const float offset = x < 0.08f ? x - 6.25f * x * x : 0.04f;
    c -= offset;
    const float peak = std::max(c.r, std::max(c.g, c.b));
    if (peak >= start) {
        constexpr float d = 1.0f - start;
        const float newPeak = 1.0f - d * d / (peak + d - start);
        c *= newPeak / peak;
        const float g = 1.0f - 1.0f / (desat * (peak - newPeak) + 1.0f);
        c = glm::mix(c, glm::vec3(newPeak), g);
    }
    return glm::pow(glm::clamp(c, glm::vec3(0.0f), glm::vec3(1.0f)), glm::vec3(1.0f / 2.2f));
}

glm::vec3 tonemapCurve(glm::vec3 x, int curve) {
    x = glm::max(x, glm::vec3(0.0f));
    if (curve == 1) return agx(x);
    if (curve == 2) return pbrNeutral(x);
    constexpr float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    glm::vec3 m = (x * (a * x + b)) / (x * (c * x + d) + e);
    m = glm::clamp(m, glm::vec3(0.0f), glm::vec3(1.0f));
    return glm::pow(m, glm::vec3(1.0f / 2.2f));
}

} // namespace

glm::vec3 tonemap(const glm::vec3& linear, float exposure, const Grade& grade) {
    // ACES divides one polynomial by another, so an infinity coming in leaves
    // as a NaN and no amount of clamping afterwards rescues it -- and a NaN cast
    // to a byte is whatever the hardware feels like, which is where a field of
    // neon speckles on a black background comes from. Bounded first, exactly as
    // composite.frag bounds it.
    glm::vec3 safe = linear;
    for (int i = 0; i < 3; ++i)
        if (!(safe[i] == safe[i])) safe[i] = 0.0f;
    safe = glm::min(safe, glm::vec3(50000.0f));
    const glm::vec3 x = glm::max(safe * exposure, glm::vec3(0.0f));
    glm::vec3 m = tonemapCurve(x, grade.curve);

    // The grade, in composite.frag's order: white balance, then the contrast
    // S-curve, then hue/saturation/value. Order matters -- grading saturation
    // before contrast gives a visibly different picture from the same numbers.
    m *= glm::vec3(1.0f + grade.warmth * 0.5f,
                   1.0f + grade.warmth * 0.06f,
                   1.0f - grade.warmth * 0.45f);
    m = glm::clamp((m - 0.5f) * (1.0f + grade.contrast) + 0.5f,
                   glm::vec3(0.0f), glm::vec3(1.0f));

    glm::vec3 hsv = rgb2hsv(m);
    hsv.x = hsv.x + grade.hueShift / 360.0f;
    hsv.x = hsv.x - std::floor(hsv.x);
    hsv.y = glm::clamp(hsv.y * grade.saturation, 0.0f, 1.0f);
    hsv.z = hsv.z * grade.value;

    // Clamped, and this is the whole point of the line rather than tidiness.
    //
    // Everything above keeps itself inside [0, 1] except the brightness gain on
    // the last line, which has nothing after it to bound the result. On the GPU
    // that does not matter: composite.frag writes into a normalised framebuffer
    // and the hardware clamps on the way in. Here the value goes on to be cast
    // to a byte as v * 255 + 0.5, and a channel at 1.02 becomes 260, which in an
    // unsigned char is 4.
    //
    // So the brightest channel of the brightest pixels wraps to nearly nothing,
    // and it does it one channel at a time: a blue that overflows leaves yellow,
    // a red that overflows leaves cyan, all three leave black. That is exactly
    // the palette that appeared in the bright end of a rendered sky, and it is
    // why it was only ever the bright end.
    return glm::clamp(hsv2rgb(hsv), glm::vec3(0.0f), glm::vec3(1.0f));
}

// --- Job --------------------------------------------------------------------

Job::~Job() { cancel(); }

void Job::cancel() {
    m_stop.store(true);
    if (m_thread.joinable()) m_thread.join();
    m_running.store(false);
}

void Job::start(std::shared_ptr<const Scene> scene, const Settings& settings) {
    cancel();

    m_scene    = std::move(scene);
    m_settings = settings;
    m_settings.width      = std::max(1, m_settings.width);
    m_settings.height     = std::max(1, m_settings.height);
    m_settings.samples    = std::max(1, m_settings.samples);
    m_settings.batch      = std::clamp(m_settings.batch, 1, m_settings.samples);
    m_settings.maxBounces = std::max(0, m_settings.maxBounces);
    if (m_settings.show != Show::Full) {
        // A diagnostic is a readout, not a picture: tonemapping and grading it
        // would put the very stages it is meant to rule out back in front of
        // the answer. It also has no noise to average away, so a handful of
        // samples for the edges is all it needs.
        m_settings.tonemap = false;
        m_settings.samples = std::min(m_settings.samples, 16);
        m_settings.batch   = std::min(m_settings.batch, m_settings.samples);
    }

    const std::size_t n = static_cast<std::size_t>(m_settings.width) * m_settings.height;
    m_accum.assign(n, glm::vec3(0.0f));
    m_tilesX = (m_settings.width  + kTileSize - 1) / kTileSize;
    m_tilesY = (m_settings.height + kTileSize - 1) / kTileSize;
    // vector<atomic> cannot be assigned or resized in place -- atomics are not
    // copyable -- so the whole thing is rebuilt.
    const std::size_t tiles = static_cast<std::size_t>(m_tilesX) * m_tilesY;
    std::vector<std::atomic<int>> fresh(tiles);
    for (auto& a : fresh) a.store(0);
    m_tileSamples = std::move(fresh);
    std::vector<std::atomic<bool>> locks(tiles);
    for (auto& a : locks) a.store(false);
    m_tileLock = std::move(locks);

    m_done.store(0);
    m_pixels.store(static_cast<int>(n));
    m_elapsed.store(0.0);
    m_triangles    = m_scene ? m_scene->triangleCount() : 0;
    m_buildSeconds.store(0.0);
    m_focus.store(m_scene ? m_scene->camera.focusDistance : 10.0f);
    m_stop.store(false);
    m_running.store(true);
    m_started = std::chrono::steady_clock::now();

    m_thread = std::thread([this] { run(); });
}

float Job::progress() const {
    const int total = std::max(1, m_settings.samples);
    return glm::clamp(static_cast<float>(m_done.load()) / static_cast<float>(total),
                      0.0f, 1.0f);
}

double Job::elapsedSeconds() const {
    if (!m_running.load()) return m_elapsed.load();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_started).count();
}

void Job::run() {
    const Scene& sc = *m_scene;

    const auto buildStart = std::chrono::steady_clock::now();
    Accel bvh;
    bvh.build(sc);
    EnvSampler envDist;
    envDist.build(sc.env);
    m_buildSeconds.store(std::chrono::duration<double>(
        std::chrono::steady_clock::now() - buildStart).count());
    if (m_stop.load()) {
        m_running.store(false);
        return;
    }

    const int W = m_settings.width, H = m_settings.height;
    const float aspect = static_cast<float>(W) / static_cast<float>(H);
    const CameraDesc& cam = sc.camera;

    // The focus, measured with the tree that now exists rather than by a
    // brute-force pass over every triangle before it did -- which on a forest
    // was the better part of a minute spent answering one question. Nothing
    // under the crosshair (the camera is pointed at the sky) keeps the
    // camera's own distance rather than focusing at zero, which would blur the
    // entire picture and look like a bug.
    float focusDist = cam.focusDistance;
    if (m_settings.autoFocus && cam.apertureRadius > 1e-5f) {
        Ray r;
        r.o = cam.position;
        r.d = glm::normalize(cam.forward);
        r.prepare();
        Hit h;
        if (bvh.closest(r, kInf, h)) focusDist = h.t;
    }
    focusDist = std::max(0.1f, focusDist);
    m_focus.store(focusDist);
    const float halfV = std::tan(glm::radians(cam.fovDegrees) * 0.5f);
    const float halfU = halfV * aspect;

    const int tileCount = m_tilesX * m_tilesY;
    const int passes    = (m_settings.samples + m_settings.batch - 1) / m_settings.batch;

    int threads = m_settings.threads > 0
                ? m_settings.threads
                : static_cast<int>(std::thread::hardware_concurrency()) - 1;
    threads = std::clamp(threads, 1, 64);

    std::atomic<long long> nextItem{0};
    const long long items = static_cast<long long>(passes) * tileCount;

    auto worker = [&]() {
        Tracer tracer(sc, bvh, envDist, m_settings.maxBounces,
                      m_settings.clampIndirect);
        std::vector<glm::vec3> local;
        local.reserve(static_cast<std::size_t>(kTileSize) * kTileSize);
        for (;;) {
            const long long item = nextItem.fetch_add(1);
            if (item >= items || m_stop.load()) return;

            const int pass = static_cast<int>(item / tileCount);
            const int tile = static_cast<int>(item % tileCount);
            const int tx = tile % m_tilesX, ty = tile / m_tilesX;
            const int x0 = tx * kTileSize, y0 = ty * kTileSize;
            const int x1 = std::min(x0 + kTileSize, W);
            const int y1 = std::min(y0 + kTileSize, H);

            // The last pass may be short, so that "samples" means exactly what
            // it says rather than "rounded up to a multiple of the batch".
            const int done = pass * m_settings.batch;
            const int spp  = std::min(m_settings.batch, m_settings.samples - done);
            if (spp <= 0) continue;

            // Into a local buffer first. Publishing every pixel as it is
            // computed would mean the sums and the count are out of step for
            // the whole length of a tile rather than for the merge below.
            local.assign(static_cast<std::size_t>(x1 - x0) * (y1 - y0),
                         glm::vec3(0.0f));

            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    // Seeded from the pixel and the pass, not from a running
                    // counter: that is what makes the image independent of how
                    // the work happened to be shared out.
                    Rng rng(static_cast<std::uint64_t>(y) * W + x,
                            static_cast<std::uint64_t>(m_settings.seed) * 9781u + pass);
                    glm::vec3 sum(0.0f);
                    for (int s = 0; s < spp; ++s) {
                        const float px = (static_cast<float>(x) + rng.uniform()) /
                                         static_cast<float>(W) * 2.0f - 1.0f;
                        const float py = 1.0f - (static_cast<float>(y) + rng.uniform()) /
                                         static_cast<float>(H) * 2.0f;
                        glm::vec3 dir = glm::normalize(cam.forward +
                                                       cam.right * (px * halfU) +
                                                       cam.up    * (py * halfV));
                        glm::vec3 org = cam.position;
                        if (cam.apertureRadius > 1e-5f) {
                            // A real lens: the ray starts somewhere on the
                            // aperture and still passes through the focal point,
                            // so the focal plane stays sharp and the rest opens up.
                            const glm::vec3 focus = org + dir *
                                (focusDist / std::max(1e-4f, glm::dot(dir, cam.forward)));
                            const glm::vec2 lens = sampleDisk(rng) * cam.apertureRadius;
                            org += cam.right * lens.x + cam.up * lens.y;
                            dir = glm::normalize(focus - org);
                        }
                        Ray r;
                        r.o = org;
                        r.d = dir;
                        sum += m_settings.show == Show::Full
                             ? tracer.radiance(r, rng)
                             : tracer.probe(r, m_settings.show);
                    }
                    local[static_cast<std::size_t>(y - y0) * (x1 - x0) +
                          (x - x0)] = sum;
                }
                if (m_stop.load()) return;
            }

            {
                TileGuard guard(m_tileLock[tile]);
                for (int y = y0; y < y1; ++y)
                    for (int x = x0; x < x1; ++x)
                        m_accum[static_cast<std::size_t>(y) * W + x] +=
                            local[static_cast<std::size_t>(y - y0) * (x1 - x0) +
                                  (x - x0)];
                m_tileSamples[tile].fetch_add(spp);
            }
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(threads));
    for (int i = 0; i < threads; ++i) pool.emplace_back(worker);

    // The coordinator publishes progress rather than the workers: "samples
    // done" is the count the SLOWEST tile has reached, which is the only figure
    // that is true of the whole image.
    while (true) {
        bool anyLeft = nextItem.load() < items && !m_stop.load();
        int lowest = m_settings.samples;
        for (int t = 0; t < tileCount; ++t)
            lowest = std::min(lowest, m_tileSamples[t].load());
        m_done.store(lowest);
        m_elapsed.store(std::chrono::duration<double>(
            std::chrono::steady_clock::now() - m_started).count());
        if (!anyLeft) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    for (std::thread& t : pool) t.join();

    int lowest = m_settings.samples;
    for (int t = 0; t < tileCount; ++t) lowest = std::min(lowest, m_tileSamples[t].load());
    m_done.store(lowest);
    m_elapsed.store(std::chrono::duration<double>(
        std::chrono::steady_clock::now() - m_started).count());
    m_running.store(false);
}

// Both snapshots walk TILES rather than pixels, because a tile is the unit the
// sums and the sample count are consistent over. Reading a pixel on its own
// would be reading half of a pair.
bool Job::snapshotHdr(std::vector<float>& out) const {
    const int n = m_pixels.load();
    if (n <= 0) return false;
    const int W = m_settings.width, H = m_settings.height;
    out.assign(static_cast<std::size_t>(n) * 3, 0.0f);

    for (int ty = 0; ty < m_tilesY; ++ty) {
        for (int tx = 0; tx < m_tilesX; ++tx) {
            const int tile = ty * m_tilesX + tx;
            TileGuard guard(m_tileLock[tile]);
            const int spp = m_tileSamples[tile].load();
            if (spp <= 0) continue;
            const float inv = 1.0f / static_cast<float>(spp);
            const int x1 = std::min(tx * kTileSize + kTileSize, W);
            const int y1 = std::min(ty * kTileSize + kTileSize, H);
            for (int y = ty * kTileSize; y < y1; ++y)
                for (int x = tx * kTileSize; x < x1; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y) * W + x;
                    const glm::vec3 c = m_accum[i] * inv;
                    out[i * 3 + 0] = c.r;
                    out[i * 3 + 1] = c.g;
                    out[i * 3 + 2] = c.b;
                }
        }
    }
    return true;
}

bool Job::snapshotLdr(std::vector<unsigned char>& out) const {
    const int n = m_pixels.load();
    if (n <= 0) return false;
    const int W = m_settings.width, H = m_settings.height;
    const float exposure = m_scene ? m_scene->exposure : 1.0f;
    const Grade grade    = m_scene ? m_scene->grade : Grade{};
    out.assign(static_cast<std::size_t>(n) * 4, 0);
    // Alpha is opaque everywhere, including the tiles nothing has reached yet:
    // a preview with transparent holes in it reads as a broken render rather
    // than an unfinished one.
    for (std::size_t i = 3; i < out.size(); i += 4) out[i] = 255;

    for (int ty = 0; ty < m_tilesY; ++ty) {
        for (int tx = 0; tx < m_tilesX; ++tx) {
            const int tile = ty * m_tilesX + tx;
            TileGuard guard(m_tileLock[tile]);
            const int spp = m_tileSamples[tile].load();
            if (spp <= 0) continue;
            const float inv = 1.0f / static_cast<float>(spp);
            const int x1 = std::min(tx * kTileSize + kTileSize, W);
            const int y1 = std::min(ty * kTileSize + kTileSize, H);
            for (int y = ty * kTileSize; y < y1; ++y)
                for (int x = tx * kTileSize; x < x1; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y) * W + x;
                    glm::vec3 c = m_accum[i] * inv;
                    c = m_settings.tonemap
                      ? tonemap(c, exposure, grade) *
                            vignette((x + 0.5f) / W, (y + 0.5f) / H,
                                     static_cast<float>(W) / static_cast<float>(H),
                                     grade.vignette)
                      : glm::clamp(c, glm::vec3(0.0f), glm::vec3(1.0f));
                    // Again here, next to the cast itself. tonemap() already
                    // guarantees this; the cast is where the cost of being
                    // wrong is a wrapped byte rather than a wrong number, and
                    // that is worth a second line.
                    c = glm::clamp(c, glm::vec3(0.0f), glm::vec3(1.0f));
                    out[i * 4 + 0] = static_cast<unsigned char>(c.r * 255.0f + 0.5f);
                    out[i * 4 + 1] = static_cast<unsigned char>(c.g * 255.0f + 0.5f);
                    out[i * 4 + 2] = static_cast<unsigned char>(c.b * 255.0f + 0.5f);
                }
        }
    }
    return true;
}

// The accelerator, built and handed over. The tracer builds its own the same
// way a line above; this exists so a second renderer does not have to have a
// BVH build of its own to disagree with.
BvhData buildBvh(const std::vector<Triangle>& triangles) {
    Blas b;
    b.build(triangles);
    BvhData out;
    out.nodes = std::move(b.nodes);
    out.index = std::move(b.index);
    return out;
}

EnvDistribution buildEnvDistribution(const Environment& env) {
    EnvSampler e;
    e.build(env);
    EnvDistribution d;
    if (!e.valid()) return d;
    d.w = e.w;
    d.h = e.h;
    d.func    = std::move(e.func);
    d.condCdf = std::move(e.condCdf);
    d.margCdf = std::move(e.margCdf);
    d.total   = e.total;
    return d;
}

AccelData buildAccel(const Scene& scene) {
    Accel a;
    a.build(scene);
    AccelData out;
    out.world.nodes = std::move(a.world.nodes);
    out.world.index = std::move(a.world.index);
    out.meshes.resize(a.meshes.size());
    for (std::size_t i = 0; i < a.meshes.size(); ++i) {
        out.meshes[i].nodes = std::move(a.meshes[i].nodes);
        out.meshes[i].index = std::move(a.meshes[i].index);
    }
    out.instances.nodes = std::move(a.top);
    out.instances.index = std::move(a.topIndex);
    return out;
}

} // namespace pathtrace
