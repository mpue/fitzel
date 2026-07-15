#pragma once

#include <cstdint>

#include <glm/glm.hpp>

#include <fitzel/graphics/Shader.hpp>

#include "FrameRender.hpp"

// How hard it is raining at a given point on the weather dial (0 = dry, 1 = full
// downpour). Shared rather than inlined at each site: the streaks, the rain sound
// and the road's wet sheen all have to agree on when it starts raining, or the
// world goes shiny before there is anything falling.
inline float rainIntensityFor(float weather) {
    return glm::smoothstep(0.45f, 0.85f, weather);
}

// Falling rain streaks: a fixed box of line segments that rides along with the
// camera, so a few thousand drops look like weather everywhere instead of a patch
// you can walk out of. The vertex shader does the falling -- the buffer is built
// once and never touched again, and each drop's phase comes from its own start
// height, so nothing has to be simulated on the CPU.
class RainRenderer : public RendererBase {
public:
    RainRenderer() = default;
    ~RainRenderer() override;
    RainRenderer(const RainRenderer&)            = delete;
    RainRenderer& operator=(const RainRenderer&) = delete;

    // Load the shader and build the streak buffer. Needs a live GL context.
    // Returns false if the shader failed to compile.
    bool init();

    // Draw into the currently-bound target. A no-op below a drizzle, so a clear
    // day costs nothing. Leaves depth mask and blending as it found them.
    void draw(const FrameContext& ctx) override;

    bool enabled = true;

private:
    fitzel::Shader m_shader;
    std::uint32_t  m_vao = 0, m_vbo = 0;
};
