#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/graphics/Shader.hpp>

#include "FrameRender.hpp"

// Foam and spray kicked up at a hull moving through water.
//
// Two kinds share one pool. Airborne droplets (flat = 0) arc under gravity and
// fall back in; flat surface foam (flat = 1) clings to the waterline, drifts and
// spreads -- a gentle ring even at rest, a trailing wake when moving. Emission is
// the vehicle's business and stays with it; what lives here is the pool, its
// integration and the draw.

// One particle. `life0` is its lifespan at birth, so `life / life0` is the fade the
// shader wants without the shader needing to know how long it was meant to last.
struct SprayP {
    glm::vec3 pos{0.0f}, vel{0.0f};
    float     life  = 0.0f;
    float     life0 = 1.0f;
    float     size  = 1.0f;
    float     flat  = 0.0f; // 0 = airborne droplet, 1 = surface foam
};

// The pool, with no GL in sight -- integration is arithmetic, and keeping it that
// way is what makes the interesting half testable without a window.
class SprayPool {
public:
    // Longer than a wake needs, short enough that a boat driven in circles for an
    // hour can't grow the buffer without bound.
    static constexpr int kMax = 2200;

    // Add a particle. Silently dropped once the pool is full: at that point the
    // wake is already dense enough that nobody can tell, and the alternative is
    // an unbounded buffer.
    void add(const SprayP& p) {
        if (static_cast<int>(m_parts.size()) < kMax) m_parts.push_back(p);
    }

    // Age the pool by `dt` and drop what's spent. Droplets die of old age or on
    // hitting the water; foam only of old age.
    void update(float dt, float waterLevel);

    void clear() { m_parts.clear(); }
    bool empty() const { return m_parts.empty(); }
    int  count() const { return static_cast<int>(m_parts.size()); }
    const std::vector<SprayP>& particles() const { return m_parts; }

    // Pack for the GPU: 6 floats per particle -- pos(3), fade(1), size(1), flat(1).
    static constexpr int kStride = 6;
    void pack(std::vector<float>& out) const;

private:
    std::vector<SprayP> m_parts;
};

// The pool plus the GPU side: a stream buffer of soft point sprites.
class SpraySystem : public RendererBase {
public:
    SpraySystem() = default;
    ~SpraySystem() override;
    SpraySystem(const SpraySystem&)            = delete;
    SpraySystem& operator=(const SpraySystem&) = delete;

    // Load the shader and allocate the stream buffer. Needs a live GL context.
    // Returns false if the shader failed to compile.
    bool init();

    // False when init() failed or was never called. Worth asking before emitting:
    // filling a pool that will never be drawn is pure work.
    bool ready() const { return m_vao != 0; }

    void add(const SprayP& p) { m_pool.add(p); }
    void update(float dt, float waterLevel) { m_pool.update(dt, waterLevel); }
    void clear() { m_pool.clear(); }
    int  count() const { return m_pool.count(); }

    // Upload the live particles and draw them into the currently-bound target.
    // Leaves depth mask, blending and point-size state as it found them.
    void draw(const FrameContext& ctx) override;

    // Droplet scale, set by whichever vehicle is doing the splashing.
    float sizeScale = 1.0f;

private:
    SprayPool          m_pool;
    std::vector<float> m_scratch; // packed upload buffer, kept to avoid a per-frame alloc
    fitzel::Shader     m_shader;
    std::uint32_t      m_vao = 0, m_vbo = 0;
};
