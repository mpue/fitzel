#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

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

    const std::vector<glm::mat4>& lightMatrices() const { return m_lightMatrices; }
    // Far view-space distance of each cascade (used for selection in the shader).
    const std::vector<float>& splitDistances() const { return m_splitDistances; }

    // Blend factor between uniform and logarithmic frustum splitting [0,1].
    float splitLambda = 0.65f;

private:
    glm::mat4 fitCascade(const Camera& camera, float aspect,
                         float nearDist, float farDist,
                         const glm::vec3& lightDir) const;

    int m_resolution = 0;
    int m_cascades   = 0;

    std::uint32_t m_fbo      = 0;
    std::uint32_t m_depthTex = 0; // GL_TEXTURE_2D_ARRAY

    std::vector<glm::mat4> m_lightMatrices;
    std::vector<float>     m_splitDistances;
};

} // namespace fitzel
