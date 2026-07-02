#pragma once

#include <cstdint>

#include <glm/glm.hpp>

namespace fitzel {

// An HDR colour cubemap render target for a dynamic environment probe: render
// the scene into all six faces from a probe position, generate mipmaps (so a
// rough reflection can sample a blurrier level), then sample it in the lit
// shader via the reflection vector. Move-only.
class CubeRenderTarget {
public:
    explicit CubeRenderTarget(int resolution = 128);
    ~CubeRenderTarget();

    CubeRenderTarget(const CubeRenderTarget&)            = delete;
    CubeRenderTarget& operator=(const CubeRenderTarget&) = delete;
    CubeRenderTarget(CubeRenderTarget&& other) noexcept;
    CubeRenderTarget& operator=(CubeRenderTarget&& other) noexcept;

    // Bind the FBO with cube face `face` (0..5) as the colour target and set the
    // viewport. The caller clears and draws.
    void beginFace(int face);

    // Build the mip chain from face 0..N (call once after all six faces are
    // rendered); rough reflections sample the coarser levels.
    void generateMipmaps() const;

    // Bind the colour cubemap to a texture unit for sampling.
    void bindTexture(std::uint32_t unit) const;

    int resolution() const { return m_res; }
    // Number of mip levels (floor(log2(res)) + 1); the max LOD is one less.
    int mipLevels() const;

    // The six cube-face view / projection helpers for a probe at `pos`.
    static glm::mat4 faceView(const glm::vec3& pos, int face);
    static glm::mat4 faceProjection(float nearZ, float farZ);

private:
    std::uint32_t m_fbo   = 0;
    std::uint32_t m_cube  = 0;
    std::uint32_t m_depth = 0;
    int           m_res   = 0;
};

} // namespace fitzel
