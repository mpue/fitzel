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

struct Hit {
    float t  = kInf;
    float u  = 0.0f, v = 0.0f;
    int   tri = -1;
};

// Moeller-Trumbore, two-sided. Two-sided on purpose: a fair amount of scene
// geometry (a road ribbon, a decal patch, an imported model with a flipped
// winding) is single-sided in the raster path only because backface culling
// hides the problem, and a tracer that honoured the winding would put holes
// where the viewport shows a surface.
bool intersectTri(const Triangle& tri, const Ray& r, float tMax, float& t,
                  float& u, float& v) {
    const glm::vec3 e1 = tri.p1 - tri.p0;
    const glm::vec3 e2 = tri.p2 - tri.p0;
    const glm::vec3 p  = glm::cross(r.d, e2);
    const float det = glm::dot(e1, p);
    if (std::fabs(det) < 1e-12f) return false;
    const float inv = 1.0f / det;
    const glm::vec3 s = r.o - tri.p0;
    u = glm::dot(s, p) * inv;
    if (u < -1e-6f || u > 1.0f + 1e-6f) return false;
    const glm::vec3 q = glm::cross(s, e1);
    v = glm::dot(r.d, q) * inv;
    if (v < -1e-6f || u + v > 1.0f + 1e-6f) return false;
    t = glm::dot(e2, q) * inv;
    return t > kRayEps && t < tMax;
}

// --- The accelerator --------------------------------------------------------
// A binned-SAH BVH. Chosen over a median split because scene geometry here is
// wildly non-uniform -- a 200-metre terrain chunk sits in the same list as a
// wing mirror -- and a median split over that produces nodes that overlap
// everything, which costs far more at trace time than the better build costs
// once.
struct Bvh {
    struct Node {
        glm::vec3 lo{0.0f}, hi{0.0f};
        int leftFirst = 0; // count > 0: first index; count == 0: left child
        int count     = 0;
    };

    std::vector<Node> nodes;
    std::vector<int>  index;     // triangle indices, reordered by the build
    const std::vector<Triangle>* tris = nullptr;

    static constexpr int kLeafSize = 4;
    static constexpr int kBins     = 12;

    void build(const std::vector<Triangle>& triangles);
    bool closest(const Ray& r, float tMax, Hit& hit) const;
    // Every hit along the ray, as a product of transmittances. Not a boolean:
    // the scene has glass and cutout foliage in it, and a shadow ray that
    // stopped at the first triangle would put a solid black shadow under a
    // windscreen.
    template <typename AlphaFn>
    glm::vec3 transmittance(const Ray& r, float tMax, const AlphaFn& alphaAt) const;

private:
    void growTo(Node& n, const Triangle& t) const {
        n.lo = glm::min(n.lo, glm::min(t.p0, glm::min(t.p1, t.p2)));
        n.hi = glm::max(n.hi, glm::max(t.p0, glm::max(t.p1, t.p2)));
    }
};

glm::vec3 triCentroid(const Triangle& t) {
    return (t.p0 + t.p1 + t.p2) * (1.0f / 3.0f);
}

float surfaceArea(const glm::vec3& lo, const glm::vec3& hi) {
    const glm::vec3 e = glm::max(hi - lo, glm::vec3(0.0f));
    return 2.0f * (e.x * e.y + e.y * e.z + e.z * e.x);
}

