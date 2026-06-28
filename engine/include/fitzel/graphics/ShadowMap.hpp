#pragma once

#include <cstdint>

#include <glm/glm.hpp>

namespace fitzel {

// A depth-only framebuffer for directional-light shadow mapping. Render the
// scene once from the light's point of view into the depth texture, then sample
// it in the main pass to decide what's in shadow.
//
// Usage per frame:
//   shadow.begin();                       // bind FBO + set viewport, clear
//   depthShader.bind();
//   depthShader.setMat4("uLightSpace", shadow.lightSpaceMatrix(dir, c, r));
//   ... draw all shadow casters with uModel ...
//   shadow.end(fbWidth, fbHeight);        // unbind + restore viewport
class ShadowMap {
public:
    explicit ShadowMap(int resolution = 2048);
    ~ShadowMap();

    ShadowMap(const ShadowMap&)            = delete;
    ShadowMap& operator=(const ShadowMap&) = delete;
    ShadowMap(ShadowMap&& other) noexcept;
    ShadowMap& operator=(ShadowMap&& other) noexcept;

    void begin();
    void end(int viewportWidth, int viewportHeight);

    // Bind the depth texture to a sampler unit for the main pass.
    void bindDepthTexture(std::uint32_t unit) const;

    // Orthographic light-space matrix that tightly bounds a sphere (center,
    // radius). `lightDir` points *towards* the light (same convention as the
    // shading code).
    glm::mat4 lightSpaceMatrix(const glm::vec3& lightDir,
                               const glm::vec3& center, float radius) const;

    int resolution() const { return m_resolution; }

private:
    int           m_resolution = 0;
    std::uint32_t m_fbo        = 0;
    std::uint32_t m_depthTex   = 0;
};

} // namespace fitzel
