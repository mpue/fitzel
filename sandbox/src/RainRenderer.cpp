#include "RainRenderer.hpp"

#include <cstdio>
#include <random>
#include <vector>

#include <glad/gl.h>

namespace {
// The drop box. Big enough that its edges stay out of shot at a normal FOV, small
// enough that 14k streaks still read as dense rain rather than a sparse drizzle.
constexpr int   kDrops   = 14000; // drops at amount 1 -- the old fixed density
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
    // Built for the heaviest rain the dial can ask for. A lighter one draws a
    // PREFIX of this, and a prefix is a uniform random subset of the box because
    // every drop below was placed independently -- so turning the rain down
    // thins it evenly instead of emptying one corner. 28k drops is 1.1 MB of
    // static buffer, which is nothing, and the draw call is the same either way.
    const int maxDrops = static_cast<int>(kDrops * kMaxAmount);
    data.reserve(static_cast<std::size_t>(maxDrops) * 2 * 5);
    for (int i = 0; i < maxDrops; ++i) {
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
    // What the dial says times what the weather says: the dial decides whether it
    // rains at all, the amount how heavy the fall is.
    const float fall = rainIntensityFor(ctx.weather) *
                       glm::clamp(amount, 0.0f, kMaxAmount);
    if (!enabled || !m_vao || fall <= 0.001f) return;
    const int drops = glm::clamp(static_cast<int>(kDrops * fall), 1,
                                 static_cast<int>(kDrops * kMaxAmount));
    // Only the very bottom of the range is faded, and not to nothing. A raindrop
    // does not become transparent because there are fewer of them -- the count
    // above is what says "less rain" -- but the first few streaks appearing at
    // full strength as the dial crosses the threshold is a pop, and this is the
    // cheapest thing that isn't one.
    const float intensity = glm::min(fall, 1.0f);

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
    m_shader.setFloat("uIntensity", glm::mix(0.55f, 1.0f, intensity));
    glBindVertexArray(m_vao);
    glDrawArrays(GL_LINES, 0, drops * 2);
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}
