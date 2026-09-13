#include "Motes.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <glad/gl.h>

bool Motes::init() {
    m_shader = fitzel::Shader::fromFiles("assets/shaders/mote.vert", "assets/shaders/mote.frag");
    if (!m_shader.isValid()) {
        std::fprintf(stderr, "Failed to load mote shader\n");
        return false;
    }
    // No base mesh: the quad's corner comes from gl_VertexID.
    glGenVertexArrays(1, &m_vao);
    glBindVertexArray(m_vao);
    glGenBuffers(1, &m_inst);
    glBindBuffer(GL_ARRAY_BUFFER, m_inst);
    const GLsizei st = kFloats * sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, st, (void*)0);
    glVertexAttribDivisor(0, 1);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, st, (void*)(3 * sizeof(float)));
    glVertexAttribDivisor(1, 1);
    glBindVertexArray(0);
    return true;
}

void Motes::place(Mote& m, const World& w, bool anywhere) {
    if (anywhere) {
        m.pos.x = w.eye.x + (uni() * 2.0f - 1.0f) * radius;
        m.pos.z = w.eye.z + (uni() * 2.0f - 1.0f) * radius;
    }
    m.ground = floorAt(w, m.pos.x, m.pos.z);
    // More of it low, where the meadow sheds it, but plenty carried up past
    // the eye -- which is where it is seen, against the hills and the shade.
    const float u = uni();
    m.above = 0.15f + u * (0.5f + 0.5f * u) * 8.0f;
    const float k = uni();
    m.kind  = k < 0.72f ? 0.0f : k < 0.80f ? 1.0f : 2.0f;
    m.size  = m.kind == 1.0f ? 0.006f + uni() * 0.004f     // a dandelion clock's seed
            : m.kind == 0.0f ? 0.0015f + uni() * 0.002f    // a grain of pollen, a clump
                             : 0.001f + uni() * 0.001f;    // dust
    m.phase = uni() * 6.2831853f;
    m.pos.y = m.ground + m.above;
}

void Motes::update(float dt, const World& w) {
    if (!enabled || !m_shader.isValid()) { m_data.clear(); return; }
    dt = std::clamp(dt, 0.0f, 0.1f);
    m_time += dt;
    m_weather = w.weather;
    ++m_frame;
    if (static_cast<int>(m_motes.size()) != count) {
        m_motes.assign(static_cast<std::size_t>(std::max(0, count)), Mote{});
        for (Mote& m : m_motes) place(m, w, true);
    }
    // Someone teleported (a shot, a respawn): the box is somewhere else.
    if (!m_motes.empty() &&
        glm::length(glm::vec2(m_motes[0].pos.x - w.eye.x, m_motes[0].pos.z - w.eye.z)) > radius * 3.0f)
        for (Mote& m : m_motes) place(m, w, true);

    const glm::vec2 wdir = w.wind ? w.wind->dir : glm::vec2(1.0f, 0.0f);
    const float     wstr = w.wind ? w.wind->strength : 0.2f;
    const float     t    = m_time;
    const float     span = 2.0f * radius;
    for (std::size_t i = 0; i < m_motes.size(); ++i) {
        Mote& m = m_motes[i];
        // The breeze, gusts and all, and the eddies in it: a mote never goes
        // in a straight line, it loops and hangs and is swept on again.
        const float g = w.wind ? wind::gust(*w.wind, glm::vec2(m.pos.x, m.pos.z)) : 1.0f;
        const float air = wstr * g * 2.4f;
        glm::vec3 v(wdir.x * air, 0.0f, wdir.y * air);
        const float ph = m.phase;
        v.x += 0.22f * std::sin(t * 0.53f + ph + m.pos.y * 0.9f);
        v.z += 0.22f * std::cos(t * 0.41f + ph * 1.7f + m.pos.x * 0.3f);
        v.y  = 0.10f * std::sin(t * 0.37f + ph * 2.3f + m.pos.z * 0.4f)
             + (m.kind == 1.0f ? 0.05f : -0.015f);           // fluff lifts, pollen settles
        // Near the ground the air is slower, and it lifts over it.
        v.x *= 0.45f + 0.55f * std::min(1.0f, m.above / 1.5f);
        v.z *= 0.45f + 0.55f * std::min(1.0f, m.above / 1.5f);
        m.pos.x += v.x * dt;
        m.pos.z += v.z * dt;
        m.above += v.y * dt;
        if (m.above < 0.1f) m.above = 0.1f + (0.1f - m.above);
        if (m.above > 8.0f) m.above = 8.0f - (m.above - 8.0f);

        // The box goes with the eye: what drifts out of it comes back in on
        // the other side, at a height of its own.
        bool wrapped = false;
        if (m.pos.x - w.eye.x >  radius) { m.pos.x -= span; wrapped = true; }
        if (m.pos.x - w.eye.x < -radius) { m.pos.x += span; wrapped = true; }
        if (m.pos.z - w.eye.z >  radius) { m.pos.z -= span; wrapped = true; }
        if (m.pos.z - w.eye.z < -radius) { m.pos.z += span; wrapped = true; }
        if (wrapped) place(m, w, false);
        else if (((static_cast<int>(i) + m_frame) & 7) == 0)
            m.ground = floorAt(w, m.pos.x, m.pos.z);            // a few each frame
        m.pos.y = m.ground + m.above;
    }

    // Instances: only what could be seen (inside the fade, above the floor of it).
    m_data.clear();
    if (m_weather > 0.85f) return;
    for (const Mote& m : m_motes) {
        const glm::vec3 d = m.pos - w.eye;
        if (glm::dot(d, d) > radius * radius) continue;
        m_data.insert(m_data.end(), {m.pos.x, m.pos.y, m.pos.z, m.size, m.kind, m.phase});
    }
}

