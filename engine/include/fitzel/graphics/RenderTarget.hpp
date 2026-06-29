#pragma once

#include <cstdint>

namespace fitzel {

// An off-screen framebuffer with a color texture and a depth renderbuffer.
// Used for render-to-texture passes (e.g. planar water reflection/refraction).
// Move-only.
class RenderTarget {
public:
    RenderTarget(int width, int height);
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

    int width()  const { return m_width; }
    int height() const { return m_height; }

private:
    std::uint32_t m_fbo       = 0;
    std::uint32_t m_colorTex  = 0;
    std::uint32_t m_depthRbo  = 0;
    int           m_width     = 0;
    int           m_height    = 0;
};

} // namespace fitzel
