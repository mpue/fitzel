#include "ProcRockTex.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <iterator>
#include <mutex>
#include <thread>

#include "ProcNodeKit.hpp"
#include "ProcNoise.hpp"
#include "UvOps.hpp"

namespace proc::rock {

namespace {

using noise::Fractal;
using noise::Kind;

// Raised whenever the painting below changes what it paints: the hash moves
// with it, so the editor paints scenes saved before the change again.
constexpr std::uint64_t kPainter = 7;

float smooth(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// What a kind of rock is made of, as sRGB colours and the strength of each of
// its features. From photographs of the real thing: granite is grey with black
// mica, glassy white quartz and pink feldspar in grains you can see; sandstone
// ochre in bands, the iron in it rusting some of them red; limestone a pale
// cream-grey, pitted where rain dissolved it; basalt near black with gas
// bubbles frozen in; slate blue-grey in thin, tilted sheets.
struct Palette {
    glm::vec3 base, dark, light, accent;
    float     grains;       // share of the colour that is visible crystals
    float     layerShow;    // how much the Layers setting shows on this rock
    float     layerPeriod;  // metres between bands
    float     layerTilt;    // degrees off level
    float     pits;         // share of grain cells that are holes
    float     rough;
};
const Palette& palette(int rock) {
    static const Palette p[] = {
        // Granite (weathered: the crystals greyed over, seen up close only)
        {{0.49f, 0.48f, 0.46f}, {0.16f, 0.16f, 0.17f}, {0.76f, 0.75f, 0.72f}, {0.62f, 0.52f, 0.47f},
         0.38f, 0.15f, 0.30f, 12.0f, 0.00f, 0.74f},
        // Sandstone
        {{0.66f, 0.52f, 0.38f}, {0.50f, 0.37f, 0.26f}, {0.78f, 0.67f, 0.52f}, {0.60f, 0.36f, 0.23f},
         0.00f, 1.00f, 0.11f, 6.0f, 0.02f, 0.86f},
        // Limestone
        {{0.75f, 0.73f, 0.67f}, {0.50f, 0.48f, 0.44f}, {0.87f, 0.86f, 0.81f}, {0.71f, 0.65f, 0.50f},
         0.00f, 0.45f, 0.24f, 4.0f, 0.10f, 0.80f},
        // Basalt
        {{0.24f, 0.24f, 0.25f}, {0.10f, 0.10f, 0.11f}, {0.38f, 0.37f, 0.36f}, {0.41f, 0.31f, 0.25f},
         0.00f, 0.10f, 0.40f, 3.0f, 0.18f, 0.78f},
        // Slate
        {{0.29f, 0.31f, 0.34f}, {0.16f, 0.17f, 0.19f}, {0.42f, 0.44f, 0.46f}, {0.37f, 0.34f, 0.29f},
         0.00f, 1.00f, 0.035f, 24.0f, 0.00f, 0.66f},
    };
    return p[std::clamp(rock, 0, static_cast<int>(std::size(p)) - 1)];
}

// The threshold a noise of about unit spread has to pass for `amount` (0..1)
// of what is eligible to be covered: 0 covers half, higher covers less.
float cover(float amount) { return (0.5f - std::clamp(amount, 0.0f, 1.0f)) * 3.2f; }

Fractal fr(Kind k, float size, int octaves, float rough, float warp, int seed) {
    Fractal f;
    f.kind = k; f.size = size; f.octaves = octaves; f.roughness = rough; f.warp = warp; f.seed = seed;
    return f;
}

// Where on the rock a texel lies and what the shape is like there.
struct Spot {
    glm::vec3 p, n;
    float     convex;   // > 0 on edges and humps, < 0 in hollows
    float     low;      // height above the bottom, as a share of the whole height
};
struct Texel {
    glm::vec3 col{0.0f};
    float     rough = 0.8f, ao = 1.0f;
    float     h = 0.0f;   // metres out of the surface, for the normal map
};

class Painter {
public:
    explicit Painter(const Look& l) : L(l), P(palette(l.rock)) {
        // The bedding planes lean a few degrees, about an axis the seed picks.
        const float a = proc::random01(3, l.seed) * kit::kTau;
        const glm::vec3 axis(std::cos(a), 0.0f, std::sin(a));
        const float t = glm::radians(P.layerTilt * (0.6f + 0.8f * proc::random01(4, l.seed)));
        layerUp = glm::vec3(glm::rotate(glm::mat4(1.0f), t, axis) * glm::vec4(0.0f, 1.0f, 0.0f, 0.0f));
        sd = l.seed * 7919;
        g  = std::clamp(l.grain, 0.002f, 1.0f);
    }

    // Everything at one texel. Without `colour` only what moves the height
    // is worked out -- the normal map asks for the height twice more nearby.
    Texel sample(const Spot& s, bool colour) const {
        const glm::vec3& p = s.p;
        const glm::vec3& n = s.n;
        Texel t;
        t.col   = P.base;
        t.rough = P.rough;
        if (colour) {
            // A rock is never one colour: broad drifts of light and dark,
            // and of its accent mineral.
            const float big = noise::fractal(fr(Kind::Perlin, 1.4f, 3, 0.5f, 0.6f, sd + 1), p);
            const float hue = noise::fractal(fr(Kind::Perlin, 0.9f, 2, 0.5f, 0.4f, sd + 2), p);
            t.col = P.base * (1.0f + 0.14f * big);
            t.col = glm::mix(t.col, P.accent, smooth(0.15f, 0.85f, hue) * 0.4f);
        }
        // Layers: bands of sediment, each its own shade, the harder ones
        // standing out a little.
        const float layer = std::clamp(L.layers, 0.0f, 1.0f) * P.layerShow;
        if (layer > 0.0f) {
            const float wob = noise::fractal(fr(Kind::Perlin, P.layerPeriod * 6.0f, 3, 0.5f, 0.0f, sd + 3), p);
            const float d   = glm::dot(p, layerUp) / P.layerPeriod + 1.2f * wob;
            const float band = std::sin(d * kit::kTau) * 0.6f + std::sin(d * 2.37f * kit::kTau + 1.3f) * 0.4f;
            if (colour) {
                const float r = noise::value(glm::vec3(std::floor(d), 0.5f, 0.5f), sd + 4);
                t.col = glm::mix(t.col, r > 0.5f ? P.light : P.dark, layer * (0.2f + 0.4f * std::fabs(r * 2.0f - 1.0f)));
            }
            t.h += layer * 0.0025f * band;
        }
        // Crystals you can see (granite): every grain one mineral.
        if (P.grains > 0.0f) {
            const noise::Cells c = noise::cells(p / g, sd + 5, 0.9f);
            const float r = noise::cellRandom(c.id, 1);
            if (colour) {
                const glm::vec3 m = r < 0.10f ? P.dark : r < 0.28f ? P.light : r < 0.50f ? P.accent : t.col;
                t.col = glm::mix(t.col, m * (0.9f + 0.2f * noise::cellRandom(c.id, 2)), P.grains);
            }
            if (r >= 0.10f && r < 0.28f) t.rough -= 0.3f * P.grains;   // glassy quartz
            t.h += g * 0.03f * (noise::cellRandom(c.id, 3) - 0.5f) * P.grains;
            t.h -= g * 0.02f * (1.0f - smooth(0.0f, 0.08f, c.f2 - c.f1)) * P.grains;   // between the grains
        }
        // Grit, on every rock.
        const float grit = noise::value(p / (g * 0.5f), sd + 6) * 0.6f + noise::value(p / (g * 0.17f), sd + 7) * 0.4f;
        if (colour) t.col *= 0.92f + 0.16f * grit;
        t.h += g * 0.02f * (grit - 0.5f);
        // Pits: dissolved (limestone), or the gas bubbles of a lava (basalt).
        if (P.pits > 0.0f) {
            const noise::Cells c = noise::cells(p / (g * 2.5f), sd + 8, 1.0f);
            const float r   = noise::cellRandom(c.id, 4);
            const float rad = 0.12f + 0.22f * noise::cellRandom(c.id, 5);
            const float pit = r < P.pits ? 1.0f - smooth(rad * 0.6f, rad, c.f1) : 0.0f;
            if (colour) t.col *= 1.0f - 0.55f * pit;
            t.ao *= 1.0f - 0.4f * pit;
            t.h  -= g * 0.12f * pit;
        }
        // Cracks: hair lines and fissures, in some parts of the rock only.
        if (L.cracks > 0.0f) {
            Fractal cf = fr(Kind::Cracks, 0.45f, 2, 0.45f, 0.8f, sd + 9);
            cf.crackWidth = 0.05f;
            const float line  = -noise::fractal(cf, p);
            const float where = smooth(-0.2f, 0.4f, noise::fractal(fr(Kind::Perlin, 0.8f, 2, 0.5f, 0.0f, sd + 10), p));
            const float k = std::clamp(line * where * L.cracks, 0.0f, 1.0f);
            if (colour) t.col *= 1.0f - 0.6f * k;
            t.ao    *= 1.0f - 0.5f * k;
            t.h     -= 0.006f * k;
            t.rough += 0.05f * k;
        }
        // What the weather does by the shape: edges rubbed light and smooth,
        // dirt and shade in the hollows, streaks down the steep sides where
        // the rain runs, and earth splashed up the foot.
        const float edge = smooth(0.08f, 0.5f, s.convex);
        const float cav  = smooth(0.05f, 0.45f, -s.convex);
        if (colour) {
            const float wn = 0.6f + 0.4f * noise::fractal(fr(Kind::Perlin, 0.15f, 2, 0.5f, 0.0f, sd + 11), p);
            t.col = glm::mix(t.col, glm::min(P.light * 1.08f, glm::vec3(1.0f)), std::clamp(edge * L.wear * wn * 0.55f, 0.0f, 1.0f));
            t.col = glm::mix(t.col, glm::vec3(0.25f, 0.21f, 0.16f), std::clamp(cav * L.dirt * 0.75f, 0.0f, 1.0f));
            if (L.dirt > 0.0f) {
                const float steep = 1.0f - std::fabs(n.y);
                const float st = noise::fractal(fr(Kind::Perlin, 1.0f, 3, 0.55f, 0.0f, sd + 12),
                                                glm::vec3(p.x * 7.0f, p.y * 0.5f, p.z * 7.0f));
                t.col *= 1.0f - L.dirt * 0.3f * steep * smooth(0.05f, 0.6f, st);
            }
        }
        t.rough -= 0.1f * edge * L.wear;
        t.ao    *= 1.0f - 0.45f * cav;
        const float footLine = 0.16f + 0.1f * noise::fractal(fr(Kind::Perlin, 0.5f, 2, 0.5f, 0.0f, sd + 13), p);
        const float foot = 1.0f - smooth(0.02f, footLine, s.low);
        if (colour) t.col = glm::mix(t.col, glm::vec3(0.30f, 0.24f, 0.17f), std::clamp(foot * L.dirt * 0.85f, 0.0f, 1.0f));
        t.ao    *= 1.0f - 0.35f * foot * (0.5f + 0.5f * L.dirt);
        t.rough += 0.1f * foot * L.dirt;
        // Lichen: crusts in lobed patches a hand to a forearm across, mostly
        // pale grey-green, some dark, now and then orange -- one kind to a
        // patch -- on everything that sees the sky, and loose rosettes round
        // them.
        if (L.lichen > 0.0f) {
            const float sky   = smooth(-0.3f, 0.6f, n.y) * (1.0f - 0.7f * cav) * (1.0f - foot);
            const float patch = noise::fractal(fr(Kind::Perlin, 0.22f, 4, 0.55f, 0.8f, sd + 14), p);
            const float score = patch / 0.35f + 1.0f * (sky - 0.7f);
            const float z0    = cover(L.lichen);
            // Crisp, lobed edges; rock showing through in specks inside.
            const float holes = smooth(0.25f, 0.45f, noise::value(p / 0.012f, sd + 22));
            float k = smooth(z0 - 0.08f, z0 + 0.08f, score) * (0.55f + 0.45f * holes);
            const noise::Cells c = noise::cells(p / 0.035f, sd + 15, 1.0f);
            const float wobble  = 0.75f + 0.5f * noise::value(p / 0.01f, sd + 23);
            const float rosette = (1.0f - smooth(0.18f, 0.32f, c.f1 * wobble)) * smooth(z0 - 0.7f, z0 - 0.15f, score);
            k = std::max(k, rosette * (noise::cellRandom(c.id, 6) < 0.3f ? 0.85f : 0.0f));
            if (k > 0.0f) {
                // The kind: by a coarse cell, so a whole patch is one.
                const noise::Cells kind = noise::cells(p / 0.3f, sd + 18, 1.0f);
                const float r = noise::cellRandom(kind.id, 7);
                const glm::vec3 lc = r < 0.06f ? glm::vec3(0.72f, 0.55f, 0.27f)
                                   : r < 0.18f ? glm::vec3(0.41f, 0.44f, 0.37f)
                                   : r < 0.64f ? glm::vec3(0.62f, 0.65f, 0.57f)
                                               : glm::vec3(0.74f, 0.75f, 0.70f);
                const float mottle = 0.82f + 0.3f * noise::value(p / 0.008f, sd + 19);
                // The rim of a patch darker, as the crust grows out from it.
                const float rim = smooth(z0 - 0.08f, z0 + 0.02f, score) - smooth(z0 + 0.02f, z0 + 0.25f, score);
                if (colour) t.col = glm::mix(t.col, lc * mottle * (1.0f - 0.35f * rim), k * 0.9f);
                t.h    += 0.0012f * k;
                t.rough = glm::mix(t.rough, 0.92f, k);
            }
        }
        // Moss: a carpet on top, creeping down the sides and up from the
        // foot, where water stays.
        if (L.moss > 0.0f) {
            const float top   = smooth(0.15f, 0.8f, n.y);
            const float m     = noise::fractal(fr(Kind::Perlin, 0.35f, 4, 0.55f, 0.7f, sd + 16), p);
            const float score = m / 0.35f + 2.6f * (std::min(top + 0.5f * cav + 0.6f * foot, 1.0f) - 0.65f);
            const float z0    = cover(L.moss);
            const float fluff = noise::value(p / 0.005f, sd + 17) * 0.6f + noise::value(p / 0.02f, sd + 20) * 0.4f;
            // Thinning out at its edge into single tufts.
            const float k = std::clamp(smooth(z0 - 0.35f, z0 + 0.3f, score) * (0.6f + 0.8f * fluff) - 0.15f, 0.0f, 1.0f);
            if (k > 0.0f) {
                const float lit = noise::fractal(fr(Kind::Perlin, 0.12f, 2, 0.5f, 0.0f, sd + 21), p) * 0.5f + 0.5f;
                if (colour)
                    t.col = glm::mix(t.col, glm::mix(glm::vec3(0.15f, 0.19f, 0.07f), glm::vec3(0.36f, 0.42f, 0.16f), lit * 0.6f + fluff * 0.4f), k);
                t.h    += k * (0.004f + 0.004f * fluff);
                t.rough = glm::mix(t.rough, 0.97f, k);
                t.ao   *= 1.0f - 0.3f * k * (1.0f - fluff);
            }
        }
        t.col   = glm::clamp(t.col * L.tint, glm::vec3(0.0f), glm::vec3(1.0f));
        t.rough = std::clamp(t.rough, 0.05f, 1.0f);
        t.ao    = std::clamp(t.ao, 0.0f, 1.0f);
        return t;
    }

private:
    const Look&    L;
    const Palette& P;
    glm::vec3      layerUp{0.0f, 1.0f, 0.0f};
    int            sd = 0;
    float          g  = 0.03f;
};

// --- What the maps are made from ---------------------------------------------------

struct Hasher {
    std::uint64_t h = 0xCBF29CE484222325ull;
    void bytes(const void* p, std::size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 0x100000001B3ull; }
    }
    template <class T> void put(const T& v) { bytes(&v, sizeof v); }
};

std::uint64_t lookHash(const EditMesh& m, const Look& l) {
    Hasher h;
    h.put(kPainter);
    h.put(l.rock); h.put(l.tint); h.put(l.grain); h.put(l.bumps); h.put(l.layers); h.put(l.cracks);
    h.put(l.wear); h.put(l.dirt); h.put(l.lichen); h.put(l.moss); h.put(l.size); h.put(l.seed);
    if (!m.verts.empty()) h.bytes(m.verts.data(), m.verts.size() * sizeof(glm::vec3));
    for (const std::vector<int>& f : m.faces) {
        h.put(f.size());
        if (!f.empty()) h.bytes(f.data(), f.size() * sizeof(int));
    }
    return h.h;
}

// Painted maps and the coordinates they were painted for, for the meshes seen
// last. A handful -- the graph being edited, the node picked in it and an undo
// step or two each way -- and no more than a few hundred megabytes of them.
struct Cached {
    std::uint64_t                    hash = 0;
    std::vector<EditMesh::FaceUV>    uvs;
    std::shared_ptr<const BakedMaps> maps;
};
std::mutex         g_cacheMu;
std::deque<Cached> g_cache;
constexpr std::size_t kCacheSize  = 6;
constexpr std::size_t kCacheBytes = std::size_t(256) << 20;

// --- Painting ----------------------------------------------------------------------

// One triangle of a face (faces are fans from their first corner, as the
// renderer draws them), with what a texel inside it needs.
struct Tri {
    glm::vec3 p[3], n[3];
    float     cv[3];
    glm::vec2 uv[3];      // in texels
    glm::vec3 T, B;       // the frame the shader builds from the derivatives (lit.frag applyNormalMap)
    float     step = 0.01f;   // metres a texel spans here
};

// Run `fn(row)` for every row on all cores.
template <class F>
void eachRow(int rows, F&& fn) {
    const unsigned n = std::max(1u, std::min(std::thread::hardware_concurrency(), 32u));
    std::atomic<int> next{0};
    auto work = [&] {
        for (int r; (r = next.fetch_add(1)) < rows;) fn(r);
    };
    std::vector<std::thread> pool;
    for (unsigned i = 1; i < n; ++i) pool.emplace_back(work);
    work();
    for (std::thread& t : pool) t.join();
}

std::shared_ptr<BakedMaps> paint(const EditMesh& m, const Look& look) {
    const int R = std::clamp(look.size, 16, 4096);
    const kit::Welded w = kit::weld(m, true);
    // How much of a hump or a hollow every point is: how far it stands out of
    // the ring of its neighbours along its normal, against the length of the
    // edges there -- then evened out over its neighbours twice.
    std::vector<float> convex(w.pos.size(), 0.0f);
    for (std::size_t v = 0; v < w.pos.size(); ++v) {
        const std::vector<int>& nb = w.next[v];
        if (nb.empty()) continue;
        glm::vec3 mid(0.0f);
        float len = 0.0f;
        for (int u : nb) {
            mid += w.pos[static_cast<std::size_t>(u)];
            len += glm::length(w.pos[static_cast<std::size_t>(u)] - w.pos[v]);
        }
        mid /= static_cast<float>(nb.size());
        len /= static_cast<float>(nb.size());
        convex[v] = glm::dot(w.pos[v] - mid, w.nrm[v]) / std::max(len, 1e-6f);
    }
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<float> next = convex;
        for (std::size_t v = 0; v < w.pos.size(); ++v) {
            if (w.next[v].empty()) continue;
            float s = 0.0f;
            for (int u : w.next[v]) s += convex[static_cast<std::size_t>(u)];
            next[v] = 0.5f * convex[v] + 0.5f * s / static_cast<float>(w.next[v].size());
        }
        convex.swap(next);
    }
    glm::vec3 lo, hi;
    m.bounds(lo, hi);
    const float tall = std::max(hi.y - lo.y, 1e-3f);

