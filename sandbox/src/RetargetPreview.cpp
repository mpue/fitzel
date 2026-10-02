#include "RetargetPreview.hpp"

#include <algorithm>
#include <cmath>

#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>

using fitzel::Mesh;
using fitzel::Vertex;

namespace {

// Everything the studio touches, taken on construction and put back on
// destruction: it draws (and uploads) in the middle of the editor's frame.
struct GlSaved {
    GLint fbo = 0, prog = 0, vao = 0, arrayBuf = 0, active = 0, tex0 = 0, vp[4] = {0, 0, 0, 0};
    GLboolean depthMask = GL_TRUE, depth = GL_FALSE, cull = GL_FALSE, blend = GL_FALSE, scissor = GL_FALSE;
    GLfloat clear[4] = {0, 0, 0, 0};
    GlSaved() {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
        glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuf);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
        glGetIntegerv(GL_VIEWPORT, vp);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
        depth = glIsEnabled(GL_DEPTH_TEST);
        cull = glIsEnabled(GL_CULL_FACE);
        blend = glIsEnabled(GL_BLEND);
        scissor = glIsEnabled(GL_SCISSOR_TEST);
    }
    ~GlSaved() {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(tex0));
        glActiveTexture(static_cast<GLenum>(active));
        glBindVertexArray(static_cast<GLuint>(vao));
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(arrayBuf));
        glUseProgram(static_cast<GLuint>(prog));
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(fbo));
        glViewport(vp[0], vp[1], vp[2], vp[3]);
        glClearColor(clear[0], clear[1], clear[2], clear[3]);
        glDepthMask(depthMask);
        auto on = [](GLenum cap, GLboolean v) { if (v) glEnable(cap); else glDisable(cap); };
        on(GL_DEPTH_TEST, depth);
        on(GL_CULL_FACE, cull);
        on(GL_BLEND, blend);
        on(GL_SCISSOR_TEST, scissor);
    }
};

const glm::vec3 kBackdrop(0.70f, 0.73f, 0.77f);
const glm::vec3 kLight = glm::normalize(glm::vec3(0.35f, -1.0f, -0.55f));
const glm::vec4 kWood(0.80f, 0.70f, 0.56f, 1.0f);

void emit(std::vector<Vertex>& out, const glm::vec3& p, const glm::vec3& n) {
    Vertex v{};
    v.position = p;
    v.normal = n;
    out.push_back(v);
}

void sphere(std::vector<Vertex>& out, const glm::vec3& c, float r) {
    const int slices = 12, stacks = 8;
    const float pi = 3.14159265f;
    auto at = [&](int i, int j) {
        const float th = pi * static_cast<float>(j) / stacks, ph = 2.0f * pi * static_cast<float>(i) / slices;
        return glm::vec3(std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph));
    };
    for (int j = 0; j < stacks; ++j)
        for (int i = 0; i < slices; ++i) {
            const glm::vec3 a = at(i, j), b = at(i + 1, j), d = at(i, j + 1), e = at(i + 1, j + 1);
            for (const glm::vec3& n : {a, d, e, a, e, b}) emit(out, c + n * r, n);
        }
}

void tube(std::vector<Vertex>& out, const glm::vec3& a, const glm::vec3& b, float r) {
    const glm::vec3 axis = b - a;
    if (glm::length(axis) < 1e-5f) return;
    const glm::vec3 d = glm::normalize(axis);
    const glm::vec3 u = glm::normalize(glm::cross(d, std::abs(d.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0)));
    const glm::vec3 w = glm::cross(d, u);
    const int seg = 12;
    for (int i = 0; i < seg; ++i) {
        const float t0 = 6.2831853f * static_cast<float>(i) / seg, t1 = 6.2831853f * static_cast<float>(i + 1) / seg;
        const glm::vec3 n0 = u * std::cos(t0) + w * std::sin(t0), n1 = u * std::cos(t1) + w * std::sin(t1);
        emit(out, a + n0 * r, n0); emit(out, b + n0 * r, n0); emit(out, b + n1 * r, n1);
        emit(out, a + n0 * r, n0); emit(out, b + n1 * r, n1); emit(out, a + n1 * r, n1);
    }
}

} // namespace

void RetargetPreview::setMannequin(int side, std::vector<Capsule> caps) {
    Side& s = m_side[side & 1];
    s.caps = std::move(caps);
    s.capsDirty = true;
}

bool RetargetPreview::init() {
    m_sh = fitzel::Shader::fromFiles("assets/shaders/retargetpreview.vert", "assets/shaders/retargetpreview.frag");
    return m_sh.isValid();
}

