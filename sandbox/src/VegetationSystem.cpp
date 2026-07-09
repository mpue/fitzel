#include "VegetationSystem.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include <glad/gl.h>
#include <imgui.h>

#include <fitzel/scene/Camera.hpp>
#include <fitzel/world/Model.hpp>
#include <fitzel/world/Terrain.hpp>

#include "SandboxMath.hpp"

using fitzel::InstancedMesh;
using fitzel::Shader;
using fitzel::Texture;
using fitzel::TerrainSettings;
using fitzel::terrainHeight;
using fitzel::terrainMoisture;

// Instance layout shared by the procedural field and the painted layer: a 7-vertex
// blade strip (aBlade x,h01) + per-instance iPos3, iRot, iHeight, iPhase, iLush.
static const float kBlade[] = {
    -0.5f, 0.0f,  0.5f, 0.0f,  -0.45f, 0.33f,  0.45f, 0.33f,
    -0.30f, 0.66f, 0.30f, 0.66f,  0.0f, 1.0f };
static InstancedMesh makeBladeField() {
    return InstancedMesh::create(
        kBlade, sizeof(kBlade) / sizeof(float), 2 * sizeof(float), {{0, 2, 0}},
        7 * sizeof(float),
        {{1, 3, 0}, {2, 1, 3 * sizeof(float)}, {3, 1, 4 * sizeof(float)},
         {4, 1, 5 * sizeof(float)}, {5, 1, 6 * sizeof(float)}});
}

VegetationSystem::VegetationSystem(fitzel::TerrainStreamer& streamer,
                                   fitzel::Camera& camera)
    : m_streamer(streamer), m_camera(camera) {}