    std::vector<Tri> tris;
    for (int f = 0; f < static_cast<int>(m.faces.size()); ++f) {
        if (!m.hasCornerUvs(f) || !m.validFace(f)) continue;
        const std::vector<int>& fv = m.faces[static_cast<std::size_t>(f)];
        const std::vector<EditMesh::FaceUV::Corner>& uc = m.faceUV[static_cast<std::size_t>(f)].corners;
        for (std::size_t k = 1; k + 1 < fv.size(); ++k) {
            Tri t;
            const std::size_t c[3] = {0, k, k + 1};
            for (int i = 0; i < 3; ++i) {
                const int v = fv[c[i]];
                const int gi = w.group[static_cast<std::size_t>(v)];
                t.p[i]  = m.verts[static_cast<std::size_t>(v)];
                t.n[i]  = w.nrm[static_cast<std::size_t>(gi)];
                t.cv[i] = convex[static_cast<std::size_t>(gi)];
                t.uv[i] = uc[c[i]].uv * static_cast<float>(R);
            }
            const glm::vec3 e1 = t.p[1] - t.p[0], e2 = t.p[2] - t.p[0];
            const glm::vec2 d1 = t.uv[1] - t.uv[0], d2 = t.uv[2] - t.uv[0];
            const glm::vec3 fn = glm::cross(e1, e2);
            const float area3 = 0.5f * glm::length(fn);
            const float areaT = 0.5f * std::fabs(d1.x * d2.y - d1.y * d2.x);
            if (area3 < 1e-12f || areaT < 1e-8f) continue;
            // The gradients of u and v along the face, as the shader's
            // cotangent frame has them (up to a common scale).
            const glm::vec3 nf = fn / (2.0f * area3);
            const glm::vec3 p2 = glm::cross(e2, nf), p1 = glm::cross(nf, e1);
            t.T = p2 * d1.x + p1 * d2.x;
            t.B = p2 * d1.y + p1 * d2.y;
            t.step = std::sqrt(area3 / areaT);
            tris.push_back(t);
        }
    }