void RetargetPreview::setModel(int side, const fitzel::ModelData* model) {
    Side& s = m_side[side & 1];
    const GlSaved saved;
    s.parts.clear();
    s.caps.clear();
    s.capsDirty = true;
    s.model = model;
    s.palette.clear();
    s.dirty = true;
    if (!model) return;
    for (std::size_t i = 0; i < model->primitives.size(); ++i) {
        const fitzel::ModelPrimitive& p = model->primitives[i];
        if (p.vertexCount() == 0) continue;
        Part part;
        part.prim = i;
        fitzel::skinPrimitive(p, {}, m_scratch);
        part.mesh = Mesh::create(m_scratch);
        if (!p.texPixels.empty() && p.texWidth > 0 && p.texHeight > 0) {
            part.tex = fitzel::Texture::fromPixels(p.texPixels.data(), p.texWidth, p.texHeight, 4);
            part.hasTex = part.tex.isValid();
        }
        part.cutout = p.alphaCutout;
        part.color = glm::vec4(p.baseColor[0], p.baseColor[1], p.baseColor[2], p.baseColor[3]);
        s.parts.push_back(std::move(part));
    }
}

void RetargetPreview::setPose(int side, const std::vector<glm::mat4>& palette, const glm::mat4& place) {
    Side& s = m_side[side & 1];
    s.palette = palette;
    s.place = place;
    s.dirty = true;
}

glm::vec2 RetargetPreview::project(const glm::vec3& p) const {
    const glm::vec4 c = m_viewProj * glm::vec4(p, 1.0f);
    if (std::abs(c.w) < 1e-6f) return glm::vec2(-1.0f);
    const glm::vec3 n = glm::vec3(c) / c.w;
    return glm::vec2((n.x * 0.5f + 0.5f) * static_cast<float>(width()),
                     (0.5f - n.y * 0.5f) * static_cast<float>(height()));
}