bool VegetationSystem::init() {
    // --- Grass: GPU-instanced blades placed on suitable terrain ---
    m_grass = Shader::fromFiles("assets/shaders/grass.vert", "assets/shaders/grass.frag");
    if (!m_grass.isValid()) { std::fprintf(stderr, "Failed to load grass shader\n"); return false; }
    m_grassField   = makeBladeField();
    m_paintedGrass = makeBladeField();

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

// --- Grass ------------------------------------------------------------------

std::vector<float> VegetationSystem::computeGrass(
    TerrainSettings s, glm::vec2 c, float waterLvl, float snowLvl, float gHeight,
    float gDensity, float R, std::uint32_t seed, std::vector<glm::vec2> road,
    float roadClear) {
    std::vector<float> out;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    const float spacing = 0.6f; // sampling grid (one ground query per cell)
    const int   per = std::max(1, static_cast<int>(120.0f * gDensity));
    for (float z = -R; z <= R; z += spacing) {
        for (float x = -R; x <= R; x += spacing) {
            if (x * x + z * z > R * R) continue;
            const float wx = c.x + x, wz = c.y + z;
            if (roadDistanceSq(road, wx, wz) < roadClear * roadClear) continue;
            const float h = terrainHeight(s, wx, wz);
            if (h < waterLvl + 0.5f || h > snowLvl - 1.5f) continue;
            const float e = 1.0f;
            const glm::vec3 n = glm::normalize(glm::vec3(
                terrainHeight(s, wx - e, wz) - terrainHeight(s, wx + e, wz),
                2.0f * e,
                terrainHeight(s, wx, wz - e) - terrainHeight(s, wx, wz + e)));
            if (n.y < 0.82f) continue;
            const float lush = glm::clamp(
                terrainMoisture(s, wx, wz)
                    - glm::smoothstep(snowLvl - 8.0f, snowLvl, h) * 0.5f,
                0.0f, 1.0f);
            if (lush < 0.22f) continue;
            // Meadow patchiness: clumps of dense/thin grass and bare gaps.
            const float patch = valNoise2(wx * 0.05f, wz * 0.05f);
            const float bare  = valNoise2(wx * 0.13f + 19.0f, wz * 0.13f + 7.0f);
            if (bare < 0.26f) continue; // bare ground -> no grass in this cell
            const float dens  = glm::mix(0.25f, 1.25f, patch);
            const float dist  = std::sqrt(x * x + z * z);
            const float rim   = 1.0f - glm::smoothstep(R * 0.82f, R, dist);
            // Distance LOD: thin out far cells hard -- big win.
            const float lod   = glm::mix(1.0f, 0.22f,
                                         glm::smoothstep(0.28f, 1.0f, dist / R));
            const int   count = static_cast<int>(per * dens
                                * glm::mix(0.35f, 1.0f, lush) * rim * lod);
            for (int b = 0; b < count; ++b) {
                const float bh = gHeight * glm::mix(0.65f, 1.25f, patch)
                                         * glm::mix(0.8f, 1.1f, u(rng));
                out.insert(out.end(), {
                    wx + (u(rng) - 0.5f) * spacing * 2.2f, h,
                    wz + (u(rng) - 0.5f) * spacing * 2.2f,
                    u(rng) * 6.2831f,
                    bh,
                    u(rng) * 6.2831f,
                    glm::clamp(lush + (patch - 0.5f) * 0.4f
                                    + (u(rng) - 0.5f) * 0.12f, 0.0f, 1.0f)});
            }
        }
    }
    return out;
}

void VegetationSystem::stampGrass(glm::vec2 c, float radius, std::mt19937& rng,
                                  float brushDensity, float waterLevel, float snowLevel) {
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    const float area  = 3.14159265f * radius * radius;
    const int   tries = std::max(4, static_cast<int>(
                            area * 2.2f * brushDensity * grassDensity));
    const TerrainSettings& s = m_streamer.settings();
    for (int i = 0; i < tries; ++i) {
        const float ang = u(rng) * 6.2831853f;
        const float rad = std::sqrt(u(rng)) * radius; // uniform in disc
        const float wx  = c.x + std::cos(ang) * rad;
        const float wz  = c.y + std::sin(ang) * rad;
        const float h   = m_streamer.heightAt(wx, wz);
        if (h < waterLevel + 0.5f || h > snowLevel - 1.5f) continue;
        const float e = 1.0f;
        const glm::vec3 n = glm::normalize(glm::vec3(
            m_streamer.heightAt(wx - e, wz) - m_streamer.heightAt(wx + e, wz),
            2.0f * e,
            m_streamer.heightAt(wx, wz - e) - m_streamer.heightAt(wx, wz + e)));
        if (n.y < 0.80f) continue; // too steep
        const float lush = glm::clamp(terrainMoisture(s, wx, wz), 0.0f, 1.0f);
        // Store a relative height (per-blade jitter only); the global "Blade
        // height" slider is applied live at draw via uHeightScale.
        const float bh   = glm::mix(0.8f, 1.15f, u(rng));
        paintedBlades.insert(paintedBlades.end(), {
            wx, h, wz,
            u(rng) * 6.2831853f,
            bh,
            u(rng) * 6.2831853f,
            glm::clamp(lush + (u(rng) - 0.5f) * 0.2f, 0.0f, 1.0f)});
    }
    paintedDirty = true;
}

void VegetationSystem::eraseGrass(glm::vec2 c, float radius) {
    const float r2 = radius * radius;
    const int   stride = 7;
    std::vector<float> kept;
    kept.reserve(paintedBlades.size());
    for (std::size_t i = 0; i + stride <= paintedBlades.size(); i += stride) {
        const float dx = paintedBlades[i]     - c.x;
        const float dz = paintedBlades[i + 2] - c.y;
        if (dx * dx + dz * dz <= r2) continue; // inside brush -> remove
        kept.insert(kept.end(), paintedBlades.begin() + i,
                                paintedBlades.begin() + i + stride);
    }
    if (kept.size() != paintedBlades.size()) {
        paintedBlades.swap(kept);
        paintedDirty = true;
    }
}

bool VegetationSystem::updateGrass(glm::vec2 camXZ, const std::vector<glm::vec2>& road,
                                   float roadClear, float waterLevel, float snowLevel) {
    bool regenerated = false;
    if (grassEnabled && !m_grassPending &&
        (grassDirty || glm::length(camXZ - m_grassCenter) > 10.0f)) {
        m_grassPending = true;
        grassDirty     = false;
        m_grassPendingCenter = camXZ;
        m_grassFuture = std::async(std::launch::async, &VegetationSystem::computeGrass,
                                   m_streamer.settings(), camXZ, waterLevel, snowLevel,
                                   grassHeight, grassDensity, grassRadius, m_grassSeed++,
                                   road, roadClear);
    }
    if (m_grassPending && m_grassFuture.valid() &&
        m_grassFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        m_grassField.upload(m_grassFuture.get());
        grassCount     = m_grassField.count();
        m_grassCenter  = m_grassPendingCenter;
        m_grassPending = false;
        regenerated    = true;
    }
    if (paintedDirty) { // painted blades changed -> push to the GPU
        m_paintedGrass.upload(paintedBlades);
        paintedDirty = false;
    }
    return regenerated;
}

void VegetationSystem::drawGrass(const VegDrawContext& c) {
    const bool drawProc    = grassEnabled && grassCount > 0;
    const bool drawPainted = m_paintedGrass.count() > 0;
    if (!drawProc && !drawPainted) return;
    glDisable(GL_CULL_FACE);
    m_grass.bind();
    m_grass.setMat4("uViewProj", c.viewProj);
    m_grass.setFloat("uTime", static_cast<float>(c.time));
    m_grass.setVec2("uWindDir", glm::normalize(glm::vec2(0.6f, 0.3f)));
    m_grass.setFloat("uWindStrength", glm::mix(0.08f, 0.55f, c.weather));
    m_grass.setVec3("uTint", grassTint);
    m_grass.setVec3("uViewPos", c.camPos);
    m_grass.setVec3("uLightDir", c.lightDir);
    m_grass.setVec3("uLightColor", c.lightColor);
    m_grass.setVec3("uAmbient", c.ambient);
    m_grass.setVec3("uFogColor", c.fogColor);
    m_grass.setVec3("uFogSunColor", c.fogSunColor);
    m_grass.setFloat("uFogDensity", c.fogDensity);
    m_grass.setFloat("uFogHeightFalloff", c.fogHeightFalloff);
    m_grass.setFloat("uFogHeight", c.fogHeight);
    // Procedural blades bake absolute height (scale 1); painted blades store a
    // relative height and take the live "Blade height" slider.
    if (drawProc) {
        m_grass.setFloat("uHeightScale", 1.0f);
        m_grassField.draw(GL_TRIANGLE_STRIP, 7);
    }
    if (drawPainted) {
        m_grass.setFloat("uHeightScale", grassHeight);
        m_paintedGrass.draw(GL_TRIANGLE_STRIP, 7);
    }
    glEnable(GL_CULL_FACE);
}

// --- Trees ------------------------------------------------------------------

VegetationSystem::~VegetationSystem() {
    if (m_treeVBO)     glDeleteBuffers(1, &m_treeVBO);
    if (m_treeInstVBO) glDeleteBuffers(1, &m_treeInstVBO);
    if (m_treeVAO)     glDeleteVertexArrays(1, &m_treeVAO);
    if (m_bbVAO)       glDeleteVertexArrays(1, &m_bbVAO);
}

bool VegetationSystem::initTrees(const std::string& modelDir, const std::string& texDir) {
    // Instanced low-poly tree model (trunk + foliage), split into per-material
    // draw groups; geometry normalized to unit height (instance scale = size).
    m_tree      = Shader::fromFiles("assets/shaders/tree.vert", "assets/shaders/tree.frag");
    m_treeDepth = Shader::fromFiles("assets/shaders/treedepth.vert",
                                    "assets/shaders/treedepth.frag");
    if (!m_tree.isValid() || !m_treeDepth.isValid()) {
        std::fprintf(stderr, "Failed to load tree shaders\n"); return false;
    }
    std::vector<float> treeVerts; // combined: pos3 normal3 uv2
    {
        fitzel::ModelData md = fitzel::loadGltf(modelDir + "/tree1.glb");
        if (md.empty() || md.height() < 0.01f) {
            std::fprintf(stderr, "Tree model failed to load\n");
        } else {
            const float scale = 1.0f / md.height();
            for (fitzel::ModelPrimitive& p : md.primitives) {
                TreePrim tp;
                tp.first  = static_cast<int>(treeVerts.size() / 8);
                tp.count  = p.vertexCount();
                tp.cutout = p.alphaCutout;
                tp.hasTex = !p.texPixels.empty();
                if (tp.hasTex)
                    tp.tex = Texture::fromPixels(p.texPixels.data(), p.texWidth, p.texHeight, 4);
                for (std::size_t i = 0; i + 7 < p.vertices.size(); i += 8) {
                    treeVerts.push_back(p.vertices[i + 0] * scale);
                    treeVerts.push_back((p.vertices[i + 1] - md.minY) * scale);
                    treeVerts.push_back(p.vertices[i + 2] * scale);
                    treeVerts.push_back(p.vertices[i + 3]);
                    treeVerts.push_back(p.vertices[i + 4]);
                    treeVerts.push_back(p.vertices[i + 5]);
                    treeVerts.push_back(p.vertices[i + 6]);
                    treeVerts.push_back(p.vertices[i + 7]);
                }
                m_treePrims.push_back(std::move(tp));
            }
            std::printf("[Fitzel] tree model: %d primitives, %d verts\n",
                        static_cast<int>(m_treePrims.size()),
                        static_cast<int>(treeVerts.size() / 8));
        }
    }
    glGenVertexArrays(1, &m_treeVAO);
    glBindVertexArray(m_treeVAO);
    glGenBuffers(1, &m_treeVBO);
    glBindBuffer(GL_ARRAY_BUFFER, m_treeVBO);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(treeVerts.size() * sizeof(float)),
                 treeVerts.data(), GL_STATIC_DRAW);
    const GLsizei ms = 8 * sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, ms, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, ms, (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, ms, (void*)(6 * sizeof(float)));
    glGenBuffers(1, &m_treeInstVBO);
    glBindBuffer(GL_ARRAY_BUFFER, m_treeInstVBO);
    const GLsizei is = 5 * sizeof(float);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, is, (void*)0);
    glVertexAttribDivisor(3, 1);
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, is, (void*)(3 * sizeof(float)));
    glVertexAttribDivisor(4, 1);
    glEnableVertexAttribArray(5);
    glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, is, (void*)(4 * sizeof(float)));
    glVertexAttribDivisor(5, 1);
    glBindVertexArray(0);

    // Billboard LOD for distant trees (camera-facing quad reusing the instance
    // buffer; corner from gl_VertexID).
    m_billboardTex = Texture::fromFile(texDir + "/billboard_tree_bled.png");
    m_billboard = Shader::fromFiles("assets/shaders/billboard.vert",
                                    "assets/shaders/billboard.frag");
    if (!m_billboard.isValid()) { std::fprintf(stderr, "Failed to load billboard shader\n"); return false; }
    if (!m_billboardTex.isValid()) std::fprintf(stderr, "Warning: tree billboard texture missing\n");
    m_bbAspect = (m_billboardTex.height() > 0)
        ? static_cast<float>(m_billboardTex.width()) / m_billboardTex.height() : 0.93f;
    glGenVertexArrays(1, &m_bbVAO);
    glBindVertexArray(m_bbVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_treeInstVBO);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, is, (void*)0);
    glVertexAttribDivisor(3, 1);
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, is, (void*)(3 * sizeof(float)));
    glVertexAttribDivisor(4, 1);
    glEnableVertexAttribArray(5);
    glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, is, (void*)(4 * sizeof(float)));
    glVertexAttribDivisor(5, 1);
    glBindVertexArray(0);
    treePaintScale = treeSize; // default painted-tree height matches the tree size
    return true;
}

