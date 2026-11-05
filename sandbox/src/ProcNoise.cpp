#include "ProcNoise.hpp"

#include <algorithm>
#include <cmath>

namespace proc::noise {

namespace {

// One well-mixed number per lattice point and seed.
inline std::uint32_t hash(int x, int y, int z, int seed) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 0x8DA6B343u ^
                      static_cast<std::uint32_t>(y) * 0xD8163841u ^
                      static_cast<std::uint32_t>(z) * 0xCB1AB31Fu ^
                      static_cast<std::uint32_t>(seed) * 0x165667B1u;
    h ^= h >> 15; h *= 0x2C1B3C6Du;
    h ^= h >> 12; h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

inline std::uint32_t remix(std::uint32_t h, std::uint32_t salt) {
    h ^= salt * 0x9E3779B9u;
    h ^= h >> 16; h *= 0x7FEB352Du;
    h ^= h >> 15; h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

inline float unit(std::uint32_t h) { return static_cast<float>(h >> 8) / 16777216.0f; }

// Perlin's twelve edge directions (four of them twice, to make sixteen).
inline float grad(std::uint32_t h, float x, float y, float z) {
    switch (h & 15u) {
        case 0:  return  x + y;
        case 1:  return -x + y;
        case 2:  return  x - y;
        case 3:  return -x - y;
        case 4:  return  x + z;
        case 5:  return -x + z;
        case 6:  return  x - z;
        case 7:  return -x - z;
        case 8:  return  y + z;
        case 9:  return -y + z;
        case 10: return  y - z;
        case 11: return -y - z;
        case 12: return  x + y;
        case 13: return -x + y;
        case 14: return -y + z;
        default: return -y - z;
    }
}

inline float fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

inline float smoothstep(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

float perlin(const glm::vec3& p, int seed) {
    const float fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
    const int   x = static_cast<int>(fx), y = static_cast<int>(fy), z = static_cast<int>(fz);
    const float dx = p.x - fx, dy = p.y - fy, dz = p.z - fz;
    const float u = fade(dx), v = fade(dy), w = fade(dz);
    const float n000 = grad(hash(x,     y,     z,     seed), dx,        dy,        dz);
    const float n100 = grad(hash(x + 1, y,     z,     seed), dx - 1.0f, dy,        dz);
    const float n010 = grad(hash(x,     y + 1, z,     seed), dx,        dy - 1.0f, dz);
    const float n110 = grad(hash(x + 1, y + 1, z,     seed), dx - 1.0f, dy - 1.0f, dz);
    const float n001 = grad(hash(x,     y,     z + 1, seed), dx,        dy,        dz - 1.0f);
    const float n101 = grad(hash(x + 1, y,     z + 1, seed), dx - 1.0f, dy,        dz - 1.0f);
    const float n011 = grad(hash(x,     y + 1, z + 1, seed), dx,        dy - 1.0f, dz - 1.0f);
    const float n111 = grad(hash(x + 1, y + 1, z + 1, seed), dx - 1.0f, dy - 1.0f, dz - 1.0f);
    return lerp(lerp(lerp(n000, n100, u), lerp(n010, n110, u), v),
                lerp(lerp(n001, n101, u), lerp(n011, n111, u), v), w);
}

float value(const glm::vec3& p, int seed) {
    const float fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
    const int   x = static_cast<int>(fx), y = static_cast<int>(fy), z = static_cast<int>(fz);
    const float u = fade(p.x - fx), v = fade(p.y - fy), w = fade(p.z - fz);
    auto at = [&](int i, int j, int k) { return unit(hash(x + i, y + j, z + k, seed)); };
    return lerp(lerp(lerp(at(0, 0, 0), at(1, 0, 0), u), lerp(at(0, 1, 0), at(1, 1, 0), u), v),
                lerp(lerp(at(0, 0, 1), at(1, 0, 1), u), lerp(at(0, 1, 1), at(1, 1, 1), u), v), w);
}

Cells cells(const glm::vec3& p, int seed, float jitter) {
    const glm::vec3 base = glm::floor(p);
    const int bx = static_cast<int>(base.x), by = static_cast<int>(base.y), bz = static_cast<int>(base.z);
    const float j = std::clamp(jitter, 0.0f, 1.0f);
    float d1 = 1e30f, d2 = 1e30f;
    Cells c;
    for (int k = -1; k <= 1; ++k)
        for (int jy = -1; jy <= 1; ++jy)
            for (int i = -1; i <= 1; ++i) {
                const std::uint32_t h = hash(bx + i, by + jy, bz + k, seed);
                const glm::vec3 r(unit(h), unit(remix(h, 1u)), unit(remix(h, 2u)));
                const glm::vec3 at = base + glm::vec3(static_cast<float>(i), static_cast<float>(jy), static_cast<float>(k)) +
                                     glm::vec3(0.5f) + (r - glm::vec3(0.5f)) * j;
                const glm::vec3 d = at - p;
                const float dd = glm::dot(d, d);
                if (dd < d1) {
                    d2 = d1;
                    d1 = dd;
                    c.id = h;
                    c.at = at;
                } else if (dd < d2) {
                    d2 = dd;
                }
            }
    c.f1 = std::sqrt(d1);
    c.f2 = std::sqrt(d2);
    return c;
}

float cellRandom(std::uint32_t id, std::uint32_t salt) { return unit(remix(id, salt + 17u)); }

const std::vector<std::string>& kindLabels() {
    static const std::vector<std::string> l = {"Perlin", "Ridged", "Billow", "Cells", "Cracks", "Value"};
    return l;
}

float fractal(const Fractal& f, const glm::vec3& at) {
    const float size = std::max(std::fabs(f.size), 1e-4f);
    glm::vec3 p = (at + f.offset) / size;
    if (f.warp > 0.0f) {
        // A coarser field of its own seeds, one per axis.
        const glm::vec3 q = p * 0.5f;
        p += f.warp * glm::vec3(perlin(q + glm::vec3(17.13f, 3.71f, 9.27f), f.seed + 101),
                                perlin(q + glm::vec3(5.31f, 21.7f, 1.13f), f.seed + 202),
                                perlin(q + glm::vec3(11.9f, 7.77f, 27.3f), f.seed + 303));
    }
    const int   octaves = std::clamp(f.octaves, 1, 12);
    const float gain    = std::clamp(f.roughness, 0.0f, 1.0f);
    const float lac     = std::clamp(f.lacunarity, 1.0f, 8.0f);
    const float width   = std::clamp(f.crackWidth, 0.001f, 1.0f);
    float sum = 0.0f, norm = 0.0f, amp = 1.0f, freq = 1.0f;
    for (int o = 0; o < octaves; ++o) {
        // Each octave a little off the last one's lattice, so their zeros
        // never line up into a visible grid.
        const glm::vec3 q = p * freq + glm::vec3(19.19f, 7.73f, 31.1f) * static_cast<float>(o);
        const int seed = f.seed + o * 1013;
        float v = 0.0f;
        switch (f.kind) {
            case Kind::Perlin: v = std::clamp(perlin(q, seed) * 1.5f, -1.0f, 1.0f); break;
            case Kind::Ridged: {
                const float r = 1.0f - std::min(std::fabs(perlin(q, seed)) * 1.5f, 1.0f);
                v = r * r * 2.0f - 1.0f;
                break;
            }
            case Kind::Billow: v = std::min(std::fabs(perlin(q, seed)) * 1.5f, 1.0f) * 2.0f - 1.0f; break;
            case Kind::Cells: {
                const float d = std::min(cells(q, seed).f1, 1.0f);
                v = 1.0f - 2.0f * d * d;
                break;
            }
            case Kind::Cracks: {
                const Cells c = cells(q, seed);
                v = -(1.0f - smoothstep(0.0f, width, c.f2 - c.f1));
                break;
            }
            case Kind::Value: v = value(q, seed) * 2.0f - 1.0f; break;
        }
        sum  += v * amp;
        norm += amp;
        amp  *= gain;
        freq *= lac;
        if (amp < 1e-4f) break;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

} // namespace proc::noise
