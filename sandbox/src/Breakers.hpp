#pragma once

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

#include "ShoreMap.hpp"

// The breakers, on the CPU: the same waves water.vert draws where the sea meets
// the land, for whatever must feel them -- the spray thrown off a crest that
// is tipping over, a boat lifted and shoved by the surf. Kept here line for line
// with the shader (see water.vert, "The breakers"); a change to one is a change
// to both, or the spray flies off where no wave is breaking.
namespace ocean {

// What the breakers are this frame: set from the sea state and the author's
// dial in main.
struct Breakers {
    float height = 0.0f;    // of a wave arriving at the shore, before it shoals, m
    float length = 20.0f;   // between crests in the surf, m
    float speed  = 4.5f;    // how fast they run in, m/s
};

struct BreakerSample {
    bool      active   = false;
    glm::vec3 offset{0.0f};      // added to the water at rest here (lean + height)
    float     breaking = 0.0f;   // 0..1: the face of a breaking wave
    float     surf     = 0.0f;   // 0..1: churned water of the surf zone
    float     depth    = 1e3f;   // still water over the ground, m (< 0 on land)
    glm::vec2 dir{0.0f};         // toward the shore
    glm::vec3 velocity{0.0f};    // of the water here, m/s: the surf shoves
};

inline BreakerSample breakerAt(const ShoreMap& map, const Breakers& br, float x, float z, float t) {
    BreakerSample s;
    if (!map.valid() || br.height <= 0.0f) return s;
    const float inMap = map.inMap(x, z);
    if (inMap <= 0.0f) return s;
    const glm::vec4 sh = map.sample(x, z);
    const float depth = sh.x, dist = sh.y;
    const glm::vec2 dir(sh.z, sh.w);
    s.depth = depth;
    const float H0 = br.height, L = std::max(br.length, 2.0f);
    if (glm::dot(dir, dir) <= 0.01f || dist >= 6.0f * L || dist <= -0.6f * L) return s;
    auto smooth = [](float a, float b, float v) {
        const float q = std::clamp((v - a) / (b - a), 0.0f, 1.0f);
        return q * q * (3.0f - 2.0f * q);
    };
    const float k     = 6.2831853f / L;
    const float omega = k * br.speed;
    const float theta = -(k * dist + omega * t);
    const glm::vec2 along(-dir.y, dir.x);
    const float set   = 0.55f + 0.45f * std::sin(theta * 0.17f + glm::dot(glm::vec2(x, z), along) * 0.021f);
    const float feel  = 1.0f - smooth(0.25f * L, 0.6f * L, depth);
    const float dpos  = std::max(depth, 0.0f);
    float H = H0 * set * std::clamp(std::pow(0.25f * L / std::max(dpos, 0.25f), 0.25f), 1.0f, 1.9f);
    const float ratio = H / std::max(dpos, 0.05f);
    const float brk   = smooth(0.55f, 0.9f, ratio);
    H = H + (std::min(H, 0.78f * dpos + 0.08f) - H) * brk;
    const float c     = 0.5f + 0.5f * std::sin(theta);
    const float sharp = 1.2f + (3.5f - 1.2f) * std::max(brk, feel * 0.4f);
    float y = H * (std::pow(c, sharp) - 0.32f);
    if (depth <= 0.0f) {
        const float run = 0.35f * H0 * set * set;
        y = run * smooth(-0.35f * L, 0.0f, dist) * std::max(std::sin(theta) * 0.5f + 0.5f, 0.0f) - 0.05f;
    }
    const float q = 0.25f + (0.9f - 0.25f) * brk;
    const float w = feel * inMap;
    s.active   = true;
    s.dir      = dir;
    s.offset   = (glm::vec3(dir.x, 0.0f, dir.y) * (q * H * 0.5f * std::cos(theta)) + glm::vec3(0.0f, y, 0.0f)) * w;
    s.breaking = brk * smooth(0.72f, 0.97f, c) * (0.55f + 0.45f * smooth(0.3f, -0.3f, std::cos(theta))) * w;
    s.surf     = w * (1.0f - smooth(0.0f, std::max(H0 * 2.2f, 0.4f), depth));
    // The water's own motion: up and down with the crest (d y / d t), and
    // forward with it -- a broken bore carries the water in with it.
    const float dydt = H * sharp * std::pow(std::max(c, 1e-3f), sharp - 1.0f) * 0.5f * std::cos(theta) * (-omega);
    s.velocity = glm::vec3(dir.x, 0.0f, dir.y) * (br.speed * (0.15f + 0.6f * brk) * c) * w +
                 glm::vec3(0.0f, dydt * w, 0.0f);
    return s;
}

} // namespace ocean
