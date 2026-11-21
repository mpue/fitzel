#include "Ecology.hpp"

#include <algorithm>
#include <cmath>

namespace ecology {

namespace {

// Keep in step with ecology.glsl, operation for operation.
inline std::uint32_t hash(std::int32_t x, std::int32_t z, std::uint32_t seed) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 0x8da6b343u
                    ^ static_cast<std::uint32_t>(z) * 0xd8163841u
                    ^ seed * 0xcb1ab31fu;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}

inline float smooth01(float e0, float e1, float x) {
    const float t = glm::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

float noise(glm::vec2 p, std::uint32_t seed) {
    const float fx = std::floor(p.x), fz = std::floor(p.y);
    const std::int32_t ix = static_cast<std::int32_t>(fx);
    const std::int32_t iz = static_cast<std::int32_t>(fz);
    float tx = p.x - fx, tz = p.y - fz;
    tx = tx * tx * (3.0f - 2.0f * tx);
    tz = tz * tz * (3.0f - 2.0f * tz);
    const auto v = [&](int dx, int dz) {
        return static_cast<float>(hash(ix + dx, iz + dz, seed) & 0xffffffu) *
               (1.0f / 16777216.0f);
    };
    const float a = v(0, 0), b = v(1, 0), c = v(0, 1), d = v(1, 1);
    const float ab = a + (b - a) * tx;
    const float cd = c + (d - c) * tx;
    return ab + (cd - ab) * tz;
}

float fbm(glm::vec2 p, int octaves, std::uint32_t seed) {
    float sum = 0.0f, amp = 0.5f, norm = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        sum  += amp * noise(p, seed + static_cast<std::uint32_t>(i) * 101u);
        norm += amp;
        p     = p * 2.03f + glm::vec2(17.3f, -9.1f);
        amp  *= 0.5f;
    }
    return sum / norm;
}

glm::vec4 Mask::at(glm::vec2 xz) const {
    if (n < 2 || rgba.size() < static_cast<std::size_t>(n) * n * 4) return glm::vec4(0.0f);
    const float fx = (xz.x - x0) / cell, fz = (xz.y - z0) / cell;
    if (fx < 0.0f || fz < 0.0f || fx > static_cast<float>(n - 1) || fz > static_cast<float>(n - 1))
        return glm::vec4(0.0f);
    const int i0 = std::min(static_cast<int>(fx), n - 2), j0 = std::min(static_cast<int>(fz), n - 2);
    const float tx = fx - static_cast<float>(i0), tz = fz - static_cast<float>(j0);
    auto px = [&](int i, int j) {
        const std::uint8_t* p = &rgba[(static_cast<std::size_t>(j) * n + i) * 4];
        return glm::vec4(p[0], p[1], p[2], p[3]) * (1.0f / 255.0f);
    };
    return glm::mix(glm::mix(px(i0, j0), px(i0 + 1, j0), tx), glm::mix(px(i0, j0 + 1), px(i0 + 1, j0 + 1), tx), tz);
}

namespace {
float settledDiscs(const Params& P, glm::vec2 xz);
} // namespace

float settled(const Params& P, glm::vec2 xz) {
    const float s = settledDiscs(P, xz);
    if (!P.mask) return s;
    const glm::vec4 m = P.mask->at(xz);
    return glm::max(s * (1.0f - m.a), m.g * m.a);
}

namespace {
float settledDiscs(const Params& P, glm::vec2 xz) {
    float s = 0.0f;
    const int n = glm::clamp(P.towns, 0, Params::kTowns);
    if (n == 0) return 0.0f;
    const float rag = noise(xz / 23.0f, 0x7e11u) - 0.5f;
    for (int i = 0; i < n; ++i) {
        const glm::vec4 t = P.town[i];
        if (t.z <= 1.0f) continue;
        const float d = glm::length(xz - glm::vec2(t.x, t.y)) / t.z + 0.25f * rag;
        s = glm::max(s, 1.0f - smooth01(0.8f, 1.15f, d));
    }
    return s;
}
} // namespace

Sample sample(const Params& P, float x, float z, float h, float ny) {
    Sample s;
    const glm::vec2 xz(x, z);
    const float stands = fbm(xz / P.standSize, 4, 0x51edu);
    const float ragged = fbm(xz / (P.standSize * 0.18f), 2, 0x3a7cu);
    const float steep  = 1.0f - ny;
    const float f      = stands + 0.22f * (ragged - 0.5f) + P.slopeLove * steep;
    // An fBm sits around 0.5 with most of its mass within +-0.12, so the cut
    // that leaves `cover` of the ground wooded is a shift of the midpoint.
    const float cut    = 0.5f + (0.5f - P.cover) * 0.45f;
    s.settled = settled(P, xz);
    s.forest  = smooth01(cut - 0.035f, cut + 0.035f, f) * (1.0f - s.settled);
    float lone = P.solitary;
    if (P.mask) {
        // The map's own woods where it has them, the noise past its edge. The
        // ragged fBm still roughens the line a little: a wood's edge on a map is
        // drawn with a ruler, the wood's own is not.
        const glm::vec4 m = P.mask->at(xz);
        if (m.a > 0.0f) {
            const float mf = smooth01(0.30f, 0.70f, m.r + 0.18f * (ragged - 0.5f));
            s.forest  = glm::mix(s.forest, mf * (1.0f - m.g), m.a);
            lone      = glm::mix(lone, P.solitary * (1.0f + 1.5f * m.g) * (1.0f - m.b), m.a);
        }
    }
    s.edge    = 4.0f * s.forest * (1.0f - s.forest);
    float d   = glm::max(s.forest, lone);
    d *= smooth01(0.62f, 0.72f, ny);                          // cliffs stay bare
    const float tl = P.treeLine + (ragged - 0.5f) * 180.0f;
    d *= 1.0f - smooth01(tl - 140.0f, tl + 30.0f, h);         // the tree line
    d *= smooth01(P.waterLevel + 0.6f, P.waterLevel + 1.6f, h); // not in the lake
    s.density = d;
    return s;
}

} // namespace ecology
