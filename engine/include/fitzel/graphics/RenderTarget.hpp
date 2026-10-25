#pragma once

#include <cstdint>

#include "fitzel/rhi/Types.hpp"

namespace fitzel {

// An off-screen framebuffer with a color texture and a depth buffer.
// Used for render-to-texture passes (e.g. planar water reflection/refraction,
// HDR scene buffer for post-processing). Move-only.
class RenderTarget {
public:
    enum class Format { RGBA8, RGBA16F };

    // `depthAsTexture` attaches a sampleable depth texture (for SSAO etc.)
    // instead of a write-only depth buffer; `stencil` gives that texture
    // an 8-bit stencil too (DEPTH32F_STENCIL8, still sampled as depth).
    RenderTarget(int width, int height, Format format = Format::RGBA8,
                 bool depthAsTexture = false, bool stencil = false);
    bool hasStencil() const { return m_stencil; }
    ~RenderTarget();

    RenderTarget(const RenderTarget&)            = delete;
    RenderTarget& operator=(const RenderTarget&) = delete;
    RenderTarget(RenderTarget&& other) noexcept;
    RenderTarget& operator=(RenderTarget&& other) noexcept;

    // Bind as the draw target and set the viewport to its size.
    void bind() const;
    // Bind the default framebuffer and restore the given viewport.
    static void unbind(int viewportWidth, int viewportHeight);

    void bindColorTexture(std::uint32_t unit) const;
    void bindDepthTexture(std::uint32_t unit) const; // valid if created with depthAsTexture

    int width()  const { return m_width; }
    int height() const { return m_height; }

    // The color texture (e.g. to display the target via ImGui::Image).
    rhi::TextureHandle colorTexture() const { return m_colorTex; }
    // The depth texture (none unless created with depthAsTexture), for a pass
    // that has to depth-test against this target's depth from another
    // framebuffer -- the motion vectors do, against the lit pass's depth.
    rhi::TextureHandle depthTexture() const { return m_depthTex; }
    // The framebuffer, for a blit into or out of this target.
    rhi::FramebufferHandle framebuffer() const { return m_fbo; }

private:
    void release();

    rhi::FramebufferHandle m_fbo;
    rhi::TextureHandle     m_colorTex;
    rhi::TextureHandle     m_depthRbo;   // not sampled: a renderbuffer on OpenGL
    rhi::TextureHandle     m_depthTex;
    bool          m_stencil = false;
    int           m_width     = 0;
    int           m_height    = 0;
};

} // namespace fitzel
