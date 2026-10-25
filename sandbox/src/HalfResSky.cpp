#include "HalfResSky.hpp"

#include <algorithm>

#include <fitzel/rhi/Device.hpp>

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
    fitzel::rhi::Device& gpu = fitzel::rhi::device();
    using fitzel::rhi::CompareOp;
    using fitzel::rhi::StencilOp;
    const int divisor = divisorFor(dst);
    gpu.setStencilTest(true);
    gpu.setStencilWriteMask(0x00);
    gpu.setStencilOp(StencilOp::Keep, StencilOp::Keep, StencilOp::Keep);
    if (divisor <= 1 || !m_blit.isValid() || !m_coverShader.isValid() ||
        !m_maskShader.isValid()) {
        gpu.setStencilFunc(CompareOp::Equal, 0, 0xFF);   // full size: straight into the sky pixels
        draw();
        return;
    }
    const int w = std::max(1, dst.width() / divisor), h = std::max(1, dst.height() / divisor);
    if (m_masked.width() != w || m_masked.height() != h) {
        m_masked = RenderTarget(w, h, RenderTarget::Format::RGBA16F, true, true);
        m_cover  = RenderTarget(w, h, RenderTarget::Format::RGBA8);
    }
    gpu.setDepthTest(false);
    gpu.setDepthWrite(false);
    gpu.setBlendEnabled(false);
    gpu.setCullEnabled(false);

    // 1) Per reduced texel: any sky in its block?
    gpu.setStencilTest(false);
    m_cover.bind();
    m_coverShader.bind();
    dst.bindDepthTexture(0);
    m_coverShader.setInt("uDepth", 0);
    m_coverShader.setInt("uDiv", divisor);
    quad.draw();

    // 2) The texels with none within one texel mark the stencil: no sky there.
    m_masked.bind();
    gpu.setStencilTest(true);
    gpu.setStencilWriteMask(0xFF);
    gpu.setClearStencil(0);
    gpu.clear(fitzel::rhi::ClearStencil);
    gpu.setStencilFunc(CompareOp::Always, 1, 0xFF);
    gpu.setStencilOp(StencilOp::Keep, StencilOp::Keep, StencilOp::Replace);
    gpu.setColorWrite(false);
    m_maskShader.bind();
    m_cover.bindColorTexture(0);
    m_maskShader.setInt("uCover", 0);
    quad.draw();
    gpu.setColorWrite(true);

    // 3) The sky, marched only on the unmarked texels.
    gpu.setStencilWriteMask(0x00);
    gpu.setStencilOp(StencilOp::Keep, StencilOp::Keep, StencilOp::Keep);
    gpu.setStencilFunc(CompareOp::Equal, 0, 0xFF);
    draw();

    // 4) Stretched over the scene's sky pixels only (its stencil is still 0).
    dst.bind();
    gpu.setStencilFunc(CompareOp::Equal, 0, 0xFF);
    gpu.setDepthTest(false);
    gpu.setDepthWrite(false);
    gpu.setCullEnabled(false);
    m_blit.bind();
    m_masked.bindColorTexture(0);
    m_blit.setInt("uSky", 0);
    quad.draw();
    gpu.setDepthWrite(true);
    gpu.setDepthTest(true);
    gpu.setCullEnabled(true);
}

void HalfResSky::render(const RenderTarget& dst, const fitzel::Mesh& quad,
                        const std::function<void()>& draw) {
    fitzel::rhi::Device& gpu = fitzel::rhi::device();
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
    gpu.setDepthTest(false);
    gpu.setDepthWrite(false);
    gpu.setCullEnabled(false);
    m_blit.bind();
    m_rt.bindColorTexture(0);
    m_blit.setInt("uSky", 0);
    quad.draw();
    gpu.setDepthWrite(true);
    gpu.setDepthTest(true);
    gpu.setCullEnabled(true);
}
