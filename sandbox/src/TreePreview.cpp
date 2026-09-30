#include "TreePreview.hpp"

#include <algorithm>
#include <cmath>

#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>

using fitzel::RenderTarget;
using fitzel::Shader;
using fitzel::Texture;

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
    if (p.vbo) glDeleteBuffers(1, &p.vbo);
    if (p.ibo) glDeleteBuffers(1, &p.ibo);
    if (p.vao) glDeleteVertexArrays(1, &p.vao);
    p = Part{};
}

void TreePreview::upload(Part& p, const std::vector<float>& v, const std::vector<std::uint32_t>& ix) {
    release(p);
    if (ix.empty()) return;
    // Runs inside the editor's frame too: leave the bindings as they were.
    GLint prevVao = 0, prevArray = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArray);
    glGenVertexArrays(1, &p.vao);
    glBindVertexArray(p.vao);
    glGenBuffers(1, &p.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, p.vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(v.size() * sizeof(float)), v.data(),
                 GL_STATIC_DRAW);
    glGenBuffers(1, &p.ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, p.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(ix.size() * sizeof(std::uint32_t)),
                 ix.data(), GL_STATIC_DRAW);
    const GLsizei st = 8 * sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(6 * sizeof(float)));
    glBindVertexArray(static_cast<GLuint>(prevVao));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(prevArray));
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
    if (m_bark.count) {
        m_barkTex.bind(0);
        const bool relief = m_barkNrm.isValid() && m_relief > 0.0f;
        if (relief) m_barkNrm.bind(2);
        sh.setInt("uHasNormal", relief ? 1 : 0);
        sh.setFloat("uNormalStrength", m_relief);
        sh.setInt("uLeaf", 0);
        glBindVertexArray(m_bark.vao);
        glDrawElements(GL_TRIANGLES, m_bark.count, GL_UNSIGNED_INT, nullptr);
    }
    if (m_leaves.count) {
        m_leafTex.bind(0);
        sh.setInt("uHasNormal", 0);
        sh.setInt("uLeaf", 1);
        glBindVertexArray(m_leaves.vao);
        glDrawElements(GL_TRIANGLES, m_leaves.count, GL_UNSIGNED_INT, nullptr);
    }
}

std::uint32_t TreePreview::render(int w, int h, const View& v) {
    if (!m_lit.isValid() || w < 8 || h < 8) return 0;
    // What we touch, to put back: this runs inside the editor's frame.
    GLint prevFbo = 0, prevProg = 0, prevVao = 0, prevActive = 0, vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    glGetIntegerv(GL_VIEWPORT, vp);
    GLboolean prevMask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &prevMask);
    GLfloat prevClear[4] = {0, 0, 0, 0};
    glGetFloatv(GL_COLOR_CLEAR_VALUE, prevClear);
    GLint prevTex[3] = {0, 0, 0};
    for (int u = 0; u < 3; ++u) {
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(u));
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex[u]);
    }
    const GLboolean depthOn = glIsEnabled(GL_DEPTH_TEST), cullOn = glIsEnabled(GL_CULL_FACE),
                    blendOn = glIsEnabled(GL_BLEND), scissorOn = glIsEnabled(GL_SCISSOR_TEST);

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

    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_CULL_FACE);
    glActiveTexture(GL_TEXTURE0);

    // Shadow map.
    m_shadow->bind();
    glClearColor(1, 1, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
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
    glClearColor(sky.r, sky.g, sky.b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
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
    glActiveTexture(GL_TEXTURE0);
    if (m_ground.count) {
        m_lit.setInt("uGround", 1);
        m_lit.setInt("uLeaf", 0);
        m_lit.setInt("uHasNormal", 0);
        m_barkTex.bind(0);
        glBindVertexArray(m_ground.vao);
        glDrawElements(GL_TRIANGLES, m_ground.count, GL_UNSIGNED_INT, nullptr);
    }
    m_lit.setInt("uGround", 0);
    drawParts(m_lit);

    // Put everything back.
    for (int u = 0; u < 3; ++u) {
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(u));
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex[u]));
    }
    glClearColor(prevClear[0], prevClear[1], prevClear[2], prevClear[3]);
    glBindVertexArray(static_cast<GLuint>(prevVao));
    glUseProgram(static_cast<GLuint>(prevProg));
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glActiveTexture(static_cast<GLenum>(prevActive));
    glDepthMask(prevMask);
    if (depthOn) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (cullOn) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (blendOn) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (scissorOn) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    return m_target->colorTexture();
}

std::vector<std::uint8_t> TreePreview::readRGBA() const {
    std::vector<std::uint8_t> px;
    if (!m_target) return px;
    const int w = m_target->width(), h = m_target->height();
    px.resize(static_cast<std::size_t>(w * h * 4));
    GLint prev = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_target->framebuffer());
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prev));
    // GL reads bottom row first.
    const std::size_t row = static_cast<std::size_t>(w * 4);
    for (int y = 0; y < h / 2; ++y)
        std::swap_ranges(px.begin() + static_cast<std::ptrdiff_t>(y * row),
                         px.begin() + static_cast<std::ptrdiff_t>((y + 1) * row),
                         px.begin() + static_cast<std::ptrdiff_t>((h - 1 - y) * row));
    return px;
}