void VegetationSystem::rebuildTreeBuffer() {
    m_treeInst.resize(m_proceduralFloats);
    m_treeInst.insert(m_treeInst.end(), paintedTrees.begin(), paintedTrees.end());
    treeCount = static_cast<int>(m_treeInst.size() / 5);
    glBindBuffer(GL_ARRAY_BUFFER, m_treeInstVBO);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(m_treeInst.size() * sizeof(float)),
                 m_treeInst.data(), GL_DYNAMIC_DRAW);
}

void VegetationSystem::regenTrees(glm::vec2 cc, const std::vector<glm::vec2>& road,
                                  float roadWidth, float waterLevel, float snowLevel) {
    m_treeInst.clear();
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    const float spacing = 7.0f;
    for (float z = -m_treeRadius; z <= m_treeRadius; z += spacing) {
        for (float x = -m_treeRadius; x <= m_treeRadius; x += spacing) {
            if (x * x + z * z > m_treeRadius * m_treeRadius) continue;
            const float wx = cc.x + x, wz = cc.y + z;
            const float roadClear = roadWidth * 0.5f + 3.0f; // keep trees clear
            if (roadDistanceSq(road, wx, wz) < roadClear * roadClear) continue;
            const float h = m_streamer.heightAt(wx, wz);
            if (h < waterLevel + 0.8f || h > snowLevel - 2.0f) continue;
            const float e = 1.5f;
            const glm::vec3 n = glm::normalize(glm::vec3(
                m_streamer.heightAt(wx - e, wz) - m_streamer.heightAt(wx + e, wz),
                2.0f * e,
                m_streamer.heightAt(wx, wz - e) - m_streamer.heightAt(wx, wz + e)));
            if (n.y < 0.86f) continue;
            // Forests cluster in moist regions; dry biomes stay open.
            const float moist  = terrainMoisture(m_streamer.settings(), wx, wz);
            const float forest = 0.5f + 0.35f * std::sin(wx * 0.03f)
                                      + 0.35f * std::cos(wz * 0.026f + 1.3f);
            const float prob = glm::clamp(forest, 0.0f, 1.0f)
                             * glm::smoothstep(0.35f, 0.75f, moist)
                             * 0.6f * treeDensity;
            if (u(m_trng) > prob) continue;
            const float tx = wx + (u(m_trng) - 0.5f) * spacing;
            const float tz = wz + (u(m_trng) - 0.5f) * spacing;
            m_treeInst.insert(m_treeInst.end(), {
                tx, m_streamer.heightAt(tx, tz) - 0.3f, tz,
                u(m_trng) * 6.2831f,
                glm::mix(treeSize * 0.75f, treeSize * 1.3f, u(m_trng))});
        }
    }
    m_proceduralFloats = m_treeInst.size();
    rebuildTreeBuffer();  // append the painted trees and upload
    treeCenter = cc;
}

