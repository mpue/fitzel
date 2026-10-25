#include "fitzel/graphics/RenderTarget.hpp"

#include <cstdio>
#include <utility>

#include "fitzel/rhi/Device.hpp"

namespace fitzel {

RenderTarget::RenderTarget(int width, int height, Format format, bool depthAsTexture,
                           bool stencil)
    : m_width(width), m_height(height) {
    rhi::Device& gpu = rhi::device();
    m_stencil = depthAsTexture && stencil;

    rhi::TextureDesc color;
    color.format  = (format == Format::RGBA16F) ? rhi::Format::RGBA16F : rhi::Format::RGBA8;
    color.width   = width;
    color.height  = height;
    color.sampler = rhi::SamplerDesc::linearClamp();
    m_colorTex = gpu.createTexture(color);

    rhi::TextureDesc depth;
    depth.width  = width;
    depth.height = height;
    if (depthAsTexture) {
        // 32-bit float depth: 24-bit fixed-point quantises badly at distance under
        // a far plane that grows with the view distance, which terraces SSAO into
        // horizontal stripes on grazing terrain. 32F keeps the reconstruction clean.
        depth.format  = m_stencil ? rhi::Format::Depth32FStencil8 : rhi::Format::Depth32F;
        depth.sampler = rhi::SamplerDesc::nearestClamp();
        m_depthTex = gpu.createTexture(depth);
    } else {
        depth.format = rhi::Format::Depth24;
        depth.usage  = rhi::UsageRenderTarget;
        m_depthRbo = gpu.createTexture(depth);
    }

    rhi::FramebufferDesc fb;
    fb.color[0]      = rhi::Attachment{m_colorTex};
    fb.depth.texture = depthAsTexture ? m_depthTex : m_depthRbo;
    m_fbo = gpu.createFramebuffer(fb);
    if (!gpu.framebufferComplete(m_fbo)) {
        std::fprintf(stderr, "[Fitzel] render target framebuffer is incomplete\n");
    }
}

void RenderTarget::release() {
    if (!m_fbo && !m_colorTex && !m_depthRbo && !m_depthTex) return;
    rhi::Device& gpu = rhi::device();
    if (m_depthRbo) gpu.destroy(m_depthRbo);
    if (m_depthTex) gpu.destroy(m_depthTex);
    if (m_colorTex) gpu.destroy(m_colorTex);
    if (m_fbo)      gpu.destroy(m_fbo);
}

RenderTarget::~RenderTarget() { release(); }

RenderTarget::RenderTarget(RenderTarget&& o) noexcept
    : m_fbo(std::exchange(o.m_fbo, rhi::FramebufferHandle{})),
      m_colorTex(std::exchange(o.m_colorTex, rhi::TextureHandle{})),
      m_depthRbo(std::exchange(o.m_depthRbo, rhi::TextureHandle{})),
      m_depthTex(std::exchange(o.m_depthTex, rhi::TextureHandle{})),
      m_stencil(std::exchange(o.m_stencil, false)),
      m_width(std::exchange(o.m_width, 0)),
      m_height(std::exchange(o.m_height, 0)) {}

RenderTarget& RenderTarget::operator=(RenderTarget&& o) noexcept {
    if (this != &o) {
        release();
        m_fbo      = std::exchange(o.m_fbo, rhi::FramebufferHandle{});
        m_colorTex = std::exchange(o.m_colorTex, rhi::TextureHandle{});
        m_depthRbo = std::exchange(o.m_depthRbo, rhi::TextureHandle{});
        m_depthTex = std::exchange(o.m_depthTex, rhi::TextureHandle{});
        m_stencil  = std::exchange(o.m_stencil, false);
        m_width    = std::exchange(o.m_width, 0);
        m_height   = std::exchange(o.m_height, 0);
    }
    return *this;
}

void RenderTarget::bind() const {
    rhi::Device& gpu = rhi::device();
    gpu.bindFramebuffer(m_fbo);
    gpu.setViewport(0, 0, m_width, m_height);
}

void RenderTarget::unbind(int viewportWidth, int viewportHeight) {
    rhi::Device& gpu = rhi::device();
    gpu.bindFramebuffer(rhi::FramebufferHandle{});
    gpu.setViewport(0, 0, viewportWidth, viewportHeight);
}

void RenderTarget::bindColorTexture(std::uint32_t unit) const {
    rhi::device().bindTexture(static_cast<int>(unit), m_colorTex);
}

void RenderTarget::bindDepthTexture(std::uint32_t unit) const {
    rhi::device().bindTexture(static_cast<int>(unit), m_depthTex);
}

} // namespace fitzel
