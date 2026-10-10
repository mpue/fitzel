#include "fitzel/graphics/CubeShadowMap.hpp"

#include <utility>

#include <glad/gl.h>

namespace fitzel {

CubeShadowMap::CubeShadowMap(int resolution) : m_res(resolution) {
    glGenRenderbuffers(1, &m_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, m_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, m_res, m_res);

    glGenFramebuffers(1, &m_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, m_depth);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

CubeShadowMap::~CubeShadowMap() {
    if (m_fbo)   glDeleteFramebuffers(1, &m_fbo);
    if (m_depth) glDeleteRenderbuffers(1, &m_depth);
}

CubeShadowMap::CubeShadowMap(CubeShadowMap&& o) noexcept
    : m_fbo(std::exchange(o.m_fbo, 0)),
      m_depth(std::exchange(o.m_depth, 0)),
      m_res(std::exchange(o.m_res, 0)),
      m_array(std::exchange(o.m_array, 0)),
      m_firstLayer(std::exchange(o.m_firstLayer, 0)) {}

CubeShadowMap& CubeShadowMap::operator=(CubeShadowMap&& o) noexcept {
    if (this != &o) {
        if (m_fbo)   glDeleteFramebuffers(1, &m_fbo);
        if (m_depth) glDeleteRenderbuffers(1, &m_depth);
        m_fbo   = std::exchange(o.m_fbo, 0);
        m_depth = std::exchange(o.m_depth, 0);
        m_res   = std::exchange(o.m_res, 0);
        m_array = std::exchange(o.m_array, 0);
        m_firstLayer = std::exchange(o.m_firstLayer, 0);
    }
    return *this;
}

void CubeShadowMap::beginFace(int face, bool clearColour) {
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, m_array, 0,
                              m_firstLayer + face);
    glViewport(0, 0, m_res, m_res);
    glClearColor(1.0f, 1.0f, 1.0f, 1.0f); // normalized far
    glClear(clearColour ? (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT) : GL_DEPTH_BUFFER_BIT);
}

void CubeShadowMap::copyLayer(std::uint32_t src, std::uint32_t dst, int layer) {
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, src, 0, layer);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, dst);
    glCopyTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, 0, 0, m_res, m_res);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
}

const glm::vec3* CubeShadowMap::faceDirs() {
    static const glm::vec3 dirs[6] = {
        { 1, 0, 0}, {-1, 0, 0}, {0,  1, 0}, {0, -1, 0}, {0, 0,  1}, {0, 0, -1}};
    return dirs;
}
const glm::vec3* CubeShadowMap::faceUps() {
    static const glm::vec3 ups[6] = {
        {0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
    return ups;
}

} // namespace fitzel
