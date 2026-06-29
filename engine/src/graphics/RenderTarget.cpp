#include "fitzel/graphics/RenderTarget.hpp"

#include <cstdio>
#include <utility>

#include <glad/gl.h>

namespace fitzel {

RenderTarget::RenderTarget(int width, int height, Format format, bool depthAsTexture)
    : m_width(width), m_height(height) {
    const bool hdr = (format == Format::RGBA16F);
    const GLint  internalFormat = hdr ? GL_RGBA16F : GL_RGBA8;
    const GLenum pixelType      = hdr ? GL_FLOAT : GL_UNSIGNED_BYTE;

    glGenFramebuffers(1, &m_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);

    glGenTextures(1, &m_colorTex);
    glBindTexture(GL_TEXTURE_2D, m_colorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0,
                 GL_RGBA, pixelType, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, m_colorTex, 0);

    if (depthAsTexture) {
        glGenTextures(1, &m_depthTex);
        glBindTexture(GL_TEXTURE_2D, m_depthTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0,
                     GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                               GL_TEXTURE_2D, m_depthTex, 0);
    } else {
        glGenRenderbuffers(1, &m_depthRbo);
        glBindRenderbuffer(GL_RENDERBUFFER, m_depthRbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER, m_depthRbo);
    }

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "[Fitzel] render target framebuffer is incomplete\n");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

RenderTarget::~RenderTarget() {
    if (m_depthRbo) glDeleteRenderbuffers(1, &m_depthRbo);
    if (m_depthTex) glDeleteTextures(1, &m_depthTex);
    if (m_colorTex) glDeleteTextures(1, &m_colorTex);
    if (m_fbo)      glDeleteFramebuffers(1, &m_fbo);
}

RenderTarget::RenderTarget(RenderTarget&& o) noexcept
    : m_fbo(std::exchange(o.m_fbo, 0)),
      m_colorTex(std::exchange(o.m_colorTex, 0)),
      m_depthRbo(std::exchange(o.m_depthRbo, 0)),
      m_depthTex(std::exchange(o.m_depthTex, 0)),
      m_width(std::exchange(o.m_width, 0)),
      m_height(std::exchange(o.m_height, 0)) {}

RenderTarget& RenderTarget::operator=(RenderTarget&& o) noexcept {
    if (this != &o) {
        if (m_depthRbo) glDeleteRenderbuffers(1, &m_depthRbo);
        if (m_depthTex) glDeleteTextures(1, &m_depthTex);
        if (m_colorTex) glDeleteTextures(1, &m_colorTex);
        if (m_fbo)      glDeleteFramebuffers(1, &m_fbo);
        m_fbo      = std::exchange(o.m_fbo, 0);
        m_colorTex = std::exchange(o.m_colorTex, 0);
        m_depthRbo = std::exchange(o.m_depthRbo, 0);
        m_depthTex = std::exchange(o.m_depthTex, 0);
        m_width    = std::exchange(o.m_width, 0);
        m_height   = std::exchange(o.m_height, 0);
    }
    return *this;
}

void RenderTarget::bind() const {
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glViewport(0, 0, m_width, m_height);
}

void RenderTarget::unbind(int viewportWidth, int viewportHeight) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, viewportWidth, viewportHeight);
}

void RenderTarget::bindColorTexture(std::uint32_t unit) const {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, m_colorTex);
}

void RenderTarget::bindDepthTexture(std::uint32_t unit) const {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, m_depthTex);
}

} // namespace fitzel
