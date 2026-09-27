#include "HalfResSky.hpp"

#include <algorithm>

#include <glad/gl.h>

using fitzel::RenderTarget;

bool HalfResSky::init() {
    m_blit = fitzel::Shader::fromFiles("assets/shaders/sky.vert", "assets/shaders/skyblit.frag");
    return m_blit.isValid();
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
