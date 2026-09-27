#include "HalfResSky.hpp"

#include <algorithm>

#include <glad/gl.h>

using fitzel::RenderTarget;

bool HalfResSky::init() {
    m_blit = fitzel::Shader::fromFiles("assets/shaders/sky.vert", "assets/shaders/skyblit.frag");
    m_coverShader = fitzel::Shader::fromFiles("assets/shaders/sky.vert", "assets/shaders/skycover.frag");
    m_maskShader  = fitzel::Shader::fromFiles("assets/shaders/sky.vert", "assets/shaders/skymask.frag");
    return m_blit.isValid();
}

int HalfResSky::divisorFor(const RenderTarget& dst) const {
    return std::max(1, dst.height() / std::max(minRows, 1));
}

void HalfResSky::renderBehind(const RenderTarget& dst, const fitzel::Mesh& quad,
                              const std::function<void()>& draw) {
    const int divisor = divisorFor(dst);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0x00);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    if (divisor <= 1 || !m_blit.isValid() || !m_coverShader.isValid() ||
        !m_maskShader.isValid()) {
        glStencilFunc(GL_EQUAL, 0, 0xFF);   // full size: straight into the sky pixels
        draw();
        return;
    }
    const int w = std::max(1, dst.width() / divisor), h = std::max(1, dst.height() / divisor);
    if (m_masked.width() != w || m_masked.height() != h) {
        m_masked = RenderTarget(w, h, RenderTarget::Format::RGBA16F, true, true);
        m_cover  = RenderTarget(w, h, RenderTarget::Format::RGBA8);
    }
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);

    // 1) Per reduced texel: any sky in its block?
    glDisable(GL_STENCIL_TEST);
    m_cover.bind();
    m_coverShader.bind();
    dst.bindDepthTexture(0);
    m_coverShader.setInt("uDepth", 0);
    m_coverShader.setInt("uDiv", divisor);
    quad.draw();

    // 2) The texels with none within one texel mark the stencil: no sky there.
    m_masked.bind();
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    m_maskShader.bind();
    m_cover.bindColorTexture(0);
    m_maskShader.setInt("uCover", 0);
    quad.draw();
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    // 3) The sky, marched only on the unmarked texels.
    glStencilMask(0x00);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glStencilFunc(GL_EQUAL, 0, 0xFF);
    draw();

    // 4) Stretched over the scene's sky pixels only (its stencil is still 0).
    dst.bind();
    glStencilFunc(GL_EQUAL, 0, 0xFF);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    m_blit.bind();
    m_masked.bindColorTexture(0);
    m_blit.setInt("uSky", 0);
    quad.draw();
    glActiveTexture(GL_TEXTURE0);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
}

void HalfResSky::render(const RenderTarget& dst, const fitzel::Mesh& quad,
                        const std::function<void()>& draw) {
    const int divisor = std::max(1, dst.height() / std::max(minRows, 1));
    if (divisor <= 1 || !m_blit.isValid()) {
        draw();
        return;
    }
    const int w = std::max(1, dst.width() / divisor), h = std::max(1, dst.height() / divisor);
    if (m_rt.width() != w || m_rt.height() != h)
        m_rt = RenderTarget(w, h, RenderTarget::Format::RGBA16F);
    m_rt.bind();
    draw();
    dst.bind();
    // Stretched over the whole target, before anything else is drawn into it:
    // no depth test, no depth written -- the scene lands on top as it always did.
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    m_blit.bind();
    m_rt.bindColorTexture(0);
    m_blit.setInt("uSky", 0);
    quad.draw();
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
}
