#include "MaterialPreview.hpp"

#include <algorithm>
#include <cmath>

#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>

#include <fitzel/graphics/Texture.hpp>

#include "SceneTypes.hpp"   // MaterialDef

using fitzel::RenderTarget;
using fitzel::Shader;

namespace {

constexpr float kPi = 3.14159265f;

// position, normal, uv -- 8 floats, the layout every preview shape shares
void vert(std::vector<float>& v, glm::vec3 p, glm::vec3 n, glm::vec2 uv) {
    v.insert(v.end(), {p.x, p.y, p.z, n.x, n.y, n.z, uv.x, uv.y});
}

void sphere(std::vector<float>& v, std::vector<std::uint32_t>& ix) {
    const int seg = 96, rings = 48;
    for (int r = 0; r <= rings; ++r) {
        const float th = kPi * static_cast<float>(r) / rings;
        for (int s = 0; s <= seg; ++s) {
            const float ph = 2.0f * kPi * static_cast<float>(s) / seg;
            const glm::vec3 n(std::sin(th) * std::sin(ph), std::cos(th), std::sin(th) * std::cos(ph));
            // twice round, so a texture keeps its proportions on the ball
            vert(v, n, n, glm::vec2(2.0f * static_cast<float>(s) / seg, 1.0f - static_cast<float>(r) / rings));
        }
    }
    for (int r = 0; r < rings; ++r)
        for (int s = 0; s < seg; ++s) {
            const std::uint32_t a = static_cast<std::uint32_t>(r * (seg + 1) + s), b = a + seg + 1;
            ix.insert(ix.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
}

void cube(std::vector<float>& v, std::vector<std::uint32_t>& ix) {
    const float s = 0.62f;
    const glm::vec3 nrm[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const glm::vec3& n : nrm) {
        const glm::vec3 up = std::abs(n.y) > 0.5f ? glm::vec3(0, 0, -n.y) : glm::vec3(0, 1, 0);
        const glm::vec3 rt = glm::cross(up, n);
        const auto base = static_cast<std::uint32_t>(v.size() / 8);
        vert(v, (n - rt - up) * s, n, {0, 0});
        vert(v, (n + rt - up) * s, n, {1, 0});
        vert(v, (n + rt + up) * s, n, {1, 1});
        vert(v, (n - rt + up) * s, n, {0, 1});
        ix.insert(ix.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
}

void tile(std::vector<float>& v, std::vector<std::uint32_t>& ix) {
    // a square facing the camera, a little tilted back, two repeats across
    const glm::vec3 n(0, 0, 1);
    vert(v, {-1, -1, 0}, n, {0, 0});
    vert(v, {1, -1, 0}, n, {2, 0});
    vert(v, {1, 1, 0}, n, {2, 2});
    vert(v, {-1, 1, 0}, n, {0, 2});
    ix = {0, 1, 2, 0, 2, 3};
}

} // namespace

MaterialPreview::~MaterialPreview() {
    release(m_sphere);
    release(m_cube);
    release(m_tile);
    release(m_quad);
}

void MaterialPreview::release(Part& p) {
    if (p.vbo) glDeleteBuffers(1, &p.vbo);
    if (p.ibo) glDeleteBuffers(1, &p.ibo);
    if (p.vao) glDeleteVertexArrays(1, &p.vao);
    p = Part{};
}

void MaterialPreview::upload(Part& p, const std::vector<float>& v, const std::vector<std::uint32_t>& ix) {
    release(p);
    if (ix.empty()) return;
    GLint prevVao = 0, prevArray = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArray);
    glGenVertexArrays(1, &p.vao);
    glBindVertexArray(p.vao);
    glGenBuffers(1, &p.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, p.vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(v.size() * sizeof(float)), v.data(), GL_STATIC_DRAW);
    glGenBuffers(1, &p.ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, p.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(ix.size() * sizeof(std::uint32_t)), ix.data(),
                 GL_STATIC_DRAW);
    const GLsizei st = 8 * sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(6 * sizeof(float)));
    p.count = static_cast<int>(ix.size());
    glBindVertexArray(static_cast<GLuint>(prevVao));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(prevArray));
}

bool MaterialPreview::init() {
    m_shader = Shader::fromFiles("assets/shaders/matpreview.vert", "assets/shaders/matpreview.frag");
    if (!m_shader.isValid()) return false;
    std::vector<float> v;
    std::vector<std::uint32_t> ix;
    sphere(v, ix); upload(m_sphere, v, ix); v.clear(); ix.clear();
    cube(v, ix);   upload(m_cube, v, ix);   v.clear(); ix.clear();
    tile(v, ix);   upload(m_tile, v, ix);   v.clear(); ix.clear();
    // the backdrop: a quad already in clip space, uv 0..1
    vert(v, {-1, -1, 0}, {0, 0, 1}, {0, 0});
    vert(v, {1, -1, 0}, {0, 0, 1}, {1, 0});
    vert(v, {1, 1, 0}, {0, 0, 1}, {1, 1});
    vert(v, {-1, 1, 0}, {0, 0, 1}, {0, 1});
    upload(m_quad, v, {0, 1, 2, 0, 2, 3});
    return true;
}

std::uint32_t MaterialPreview::render(const MaterialDef& md, int size, Shape shape, float yawDeg) {
    if (!m_shader.isValid() || size < 8) return 0;
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
    GLint prevTex[5] = {0, 0, 0, 0, 0};
    for (int u = 0; u < 5; ++u) {
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(u));
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex[u]);
    }
    GLint prevSrcRgb = 0, prevDstRgb = 0, prevSrcA = 0, prevDstA = 0, prevCull = GL_BACK;
    glGetIntegerv(GL_BLEND_SRC_RGB, &prevSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &prevDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &prevDstA);
    glGetIntegerv(GL_CULL_FACE_MODE, &prevCull);
    const GLboolean depthOn = glIsEnabled(GL_DEPTH_TEST), cullOn = glIsEnabled(GL_CULL_FACE),
                    blendOn = glIsEnabled(GL_BLEND), scissorOn = glIsEnabled(GL_SCISSOR_TEST);

    if (!m_target || m_target->width() != size || m_target->height() != size)
        m_target = std::make_unique<RenderTarget>(size, size, RenderTarget::Format::RGBA8);

    const float kD = kPi / 180.0f;
    const glm::vec3 eye(0.0f, 0.42f, 4.5f);   // the ball with a margin round it
    const glm::mat4 proj = glm::perspective(30.0f * kD, 1.0f, 0.1f, 20.0f);
    const glm::mat4 viewProj = proj * glm::lookAt(eye, glm::vec3(0.0f), glm::vec3(0, 1, 0));
    glm::mat4 model = glm::rotate(glm::mat4(1.0f), yawDeg * kD, glm::vec3(0, 1, 0));
    if (shape == Shape::Cube) model = glm::rotate(model, 0.45f, glm::vec3(1, 0, 0.3f));
    if (shape == Shape::Tile) model = glm::rotate(glm::mat4(1.0f), -0.35f, glm::vec3(1, 0, 0)) *
                                      glm::scale(glm::mat4(1.0f), glm::vec3(0.92f));

    m_target->bind();
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.15f, 0.15f, 0.16f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    m_shader.bind();
    m_shader.setMat4("uViewProj", viewProj);
    m_shader.setMat4("uModel", model);
    m_shader.setVec3("uEye", eye);

    // The backdrop, behind everything.
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    m_shader.setInt("uBackdrop", 1);
    glBindVertexArray(m_quad.vao);
    glDrawElements(GL_TRIANGLES, m_quad.count, GL_UNSIGNED_INT, nullptr);

    // The material.
    m_shader.setInt("uBackdrop", 0);
    m_shader.setVec2("uUvScale", glm::vec2(1.0f));
    const fitzel::Texture* base = md.tex ? md.tex.get() : (md.modelTex ? md.modelTex.get() : nullptr);
    m_shader.setInt("uTex", 0);
    m_shader.setInt("uNormalMap", 1);
    m_shader.setInt("uOrmMap", 2);
    m_shader.setInt("uEmissionMap", 3);
    m_shader.setInt("uOpacityMap", 4);
    if (base && base->isValid()) { base->bind(0); m_shader.setInt("uHasTex", 1); }
    else m_shader.setInt("uHasTex", 0);
    m_shader.setVec3("uAlbedo", md.albedo);
    m_shader.setVec3("uTint", md.tex ? md.tint : glm::vec3(1.0f));
    const fitzel::Texture* nrm = md.normalTex ? md.normalTex.get() : (md.modelNormalTex ? md.modelNormalTex.get() : nullptr);
    if (nrm && nrm->isValid()) {
        nrm->bind(1);
        m_shader.setInt("uHasNormal", 1);
        m_shader.setInt("uNormalTopDown", nrm->bottomUp() ? 0 : 1);
    } else {
        m_shader.setInt("uHasNormal", 0);
    }
    if (md.ormTex && md.ormTex->isValid()) {
        md.ormTex->bind(2);
        m_shader.setInt("uHasOrm", 1);
        m_shader.setVec2("uOrmMean", md.ormMean);
    } else {
        m_shader.setInt("uHasOrm", 0);
    }
    const fitzel::Texture* emi = md.emissionTex ? md.emissionTex.get()
                                                : (md.modelEmissionTex ? md.modelEmissionTex.get() : nullptr);
    if (emi && emi->isValid()) { emi->bind(3); m_shader.setInt("uHasEmissionMap", 1); }
    else m_shader.setInt("uHasEmissionMap", 0);
    if (md.opacityTex && md.opacityTex->isValid()) { md.opacityTex->bind(4); m_shader.setInt("uHasOpacityMap", 1); }
    else m_shader.setInt("uHasOpacityMap", 0);
    m_shader.setFloat("uRoughness", md.roughness);
    m_shader.setFloat("uReflectivity", md.reflectivity);
    m_shader.setVec3("uEmission", md.emission);
    m_shader.setFloat("uEmissionStrength", md.emissionStrength);
    m_shader.setFloat("uOpacity", md.opacity);
    m_shader.setInt("uAlphaMode", md.alphaMode == AlphaMode::Cutout ? 1 : md.alphaMode == AlphaMode::Blend ? 2 : 0);
    m_shader.setFloat("uAlphaCutoff", md.alphaCutoff);
    m_shader.setInt("uGlass", md.glass ? 1 : 0);

    const Part& part = shape == Shape::Cube ? m_cube : shape == Shape::Tile ? m_tile : m_sphere;
    const bool seeThrough = md.glass || md.alphaMode == AlphaMode::Blend;
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glBindVertexArray(part.vao);
    if (seeThrough) {
        // Back faces first, then the front: a see-through ball shows its far
        // side through the near one. The target's alpha stays 1 for ImGui.
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT);
        glDrawElements(GL_TRIANGLES, part.count, GL_UNSIGNED_INT, nullptr);
        glCullFace(GL_BACK);
        glDrawElements(GL_TRIANGLES, part.count, GL_UNSIGNED_INT, nullptr);
    } else {
        glDisable(GL_BLEND);
        if (shape == Shape::Tile || md.doubleSided) glDisable(GL_CULL_FACE);
        else { glEnable(GL_CULL_FACE); glCullFace(GL_BACK); }
        glDrawElements(GL_TRIANGLES, part.count, GL_UNSIGNED_INT, nullptr);
    }

    // Put everything back.
    for (int u = 0; u < 5; ++u) {
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(u));
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex[u]));
    }
    glActiveTexture(static_cast<GLenum>(prevActive));
    glBlendFuncSeparate(static_cast<GLenum>(prevSrcRgb), static_cast<GLenum>(prevDstRgb),
                        static_cast<GLenum>(prevSrcA), static_cast<GLenum>(prevDstA));
    glCullFace(static_cast<GLenum>(prevCull));
    glClearColor(prevClear[0], prevClear[1], prevClear[2], prevClear[3]);
    glBindVertexArray(static_cast<GLuint>(prevVao));
    glUseProgram(static_cast<GLuint>(prevProg));
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glDepthMask(prevMask);
    if (depthOn) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (cullOn) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (blendOn) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (scissorOn) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    return m_target->colorTexture();
}