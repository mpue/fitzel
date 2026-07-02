#include "fitzel/graphics/CubeRenderTarget.hpp"

#include "fitzel/graphics/CubeShadowMap.hpp" // reuse the cube face dirs / ups

#include <cmath>
#include <utility>

#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>

namespace fitzel {

CubeRenderTarget::CubeRenderTarget(int resolution) : m_res(resolution) {
    glGenTextures(1, &m_cube);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_cube);
    for (int f = 0; f < 6; ++f) {
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA16F,
                     m_res, m_res, 0, GL_RGBA, GL_FLOAT, nullptr);
    }
    // Trilinear + mip so a rough surface can sample a blurrier reflection.
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glGenerateMipmap(GL_TEXTURE_CUBE_MAP); // allocate the mip chain up front

    glGenRenderbuffers(1, &m_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, m_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, m_res, m_res);

    glGenFramebuffers(1, &m_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, m_depth);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

CubeRenderTarget::~CubeRenderTarget() {
    if (m_fbo)   glDeleteFramebuffers(1, &m_fbo);
    if (m_depth) glDeleteRenderbuffers(1, &m_depth);
    if (m_cube)  glDeleteTextures(1, &m_cube);
}

CubeRenderTarget::CubeRenderTarget(CubeRenderTarget&& o) noexcept
    : m_fbo(std::exchange(o.m_fbo, 0)),
      m_cube(std::exchange(o.m_cube, 0)),
      m_depth(std::exchange(o.m_depth, 0)),
      m_res(std::exchange(o.m_res, 0)) {}

CubeRenderTarget& CubeRenderTarget::operator=(CubeRenderTarget&& o) noexcept {
    if (this != &o) {
        if (m_fbo)   glDeleteFramebuffers(1, &m_fbo);
        if (m_depth) glDeleteRenderbuffers(1, &m_depth);
        if (m_cube)  glDeleteTextures(1, &m_cube);
        m_fbo   = std::exchange(o.m_fbo, 0);
        m_cube  = std::exchange(o.m_cube, 0);
        m_depth = std::exchange(o.m_depth, 0);
        m_res   = std::exchange(o.m_res, 0);
    }
    return *this;
}

void CubeRenderTarget::beginFace(int face) {
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, m_cube, 0);
    glViewport(0, 0, m_res, m_res);
}

void CubeRenderTarget::generateMipmaps() const {
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_cube);
    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
}

void CubeRenderTarget::bindTexture(std::uint32_t unit) const {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_cube);
}

int CubeRenderTarget::mipLevels() const {
    return 1 + static_cast<int>(std::floor(std::log2(static_cast<float>(m_res))));
}

glm::mat4 CubeRenderTarget::faceView(const glm::vec3& pos, int face) {
    const glm::vec3* dirs = CubeShadowMap::faceDirs();
    const glm::vec3* ups  = CubeShadowMap::faceUps();
    return glm::lookAt(pos, pos + dirs[face], ups[face]);
}

glm::mat4 CubeRenderTarget::faceProjection(float nearZ, float farZ) {
    return glm::perspective(glm::radians(90.0f), 1.0f, nearZ, farZ);
}

} // namespace fitzel
