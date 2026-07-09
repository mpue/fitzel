#include "VegetationSystem.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

#include <glad/gl.h>
#include <imgui.h>

#include <fitzel/scene/Camera.hpp>
#include <fitzel/world/Terrain.hpp>

#include "SandboxMath.hpp"

using fitzel::InstancedMesh;
using fitzel::Shader;

VegetationSystem::VegetationSystem(fitzel::TerrainStreamer& streamer,
                                   fitzel::Camera& camera)
    : m_streamer(streamer), m_camera(camera) {}

bool VegetationSystem::init() {
    // --- Birds: a small flock of flapping billboards circling overhead ---
    m_bird = Shader::fromFiles("assets/shaders/bird.vert", "assets/shaders/bird.frag");
    if (!m_bird.isValid()) { std::fprintf(stderr, "Failed to load bird shader\n"); return false; }
    // Gull silhouette: small body + swept, bent wings. pos3, flap (flap rises
    // toward the tips so the wings flex when they beat). +Z forward.
    const float bm[] = {
            // body (diamond: nose, shoulders, tail)
             0.00f, 0.0f,  0.45f, 0.0f,  -0.12f, 0.0f, 0.05f, 0.0f,   0.12f, 0.0f, 0.05f, 0.0f,
            -0.12f, 0.0f,  0.05f, 0.0f,   0.00f, 0.0f,-0.55f, 0.0f,   0.12f, 0.0f, 0.05f, 0.0f,
            // left wing (inner + outer panel, trailing to the tail)
            -0.12f, 0.0f,  0.05f, 0.0f,  -0.55f, 0.05f,-0.05f, 0.4f,  0.00f, 0.0f,-0.55f, 0.0f,
            -0.55f, 0.05f,-0.05f, 0.4f,  -1.05f, 0.0f,-0.35f, 1.0f,   0.00f, 0.0f,-0.55f, 0.0f,
            // right wing
             0.12f, 0.0f,  0.05f, 0.0f,   0.00f, 0.0f,-0.55f, 0.0f,   0.55f, 0.05f,-0.05f, 0.4f,
             0.55f, 0.05f,-0.05f, 0.4f,   0.00f, 0.0f,-0.55f, 0.0f,   1.05f, 0.0f,-0.35f, 1.0f };
    // base: pos3 + flap ; instance: iPos3, iYaw, iPhase.
    m_birdField = InstancedMesh::create(
        bm, sizeof(bm) / sizeof(float), 4 * sizeof(float),
        {{0, 3, 0}, {1, 1, 3 * sizeof(float)}},
        5 * sizeof(float),
        {{2, 3, 0}, {3, 1, 3 * sizeof(float)}, {4, 1, 4 * sizeof(float)}});

    // --- Fireflies: additive glowing points that wander the grass at night --
    m_firefly = Shader::fromFiles("assets/shaders/firefly.vert",
                                  "assets/shaders/firefly.frag");
    if (!m_firefly.isValid()) { std::fprintf(stderr, "Failed to load firefly shader\n"); return false; }
    // Instance-only (quad corner comes from gl_VertexID): iPos3, iPhase.
    m_fireflyField = InstancedMesh::create(
        nullptr, 0, 0, {}, 4 * sizeof(float),
        {{0, 3, 0}, {1, 1, 3 * sizeof(float)}});
    // Home xz + blink phase per firefly; homes start "far" so they seed near the
    // camera on the first night frame.
    m_fireflies.assign(256, glm::vec3(1e9f, 1e9f, 0.0f));
    for (auto& f : m_fireflies) f.z = m_flyU(m_flyRng) * 6.2831f;
    return true;
}

