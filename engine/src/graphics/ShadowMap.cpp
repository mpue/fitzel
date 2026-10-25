#include "fitzel/graphics/ShadowMap.hpp"

#include <cstdio>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>

#include "fitzel/rhi/Device.hpp"

namespace fitzel {

ShadowMap::ShadowMap(int resolution) : m_resolution(resolution) {
    rhi::Device& gpu = rhi::device();
    rhi::TextureDesc d;
    d.format = rhi::Format::Depth24;
    d.width  = resolution;
    d.height = resolution;
    d.sampler = rhi::SamplerDesc::nearestClamp();
    // Clamp to a white border so samples outside the light frustum read as lit.
    // (WebGL2 has no border colour: there the device clamps to the edge, and a
    // sample past it reads the outermost texel instead of "lit" -- the cascade
    // pick keeps lookups inside, so that costs at most a shadow smeared along
    // the far rim.)
    d.sampler.wrapU = d.sampler.wrapV = rhi::Wrap::ClampToBorder;
    d.sampler.borderColor = glm::vec4(1.0f);
    m_depthTex = gpu.createTexture(d);

    // Depth-only: no color buffer.
    rhi::FramebufferDesc fb;
    fb.depth.texture = m_depthTex;
    m_fbo = gpu.createFramebuffer(fb);
    if (!gpu.framebufferComplete(m_fbo)) {
        std::fprintf(stderr, "[Fitzel] shadow framebuffer is incomplete\n");
    }
}

ShadowMap::~ShadowMap() {
    if (m_fbo)      rhi::device().destroy(m_fbo);
    if (m_depthTex) rhi::device().destroy(m_depthTex);
}

ShadowMap::ShadowMap(ShadowMap&& other) noexcept
    : m_resolution(std::exchange(other.m_resolution, 0)),
      m_fbo(std::exchange(other.m_fbo, rhi::FramebufferHandle{})),
      m_depthTex(std::exchange(other.m_depthTex, rhi::TextureHandle{})) {}

ShadowMap& ShadowMap::operator=(ShadowMap&& other) noexcept {
    if (this != &other) {
        if (m_fbo)      rhi::device().destroy(m_fbo);
        if (m_depthTex) rhi::device().destroy(m_depthTex);
        m_resolution = std::exchange(other.m_resolution, 0);
        m_fbo        = std::exchange(other.m_fbo, rhi::FramebufferHandle{});
        m_depthTex   = std::exchange(other.m_depthTex, rhi::TextureHandle{});
    }
    return *this;
}

void ShadowMap::begin() {
    rhi::Device& gpu = rhi::device();
    gpu.setViewport(0, 0, m_resolution, m_resolution);
    gpu.bindFramebuffer(m_fbo);
    gpu.clear(rhi::ClearDepth);
    // Slope-scaled depth bias pushes caster depth away from the receiver,
    // suppressing shadow acne without front-face culling (safe for terrain).
    gpu.setPolygonOffset(true, 2.0f, 4.0f);
}

void ShadowMap::end(int viewportWidth, int viewportHeight) {
    rhi::Device& gpu = rhi::device();
    gpu.setPolygonOffsetEnabled(false);
    gpu.bindFramebuffer(rhi::FramebufferHandle{});
    gpu.setViewport(0, 0, viewportWidth, viewportHeight);
}

void ShadowMap::bindDepthTexture(std::uint32_t unit) const {
    rhi::device().bindTexture(static_cast<int>(unit), m_depthTex);
}

glm::mat4 ShadowMap::lightSpaceMatrix(const glm::vec3& lightDir,
                                      const glm::vec3& center,
                                      float radius) const {
    const glm::vec3 dir = glm::normalize(lightDir);
    const glm::vec3 eye = center + dir * (radius * 2.0f);

    // Pick an up vector that isn't parallel to the light direction.
    const glm::vec3 up = (std::abs(dir.y) > 0.99f)
                             ? glm::vec3(0.0f, 0.0f, 1.0f)
                             : glm::vec3(0.0f, 1.0f, 0.0f);

    const glm::mat4 view = glm::lookAt(eye, center, up);
    const glm::mat4 proj = glm::ortho(-radius, radius, -radius, radius,
                                      0.1f, radius * 4.0f);
    return proj * view;
}

} // namespace fitzel
