#include "fitzel/graphics/CascadedShadowMap.hpp"

#include "fitzel/scene/Camera.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>

namespace fitzel {

CascadedShadowMap::CascadedShadowMap(int resolution, int cascades)
    : m_resolution(resolution), m_cascades(std::max(1, cascades)) {
    glGenTextures(1, &m_depthTex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_depthTex);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT24,
                 resolution, resolution, m_cascades, 0,
                 GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    const float border[] = {1.0f, 1.0f, 1.0f, 1.0f};
    glTexParameterfv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BORDER_COLOR, border);

    glGenFramebuffers(1, &m_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    m_lightMatrices.resize(m_cascades, glm::mat4(1.0f));
    m_splitDistances.resize(m_cascades, 0.0f);
}

CascadedShadowMap::~CascadedShadowMap() {
    if (m_fbo)      glDeleteFramebuffers(1, &m_fbo);
    if (m_depthTex) glDeleteTextures(1, &m_depthTex);
}

CascadedShadowMap::CascadedShadowMap(CascadedShadowMap&& o) noexcept
    : m_resolution(std::exchange(o.m_resolution, 0)),
      m_cascades(std::exchange(o.m_cascades, 0)),
      m_fbo(std::exchange(o.m_fbo, 0)),
      m_depthTex(std::exchange(o.m_depthTex, 0)),
      m_lightMatrices(std::move(o.m_lightMatrices)),
      m_splitDistances(std::move(o.m_splitDistances)) {}

CascadedShadowMap& CascadedShadowMap::operator=(CascadedShadowMap&& o) noexcept {
    if (this != &o) {
        if (m_fbo)      glDeleteFramebuffers(1, &m_fbo);
        if (m_depthTex) glDeleteTextures(1, &m_depthTex);
        m_resolution     = std::exchange(o.m_resolution, 0);
        m_cascades       = std::exchange(o.m_cascades, 0);
        m_fbo            = std::exchange(o.m_fbo, 0);
        m_depthTex       = std::exchange(o.m_depthTex, 0);
        m_lightMatrices  = std::move(o.m_lightMatrices);
        m_splitDistances = std::move(o.m_splitDistances);
    }
    return *this;
}

void CascadedShadowMap::update(const Camera& camera, float aspect,
                               const glm::vec3& lightDir) {
    const float nearC = camera.nearPlane();
    const float farC  = camera.farPlane();

    // Practical split scheme: blend logarithmic and uniform distributions.
    float prev = nearC;
    for (int i = 0; i < m_cascades; ++i) {
        const float p   = static_cast<float>(i + 1) / static_cast<float>(m_cascades);
        const float log = nearC * std::pow(farC / nearC, p);
        const float uni = nearC + (farC - nearC) * p;
        const float d   = splitLambda * log + (1.0f - splitLambda) * uni;

        m_splitDistances[i] = d;
        m_lightMatrices[i]  = fitCascade(camera, aspect, prev, d, lightDir);
        prev = d;
    }
}

glm::mat4 CascadedShadowMap::fitCascade(const Camera& camera, float aspect,
                                        float nearDist, float farDist,
                                        const glm::vec3& lightDir) const {
    const glm::mat4 proj =
        glm::perspective(glm::radians(camera.fov()), aspect, nearDist, farDist);
    const glm::mat4 inv = glm::inverse(proj * camera.viewMatrix());

    // Eight world-space corners of this sub-frustum.
    std::array<glm::vec3, 8> corners;
    int c = 0;
    for (int x = 0; x < 2; ++x)
        for (int y = 0; y < 2; ++y)
            for (int z = 0; z < 2; ++z) {
                const glm::vec4 pt =
                    inv * glm::vec4(2.0f * x - 1.0f, 2.0f * y - 1.0f,
                                    2.0f * z - 1.0f, 1.0f);
                corners[c++] = glm::vec3(pt) / pt.w;
            }

    glm::vec3 center(0.0f);
    for (const auto& p : corners) center += p;
    center /= static_cast<float>(corners.size());

    const glm::vec3 dir = glm::normalize(lightDir);
    const glm::vec3 up  = (std::abs(dir.y) > 0.99f) ? glm::vec3(0, 0, 1)
                                                     : glm::vec3(0, 1, 0);
    const glm::mat4 lightView = glm::lookAt(center + dir, center, up);

    float minX = std::numeric_limits<float>::max(), maxX = std::numeric_limits<float>::lowest();
    float minY = minX, maxY = maxX;
    float minZ = minX, maxZ = maxX;
    for (const auto& p : corners) {
        const glm::vec4 ls = lightView * glm::vec4(p, 1.0f);
        minX = std::min(minX, ls.x); maxX = std::max(maxX, ls.x);
        minY = std::min(minY, ls.y); maxY = std::max(maxY, ls.y);
        minZ = std::min(minZ, ls.z); maxZ = std::max(maxZ, ls.z);
    }

    // Pull the near plane back / push the far plane out so casters outside the
    // frustum (between the light and the slice) still write depth.
    constexpr float zMult = 10.0f;
    minZ = (minZ < 0.0f) ? minZ * zMult : minZ / zMult;
    maxZ = (maxZ < 0.0f) ? maxZ / zMult : maxZ * zMult;

    const glm::mat4 lightProj = glm::ortho(minX, maxX, minY, maxY, minZ, maxZ);
    return lightProj * lightView;
}

void CascadedShadowMap::beginCascade(int index) {
    glViewport(0, 0, m_resolution, m_resolution);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              m_depthTex, 0, index);
    if (index == 0 &&
        glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "[Fitzel] CSM framebuffer is incomplete\n");
    }
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_POLYGON_OFFSET_FILL);
    // Slope/constant depth bias in the shadow pass. Kept modest so the shadow
    // stays attached to its caster (a large offset here reads as peter-panning);
    // the fragment shader adds a small remaining bias.
    glPolygonOffset(1.2f, 2.0f);
}

void CascadedShadowMap::end(int viewportWidth, int viewportHeight) {
    glDisable(GL_POLYGON_OFFSET_FILL);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, viewportWidth, viewportHeight);
}

void CascadedShadowMap::bindTextureArray(std::uint32_t unit) const {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_depthTex);
}

} // namespace fitzel
