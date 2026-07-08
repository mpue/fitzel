#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fitzel {

// Decoded 8-bit image pixels on the CPU (no GL). Produced off the render thread
// so a heavy decode never stalls the frame; uploaded later with fromImagePixels.
struct ImagePixels {
    std::vector<unsigned char> pixels;
    int width = 0, height = 0, channels = 0;
    bool valid() const { return !pixels.empty() && width > 0 && height > 0; }
};

// A 2D OpenGL texture. Move-only RAII wrapper around a GL texture object.
class Texture {
public:
    Texture() = default;
    ~Texture();

    Texture(const Texture&)            = delete;
    Texture& operator=(const Texture&) = delete;
    Texture(Texture&& other) noexcept;
    Texture& operator=(Texture&& other) noexcept;

    // Load from an image file (PNG/JPG/...) via stb_image. Returns an invalid
    // Texture on failure (check isValid()).
    static Texture fromFile(const std::string& path, bool flipVertically = true);

    // Upload raw 8-bit pixel data. `channels` is 1 (R), 3 (RGB) or 4 (RGBA).
    static Texture fromPixels(const unsigned char* pixels, int width, int height,
                              int channels);

    // Procedural checkerboard -- useful as a default/placeholder texture.
    static Texture checkerboard(int size = 256, int cells = 8);

    // Small preview texture: decode `path` (PNG/JPG/EXR) and box-downsample it so
    // its longest side is <= maxDim. Cheap to keep resident (a few KB of VRAM),
    // meant for asset-browser thumbnails. Invalid Texture on failure. Not flipped,
    // so it draws upright through ImGui::Image with default UVs.
    static Texture thumbnail(const std::string& path, int maxDim = 128);

    // CPU half of thumbnail(): decode + downsample to pixels, no GL calls -- safe
    // to run on a worker thread. Upload the result with fromImagePixels().
    static ImagePixels decodeThumbnail(const std::string& path, int maxDim = 128);

    // Upload CPU pixels to a GL texture (render thread). Invalid on empty input.
    static Texture fromImagePixels(const ImagePixels& img);

    bool isValid() const { return m_id != 0; }

    // Bind to a texture unit (0 by default).
    void bind(std::uint32_t unit = 0) const;

    int width() const { return m_width; }
    int height() const { return m_height; }
    std::uint32_t id() const { return m_id; }

private:
    std::uint32_t m_id     = 0;
    int           m_width  = 0;
    int           m_height = 0;
};

} // namespace fitzel
