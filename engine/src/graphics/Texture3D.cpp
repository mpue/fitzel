#include "fitzel/graphics/Texture3D.hpp"

#include <utility>

#include <glad/gl.h>

namespace fitzel {

Texture3D::~Texture3D() {
    if (m_id) glDeleteTextures(1, &m_id);
}

Texture3D::Texture3D(Texture3D&& other) noexcept
    : m_id(std::exchange(other.m_id, 0)),
      m_width(std::exchange(other.m_width, 0)),
      m_height(std::exchange(other.m_height, 0)),
      m_depth(std::exchange(other.m_depth, 0)) {}

Texture3D& Texture3D::operator=(Texture3D&& other) noexcept {
    if (this != &other) {
        if (m_id) glDeleteTextures(1, &m_id);
        m_id     = std::exchange(other.m_id, 0);
        m_width  = std::exchange(other.m_width, 0);
        m_height = std::exchange(other.m_height, 0);
        m_depth  = std::exchange(other.m_depth, 0);
    }
    return *this;
}

Texture3D Texture3D::create(int width, int height, int depth,
                            const std::vector<float>& rgba) {
    Texture3D t;
    if (width <= 0 || height <= 0 || depth <= 0) return t;

    const std::size_t need =
        static_cast<std::size_t>(width) * height * depth * 4;
    const std::vector<float> black(need, 0.0f);
    const float* src = rgba.size() == need ? rgba.data() : black.data();

    t.m_width  = width;
    t.m_height = height;
    t.m_depth  = depth;

    glGenTextures(1, &t.m_id);
    glBindTexture(GL_TEXTURE_3D, t.m_id);
    // Alignment set explicitly: a stale GL_UNPACK_* from elsewhere is exactly
    // the kind of state leak that shears a volume and gets blamed on the data.
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA16F, width, height, depth, 0,
                 GL_RGBA, GL_FLOAT, src);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // Clamped on every axis. A light grid does not tile: a surface just outside
    // it should take the nearest probe's answer, not the one from the far side
    // of the world.
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_3D, 0);
    return t;
}

bool Texture3D::update(const std::vector<float>& rgba) {
    const std::size_t need =
        static_cast<std::size_t>(m_width) * m_height * m_depth * 4;
    if (!m_id || rgba.size() != need) return false;
    glBindTexture(GL_TEXTURE_3D, m_id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, m_width, m_height, m_depth,
                    GL_RGBA, GL_FLOAT, rgba.data());
    glBindTexture(GL_TEXTURE_3D, 0);
    return true;
}

void Texture3D::bind(std::uint32_t unit) const {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_3D, m_id);
}

} // namespace fitzel
