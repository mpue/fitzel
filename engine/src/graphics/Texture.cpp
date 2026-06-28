#include "fitzel/graphics/Texture.hpp"

#include <cstdio>
#include <utility>
#include <vector>

#include <glad/gl.h>
#include <stb_image.h>

namespace fitzel {

namespace {

GLenum formatForChannels(int channels) {
    switch (channels) {
        case 1:  return GL_RED;
        case 3:  return GL_RGB;
        case 4:  return GL_RGBA;
        default: return GL_RGB;
    }
}

} // namespace

Texture::~Texture() {
    if (m_id) {
        glDeleteTextures(1, &m_id);
    }
}

Texture::Texture(Texture&& other) noexcept
    : m_id(std::exchange(other.m_id, 0)),
      m_width(std::exchange(other.m_width, 0)),
      m_height(std::exchange(other.m_height, 0)) {}

Texture& Texture::operator=(Texture&& other) noexcept {
    if (this != &other) {
        if (m_id) {
            glDeleteTextures(1, &m_id);
        }
        m_id     = std::exchange(other.m_id, 0);
        m_width  = std::exchange(other.m_width, 0);
        m_height = std::exchange(other.m_height, 0);
    }
    return *this;
}

Texture Texture::fromPixels(const unsigned char* pixels, int width, int height,
                            int channels) {
    Texture tex;
    tex.m_width  = width;
    tex.m_height = height;

    glGenTextures(1, &tex.m_id);
    glBindTexture(GL_TEXTURE_2D, tex.m_id);

    // Tightly-packed rows (handles widths that aren't multiples of 4).
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    const GLenum format = formatForChannels(channels);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(format), width, height, 0,
                 format, GL_UNSIGNED_BYTE, pixels);
    glGenerateMipmap(GL_TEXTURE_2D);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

Texture Texture::fromFile(const std::string& path, bool flipVertically) {
    stbi_set_flip_vertically_on_load(flipVertically ? 1 : 0);

    int width = 0, height = 0, channels = 0;
    unsigned char* data = stbi_load(path.c_str(), &width, &height, &channels, 0);
    if (!data) {
        std::fprintf(stderr, "[Fitzel] failed to load texture '%s': %s\n",
                     path.c_str(), stbi_failure_reason());
        return Texture{};
    }

    Texture tex = fromPixels(data, width, height, channels);
    stbi_image_free(data);
    return tex;
}

Texture Texture::checkerboard(int size, int cells) {
    std::vector<unsigned char> pixels(static_cast<std::size_t>(size) * size * 3);
    const int cellSize = (cells > 0) ? size / cells : size;

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const bool on = ((x / cellSize) + (y / cellSize)) % 2 == 0;
            const unsigned char v = on ? 220 : 40;
            const std::size_t i = (static_cast<std::size_t>(y) * size + x) * 3;
            pixels[i + 0] = v;
            pixels[i + 1] = v;
            pixels[i + 2] = static_cast<unsigned char>(on ? 235 : 60);
        }
    }

    return fromPixels(pixels.data(), size, size, 3);
}

void Texture::bind(std::uint32_t unit) const {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, m_id);
}

} // namespace fitzel
