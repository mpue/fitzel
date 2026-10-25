#include "fitzel/graphics/CubeShadowMap.hpp"

#include <utility>

#include "fitzel/rhi/Device.hpp"

namespace fitzel {

CubeShadowMap::CubeShadowMap(int resolution) : m_res(resolution) {
    rhi::Device& gpu = rhi::device();
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

CubeShadowMap::~CubeShadowMap() {
    if (m_fbo)   rhi::device().destroy(m_fbo);
    if (m_depth) rhi::device().destroy(m_depth);
}

CubeShadowMap::CubeShadowMap(CubeShadowMap&& o) noexcept
    : m_fbo(std::exchange(o.m_fbo, rhi::FramebufferHandle{})),
      m_depth(std::exchange(o.m_depth, rhi::TextureHandle{})),
      m_res(std::exchange(o.m_res, 0)),
      m_array(std::exchange(o.m_array, rhi::TextureHandle{})),
      m_firstLayer(std::exchange(o.m_firstLayer, 0)) {}

CubeShadowMap& CubeShadowMap::operator=(CubeShadowMap&& o) noexcept {
    if (this != &o) {
        if (m_fbo)   rhi::device().destroy(m_fbo);
        if (m_depth) rhi::device().destroy(m_depth);
        m_fbo   = std::exchange(o.m_fbo, rhi::FramebufferHandle{});
        m_depth = std::exchange(o.m_depth, rhi::TextureHandle{});
        m_res   = std::exchange(o.m_res, 0);
        m_array = std::exchange(o.m_array, rhi::TextureHandle{});
        m_firstLayer = std::exchange(o.m_firstLayer, 0);
    }
    return *this;
}

void CubeShadowMap::beginFace(int face, bool clearColour) {
    rhi::Device& gpu = rhi::device();
    rhi::FramebufferDesc fb;
    fb.color[0]      = rhi::Attachment{m_array, 0, m_firstLayer + face};
    fb.depth.texture = m_depth;
    gpu.setAttachments(m_fbo, fb);
    gpu.bindFramebuffer(m_fbo);
    gpu.setViewport(0, 0, m_res, m_res);
    gpu.setClearColor(glm::vec4(1.0f)); // normalized far
    gpu.clear(clearColour ? (rhi::ClearColor | rhi::ClearDepth) : rhi::ClearDepth);
}

void CubeShadowMap::copyLayer(rhi::TextureHandle src, rhi::TextureHandle dst, int layer) {
    rhi::Device& gpu = rhi::device();
    rhi::FramebufferDesc fb;
    fb.color[0]      = rhi::Attachment{src, 0, layer};
    fb.depth.texture = m_depth;
    gpu.setAttachments(m_fbo, fb);
    gpu.bindFramebuffer(m_fbo);
    gpu.copyToTexture(m_fbo, 0, rhi::Rect{0, 0, m_res, m_res}, dst, 0, layer, 0, 0);
}

const glm::vec3* CubeShadowMap::faceDirs() {
    static const glm::vec3 dirs[6] = {
        { 1, 0, 0}, {-1, 0, 0}, {0,  1, 0}, {0, -1, 0}, {0, 0,  1}, {0, 0, -1}};
    return dirs;
}
const glm::vec3* CubeShadowMap::faceUps() {
    static const glm::vec3 ups[6] = {
        {0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
    return ups;
}

} // namespace fitzel