void VegetationSystem::drawBirds(const glm::mat4& viewProj, double time,
                                 const glm::vec3& camPos) {
    if (!birdsEnabled || m_birdCount <= 0) return;
    const float cx = camPos.x, cz = camPos.z;
    const float baseY = m_streamer.heightAt(cx, cz) + 95.0f; // fly higher
    std::vector<float> bi;
    bi.reserve(m_birdCount * 5);
    for (int i = 0; i < m_birdCount; ++i) {
        const float ph = static_cast<float>(i) * 2.39996f;
        const float R  = 50.0f + 45.0f * vhash2(static_cast<float>(i), 3.0f);
        const float sp = 0.12f + 0.10f * vhash2(static_cast<float>(i), 9.0f);
        const float hY = baseY + 30.0f * vhash2(static_cast<float>(i), 5.0f);
        const float ang = static_cast<float>(time) * sp + ph;
        const float bx = cx + std::cos(ang) * R;
        const float bz = cz + std::sin(ang) * R;
        const float by = hY + 3.0f * std::sin(ang * 0.7f + ph);
        bi.insert(bi.end(), {bx, by, bz, ang, ph});
    }
    glDisable(GL_CULL_FACE);
    m_birdField.upload(bi);
    m_bird.bind();
    m_bird.setMat4("uViewProj", viewProj);
    m_bird.setFloat("uTime", static_cast<float>(time));
    m_bird.setFloat("uSize", m_birdSize);
    m_bird.setVec3("uColor", glm::vec3(0.02f, 0.02f, 0.03f));
    m_birdField.draw(GL_TRIANGLES, 18);
    glEnable(GL_CULL_FACE);
}

void VegetationSystem::drawFireflies(const glm::mat4& viewProj, double time,
                                     float night, const glm::vec3& camPos) {
    if (!fireflyEnabled || m_fireflyCount <= 0 || night <= 0.03f) return;
    const glm::vec2 camXZ(camPos.x, camPos.z);
    std::vector<float> fi;
    fi.reserve(m_fireflyCount * 4);
    for (int i = 0; i < m_fireflyCount; ++i) {
        glm::vec3& f = m_fireflies[i];
        glm::vec2 home(f.x, f.y);
        if (glm::length(home - camXZ) > m_fireflyRadius) {
            const float ang = m_flyU(m_flyRng) * 6.2831f;
            const float rad = std::sqrt(m_flyU(m_flyRng)) * m_fireflyRadius;
            home = camXZ + rad * glm::vec2(std::cos(ang), std::sin(ang));
            f.x = home.x; f.y = home.y;
        }
        const float ph = f.z;
        const float t  = static_cast<float>(time);
        const float wx = home.x + std::sin(t * 0.7f + ph) * 1.3f;
        const float wz = home.y + std::cos(t * 0.9f + ph * 1.7f) * 1.3f;
        const float hover = 0.5f + 0.5f * std::sin(t * 1.1f + ph * 2.3f);
        const float wy = m_streamer.heightAt(wx, wz) + 0.4f + hover * 0.9f;
        fi.insert(fi.end(), {wx, wy, wz, ph});
    }
    const glm::vec3 camRight = m_camera.right();
    const glm::vec3 camUp = glm::normalize(glm::cross(camRight, m_camera.front()));
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE); // additive glow
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    m_fireflyField.upload(fi);
    m_firefly.bind();
    m_firefly.setMat4("uViewProj", viewProj);
    m_firefly.setVec3("uCamRight", camRight);
    m_firefly.setVec3("uCamUp", camUp);
    m_firefly.setFloat("uSize", m_fireflySize);
    m_firefly.setFloat("uTime", static_cast<float>(time));
    m_firefly.setFloat("uNight", night);
    m_firefly.setVec3("uColor", glm::vec3(0.7f, 1.0f, 0.35f));
    m_fireflyField.draw(GL_TRIANGLE_STRIP, 4);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
}

void VegetationSystem::panelBirdsFireflies() {
    ImGui::SeparatorText("Birds");
    ImGui::Checkbox("Birds", &birdsEnabled);
    ImGui::SliderInt("Flock size", &m_birdCount, 0, 60);
    ImGui::SliderFloat("Bird size", &m_birdSize, 0.8f, 5.0f);

    ImGui::SeparatorText("Fireflies (night)");
    ImGui::Checkbox("Fireflies", &fireflyEnabled);
    ImGui::SliderInt("Count", &m_fireflyCount, 0, 256);
    ImGui::SliderFloat("Firefly size", &m_fireflySize, 0.03f, 0.25f, "%.2f");
}
