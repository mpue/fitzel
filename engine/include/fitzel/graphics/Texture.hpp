#pragma once

#include <cstdint>
#include <string>

namespace fitzel {

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