void VegetationSystem::updateTrees(glm::vec2 camXZ, const std::vector<glm::vec2>& road,
                                   float roadWidth, float waterLevel, float snowLevel) {
    if (treeEnabled && glm::length(camXZ - treeCenter) > 25.0f)
        regenTrees(camXZ, road, roadWidth, waterLevel, snowLevel);
}

void VegetationSystem::stampTree(glm::vec2 c, float radius, std::mt19937& rng,
                                 float waterLevel, float snowLevel) {
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    const float area  = 3.14159265f * radius * radius;
    const int   tries = std::max(1, static_cast<int>(area * 0.02f * treeBrushDensity));
    const float minSp2 = treeMinSpacing * treeMinSpacing;
    for (int i = 0; i < tries; ++i) {
        const float ang = u(rng) * 6.2831853f;
        const float rad = std::sqrt(u(rng)) * radius; // uniform in disc
        const float wx  = c.x + std::cos(ang) * rad;
        const float wz  = c.y + std::sin(ang) * rad;
        const float h   = m_streamer.heightAt(wx, wz);
        if (h < waterLevel + 0.8f || h > snowLevel - 2.0f) continue;
        const float e = 1.5f;
        const glm::vec3 n = glm::normalize(glm::vec3(
            m_streamer.heightAt(wx - e, wz) - m_streamer.heightAt(wx + e, wz),
            2.0f * e,
            m_streamer.heightAt(wx, wz - e) - m_streamer.heightAt(wx, wz + e)));
        if (n.y < 0.86f) continue; // too steep for a trunk
        bool tooClose = false;
        for (std::size_t t = 0; t + 5 <= paintedTrees.size(); t += 5) {
            const float dx = wx - paintedTrees[t], dz = wz - paintedTrees[t + 2];
            if (dx * dx + dz * dz < minSp2) { tooClose = true; break; }
        }
        if (tooClose) continue;
        const float sc = glm::mix(treePaintScale * 0.8f, treePaintScale * 1.25f, u(rng));
        paintedTrees.insert(paintedTrees.end(), {
            wx, m_streamer.heightAt(wx, wz) - 0.3f, wz, u(rng) * 6.2831853f, sc});
    }
    rebuildTreeBuffer();
}

