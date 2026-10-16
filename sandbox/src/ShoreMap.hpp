#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

#include <glm/glm.hpp>

// Where the coast is, around the eye: for every few metres of water, how deep it
// is, how far it is to the shore and which way the shore lies.
//
// WHY THE SEA NEEDS TO KNOW. The open sea's waves (Ocean.hpp) run where the wind
// sends them, the same in a bay as mid-ocean. Real waves feel the bottom: as the
// water shallows they slow, crowd together, swing round to meet the beach square
// on, stand up taller -- and when a wave is about eight tenths as high as the
// water is deep, it breaks. All of that turns on depth and on the direction to
// the shore, and neither is anything the surface shader can find out for itself.
//
// So this samples the terrain (thread-safe; on a worker, every few seconds or
// when the eye has moved on) into a square of cells, marks water and land, and
// measures each cell's distance to the other kind -- positive out on the water,
// negative up the beach -- with the direction to the shore from its gradient,
// smoothed so a ragged coastline does not send the waves every which way. The
// water shaders read it as a texture (OceanRenderer), and the breakers come out
// of it (water.vert).
namespace ocean {

class ShoreMap {
public:
    using HeightFn = std::function<float(float, float)>;

    explicit ShoreMap(int cells = 256, float cellSize = 2.0f);
    ~ShoreMap();
    ShoreMap(const ShoreMap&) = delete;
    ShoreMap& operator=(const ShoreMap&) = delete;

    // Once a frame. Rebuilds on a worker when the eye has moved far enough, the
    // water level changed, or a few seconds have passed (the ground may have been
    // sculpted). `enabled` false (no ground, no water) drops the map.
    void update(glm::vec2 eye, float level, double now, HeightFn height, bool enabled);

    bool valid() const { return m_valid; }
    // Per cell: depth (m, < 0 on land), distance to the shore (m, < 0 on land),
    // and the direction toward the shore (unit, zero where there is none near).
    const std::vector<glm::vec4>& texels() const { return m_tex; }
    int       cells() const { return m_n; }
    glm::vec2 origin() const { return m_origin; }   // world xz of the lower corner
    float     extent() const { return m_n * m_cell; }
    std::uint64_t generation() const { return m_gen; }
    // One texel's worth, read the way the shader reads it (bilinear, texel
    // centres). Outside the map: no shore (deep, far, no direction).
    glm::vec4 sample(float x, float z) const;
    // 0 at the map's edge, 1 well inside: how much the shore counts here.
    float inMap(float x, float z) const;

    // The map built synchronously, for tests: `n` cells of `cell` metres from
    // `origin`, for water standing at `level`.
    static void build(int n, float cell, glm::vec2 origin, float level, const HeightFn& height,
                      std::vector<glm::vec4>& out);

private:
    int   m_n;
    float m_cell;
    bool  m_valid = false;
    glm::vec2 m_origin{0.0f};
    std::vector<glm::vec4> m_tex;
    std::uint64_t m_gen = 0;

    // The build in flight, if any.
    std::thread       m_worker;
    std::atomic<bool> m_done{false};
    bool              m_busy = false;
    std::vector<glm::vec4> m_next;
    glm::vec2 m_nextOrigin{0.0f};
    glm::vec2 m_builtEye{1e9f};
    float     m_builtLevel = -1e9f;
    double    m_builtAt = -1e9;
};

} // namespace ocean
