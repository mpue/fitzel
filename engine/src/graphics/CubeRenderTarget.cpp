#include "fitzel/graphics/CubeRenderTarget.hpp"

#include "fitzel/graphics/CubeShadowMap.hpp" // reuse the cube face dirs / ups

#include <cmath>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>

#include "fitzel/rhi/Device.hpp"

namespace fitzel {

CubeRenderTarget::CubeRenderTarget(int resolution) : m_res(resolution) {
    rhi::Device& gpu = rhi::device();
    rhi::TextureDesc c;
    c.type      = rhi::TextureType::Cube;
    c.format    = rhi::Format::RGBA16F;
    c.width     = m_res;
    c.height    = m_res;
    c.mipLevels = 0;
    // Trilinear + mip so a rough surface can sample a blurrier reflection.
    c.sampler = rhi::SamplerDesc::linearClamp();
    c.sampler.mipFilter = rhi::MipFilter::Linear;
    m_cube = gpu.createTexture(c);
    gpu.generateMipmaps(m_cube); // allocate the mip chain up front

    rhi::TextureDesc d;
    d.format = rhi::Format::Depth24;
    d.width  = m_res;
    d.height = m_res;
    d.usage  = rhi::UsageRenderTarget;
    m_depth = gpu.createTexture(d);

    rhi::FramebufferDesc fb;
    fb.depth.texture = m_depth;
    m_fbo = gpu.createFramebuffer(fb);
}

CubeRenderTarget::~CubeRenderTarget() {
    rhi::Device& gpu = rhi::device();
    if (m_fbo)   gpu.destroy(m_fbo);
    if (m_depth) gpu.destroy(m_depth);
    if (m_cube)  gpu.destroy(m_cube);
}

CubeRenderTarget::CubeRenderTarget(CubeRenderTarget&& o) noexcept
    : m_fbo(std::exchange(o.m_fbo, rhi::FramebufferHandle{})),
      m_cube(std::exchange(o.m_cube, rhi::TextureHandle{})),
      m_depth(std::exchange(o.m_depth, rhi::TextureHandle{})),
      m_res(std::exchange(o.m_res, 0)) {}

CubeRenderTarget& CubeRenderTarget::operator=(CubeRenderTarget&& o) noexcept {
    if (this != &o) {
        rhi::Device& gpu = rhi::device();
        if (m_fbo)   gpu.destroy(m_fbo);
        if (m_depth) gpu.destroy(m_depth);
        if (m_cube)  gpu.destroy(m_cube);
        m_fbo   = std::exchange(o.m_fbo, rhi::FramebufferHandle{});
        m_cube  = std::exchange(o.m_cube, rhi::TextureHandle{});
        m_depth = std::exchange(o.m_depth, rhi::TextureHandle{});
        m_res   = std::exchange(o.m_res, 0);
    }
    return *this;
}

void CubeRenderTarget::beginFace(int face) {
    rhi::Device& gpu = rhi::device();
    rhi::FramebufferDesc fb;
    fb.color[0]      = rhi::Attachment{m_cube, 0, face};
    fb.depth.texture = m_depth;
    gpu.setAttachments(m_fbo, fb);
    gpu.bindFramebuffer(m_fbo);
    gpu.setViewport(0, 0, m_res, m_res);
}

void CubeRenderTarget::generateMipmaps() const {
    rhi::device().generateMipmaps(m_cube);
}

void CubeRenderTarget::bindTexture(std::uint32_t unit) const {
    rhi::device().bindTexture(static_cast<int>(unit), m_cube);
}

int CubeRenderTarget::mipLevels() const {
    return 1 + static_cast<int>(std::floor(std::log2(static_cast<float>(m_res))));
}

glm::mat4 CubeRenderTarget::faceView(const glm::vec3& pos, int face) {
    const glm::vec3* dirs = CubeShadowMap::faceDirs();
    const glm::vec3* ups  = CubeShadowMap::faceUps();
    return glm::lookAt(pos, pos + dirs[face], ups[face]);
}

glm::mat4 CubeRenderTarget::faceProjection(float nearZ, float farZ) {
    return glm::perspective(glm::radians(90.0f), 1.0f, nearZ, farZ);
}

} // namespace fitzel