void VegetationSystem::eraseTree(glm::vec2 c, float radius) {
    const float r2 = radius * radius;
    std::vector<float> kept;
    kept.reserve(paintedTrees.size());
    for (std::size_t i = 0; i + 5 <= paintedTrees.size(); i += 5) {
        const float dx = paintedTrees[i] - c.x, dz = paintedTrees[i + 2] - c.y;
        if (dx * dx + dz * dz <= r2) continue; // inside brush -> remove
        kept.insert(kept.end(), paintedTrees.begin() + i, paintedTrees.begin() + i + 5);
    }
    if (kept.size() != paintedTrees.size()) {
        paintedTrees.swap(kept);
        rebuildTreeBuffer();
    }
}

void VegetationSystem::drawTreeShadow(const glm::mat4& lightSpace, double time,
                                      float weather) {
    if (!treeEnabled || treeCount == 0 || m_treePrims.empty()) return;
    glDisable(GL_CULL_FACE);
    m_treeDepth.bind();
    m_treeDepth.setMat4("uLightSpace", lightSpace);
    m_treeDepth.setFloat("uTime", static_cast<float>(time));
    m_treeDepth.setVec2("uWindDir", glm::normalize(glm::vec2(0.6f, 0.3f)));
    m_treeDepth.setFloat("uWindStrength", glm::mix(0.05f, 0.4f, weather));
    m_treeDepth.setFloat("uTreeHeight", m_treeLocalHeight);
    m_treeDepth.setInt("uTex", 0);
    glBindVertexArray(m_treeVAO);
    for (const TreePrim& tp : m_treePrims) {
        if (tp.hasTex) tp.tex.bind(0);
        m_treeDepth.setInt("uAlphaCutout", tp.cutout ? 1 : 0);
        glDrawArraysInstanced(GL_TRIANGLES, tp.first, tp.count, treeCount);
    }
    glBindVertexArray(0);
    glEnable(GL_CULL_FACE);
}

