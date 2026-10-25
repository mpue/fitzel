#include "TreePreview.hpp"

#include <algorithm>
#include <cmath>

#include <fitzel/graphics/Shader.hpp>
#include <fitzel/rhi/Device.hpp>
#include <glm/gtc/matrix_transform.hpp>

using fitzel::RenderTarget;
using fitzel::Shader;
using fitzel::Texture;
namespace rhi = fitzel::rhi;

TreePreview::~TreePreview() {
    release(m_bark);
    release(m_leaves);
    release(m_ground);
}

bool TreePreview::init() {
    m_lit   = Shader::fromFiles("assets/shaders/treepreview.vert", "assets/shaders/treepreview.frag");
    m_depth = Shader::fromFiles("assets/shaders/treepreviewdepth.vert",
                                "assets/shaders/treepreviewdepth.frag");
    return m_lit.isValid() && m_depth.isValid();
}

void TreePreview::release(Part& p) {
    rhi::Device& gpu = rhi::device();
    if (p.vbo) gpu.destroy(p.vbo);
    if (p.ibo) gpu.destroy(p.ibo);
    if (p.vao) gpu.destroy(p.vao);
    p = Part{};
}

void TreePreview::upload(Part& p, const std::vector<float>& v, const std::vector<std::uint32_t>& ix) {
    release(p);
    if (ix.empty()) return;
    // Runs inside the editor's frame too -- making buffers and a vertex array
    // binds nothing, so the editor's bindings stay as they were.
    rhi::Device& gpu = rhi::device();
    p.vbo = gpu.createBuffer(rhi::BufferKind::Vertex, rhi::BufferUsage::Static,
                             v.size() * sizeof(float), v.data());
    p.ibo = gpu.createBuffer(rhi::BufferKind::Index, rhi::BufferUsage::Static,
                             ix.size() * sizeof(std::uint32_t), ix.data());
    const std::uint32_t st = 8 * sizeof(float);
    const rhi::VertexAttribute attrs[] = {
        {0, p.vbo, rhi::VertexFormat::Float3, 0, st},
        {1, p.vbo, rhi::VertexFormat::Float3, 3 * sizeof(float), st},
        {2, p.vbo, rhi::VertexFormat::Float2, 6 * sizeof(float), st},
    };
    rhi::VertexArrayDesc desc;
    desc.attributes     = attrs;
    desc.attributeCount = 3;
    desc.indexBuffer    = p.ibo;
    p.vao = gpu.createVertexArray(desc);
    p.count = static_cast<int>(ix.size());
}

void TreePreview::setMesh(const treegen::Mesh& m) {
    upload(m_bark, m.bark, m.barkIdx);
    upload(m_leaves, m.leaves, m.leafIdx);
    if (!m.empty()) { m_lo = m.lo; m_hi = m.hi; }
    // A patch of ground a little wider than the crown.
    m_groundR = std::max({std::abs(m_lo.x), std::abs(m_hi.x), std::abs(m_lo.z), std::abs(m_hi.z),
                          0.3f * (m_hi.y - m_lo.y)}) * 1.6f + 1.0f;
    const float r = m_groundR;
    const std::vector<float> g = {-r, 0, -r, 0, 1, 0, 0, 0,  r, 0, -r, 0, 1, 0, 1, 0,
                                   r, 0,  r, 0, 1, 0, 1, 1, -r, 0,  r, 0, 1, 0, 0, 1};
    upload(m_ground, g, {0, 2, 1, 0, 3, 2});
}

void TreePreview::setTextures(Texture bark, Texture leaf, Texture barkNormal) {
    m_barkTex = std::move(bark);
    m_leafTex = std::move(leaf);
    m_barkNrm = std::move(barkNormal);
}

void TreePreview::drawParts(const Shader& sh) const {
    rhi::Device& gpu = rhi::device();
    if (m_bark.count) {
        m_barkTex.bind(0);
        const bool relief = m_barkNrm.isValid() && m_relief > 0.0f;
        if (relief) m_barkNrm.bind(2);
        sh.setInt("uHasNormal", relief ? 1 : 0);
        sh.setFloat("uNormalStrength", m_relief);
        sh.setInt("uLeaf", 0);
        gpu.bindVertexArray(m_bark.vao);
        gpu.drawIndexed(rhi::Primitive::Triangles, m_bark.count, rhi::IndexType::U32);
    }
    if (m_leaves.count) {
        m_leafTex.bind(0);
        sh.setInt("uHasNormal", 0);
        sh.setInt("uLeaf", 1);
        gpu.bindVertexArray(m_leaves.vao);
        gpu.drawIndexed(rhi::Primitive::Triangles, m_leaves.count, rhi::IndexType::U32);
    }
}