void Motes::draw(const FrameContext& c, float pxScale) {
    if (!enabled || !m_shader.isValid() || m_data.empty()) return;
    glBindBuffer(GL_ARRAY_BUFFER, m_inst);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(m_data.size() * sizeof(float)),
                 m_data.data(), GL_STREAM_DRAW);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    m_shader.bind();
    m_shader.setMat4("uViewProj", c.viewProj);
    m_shader.setVec3("uViewPos", c.camPos);
    m_shader.setVec3("uLightDir", c.lightDir);
    m_shader.setVec3("uLightColor", c.lightColor);
    m_shader.setVec3("uAmbient", c.ambient);
    m_shader.setFloat("uPxScale", pxScale);
    m_shader.setFloat("uRadius", radius);
    m_shader.setFloat("uTime", m_time);
    m_shader.setFloat("uClear", 1.0f - std::clamp(m_weather / 0.85f, 0.0f, 1.0f));
    m_shader.setInt("uReactive", 0);
    applySunShadows(m_shader, c);
    glBindVertexArray(m_vao);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(m_data.size() / kFloats));
    glBindVertexArray(0);
    glEnable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    m_drawnThisFrame = true;
}

void Motes::drawReactive(const glm::mat4& viewProj) {
    if (!m_drawnThisFrame || m_data.empty()) return;
    m_drawnThisFrame = false;
    // The instances draw() uploaded, the same (jittered) projection, so the
    // flag lands on exactly the pixels the colour did. Everything else the
    // shader wants is still set from draw().
    //
    // With depth written this time: the post chain's depth of field reads the
    // depth buffer, and a mote that left none there is blurred as if it were
    // the far hillside behind it -- a speck smeared into a faint pink disc.
    // The motion pass is the last thing before the post chain, so nothing in
    // the scene is left to be hidden behind a grain of pollen.
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glDisable(GL_CULL_FACE);
    m_shader.bind();
    m_shader.setMat4("uViewProj", viewProj);
    m_shader.setInt("uReactive", 1);
    glBindVertexArray(m_vao);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(m_data.size() / kFloats));
    glBindVertexArray(0);
    m_shader.setInt("uReactive", 0);
    glEnable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
}