void Bvh::build(const std::vector<Triangle>& triangles) {
    tris = &triangles;
    nodes.clear();
    index.clear();
    if (triangles.empty()) return;

    index.resize(triangles.size());
    for (std::size_t i = 0; i < triangles.size(); ++i) index[i] = static_cast<int>(i);

    // Centroids are computed once. Recomputing them inside the binning loop is
    // the single easiest way to make a BVH build take minutes on a big scene.
    std::vector<glm::vec3> centroid(triangles.size());
    for (std::size_t i = 0; i < triangles.size(); ++i)
        centroid[i] = triCentroid(triangles[i]);

    nodes.reserve(triangles.size() * 2);
    nodes.push_back({});
    Node& root = nodes[0];
    root.lo = glm::vec3(kInf);
    root.hi = glm::vec3(-kInf);
    root.leftFirst = 0;
    root.count     = static_cast<int>(triangles.size());
    for (const Triangle& t : triangles) growTo(root, t);

    std::vector<int> stack;
    stack.push_back(0);

    while (!stack.empty()) {
        const int ni = stack.back();
        stack.pop_back();
        if (nodes[ni].count <= kLeafSize) continue;

        const int first = nodes[ni].leftFirst;
        const int count = nodes[ni].count;

        // Bin over the centroid bounds, not the node bounds: a handful of huge
        // triangles otherwise stretch the range so far that every centroid
        // lands in bin 0 and no split is ever found.
        glm::vec3 cLo(kInf), cHi(-kInf);
        for (int i = 0; i < count; ++i) {
            const glm::vec3& c = centroid[index[first + i]];
            cLo = glm::min(cLo, c);
            cHi = glm::max(cHi, c);
        }

        float bestCost = kInf;
        int   bestAxis = -1;
        float bestSplit = 0.0f;

        for (int axis = 0; axis < 3; ++axis) {
            const float lo = cLo[axis], hi = cHi[axis];
            if (hi - lo < 1e-6f) continue;
            const float scale = static_cast<float>(kBins) / (hi - lo);

            glm::vec3 binLo[kBins], binHi[kBins];
            int       binN[kBins] = {0};
            for (int b = 0; b < kBins; ++b) { binLo[b] = glm::vec3(kInf); binHi[b] = glm::vec3(-kInf); }

            for (int i = 0; i < count; ++i) {
                const int ti = index[first + i];
                int b = static_cast<int>((centroid[ti][axis] - lo) * scale);
                b = std::clamp(b, 0, kBins - 1);
                const Triangle& t = triangles[ti];
                binN[b]++;
                binLo[b] = glm::min(binLo[b], glm::min(t.p0, glm::min(t.p1, t.p2)));
                binHi[b] = glm::max(binHi[b], glm::max(t.p0, glm::max(t.p1, t.p2)));
            }

            // Sweep from both ends so each candidate plane knows the cost of
            // both sides in one pass.
            float leftArea[kBins], rightArea[kBins];
            int   leftN[kBins],    rightN[kBins];
            glm::vec3 acLo(kInf), acHi(-kInf);
            int acN = 0;
            for (int b = 0; b < kBins; ++b) {
                acLo = glm::min(acLo, binLo[b]);
                acHi = glm::max(acHi, binHi[b]);
                acN += binN[b];
                leftN[b]    = acN;
                leftArea[b] = acN ? surfaceArea(acLo, acHi) : 0.0f;
            }
            acLo = glm::vec3(kInf); acHi = glm::vec3(-kInf); acN = 0;
            for (int b = kBins - 1; b >= 0; --b) {
                acLo = glm::min(acLo, binLo[b]);
                acHi = glm::max(acHi, binHi[b]);
                acN += binN[b];
                rightN[b]    = acN;
                rightArea[b] = acN ? surfaceArea(acLo, acHi) : 0.0f;
            }

            for (int b = 0; b < kBins - 1; ++b) {
                if (leftN[b] == 0 || rightN[b + 1] == 0) continue;
                const float cost = leftArea[b] * static_cast<float>(leftN[b]) +
                                   rightArea[b + 1] * static_cast<float>(rightN[b + 1]);
                if (cost < bestCost) {
                    bestCost  = cost;
                    bestAxis  = axis;
                    bestSplit = lo + (static_cast<float>(b) + 1.0f) / scale;
                }
            }
        }

        // Splitting has to beat leaving the node alone, or a leaf is the right
        // answer -- this is the termination rule, not the depth.
        const float leafCost = surfaceArea(nodes[ni].lo, nodes[ni].hi) *
                               static_cast<float>(count);
        if (bestAxis < 0 || bestCost >= leafCost) continue;

        const auto mid = std::partition(
            index.begin() + first, index.begin() + first + count,
            [&](int ti) { return centroid[ti][bestAxis] < bestSplit; });
        const int leftCount = static_cast<int>(mid - (index.begin() + first));
        if (leftCount == 0 || leftCount == count) continue;

        const int leftIdx = static_cast<int>(nodes.size());
        Node left{}, right{};
        left.lo = right.lo = glm::vec3(kInf);
        left.hi = right.hi = glm::vec3(-kInf);
        left.leftFirst  = first;
        left.count      = leftCount;
        right.leftFirst = first + leftCount;
        right.count     = count - leftCount;
        for (int i = 0; i < leftCount; ++i)      growTo(left,  triangles[index[first + i]]);
        for (int i = leftCount; i < count; ++i)  growTo(right, triangles[index[first + i]]);

        nodes.push_back(left);
        nodes.push_back(right);
        nodes[ni].leftFirst = leftIdx;
        nodes[ni].count     = 0;

        stack.push_back(leftIdx);
        stack.push_back(leftIdx + 1);
    }
}

