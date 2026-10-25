#include "fitzel/graphics/Texture3D.hpp"

#include <utility>

#include "fitzel/rhi/Device.hpp"

namespace fitzel {

Texture3D::~Texture3D() {
    if (m_id) rhi::device().destroy(m_id);
}

Texture3D::Texture3D(Texture3D&& other) noexcept
    : m_id(std::exchange(other.m_id, rhi::TextureHandle{})),
      m_width(std::exchange(other.m_width, 0)),
      m_height(std::exchange(other.m_height, 0)),
      m_depth(std::exchange(other.m_depth, 0)) {}

Texture3D& Texture3D::operator=(Texture3D&& other) noexcept {
    if (this != &other) {
        if (m_id) rhi::device().destroy(m_id);
        m_id     = std::exchange(other.m_id, rhi::TextureHandle{});
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

    rhi::TextureDesc d;
    d.type   = rhi::TextureType::Tex3D;
    d.format = rhi::Format::RGBA16F;
    d.width  = width;
    d.height = height;
    d.depth  = depth;
    // Clamped on every axis. A light grid does not tile: a surface just outside
    // it should take the nearest probe's answer, not the one from the far side
    // of the world.
    d.sampler = rhi::SamplerDesc::linearClamp();
    t.m_id = rhi::device().createTexture(d, src, rhi::Format::RGBA32F);
    return t;
}

bool Texture3D::update(const std::vector<float>& rgba) {
    const std::size_t need =
        static_cast<std::size_t>(m_width) * m_height * m_depth * 4;
    if (!m_id || rgba.size() != need) return false;
    rhi::device().updateTexture(m_id, rhi::TextureRegion{}, rhi::Format::RGBA32F, rgba.data());
    return true;
}

void Texture3D::bind(std::uint32_t unit) const {
    rhi::device().bindTexture(static_cast<int>(unit), m_id);
}

} // namespace fitzel