void VegetationSystem::drawTrees(const VegDrawContext& c) {
    if (!treeEnabled || treeCount == 0 || m_treePrims.empty()) return;
    glDisable(GL_CULL_FACE);
    m_tree.bind();
    m_tree.setMat4("uViewProj", c.viewProj);
    m_tree.setFloat("uTime", static_cast<float>(c.time));
    m_tree.setVec2("uWindDir", glm::normalize(glm::vec2(0.6f, 0.3f)));
    m_tree.setFloat("uWindStrength", glm::mix(0.05f, 0.4f, c.weather));
    m_tree.setFloat("uTreeHeight", m_treeLocalHeight);
    m_tree.setVec3("uCamPos", c.camPos);
    m_tree.setFloat("uLodNear", lodNear);
    m_tree.setVec3("uViewPos", c.camPos);
    m_tree.setVec3("uLightDir", c.lightDir);
    m_tree.setVec3("uLightColor", c.lightColor);
    m_tree.setVec3("uAmbient", c.ambient);
    m_tree.setVec3("uFogColor", c.fogColor);
    m_tree.setVec3("uFogSunColor", c.fogSunColor);
    m_tree.setFloat("uFogDensity", c.fogDensity);
    m_tree.setFloat("uFogHeightFalloff", c.fogHeightFalloff);
    m_tree.setFloat("uFogHeight", c.fogHeight);
    m_tree.setInt("uTex", 0);
    glBindVertexArray(m_treeVAO);
    for (const TreePrim& tp : m_treePrims) {
        if (tp.hasTex) tp.tex.bind(0);
        m_tree.setInt("uAlphaCutout", tp.cutout ? 1 : 0);
        glDrawArraysInstanced(GL_TRIANGLES, tp.first, tp.count, treeCount);
    }
    glBindVertexArray(0);
    glEnable(GL_CULL_FACE);
}

void VegetationSystem::drawTreeBillboards(const VegDrawContext& c,
                                          const glm::vec3& camRight) {
    if (!treeEnabled || treeCount == 0 || !m_billboardTex.isValid()) return;
    glDisable(GL_CULL_FACE);
    m_billboard.bind();
    m_billboard.setMat4("uViewProj", c.viewProj);
    m_billboard.setVec3("uCamRight", camRight);
    m_billboard.setVec3("uCamPos", c.camPos);
    m_billboard.setFloat("uLodNear", lodNear);
    m_billboard.setFloat("uTreeHeight", m_treeLocalHeight);
    m_billboard.setFloat("uAspect", m_bbAspect);
    m_billboard.setVec3("uViewPos", c.camPos);
    m_billboard.setVec3("uLightDir", c.lightDir);
    m_billboard.setVec3("uLightColor", c.lightColor);
    m_billboard.setVec3("uAmbient", c.ambient);
    m_billboard.setVec3("uFogColor", c.fogColor);
    m_billboard.setVec3("uFogSunColor", c.fogSunColor);
    m_billboard.setFloat("uFogDensity", c.fogDensity);
    m_billboard.setFloat("uFogHeightFalloff", c.fogHeightFalloff);
    m_billboard.setFloat("uFogHeight", c.fogHeight);
    m_billboard.setInt("uTex", 0);
    m_billboardTex.bind(0);
    glBindVertexArray(m_bbVAO);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, treeCount);
    glBindVertexArray(0);
    glEnable(GL_CULL_FACE);
}