    // Which triangle every texel centre is in, and where in it.
    struct Hit { int tri = -1; float a = 0.0f, b = 0.0f; };
    std::vector<Hit> hits(static_cast<std::size_t>(R) * R);
    for (int ti = 0; ti < static_cast<int>(tris.size()); ++ti) {
        const Tri& t = tris[static_cast<std::size_t>(ti)];
        const glm::vec2 a = t.uv[0], b = t.uv[1], c = t.uv[2];
        const float den = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
        if (std::fabs(den) < 1e-12f) continue;
        const int x0 = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
        const int x1 = std::min(R - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
        const int y0 = std::max(0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
        const int y1 = std::min(R - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const glm::vec2 q(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
                const float l0 = ((b.y - c.y) * (q.x - c.x) + (c.x - b.x) * (q.y - c.y)) / den;
                const float l1 = ((c.y - a.y) * (q.x - c.x) + (a.x - c.x) * (q.y - c.y)) / den;
                const float l2 = 1.0f - l0 - l1;
                if (l0 < -1e-4f || l1 < -1e-4f || l2 < -1e-4f) continue;
                hits[static_cast<std::size_t>(y) * R + x] = {ti, l1, l2};
            }
    }

    // Paint, a row at a time on every core. Rows are counted from the bottom
    // here (v up, as the texture coordinates run) and written top row first.
    auto out = std::make_shared<BakedMaps>();
    out->w = out->h = R;
    const std::size_t texels = static_cast<std::size_t>(R) * R;
    out->color.assign(texels * 4, 0);
    out->normal.assign(texels * 4, 0);
    out->orm.assign(texels * 4, 0);
    std::vector<char> filled(texels, 0);
    const Painter painter(look);
    const float bumps = std::max(look.bumps, 0.0f);
    auto put = [&](std::vector<std::uint8_t>& img, std::size_t at, float r, float g, float b) {
        img[at * 4 + 0] = static_cast<std::uint8_t>(std::lround(std::clamp(r, 0.0f, 1.0f) * 255.0f));
        img[at * 4 + 1] = static_cast<std::uint8_t>(std::lround(std::clamp(g, 0.0f, 1.0f) * 255.0f));
        img[at * 4 + 2] = static_cast<std::uint8_t>(std::lround(std::clamp(b, 0.0f, 1.0f) * 255.0f));
        img[at * 4 + 3] = 255;
    };
    // Where texel (x, y) is on the rock, and which way the surface faces there.
    auto spotAt = [&](const Hit& h) {
        const Tri& t = tris[static_cast<std::size_t>(h.tri)];
        const float a0 = 1.0f - h.a - h.b;
        Spot s;
        s.p = t.p[0] * a0 + t.p[1] * h.a + t.p[2] * h.b;
        const glm::vec3 n = t.n[0] * a0 + t.n[1] * h.a + t.n[2] * h.b;
        s.n      = glm::dot(n, n) > 1e-12f ? glm::normalize(n) : glm::vec3(0.0f, 1.0f, 0.0f);
        s.convex = t.cv[0] * a0 + t.cv[1] * h.a + t.cv[2] * h.b;
        s.low    = (s.p.y - lo.y) / tall;
        return s;
    };
    std::vector<float> height(texels, 0.0f);
    eachRow(R, [&](int y) {
        for (int x = 0; x < R; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * R + x;
            if (hits[i].tri < 0) continue;
            const Texel c = painter.sample(spotAt(hits[i]), true);
            height[i] = c.h;
            const std::size_t at = static_cast<std::size_t>(R - 1 - y) * R + x;
            put(out->color, at, c.col.x, c.col.y, c.col.z);
            put(out->orm, at, c.ao, c.rough, 1.0f);
            filled[at] = 1;
        }
    });
    // The normal map: the height's slope across the surface, fitted to the
    // texels round each one that lie beside it on the rock too (a neighbour
    // in the tile may be another piece, far away), tilting the normal -- then
    // said in the frame the shader will read it in (lit.frag applyNormalMap).
    eachRow(R, [&](int y) {
        for (int x = 0; x < R; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * R + x;
            const Hit& h = hits[i];
            if (h.tri < 0) continue;
            const Tri& t = tris[static_cast<std::size_t>(h.tri)];
            const Spot s = spotAt(h);
            glm::vec3 nt(0.0f, 0.0f, 1.0f);
            if (bumps > 0.0f) {
                const glm::vec3 helper = std::fabs(s.n.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
                const glm::vec3 tu = glm::normalize(glm::cross(helper, s.n));
                const glm::vec3 tv = glm::cross(s.n, tu);
                float a00 = 0.0f, a01 = 0.0f, a11 = 0.0f, b0 = 0.0f, b1 = 0.0f;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int xx = x + dx, yy = y + dy;
                        if ((!dx && !dy) || xx < 0 || yy < 0 || xx >= R || yy >= R) continue;
                        const std::size_t j = static_cast<std::size_t>(yy) * R + xx;
                        if (hits[j].tri < 0) continue;
                        const glm::vec3 d = spotAt(hits[j]).p - s.p;
                        if (glm::dot(d, d) > 9.0f * t.step * t.step) continue;
                        const float du = glm::dot(d, tu), dv = glm::dot(d, tv), dh = height[j] - height[i];
                        a00 += du * du; a01 += du * dv; a11 += dv * dv;
                        b0 += du * dh;  b1 += dv * dh;
                    }
                const float det = a00 * a11 - a01 * a01;
                if (det > 1e-6f * t.step * t.step * t.step * t.step) {
                    const float gu = (a11 * b0 - a01 * b1) / det, gv = (a00 * b1 - a01 * b0) / det;
                    const glm::vec3 want = glm::normalize(s.n - bumps * (gu * tu + gv * tv));
                    const float sc = 1.0f / std::sqrt(std::max(std::max(glm::dot(t.T, t.T), glm::dot(t.B, t.B)), 1e-20f));
                    const glm::mat3 frame(t.T * sc, t.B * sc, s.n);
                    if (std::fabs(glm::determinant(frame)) > 1e-6f) {
                        nt = glm::inverse(frame) * want;
                        nt = glm::dot(nt, nt) > 1e-12f ? glm::normalize(nt) : glm::vec3(0.0f, 0.0f, 1.0f);
                        if (nt.z < 0.05f) nt = glm::normalize(glm::vec3(nt.x, nt.y, 0.05f));
                    }
                }
            }
            const std::size_t at = static_cast<std::size_t>(R - 1 - y) * R + x;
            put(out->normal, at, nt.x * 0.5f + 0.5f, nt.y * 0.5f + 0.5f, nt.z * 0.5f + 0.5f);
        }
    });

    // The gaps between the pieces take the colour of the piece beside them,
    // a ring of texels at a time, so neither filtering nor the smaller mips
    // pull in black at a seam; whatever is left is the average.
    double rough = 0.0;
    std::size_t count = 0;
    for (std::size_t i = 0; i < texels; ++i)
        if (filled[i]) { rough += out->orm[i * 4 + 1]; ++count; }
    out->roughness = count ? static_cast<float>(rough / (255.0 * count)) : 1.0f;
    std::vector<std::uint8_t>* maps[3] = {&out->color, &out->normal, &out->orm};
    const int rings = std::max(4, R / 128);
    std::vector<char> grown;
    for (int pass = 0; pass < rings; ++pass) {
        grown = filled;
        std::atomic<bool> any{false};
        // A ring is read from what was filled before it and written only
        // where nothing was: rows on different cores never meet.
        eachRow(R, [&](int y) {
            for (int x = 0; x < R; ++x) {
                const std::size_t at = static_cast<std::size_t>(y) * R + x;
                if (filled[at]) continue;
                int sum[3][3] = {}, k = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= R || yy >= R) continue;
                        const std::size_t o = static_cast<std::size_t>(yy) * R + xx;
                        if (!filled[o]) continue;
                        for (int m3 = 0; m3 < 3; ++m3)
                            for (int ch = 0; ch < 3; ++ch) sum[m3][ch] += (*maps[m3])[o * 4 + static_cast<std::size_t>(ch)];
                        ++k;
                    }
                if (!k) continue;
                for (int m3 = 0; m3 < 3; ++m3) {
                    for (int ch = 0; ch < 3; ++ch)
                        (*maps[m3])[at * 4 + static_cast<std::size_t>(ch)] = static_cast<std::uint8_t>(sum[m3][ch] / k);
                    (*maps[m3])[at * 4 + 3] = 255;
                }
                grown[at] = 1;
                any = true;
            }
        });
        filled.swap(grown);
        if (!any) break;
    }
    std::uint64_t mean[3][3] = {};
    count = 0;
    for (std::size_t i = 0; i < texels; ++i)
        if (filled[i]) {
            for (int m3 = 0; m3 < 3; ++m3)
                for (int ch = 0; ch < 3; ++ch) mean[m3][ch] += (*maps[m3])[i * 4 + static_cast<std::size_t>(ch)];
            ++count;
        }
    for (std::size_t i = 0; i < texels; ++i) {
        if (filled[i]) continue;
        for (int m3 = 0; m3 < 3; ++m3) {
            for (int ch = 0; ch < 3; ++ch)
                (*maps[m3])[i * 4 + static_cast<std::size_t>(ch)] =
                    static_cast<std::uint8_t>(count ? mean[m3][ch] / count : (m3 == 1 ? (ch == 2 ? 255 : 128) : 128));
            (*maps[m3])[i * 4 + 3] = 255;
        }
    }
    return out;
}

} // namespace

float heightAt(const Look& look, const glm::vec3& p, const glm::vec3& n, float convex, float low) {
    return Painter(look).sample({p, n, convex, low}, false).h;
}

const std::vector<std::string>& rockLabels() {
    static const std::vector<std::string> l = {"Granite", "Sandstone", "Limestone", "Basalt", "Slate"};
    return l;
}

std::shared_ptr<const BakedMaps> bake(EditMesh& m, const Look& look, std::uint64_t* hashOut) {
    const std::uint64_t h = lookHash(m, look);
    if (hashOut) *hashOut = h;
    {
        std::lock_guard<std::mutex> lock(g_cacheMu);
        for (auto it = g_cache.begin(); it != g_cache.end(); ++it)
            if (it->hash == h) {
                Cached c = *it;
                g_cache.erase(it);
                g_cache.push_front(c);
                m.faceUV = c.uvs;
                return c.maps;
            }
    }
    std::vector<int> faces;
    for (int f = 0; f < static_cast<int>(m.faces.size()); ++f)
        if (m.validFace(f)) faces.push_back(f);
    if (faces.empty()) return nullptr;
    const int R = std::clamp(look.size, 16, 4096);
    // Pieces leaning up to 55 degrees off their direction: more pieces than
    // Blender's 66, less stretch on a round rock. Four texels between them.
    uvops::smartProject(m, faces, 55.0f, 4.0f / static_cast<float>(R));
    std::shared_ptr<const BakedMaps> maps = paint(m, look);
    std::lock_guard<std::mutex> lock(g_cacheMu);
    g_cache.push_front({h, m.faceUV, maps});
    auto bytes = [] {
        std::size_t b = 0;
        for (const Cached& c : g_cache)
            if (c.maps) b += c.maps->color.size() + c.maps->normal.size() + c.maps->orm.size();
        return b;
    };
    while (g_cache.size() > kCacheSize || (g_cache.size() > 1 && bytes() > kCacheBytes)) g_cache.pop_back();
    return maps;
}

} // namespace proc::rock
