#include "RainRenderer.hpp"

#include <cstdio>
#include <random>
#include <vector>

#include <glad/gl.h>

namespace {
// The drop box. Big enough that its edges stay out of shot at a normal FOV, small
// enough that 14k streaks still read as dense rain rather than a sparse drizzle.
constexpr int   kDrops   = 14000;
constexpr float kBoxHalf = 55.0f; // half the box's width/depth (m)
constexpr float kBoxH    = 95.0f; // its height (m)
} // namespace

RainRenderer::~RainRenderer() {
    // Guarded so a default-constructed (never init'd) instance is harmless.
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
}

bool RainRenderer::init() {
    m_shader = fitzel::Shader::fromFiles("assets/shaders/rain.vert",
                                         "assets/shaders/rain.frag");
    if (!m_shader.isValid()) {
        std::fprintf(stderr, "Failed to load rain shader\n");
        return false;
    }

    // Two vertices per drop -- the streak's head and tail. Position is where the
    // drop starts, and its start height doubles as the fall phase, so the shader
    // can wrap each drop independently without any CPU work per frame.
    std::mt19937 rr(99u); // fixed seed: the same rain every run, so A/B shots match
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    std::vector<float> data;
    data.reserve(static_cast<std::size_t>(kDrops) * 2 * 5);
    for (int i = 0; i < kDrops; ++i) {
        const float bx = (u(rr) - 0.5f) * 2.0f * kBoxHalf;
        const float bz = (u(rr) - 0.5f) * 2.0f * kBoxHalf;
        const float ys = u(rr) * kBoxH;
        const float sp = glm::mix(30.0f, 55.0f, u(rr)); // fall speed (m/s)
        data.insert(data.end(), {bx, ys, bz, sp, 0.0f}); // tail
        data.insert(data.end(), {bx, ys, bz, sp, 1.0f}); // head
    }

    glGenVertexArrays(1, &m_vao);
    glBindVertexArray(m_vao);
    glGenBuffers(1, &m_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(data.size() * sizeof(float)),
                 data.data(), GL_STATIC_DRAW);
    const GLsizei stride = 5 * sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, (void*)(4 * sizeof(float)));
    glBindVertexArray(0);
    return true;
}

void RainRenderer::draw(const FrameContext& ctx) {
    const float intensity = rainIntensityFor(ctx.weather);
    if (!enabled || !m_vao || intensity <= 0.001f) return;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    m_shader.bind();
    m_shader.setMat4("uViewProj", ctx.viewProj);
    m_shader.setVec3("uBoxCenter", ctx.camPos); // the box follows the eye
    m_shader.setFloat("uBoxHeight", kBoxH);
    m_shader.setFloat("uBoxHalf", kBoxHalf);
    m_shader.setFloat("uStreak", glm::mix(1.2f, 3.0f, ctx.weather));
    m_shader.setFloat("uTime", static_cast<float>(ctx.time));
    m_shader.setVec3("uWind", glm::normalize(glm::vec3(0.6f, 0.0f, 0.3f)) *
                                  glm::mix(0.05f, 0.6f, ctx.weather));
    // Rain takes its colour from the sky it falls through, not from the sun, or a
    // storm's streaks glow against their own overcast.
    m_shader.setVec3("uRainColor",
                     glm::clamp(ctx.ambient * 2.5f + ctx.lightColor * 0.12f,
                                glm::vec3(0.0f), glm::vec3(2.0f)));
    m_shader.setFloat("uIntensity", intensity);
    glBindVertexArray(m_vao);
    glDrawArrays(GL_LINES, 0, kDrops * 2);
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}