// Slab test. Returns the near distance so the traversal can visit the closer
// child first and cull the far one against a hit it already has.
inline bool slab(const Bvh::Node& n, const Ray& r, float tMax, float& tNear) {
    const glm::vec3 t0 = (n.lo - r.o) * r.invD;
    const glm::vec3 t1 = (n.hi - r.o) * r.invD;
    const glm::vec3 lo = glm::min(t0, t1);
    const glm::vec3 hi = glm::max(t0, t1);
    const float a = std::max(std::max(lo.x, lo.y), std::max(lo.z, 0.0f));
    const float b = std::min(std::min(hi.x, hi.y), std::min(hi.z, tMax));
    tNear = a;
    return a <= b;
}

bool Bvh::closest(const Ray& r, float tMax, Hit& hit) const {
    if (nodes.empty()) return false;
    const std::vector<Triangle>& T = *tris;

    int   stack[128];
    float stackT[128];
    int   sp = 0;
    int   node = 0;
    float nodeT = 0.0f;
    if (!slab(nodes[0], r, tMax, nodeT)) return false;

    hit.t = tMax;
    for (;;) {
        const Node& n = nodes[node];
        if (n.count > 0) {
            for (int i = 0; i < n.count; ++i) {
                const int ti = index[n.leftFirst + i];
                float t, u, v;
                if (intersectTri(T[ti], r, hit.t, t, u, v)) {
                    hit.t = t; hit.u = u; hit.v = v; hit.tri = ti;
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
                if (sp < 128) { stack[sp] = farI; stackT[sp] = farT; ++sp; }
                node = nearI;
                continue;
            }
            if (hl) { node = l;  continue; }
            if (hr) { node = rr; continue; }
        }
        // Pop, discarding anything the closest hit has already overtaken.
        do {
            if (sp == 0) return hit.tri >= 0;
            --sp;
            node  = stack[sp];
            nodeT = stackT[sp];
        } while (nodeT > hit.t);
    }
}

template <typename AlphaFn>
glm::vec3 Bvh::transmittance(const Ray& r, float tMax, const AlphaFn& alphaAt) const {
    if (nodes.empty()) return glm::vec3(1.0f);
    const std::vector<Triangle>& T = *tris;

    glm::vec3 tr(1.0f);
    int stack[128];
    int sp = 0;
    int node = 0;
    float dummy;
    if (!slab(nodes[0], r, tMax, dummy)) return tr;

    for (;;) {
        const Node& n = nodes[node];
        if (n.count > 0) {
            for (int i = 0; i < n.count; ++i) {
                const int ti = index[n.leftFirst + i];
                float t, u, v;
                if (!intersectTri(T[ti], r, tMax, t, u, v)) continue;
                tr *= alphaAt(ti, u, v);
                // Fully blocked: nothing further along the ray can matter.
                if (luminance(tr) < 1e-4f) return glm::vec3(0.0f);
            }
        } else {
            const int l = n.leftFirst, rr = n.leftFirst + 1;
            float tl, trr;
            const bool hl = slab(nodes[l],  r, tMax, tl);
            const bool hr = slab(nodes[rr], r, tMax, trr);
            if (hl && hr) {
                if (sp < 128) stack[sp++] = rr;
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

struct Tracer {
    const Scene&      sc;
    const Bvh&        bvh;
    const EnvSampler& envDist;
    int               maxBounces;
    float             clampIndirect;

    // The sun as a disc rather than a direction. Both estimators need it: the
    // light sampler needs its solid angle, and a ray that escapes needs to be
    // able to HIT it -- which is also what puts a sun in a chrome reflection,
    // where the raster path only ever had a bright blob from the probe.
    glm::vec3 sunAxis{0.0f, 1.0f, 0.0f};
    float     sunCosMax = 1.0f;
    float     sunPdf    = 0.0f;  // solid-angle density; 0 marks a hard-edged sun
    glm::vec3 sunRadiance{0.0f};

    Tracer(const Scene& scene, const Bvh& accel, const EnvSampler& env,
           int bounces, float clamp)
        : sc(scene), bvh(accel), envDist(env), maxBounces(bounces),
          clampIndirect(clamp) {
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

    glm::vec2 uvAt(int tri, float u, float v) const {
        const Triangle& t = sc.triangles[tri];
        const float w = 1.0f - u - v;
        return t.uv0 * w + t.uv1 * u + t.uv2 * v;
    }

    // The terrain paint at a hit. Zero when the scene carries none, which is
    // the same answer an unpainted terrain gives -- so nothing downstream has
    // to know whether the side table exists.
    glm::vec4 paintAt(int tri, float u, float v) const {
        if (sc.vertexPaint.empty()) return glm::vec4(0.0f);
        const std::size_t o = static_cast<std::size_t>(tri) * 3;
        if (o + 2 >= sc.vertexPaint.size()) return glm::vec4(0.0f);
        const float w = 1.0f - u - v;
        return sc.vertexPaint[o] * w + sc.vertexPaint[o + 1] * u +
               sc.vertexPaint[o + 2] * v;
    }

    // How much light survives one crossing of this triangle, as a colour. Glass
    // tints what passes through it, which is what stops a green-tinted
    // windscreen from casting a grey shadow.
    glm::vec3 shadowFactor(int tri, float u, float v) const {
        const Material& m = sc.materials[sc.triangles[tri].material];
        const float a = coverageAt(m, uvAt(tri, u, v));
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

        glm::vec3 acc(0.0f);
        float wsum = 0.0f;
        for (std::size_t i = 0; i < m.layers.size(); ++i) {
            const TerrainLayer& L = m.layers[i];
            const float autoW = band(h, L.band.x, L.band.y, 1.5f) *
                                band(slopeDeg, L.band.z, L.band.w, 6.0f);
            const float pw = i < 4 ? p[static_cast<int>(i)] : 0.0f;
            const float w  = autoW * (1.0f - cover) + pw;
            if (w <= 0.0f) continue;
            if (L.texture < 0 || L.texture >= static_cast<int>(sc.textures.size()))
                continue;
            acc  += triplanar(sc.textures[L.texture], wp, n, L.scale) * w;
            wsum += w;
        }
        if (wsum < 1e-4f) return false;
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
        s.coverage  = coverageAt(m, uv);
        s.glass     = m.glass;
        return s;
    }

    // The BRDF, without the cosine. Diffuse plus a GGX lobe. `specScale` is the
    // energy correction that comes with widening the lobe for a light of finite
    // size -- 1 everywhere else.
    glm::vec3 evalBsdf(const Surface& s, const glm::vec3& N, const glm::vec3& V,
                       const glm::vec3& L, float specScale = 1.0f) const {
        const float NoL = glm::dot(N, L);
        const float NoV = glm::dot(N, V);
        if (NoL <= 0.0f || NoV <= 0.0f) return glm::vec3(0.0f);
        const glm::vec3 H = glm::normalize(V + L);
        const float NoH = std::max(0.0f, glm::dot(N, H));
        const float VoH = std::max(0.0f, glm::dot(V, H));
        const glm::vec3 spec = fresnel(s.F0, VoH) *
                               (ggxD(NoH, s.alpha) * smithVis(NoV, NoL, s.alpha));
        return s.diffuse * (1.0f / kPi) + spec * specScale;
    }

    // The pdf the sampler below would have used for this direction. Needed
    // separately because the sampler picks between two lobes and the weight has
    // to divide by the combined density, not by the one that happened to fire.
    float pdfBsdf(const Surface& s, const glm::vec3& N, const glm::vec3& V,
                  const glm::vec3& L, float pSpec) const {
        const float NoL = glm::dot(N, L);
        const float NoV = glm::dot(N, V);
        if (NoL <= 0.0f || NoV <= 0.0f) return 0.0f;
        const glm::vec3 H = glm::normalize(V + L);
        const float NoH = std::max(0.0f, glm::dot(N, H));
        const float pdfSpec = ggxD(NoH, s.alpha) * smithG1(NoV, s.alpha) /
                              std::max(4.0f * NoV, 1e-6f);
        const float pdfDiff = NoL * (1.0f / kPi);
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
    glm::vec3 directLight(const glm::vec3& P, const glm::vec3& N, const glm::vec3& V,
                          const Surface& s, float pSpec, Rng& rng) const {
        glm::vec3 L(0.0f);
        const auto alphaFn = [&](int tri, float u, float v) { return shadowFactor(tri, u, v); };

        if (sc.sun.enabled && luminance(sc.sun.color) > 1e-5f) {
            const glm::vec3 dir = sunPdf > 0.0f ? sampleCone(sunAxis, sunCosMax, rng)
                                                : sunAxis;
            const float NoL = glm::dot(N, dir);
            if (NoL > 0.0f) {
                Ray sr;
                sr.o = P + N * kRayEps;
                sr.d = dir;
                sr.prepare();
                const glm::vec3 tr = bvh.transmittance(sr, kInf, alphaFn);
                if (luminance(tr) > 1e-4f) {
                    // sun.color is already radiance divided by this sampler's
                    // own pdf, which is why it appears here unscaled.
                    glm::vec3 c = sc.sun.color * tr * evalBsdf(s, N, V, dir) * NoL;
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
            if (pdfE > 1e-4f && NoL > 0.0f) {
                Ray sr;
                sr.o = P + N * kRayEps;
                sr.d = dir;
                sr.prepare();
                const glm::vec3 tr = bvh.transmittance(sr, kInf, alphaFn);
                if (luminance(tr) > 1e-4f) {
                    const glm::vec3 Le = sc.env.sample(dir) * sc.env.intensity;
                    const float wgt = misWeight(pdfE, pdfBsdf(s, N, V, dir, pSpec));
                    const glm::vec3 c = Le * tr * evalBsdf(s, N, V, dir) *
                                        (NoL * wgt / pdfE);
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
            const glm::vec3 tr = bvh.transmittance(sr, sdist - kRayEps, alphaFn);
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
        ray.prepare();
        Hit hit;
        if (!bvh.closest(ray, kInf, hit)) return glm::vec3(0.0f);

        const Triangle& tri = sc.triangles[hit.tri];
        const Material& mat = sc.materials[tri.material];
        const float w = 1.0f - hit.u - hit.v;
        const glm::vec2 uv = tri.uv0 * w + tri.uv1 * hit.u + tri.uv2 * hit.v;

        if (mode == Show::BaseColor) {
            // As AUTHORED, before the sRGB->linear step, so the image is
            // literally the colour the texture holds and the inspector shows.
            // Linearising here would make every diagnostic look too dark and
            // start a second hunt.
            glm::vec3 N = tri.n0 * w + tri.n1 * hit.u + tri.n2 * hit.v;
            N = glm::dot(N, N) < 1e-12f
              ? glm::normalize(glm::cross(tri.p1 - tri.p0, tri.p2 - tri.p0))
              : glm::normalize(N);
            glm::vec3 terrain;
            if (terrainColorAt(mat, ray.o + ray.d * hit.t, N,
                               paintAt(hit.tri, hit.u, hit.v), terrain))
                return terrain;
            if (mat.texture >= 0 && mat.texture < static_cast<int>(sc.textures.size()))
                return glm::vec3(sc.textures[mat.texture].sample(uv.x, uv.y)) * mat.tint;
            return mat.albedo;
        }
        if (mode == Show::Normal) {
            glm::vec3 N = tri.n0 * w + tri.n1 * hit.u + tri.n2 * hit.v;
            N = glm::dot(N, N) < 1e-12f
              ? glm::normalize(glm::cross(tri.p1 - tri.p0, tri.p2 - tri.p0))
              : glm::normalize(N);
            return N * 0.5f + 0.5f;
        }
        // Depth, on a soft ramp so both a wing mirror and a distant hill land
        // somewhere readable rather than at the ends.
        const float d = hit.t / (hit.t + 20.0f);
        return glm::vec3(d);
    }

    glm::vec3 radiance(Ray ray, Rng& rng) const {
        glm::vec3 L(0.0f), beta(1.0f);
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

        for (int bounce = 0; bounce <= maxBounces; ++bounce) {
            ray.prepare();
            Hit hit;
            if (!bvh.closest(ray, kInf, hit)) {
                glm::vec3 sky = sc.env.sample(ray.d) * sc.env.intensity;
                // The sky is now also aimed at by the light sampler, so a ray
                // that arrives here by scattering only claims its share --
                // except off a delta lobe or straight from the camera, where
                // the light sampler never had a chance.
                if (!lastDelta && envDist.valid())
                    sky *= misWeight(lastPdf, envDist.pdfFor(ray.d));
                if (sc.sun.enabled && sunPdf > 0.0f &&
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
            if (primaryDist < 0.0f) primaryDist = hit.t;

            const Triangle& tri = sc.triangles[hit.tri];
            const Material& mat = sc.materials[tri.material];
            const float w = 1.0f - hit.u - hit.v;
            const glm::vec3 P = ray.o + ray.d * hit.t;
            const glm::vec2 uv = tri.uv0 * w + tri.uv1 * hit.u + tri.uv2 * hit.v;

            glm::vec3 N = tri.n0 * w + tri.n1 * hit.u + tri.n2 * hit.v;
            if (glm::dot(N, N) < 1e-12f)
                N = glm::normalize(glm::cross(tri.p1 - tri.p0, tri.p2 - tri.p0));
            else
                N = glm::normalize(N);
            const glm::vec3 gN = N;
            if (glm::dot(N, -ray.d) < 0.0f) N = -N; // shade the side we can see

            float texA = 1.0f;
            // gN, not N: the slope a layer band tests is the ground's, not the
            // one turned to face the eye.
            const Surface s = surfaceAt(mat, uv, P, gN,
                                        paintAt(hit.tri, hit.u, hit.v), texA);

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
                if (++passthrough > 32) break;
                ray.o = P + ray.d * kRayEps;
                --bounce; // a pane is not a bounce
                continue;
            }

            L += clamped(beta * s.emission, bounce);

            const glm::vec3 V = -ray.d;

            if (mat.glass) {
                // A dielectric: reflect or refract, chosen by Fresnel. No next
                // event estimation -- the lobe is a delta and a shadow ray at a
                // mirror direction would contribute nothing but noise.
                const bool entering = glm::dot(gN, ray.d) < 0.0f;
                const float ior = entering ? (1.0f / 1.5f) : 1.5f;
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
                    ray.d = glm::refract(ray.d, N, ior);
                    if (glm::dot(ray.d, ray.d) < 1e-8f) ray.d = glm::reflect(ray.d, N);
                    beta *= s.baseColor; // the tint the pane carries
                }
                ray.o = P + ray.d * kRayEps;
                lastDelta = true;
                continue;
            }

            // Chosen before the light sampling, not after: next event
            // estimation has to weigh itself against the SAME densities the
            // scatter below will use, or the two strategies do not add up to one.
            const float pSpec = specProbability(s);
            L += clamped(beta * directLight(P, N, V, s, pSpec, rng), bounce);

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
                nextDir = cosineHemisphere(N, rng);
            }
            const float NoL = glm::dot(N, nextDir);
            if (NoL <= 0.0f) break;

            const float pdf = pdfBsdf(s, N, V, nextDir, pSpec);
            if (pdf < 1e-6f) break;
            beta *= evalBsdf(s, N, V, nextDir) * (NoL / pdf);
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

            ray.o = P + N * kRayEps;
            ray.d = nextDir;
        }

        // The same guard lit.frag ends with, and for the same reason. A single
        // non-finite sample is not one bad pixel: it is added into a tile's
        // running sum, and from then on every sample that tile ever takes is
        // averaged with a NaN. One firefly becomes a permanent hole.
        if (!(L.x == L.x) || !(L.y == L.y) || !(L.z == L.z)) return glm::vec3(0.0f);
        L = glm::min(L, glm::vec3(50000.0f));
        if (primaryDist > 0.0f) L = applyFog(L, eye, eyeDir, primaryDist);
        if (!(L.x == L.x) || !(L.y == L.y) || !(L.z == L.z)) return glm::vec3(0.0f);
        return L;
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

    auto wrap = [](int a, int n) { const int m = a % n; return m < 0 ? m + n : m; };
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

glm::vec3 Environment::sample(const glm::vec3& dir) const {
    if (hasMap()) {
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

        auto wrapX = [this](int a) { const int m = a % width; return m < 0 ? m + width : m; };
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
    // No panorama: the flat ambient the raster path would have used, spread
    // over a horizon so that a surface facing up and one facing down are not
    // lit identically. Not a sky model, and not pretending to be one.
    const float t = glm::clamp(dir.y, -1.0f, 1.0f);
    if (t >= 0.0f) return glm::mix(horizon, zenith, std::sqrt(t));
    return glm::mix(horizon, ground, std::sqrt(-t));
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
    return best < kInf ? best : 0.0f;
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
    constexpr float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    glm::vec3 m = (x * (a * x + b)) / (x * (c * x + d) + e);
    m = glm::clamp(m, glm::vec3(0.0f), glm::vec3(1.0f));
    m = glm::pow(m, glm::vec3(1.0f / 2.2f));

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
    m_triangles    = static_cast<long long>(m_scene ? m_scene->triangles.size() : 0);
    m_buildSeconds = 0.0;
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
    Bvh bvh;
    bvh.build(sc.triangles);
    EnvSampler envDist;
    envDist.build(sc.env);
    m_buildSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - buildStart).count();

    const int W = m_settings.width, H = m_settings.height;
    const float aspect = static_cast<float>(W) / static_cast<float>(H);
    const CameraDesc& cam = sc.camera;
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
                                (cam.focusDistance / std::max(1e-4f, glm::dot(dir, cam.forward)));
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
                      ? tonemap(c, exposure, grade)
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

} // namespace pathtrace