rhi::TextureHandle TreePreview::render(int w, int h, const View& v) {
    if (!m_lit.isValid() || w < 8 || h < 8) return {};
    rhi::Device& gpu = rhi::device();
    // What we touch, to put back: this runs inside the editor's frame.
    const rhi::FramebufferHandle prevFbo  = gpu.boundFramebuffer();
    const rhi::ProgramHandle     prevProg = gpu.boundProgram();
    const rhi::VertexArrayHandle prevVao  = gpu.boundVertexArray();
    const rhi::Rect              vp       = gpu.viewport();
    const glm::vec4              prevClear = gpu.clearColor();
    const rhi::RenderState       prevState = gpu.renderState();
    rhi::TextureHandle prevTex[3];
    for (int u = 0; u < 3; ++u) prevTex[u] = gpu.boundTexture(u);

    if (!m_target || m_target->width() != w || m_target->height() != h)
        m_target = std::make_unique<RenderTarget>(w, h, RenderTarget::Format::RGBA8);
    if (!m_shadow) m_shadow = std::make_unique<RenderTarget>(2048, 2048, RenderTarget::Format::RGBA8, true);

    const float kD = 3.14159265f / 180.0f;
    const glm::vec3 center = 0.5f * (m_lo + m_hi);
    const float radius = std::max(0.5f * glm::length(m_hi - m_lo), 0.5f);
    const glm::vec3 sun = glm::normalize(glm::vec3(std::cos(v.sunPitch * kD) * std::sin(v.sunYaw * kD),
                                                   std::sin(v.sunPitch * kD),
                                                   std::cos(v.sunPitch * kD) * std::cos(v.sunYaw * kD)));
    // The sun's view: an ortho box round the tree and the ground under it.
    const float boxR = std::max(radius, m_groundR) * 1.05f;
    const glm::mat4 lightView = glm::lookAt(center + sun * (boxR * 2.0f), center,
                                            std::abs(sun.y) > 0.95f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0));
    const glm::mat4 lightVP = glm::ortho(-boxR, boxR, -boxR, boxR, 0.05f, boxR * 4.0f) * lightView;

    gpu.setBlendEnabled(false);
    gpu.setScissorTest(false);
    gpu.setDepthTest(true);
    gpu.setDepthWrite(true);
    gpu.setCullEnabled(false);

    // Shadow map.
    m_shadow->bind();
    gpu.setClearColor(glm::vec4(1.0f));
    gpu.clear(rhi::ClearColor | rhi::ClearDepth);
    m_depth.bind();
    m_depth.setMat4("uViewProj", lightVP);
    m_depth.setInt("uTex", 0);
    drawParts(m_depth);

    // The picture.
    const float fov = 32.0f;
    const float dist = radius / std::sin(0.5f * fov * kD) * std::max(v.zoom, 0.05f) * 0.92f;
    const glm::vec3 look = center + glm::vec3(0.0f, v.lift * (m_hi.y - m_lo.y), 0.0f);
    const glm::vec3 eye = look + dist * glm::vec3(std::cos(v.pitch * kD) * std::sin(v.yaw * kD),
                                                  std::sin(v.pitch * kD),
                                                  std::cos(v.pitch * kD) * std::cos(v.yaw * kD));
    const glm::mat4 proj = glm::perspective(fov * kD, static_cast<float>(w) / static_cast<float>(h),
                                            std::max(0.02f, dist * 0.02f), dist * 6.0f + m_groundR * 2.0f);
    const glm::mat4 viewProj = proj * glm::lookAt(eye, look, glm::vec3(0, 1, 0));
    const glm::vec3 sky(0.66f, 0.74f, 0.82f);

    m_target->bind();
    gpu.setClearColor(glm::vec4(sky, 1.0f));
    gpu.clear(rhi::ClearColor | rhi::ClearDepth);
    m_lit.bind();
    m_lit.setMat4("uViewProj", viewProj);
    m_lit.setMat4("uLightVP", lightVP);
    m_lit.setVec3("uSun", sun);
    m_lit.setVec3("uEye", eye);
    m_lit.setVec3("uSky", sky);
    m_lit.setFloat("uGroundR", m_groundR);
    m_lit.setInt("uTex", 0);
    m_lit.setInt("uShadow", 1);
    m_lit.setInt("uNormalTex", 2);
    m_shadow->bindDepthTexture(1);
    if (m_ground.count) {
        m_lit.setInt("uGround", 1);
        m_lit.setInt("uLeaf", 0);
        m_lit.setInt("uHasNormal", 0);
        m_barkTex.bind(0);
        gpu.bindVertexArray(m_ground.vao);
        gpu.drawIndexed(rhi::Primitive::Triangles, m_ground.count, rhi::IndexType::U32);
    }
    m_lit.setInt("uGround", 0);
    drawParts(m_lit);

    // Put everything back.
    for (int u = 0; u < 3; ++u) gpu.bindTexture(u, prevTex[u]);
    gpu.setClearColor(prevClear);
    gpu.bindVertexArray(prevVao);
    gpu.bindProgram(prevProg);
    gpu.bindFramebuffer(prevFbo);
    gpu.setViewport(vp);
    gpu.setRenderState(prevState);
    return m_target->colorTexture();
}

std::vector<std::uint8_t> TreePreview::readRGBA() const {
    std::vector<std::uint8_t> px;
    if (!m_target) return px;
    const int w = m_target->width(), h = m_target->height();
    px.resize(static_cast<std::size_t>(w * h * 4));
    rhi::device().readPixels(m_target->framebuffer(), 0, rhi::Rect{0, 0, w, h}, rhi::Format::RGBA8,
                             px.data());
    // The read comes bottom row first.
    const std::size_t row = static_cast<std::size_t>(w * 4);
    for (int y = 0; y < h / 2; ++y)
        std::swap_ranges(px.begin() + static_cast<std::ptrdiff_t>(y * row),
                         px.begin() + static_cast<std::ptrdiff_t>((y + 1) * row),
                         px.begin() + static_cast<std::ptrdiff_t>((h - 1 - y) * row));
    return px;
}