std::uint32_t RetargetPreview::render(int w, int h, const View& v) {
    if (!m_sh.isValid() || w < 8 || h < 8) return 0;
    const GlSaved saved;
    if (!m_target || m_target->width() != w || m_target->height() != h)
        m_target = std::make_unique<fitzel::RenderTarget>(w, h, fitzel::RenderTarget::Format::RGBA8);

    // New poses: skin on the CPU, refill the meshes.
    for (Side& s : m_side) {
        if (s.capsDirty) {
            m_scratch.clear();
            for (const Capsule& c : s.caps) {
                tube(m_scratch, c.a, c.b, c.r);
                sphere(m_scratch, c.a, c.r);
                sphere(m_scratch, c.b, c.r);
            }
            if (!m_scratch.empty()) s.mannequin.update(m_scratch);
            s.capsDirty = false;
        }
        if (!s.dirty || !s.model) continue;
        for (Part& part : s.parts) {
            fitzel::skinPrimitive(s.model->primitives[part.prim], s.palette, m_scratch);
            part.mesh.update(m_scratch);
        }
        s.dirty = false;
    }
    if (!m_floor.vertexCount()) {
        const float r = 1.0f;
        std::vector<Vertex> q(4);
        const float xz[4][2] = {{-r, -r}, {r, -r}, {r, r}, {-r, r}};
        for (int i = 0; i < 4; ++i) {
            q[static_cast<std::size_t>(i)].position = glm::vec3(xz[i][0], 0.0f, xz[i][1]);
            q[static_cast<std::size_t>(i)].normal = glm::vec3(0.0f, 1.0f, 0.0f);
        }
        m_floor = Mesh::create(q, {0, 2, 1, 0, 3, 2});
    }

    const float kD = 3.14159265f / 180.0f;
    const float fov = 30.0f;
    const glm::vec3 eye = v.target + v.dist * glm::vec3(0.0f, std::sin(v.pitch * kD), std::cos(v.pitch * kD));
    const glm::mat4 proj = glm::perspective(fov * kD, static_cast<float>(w) / static_cast<float>(h),
                                            std::max(0.05f, v.dist * 0.02f), v.dist * 4.0f + v.floorR * 2.0f);
    m_viewProj = proj * glm::lookAt(eye, v.target, glm::vec3(0.0f, 1.0f, 0.0f));

    m_target->bind();
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glClearColor(kBackdrop.r, kBackdrop.g, kBackdrop.b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    m_sh.bind();
    m_sh.setMat4("uViewProj", m_viewProj);
    m_sh.setVec3("uLightDir", kLight);
    m_sh.setVec3("uEye", eye);
    m_sh.setVec3("uBackdrop", kBackdrop);
    m_sh.setFloat("uFloorR", v.floorR);
    m_sh.setInt("uTex", 0);
    m_sh.setInt("uFlatten", 0);
    glActiveTexture(GL_TEXTURE0);

    // The floor, then the shadows on it (opaque: two overlapping are no darker).
    m_sh.setInt("uMode", 1);
    m_sh.setMat4("uModel", glm::scale(glm::mat4(1.0f), glm::vec3(v.floorR, 1.0f, v.floorR)));
    m_floor.draw();
    m_sh.setInt("uMode", 2);
    m_sh.setInt("uFlatten", 1);
    glDepthMask(GL_FALSE);
    for (const Side& s : m_side) {
        m_sh.setMat4("uModel", s.place);
        for (const Part& part : s.parts) part.mesh.draw();
        if (!s.caps.empty()) {
            m_sh.setMat4("uModel", glm::mat4(1.0f));
            s.mannequin.draw();
        }
    }
    glDepthMask(GL_TRUE);
    m_sh.setInt("uFlatten", 0);

    // The figures.
    m_sh.setInt("uMode", 0);
    for (const Side& s : m_side) {
        m_sh.setMat4("uModel", s.place);
        for (const Part& part : s.parts) {
            m_sh.setInt("uHasTex", part.hasTex ? 1 : 0);
            m_sh.setInt("uCutout", part.cutout ? 1 : 0);
            m_sh.setVec4("uColor", part.color);
            if (part.hasTex) part.tex.bind(0);
            part.mesh.draw();
        }
        if (!s.caps.empty()) {
            m_sh.setMat4("uModel", glm::mat4(1.0f));
            m_sh.setInt("uHasTex", 0);
            m_sh.setInt("uCutout", 0);
            m_sh.setVec4("uColor", glm::vec4(glm::pow(glm::vec3(kWood), glm::vec3(2.2f)), 1.0f));
            s.mannequin.draw();
        }
    }

    // The bones, over everything: camera-facing ribbons, a dot at each joint.
    if (v.bones && !m_segs.empty()) {
        std::vector<Vertex> vs;
        std::vector<std::uint32_t> ix;
        vs.reserve(m_segs.size() * 8);
        auto quad = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& side, const glm::vec4& col) {
            const std::uint32_t o = static_cast<std::uint32_t>(vs.size());
            for (const glm::vec3& p : {a - side, a + side, b + side, b - side}) {
                Vertex x{};
                x.position = p;
                x.normal = glm::vec3(0.0f, 1.0f, 0.0f);
                x.paint = col;
                vs.push_back(x);
            }
            for (std::uint32_t k : {0u, 1u, 2u, 0u, 2u, 3u}) ix.push_back(o + k);
        };
        for (const Segment& s : m_segs) {
            const glm::vec3 mid = 0.5f * (s.a + s.b);
            const glm::vec3 view = glm::normalize(eye - mid);
            glm::vec3 d = s.b - s.a;
            if (glm::length(d) < 1e-5f) d = glm::vec3(0.0f, 1e-3f, 0.0f);
            glm::vec3 side = glm::cross(d, view);
            side = glm::length(side) > 1e-6f ? glm::normalize(side) * (0.5f * s.width) : glm::vec3(0.0f);
            quad(s.a, s.b, side, s.color);
            // The joint: a small square facing the camera.
            const glm::vec3 right = glm::normalize(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), view));
            const glm::vec3 up = glm::cross(view, right);
            quad(s.b - up * s.width, s.b + up * s.width, right * s.width, s.color);
        }
        if (!m_haveBones) { m_bones = Mesh::create(vs, ix); m_haveBones = true; }
        else m_bones.update(vs, ix);
        glDisable(GL_DEPTH_TEST);
        m_sh.setInt("uMode", 3);
        m_sh.setMat4("uModel", glm::mat4(1.0f));
        m_bones.draw();
    }
    return m_target->colorTexture();
}

std::vector<std::uint8_t> RetargetPreview::readRGBA() const {
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
    const std::size_t row = static_cast<std::size_t>(w * 4);
    for (int y = 0; y < h / 2; ++y)
        std::swap_ranges(px.begin() + static_cast<std::ptrdiff_t>(y * row),
                         px.begin() + static_cast<std::ptrdiff_t>((y + 1) * row),
                         px.begin() + static_cast<std::ptrdiff_t>((h - 1 - y) * row));
    return px;
}