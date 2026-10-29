#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "fitzel/rhi/Types.hpp"

namespace fitzel {

class Camera;

// Cascaded Shadow Maps for a directional light. The camera frustum is split
// into N depth ranges (cascades), each rendered into its own layer of a depth
// texture array with a tightly-fit light-space matrix. The shading pass picks
// a cascade by view-space depth, trading shadow resolution gracefully with
// distance.
//
// Per frame:
//   csm.update(camera, aspect, lightDir);
//   for (int i = 0; i < csm.cascadeCount(); ++i) {
//       csm.beginCascade(i);
//       depthShader.setMat4("uLightSpace", csm.lightMatrices()[i]);
//       ... draw casters ...
//   }
//   csm.end(fbWidth, fbHeight);
class CascadedShadowMap {
public:
    // The shaders size their cascade arrays to this (MAX_CASCADES in lit.frag
    // and volfog.frag); more would index past them. Counts are clamped to it.
    static constexpr int kMaxCascades = 4;

    explicit CascadedShadowMap(int resolution = 2048, int cascades = 4);
    ~CascadedShadowMap();

    CascadedShadowMap(const CascadedShadowMap&)            = delete;
    CascadedShadowMap& operator=(const CascadedShadowMap&) = delete;
    CascadedShadowMap(CascadedShadowMap&& other) noexcept;
    CascadedShadowMap& operator=(CascadedShadowMap&& other) noexcept;

    // Recompute the per-cascade split distances and light-space matrices.
    // `lightDir` points *towards* the light.
    void update(const Camera& camera, float aspect, const glm::vec3& lightDir);

    void beginCascade(int index);
    void end(int viewportWidth, int viewportHeight);

    void bindTextureArray(std::uint32_t unit) const;

    int cascadeCount() const { return m_cascades; }
    int resolution()   const { return m_resolution; }
    // Reallocate the cascade array at a new square resolution. One texture and no
    // scene work, so it is cheap enough to drive from a settings menu -- and a
    // no-op when the size already matches, which is what lets a caller push the
    // current value every frame without tracking whether it changed.
    void setResolution(int resolution);

    const std::vector<glm::mat4>& lightMatrices() const { return m_lightMatrices; }
    // Far view-space distance of each cascade (used for selection in the shader).
    const std::vector<float>& splitDistances() const { return m_splitDistances; }

    // Blend factor between uniform and logarithmic frustum splitting [0,1].
    // 0.9 rather than the 0.65 it was: over a shadowed range of several hundred
    // metres the uniform share took the first cascade out to 67 m -- a sphere
    // 230 m across on a wide screen, 5.7 cm texels even at 4096 -- and with
    // the normal offset and the filter both measured in texels, a ball's
    // shadow started a hand's width beside it and blurred. At 0.9 the first
    // cascade ends near 20 m (1.7 cm); only the band past ~130 m gets coarser.
    static constexpr float kDefaultSplitLambda = 0.9f;
    // The default before, which every scene saved until then holds whether
    // anyone chose it or not (see the scene loader).
    static constexpr float kOldDefaultSplitLambda = 0.65f;
    float splitLambda = kDefaultSplitLambda;
    // How far the cascades reach, in metres. 0 = out to the camera's far plane,
    // which is the old behaviour and the right default for a scene whose far
    // plane is set by what it can shade.
    //
    // It exists because the two distances stopped being the same one: a far plane
    // pushed out so a distant skyline is not sliced in half would otherwise drag
    // the cascades out with it, and cascades are a FIXED texture budget spread
    // over whatever range they are given -- doubling the range halves the
    // resolution everywhere, so the whole scene's shadows go soft to shade a city
    // a kilometre away that has nothing under it to receive one. Capping this
    // keeps the shadowed range where the receivers are.
    float shadowDistance = 0.0f;

private:
    glm::mat4 fitCascade(const Camera& camera, float aspect,
                         float nearDist, float farDist,
                         const glm::vec3& lightDir) const;

    int m_resolution = 0;
    int m_cascades   = 0;

    rhi::FramebufferHandle m_fbo;
    rhi::TextureHandle     m_depthTex; // a 2D array, one layer per cascade

    std::vector<glm::mat4> m_lightMatrices;
    std::vector<float>     m_splitDistances;
};

} // namespace fitzel
