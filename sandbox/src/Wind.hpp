#pragma once

#include <cmath>
#include <cstdint>

#include <glm/glm.hpp>

#include <fitzel/graphics/Shader.hpp>

// --- The wind (see assets/shaders/wind.glsl) -----------------------------------
//
// One air for everything that moves in it. The host decides it once a frame --
// direction veering slowly, a mean strength from the scene and the storm, how
// gusty -- and every layer that sways hands the same state to its shader, each
// with its own gain (a blade of grass moves further than a trunk in the same
// breeze, but at the same moment). gust() is the CPU half of windGust, for the
// grass path tracer, which has to pose the blades the viewport shows.
namespace wind {

struct State {
    glm::vec2 dir{0.894f, 0.447f};   // unit XZ, where the air goes
    float     strength = 0.35f;      // 0 calm .. 1 stiff breeze .. 1.5 storm
    float     gustiness = 0.6f;      // 0 steady .. 1 very gusty
    float     time = 0.0f;           // the gust clock, seconds
};

inline std::uint32_t hash(std::int32_t x, std::int32_t z, std::uint32_t seed) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 0x8da6b343u
                    ^ static_cast<std::uint32_t>(z) * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return h;
}
inline float noise(glm::vec2 p, std::uint32_t seed) {
    const float fx = std::floor(p.x), fz = std::floor(p.y);
    const std::int32_t ix = static_cast<std::int32_t>(fx), iz = static_cast<std::int32_t>(fz);
    float tx = p.x - fx, tz = p.y - fz;
    tx = tx * tx * (3.0f - 2.0f * tx);
    tz = tz * tz * (3.0f - 2.0f * tz);
    const auto v = [&](int dx, int dz) {
        return static_cast<float>(hash(ix + dx, iz + dz, seed) & 0xffffffu) * (1.0f / 16777216.0f);
    };
    const float a = v(0, 0), b = v(1, 0), c = v(0, 1), d = v(1, 1);
    const float ab = a + (b - a) * tx, cd = c + (d - c) * tx;
    return ab + (cd - ab) * tz;
}
// windGust() of wind.glsl.
inline float gust(const State& w, glm::vec2 xz) {
    const float kSize = 55.0f, kSpeed = 7.0f;
    const glm::vec2 q = (xz - w.dir * (w.time * kSpeed)) / kSize;
    const float n = 0.62f * noise(q, 0x9e37u) +
                    0.38f * noise(q * 2.7f + glm::vec2(3.1f, 7.7f), 0x7f4au);
    const float t = glm::clamp((n - 0.30f) / 0.50f, 0.0f, 1.0f);
    const float g = t * t * (3.0f - 2.0f * t);
    return glm::mix(1.0f, glm::mix(0.35f, 1.5f, g), w.gustiness);
}

// wind.glsl's uniforms, with this layer's gain on the strength.
inline void apply(const fitzel::Shader& s, const State& w, float gain) {
    s.setVec2("uWindDir", w.dir);
    s.setFloat("uWindStrength", w.strength * gain);
    s.setFloat("uWindGust", w.gustiness);
    s.setFloat("uWindTime", w.time);
}

} // namespace wind
