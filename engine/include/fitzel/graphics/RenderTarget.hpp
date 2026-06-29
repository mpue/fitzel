#pragma once

#include <cstdint>

namespace fitzel {

// An off-screen framebuffer with a color texture and a depth renderbuffer.
// Used for render-to-texture passes (e.g. planar water reflection/refraction,
// HDR scene buffer for post-processing). Move-only.
class RenderTarget {
public:
    enum class Format { RGBA8, RGBA16F };

    // `depthAsTexture` attaches a sampleable depth texture (for SSAO etc.)
    // instead of a write-only depth renderbuffer.
    RenderTarget(int width, int height, Format format = Format::RGBA8,
                 bool depthAsTexture = false);
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

private:
    std::uint32_t m_fbo       = 0;
    std::uint32_t m_colorTex  = 0;
    std::uint32_t m_depthRbo  = 0;
    std::uint32_t m_depthTex  = 0;
    int           m_width     = 0;
    int           m_height    = 0;
};

} // namespace fitzel
