#include "fitzel/graphics/ShadowMap.hpp"

#include <cstdio>
#include <utility>

#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>

namespace fitzel {

ShadowMap::ShadowMap(int resolution) : m_resolution(resolution) {
    glGenTextures(1, &m_depthTex);
    glBindTexture(GL_TEXTURE_2D, m_depthTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24,
                 resolution, resolution, 0,
                 GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    // Clamp to a white border so samples outside the light frustum read as lit.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    const float border[] = {1.0f, 1.0f, 1.0f, 1.0f};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);

    glGenFramebuffers(1, &m_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                           GL_TEXTURE_2D, m_depthTex, 0);
    // Depth-only: no color buffer.
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "[Fitzel] shadow framebuffer is incomplete\n");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

ShadowMap::~ShadowMap() {
    if (m_fbo)      glDeleteFramebuffers(1, &m_fbo);
    if (m_depthTex) glDeleteTextures(1, &m_depthTex);
}

ShadowMap::ShadowMap(ShadowMap&& other) noexcept
    : m_resolution(std::exchange(other.m_resolution, 0)),
      m_fbo(std::exchange(other.m_fbo, 0)),
      m_depthTex(std::exchange(other.m_depthTex, 0)) {}

ShadowMap& ShadowMap::operator=(ShadowMap&& other) noexcept {
    if (this != &other) {
        if (m_fbo)      glDeleteFramebuffers(1, &m_fbo);
        if (m_depthTex) glDeleteTextures(1, &m_depthTex);
        m_resolution = std::exchange(other.m_resolution, 0);
        m_fbo        = std::exchange(other.m_fbo, 0);
        m_depthTex   = std::exchange(other.m_depthTex, 0);
    }
    return *this;
}

void ShadowMap::begin() {
    glViewport(0, 0, m_resolution, m_resolution);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glClear(GL_DEPTH_BUFFER_BIT);
    // Slope-scaled depth bias pushes caster depth away from the receiver,
    // suppressing shadow acne without front-face culling (safe for terrain).
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(2.0f, 4.0f);
}

void ShadowMap::end(int viewportWidth, int viewportHeight) {
    glDisable(GL_POLYGON_OFFSET_FILL);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, viewportWidth, viewportHeight);
}

void ShadowMap::bindDepthTexture(std::uint32_t unit) const {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, m_depthTex);
}

glm::mat4 ShadowMap::lightSpaceMatrix(const glm::vec3& lightDir,
                                      const glm::vec3& center,
                                      float radius) const {
    const glm::vec3 dir = glm::normalize(lightDir);
    const glm::vec3 eye = center + dir * (radius * 2.0f);

    // Pick an up vector that isn't parallel to the light direction.
    const glm::vec3 up = (std::abs(dir.y) > 0.99f)
                             ? glm::vec3(0.0f, 0.0f, 1.0f)
                             : glm::vec3(0.0f, 1.0f, 0.0f);

    const glm::mat4 view = glm::lookAt(eye, center, up);
    const glm::mat4 proj = glm::ortho(-radius, radius, -radius, radius,
                                      0.1f, radius * 4.0f);
    return proj * view;
}

} // namespace fitzel
