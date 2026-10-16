#include "SpraySystem.hpp"

#include <glad/gl.h>

void SprayPool::update(float dt, const SurfaceFn& surface, glm::vec3 wind) {
    for (std::size_t i = 0; i < m_parts.size();) {
        SprayP& p = m_parts[i];
        if (p.flat > 1.5f) {
            // Mist: hangs, drifts with the wind, swells and thins.
            p.vel += (wind - p.vel) * glm::min(1.0f, dt * 1.2f);
            p.vel.y += dt * 0.15f;
            p.pos += p.vel * dt;
            p.size += dt * 1.8f;
            p.life -= dt;
            if (p.life <= 0.0f) { p = m_parts.back(); m_parts.pop_back(); continue; }
        } else if (p.flat > 0.5f) {
            // Surface foam: rides the water, drifts and spreads, no gravity.
            p.pos += p.vel * dt;
            p.pos.y = surface(p.pos.x, p.pos.z) + 0.03f;
            p.vel *= glm::max(0.0f, 1.0f - dt * 1.6f); // drag out
            p.size += dt * 2.5f;                       // spread
            p.life -= dt;
            if (p.life <= 0.0f) { p = m_parts.back(); m_parts.pop_back(); continue; }
        } else {
            // Airborne droplet: arcs and falls back into the water.
            p.vel.y -= 9.81f * dt;
            p.vel += (wind * 0.35f - glm::vec3(p.vel.x, 0.0f, p.vel.z)) * glm::min(1.0f, dt * 0.25f);
            p.vel *= glm::max(0.0f, 1.0f - dt * 0.6f); // air drag
            p.pos += p.vel * dt;
            p.life -= dt;
            // The 0.3 m of slack lets a droplet break the surface before it dies,
            // so it reads as going *in* rather than winking out on contact.
            // Only asked of droplets on the way down: one coming up out of a
            // crest starts below where the crest is a moment later.
            if (p.life <= 0.0f ||
                (p.vel.y < 0.0f && p.pos.y < surface(p.pos.x, p.pos.z) - 0.3f)) {
                p = m_parts.back(); m_parts.pop_back(); continue;
            }
        }
        ++i;
    }
}

void SprayPool::pack(std::vector<float>& out) const {
    out.clear();
    out.reserve(m_parts.size() * kStride);
    for (const SprayP& p : m_parts)
        out.insert(out.end(), {p.pos.x, p.pos.y, p.pos.z, p.life / p.life0, p.size,
                               p.flat});
}

SpraySystem::~SpraySystem() {
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
}

bool SpraySystem::init() {
    m_shader = fitzel::Shader::fromFiles("assets/shaders/spray.vert",
                                         "assets/shaders/spray.frag");
    if (!m_shader.isValid()) return false;

    glGenVertexArrays(1, &m_vao);
    glBindVertexArray(m_vao);
    glGenBuffers(1, &m_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    // Sized for a full pool once; every frame then just overwrites the front of it.
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(SprayPool::kMax * SprayPool::kStride *
                                         sizeof(float)),
                 nullptr, GL_STREAM_DRAW);
    const GLsizei stride = SprayPool::kStride * sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, (void*)(4 * sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, (void*)(5 * sizeof(float)));
    glBindVertexArray(0);
    return true;
}

void SpraySystem::draw(const FrameContext& ctx) {
    if (!m_vao || m_pool.empty()) return;
    m_pool.pack(m_scratch);
    if (m_scratch.empty()) return;

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    static_cast<GLsizeiptr>(m_scratch.size() * sizeof(float)),
                    m_scratch.data());
    glEnable(GL_PROGRAM_POINT_SIZE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    m_shader.bind();
    m_shader.setMat4("uViewProj", ctx.viewProj);
    m_shader.setVec3("uCam", ctx.camPos);
    m_shader.setFloat("uSize", sizeScale);
    // Lit like the water it came off: dim at dusk, not a white sticker at night.
    m_shader.setVec3("uLight", ctx.lightColor * glm::max(ctx.lightDir.y, 0.0f));
    m_shader.setVec3("uAmbient", ctx.ambient);
    glBindVertexArray(m_vao);
    glDrawArrays(GL_POINTS, 0,
                 static_cast<GLsizei>(m_scratch.size() / SprayPool::kStride));
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_PROGRAM_POINT_SIZE);
}
