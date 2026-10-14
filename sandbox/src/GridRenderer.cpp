#include "GridRenderer.hpp"

#include <algorithm>
#include <cstdio>

#include <glad/gl.h>

GridRenderer::~GridRenderer() {
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
}

bool GridRenderer::init() {
    m_shader = fitzel::Shader::fromFiles("assets/shaders/grid.vert",
                                         "assets/shaders/grid.frag");
    if (!m_shader.isValid()) {
        std::fprintf(stderr, "Failed to load grid shader\n");
        return false;
    }
    // No vertex data at all -- the quad is four gl_VertexID corners. A VAO is
    // still required to draw in a core profile, so keep an empty one.
    glGenVertexArrays(1, &m_vao);
    return true;
}

void GridRenderer::draw(const FrameContext& ctx) {
    if (!enabled || !m_vao || cell <= 1.0e-4f || fade <= 1.0f) return;

    // How far the quad reaches. The fade distance is measured horizontally from
    // the eye, so that is all the plane the shader can put anything on -- with a
    // margin, since the corners of a square lie further out than its edges.
    const float extent = fade * 1.5f;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    // No depth buffer here at all: the target is the finished image, whose depth
    // (if it has any) has nothing to do with the scene. Occlusion is the
    // shader's job, against the scene depth texture the caller bound.
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE); // the plane is worth seeing from underneath too

    m_shader.bind();
    m_shader.setMat4("uViewProj", ctx.viewProj);
    m_shader.setVec3("uCamPos", ctx.camPos);
    // Lifted off the plane towards the eye: from below, or from the far side
    // of an upright grid, "up" is the wrong way.
    const int   ax   = std::clamp(axis, 0, 2);
    const float side = ctx.camPos[ax] >= plane ? 1.0f : -1.0f;
    // Each in-plane axis's own colour for the line through the origin along
    // it: X red, Y green, Z blue -- the line along the first in-plane axis is
    // where the second is zero, and the other way round.
    static const glm::vec3 kAxisCol[3] = {{0.80f, 0.24f, 0.24f},
                                          {0.36f, 0.70f, 0.30f},
                                          {0.28f, 0.46f, 0.92f}};
    const int a = ax == 0 ? 2 : 0;          // (z, y) / (x, z) / (x, y)
    const int b = ax == 1 ? 2 : 1;
    m_shader.setInt("uAxis", ax);
    m_shader.setFloat("uPlane", plane + lift * side);
    m_shader.setVec3("uAlongA", kAxisCol[a]);
    m_shader.setVec3("uAlongB", kAxisCol[b]);
    m_shader.setFloat("uOrtho", ortho ? 1.0f : 0.0f);
    m_shader.setVec3("uViewDir", viewDir);
    m_shader.setFloat("uExtent", extent);
    m_shader.setFloat("uCell", cell);
    m_shader.setFloat("uFade", fade);
    m_shader.setVec3("uColor", color);
    m_shader.setFloat("uOpacity", std::clamp(opacity, 0.0f, 1.0f));
    m_shader.setVec3("uCursor", cursor);
    m_shader.setFloat("uCursorCell", highlightCursorCell ? 1.0f : 0.0f);
    m_shader.setFloat("uAxes", axes ? 1.0f : 0.0f);
    m_shader.setInt("uSceneDepth", sceneDepthUnit);
    m_shader.setVec2("uViewport", glm::max(viewportPx, glm::vec2(1.0f)));

    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    glEnable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
}
