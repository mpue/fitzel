#include "VegetationSystem.hpp"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <random>
#include <vector>

#include <glad/gl.h>
#include <imgui.h>
#include <nlohmann/json.hpp>

#include <fitzel/scene/Camera.hpp>
#include <fitzel/world/Model.hpp>
#include <fitzel/world/Terrain.hpp>

#include "Primitives.hpp"
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
    float roadClear, float chaos) {
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
            // Meadow patchiness at several scales. `chaos` scales how much each
            // irregularity kicks in: at 0 the field is a near-uniform lawn (even
            // height, smooth density); at 1 it's a wild meadow; higher piles on
            // taller outliers and more gaps. Each varying factor is lerped from
            // 1.0 (neutral) toward its noisy value by `chaos`.
            const float patch  = valNoise2(wx * 0.05f, wz * 0.05f);
            const float patch2 = valNoise2(wx * 0.17f + 60.0f, wz * 0.17f + 60.0f);
            const float bare   = valNoise2(wx * 0.13f + 19.0f, wz * 0.13f + 7.0f);
            const float bare2  = valNoise2(wx * 0.31f + 3.0f,  wz * 0.31f + 23.0f);
            // Broad bare patches always apply; the finer holes only with chaos.
            if (bare < 0.26f || bare2 < 0.12f * chaos) continue;
            const float densJit = 1.0f + (glm::mix(0.55f, 1.20f, patch2) - 1.0f) * chaos;
            const float dens    = glm::mix(0.25f, 1.25f, patch) * densJit;
            const float dist  = std::sqrt(x * x + z * z);
            const float rim   = 1.0f - glm::smoothstep(R * 0.82f, R, dist);
            // Distance LOD: thin out far cells hard -- big win.
            const float lod   = glm::mix(1.0f, 0.22f,
                                         glm::smoothstep(0.28f, 1.0f, dist / R));
            // Per-cell count jitter breaks the even grid density (chaos-scaled).
            const float cellJit = 1.0f + (glm::mix(0.60f, 1.30f, u(rng)) - 1.0f) * chaos;
            const int   count = static_cast<int>(per * dens
                                * glm::mix(0.35f, 1.0f, lush) * rim * lod * cellJit);
            // Height clumps have their OWN frequency (independent of density), so
            // tall tufts and low turf don't line up with thick/thin -- the main
            // cue that turns an even lawn into a wild meadow.
            const float tuft = valNoise2(wx * 0.11f + 40.0f, wz * 0.11f + 40.0f);
            const float jitPos = spacing * (2.2f + 0.4f * chaos);
            for (int b = 0; b < count; ++b) {
                const float tuftF = 1.0f + (glm::mix(0.50f, 1.40f, tuft) - 1.0f) * chaos;
                const float jitF  = 1.0f + (glm::mix(0.65f, 1.30f, u(rng)) - 1.0f) * chaos;
                float bh = gHeight * glm::max(0.15f, tuftF * jitF);
                // A few stalks shoot well above the canopy (grass gone to seed).
                if (u(rng) < 0.05f * chaos) bh *= glm::mix(1.4f, 2.0f, u(rng));
                out.insert(out.end(), {
                    wx + (u(rng) - 0.5f) * jitPos, h,
                    wz + (u(rng) - 0.5f) * jitPos,
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
                                   road, roadClear, grassChaos);
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
    for (TreeSpecies& sp : m_species) {
        for (TreeLOD& lod : sp.lods) {
            if (lod.vbo) glDeleteBuffers(1, &lod.vbo);
            if (lod.vao) glDeleteVertexArrays(1, &lod.vao);
        }
        if (sp.instVBO) glDeleteBuffers(1, &sp.instVBO);
        if (sp.bbVAO)   glDeleteVertexArrays(1, &sp.bbVAO);
    }
}

// Wire the per-instance attributes (iPos3, iRot, iScale) of the currently-bound
// VAO to a species' instance buffer. Same 5-float layout for the mesh and the
// billboard VAOs.
static void bindTreeInstanceAttribs(std::uint32_t instVBO) {
    glBindBuffer(GL_ARRAY_BUFFER, instVBO);
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
}

void VegetationSystem::scanTreeAssets() {
    m_modelFiles.clear();
    m_texFiles.clear();
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(m_modelDir, ec)) {
        if (!e.is_regular_file()) continue;
        std::string ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        if (ext == ".glb" || ext == ".gltf")
            m_modelFiles.push_back(e.path().filename().string());
    }
    for (const auto& e : std::filesystem::directory_iterator(m_texDir, ec)) {
        if (!e.is_regular_file()) continue;
        std::string ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        if (ext == ".png") m_texFiles.push_back(e.path().filename().string());
    }
    std::sort(m_modelFiles.begin(), m_modelFiles.end());
    std::sort(m_texFiles.begin(), m_texFiles.end());
}

bool VegetationSystem::loadTreeMesh(const std::string& path, TreeSpecies& sp, TreeLOD& lod) {
    lod.prims.clear();
    if (lod.vbo) { glDeleteBuffers(1, &lod.vbo); lod.vbo = 0; }
    if (lod.vao) { glDeleteVertexArrays(1, &lod.vao); lod.vao = 0; }

    // Instanced mesh geometry normalized to unit height (instance scale = size),
    // split into per-material draw groups (combined layout: pos3 normal3 uv2).
    fitzel::ModelData md = fitzel::loadGltf(path);
    if (md.empty() || md.height() < 0.01f) {
        std::fprintf(stderr, "Tree model failed to load: %s\n", path.c_str());
        return false;
    }
    std::vector<float> verts;
    const float scale = 1.0f / md.height();
    for (fitzel::ModelPrimitive& p : md.primitives) {
        TreeLOD::Prim tp;
        tp.first  = static_cast<int>(verts.size() / 8);
        tp.count  = p.vertexCount();
        tp.cutout = p.alphaCutout;
        tp.hasTex = !p.texPixels.empty();
        if (tp.hasTex)
            tp.tex = Texture::fromPixels(p.texPixels.data(), p.texWidth, p.texHeight, 4);
        for (std::size_t i = 0; i + 7 < p.vertices.size(); i += 8) {
            verts.push_back(p.vertices[i + 0] * scale);
            verts.push_back((p.vertices[i + 1] - md.minY) * scale);
            verts.push_back(p.vertices[i + 2] * scale);
            verts.push_back(p.vertices[i + 3]);
            verts.push_back(p.vertices[i + 4]);
            verts.push_back(p.vertices[i + 5]);
            verts.push_back(p.vertices[i + 6]);
            verts.push_back(p.vertices[i + 7]);
        }
        lod.prims.push_back(std::move(tp));
    }
    glGenVertexArrays(1, &lod.vao);
    glBindVertexArray(lod.vao);
    glGenBuffers(1, &lod.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, lod.vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_STATIC_DRAW);
    const GLsizei ms = 8 * sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, ms, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, ms, (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, ms, (void*)(6 * sizeof(float)));
    bindTreeInstanceAttribs(sp.instVBO);
    glBindVertexArray(0);
    return true;
}

int VegetationSystem::addSpecies() {
    TreeSpecies sp;
    sp.name = "Tree " + std::to_string(m_species.size() + 1);
    glGenBuffers(1, &sp.instVBO);
    // Billboard LOD VAO (camera-facing quad; corner from gl_VertexID) reusing the
    // species' instance buffer.
    glGenVertexArrays(1, &sp.bbVAO);
    glBindVertexArray(sp.bbVAO);
    bindTreeInstanceAttribs(sp.instVBO);
    glBindVertexArray(0);
    // Default LOD0 = first available model, so a fresh species is visible at once.
    TreeLOD lod;
    lod.dist = 45.0f;
    if (!m_modelFiles.empty()) {
        lod.model = m_modelFiles.front();
        loadTreeMesh(m_modelDir + "/" + lod.model, sp, lod);
    }
    sp.lods.push_back(std::move(lod));
    // Default billboard: the classic tree impostor if present, else the first PNG.
    std::string bb;
    for (const std::string& f : m_texFiles)
        if (f == "billboard_tree_bled.png") { bb = f; break; }
    if (bb.empty() && !m_texFiles.empty()) bb = m_texFiles.front();
    if (!bb.empty()) {
        sp.billboard = bb;
        sp.bbTex = Texture::fromFile(m_texDir + "/" + bb);
        if (sp.bbTex.height() > 0)
            sp.bbAspect = static_cast<float>(sp.bbTex.width()) / sp.bbTex.height();
    }
    m_species.push_back(std::move(sp));
    treeCenter = glm::vec2(1e9f); // new species appears on the next regrow
    return static_cast<int>(m_species.size()) - 1;
}

void VegetationSystem::removeSpecies(int s) {
    if (s < 0 || s >= static_cast<int>(m_species.size())) return;
    TreeSpecies& sp = m_species[s];
    for (TreeLOD& lod : sp.lods) {
        if (lod.vbo) glDeleteBuffers(1, &lod.vbo);
        if (lod.vao) glDeleteVertexArrays(1, &lod.vao);
    }
    if (sp.instVBO) glDeleteBuffers(1, &sp.instVBO);
    if (sp.bbVAO)   glDeleteVertexArrays(1, &sp.bbVAO);
    m_species.erase(m_species.begin() + s);
    // Fix up painted trees: drop those on the removed species, shift higher ids.
    std::vector<float> kept;
    kept.reserve(paintedTrees.size());
    for (std::size_t t = 0; t + 6 <= paintedTrees.size(); t += 6) {
        int idx = static_cast<int>(std::lround(paintedTrees[t + 5]));
        if (idx == s) continue;
        kept.insert(kept.end(), paintedTrees.begin() + t, paintedTrees.begin() + t + 6);
        if (idx > s) kept[kept.size() - 1] = static_cast<float>(idx - 1);
    }
    paintedTrees.swap(kept);
    paintSpecies = glm::clamp(paintSpecies, 0,
                              std::max(0, static_cast<int>(m_species.size()) - 1));
    treeCenter = glm::vec2(1e9f);
    rebuildTreeBuffers();
}

void VegetationSystem::addLOD(int s) {
    if (s < 0 || s >= static_cast<int>(m_species.size())) return;
    TreeSpecies& sp = m_species[s];
    TreeLOD lod;
    lod.dist = sp.lods.empty() ? 45.0f : sp.lods.back().dist + 25.0f;
    lod.model = sp.lods.empty() ? (m_modelFiles.empty() ? std::string{} : m_modelFiles.front())
                                : sp.lods.back().model;
    if (!lod.model.empty()) loadTreeMesh(m_modelDir + "/" + lod.model, sp, lod);
    sp.lods.push_back(std::move(lod));
}

void VegetationSystem::removeLOD(int s, int lod) {
    if (s < 0 || s >= static_cast<int>(m_species.size())) return;
    TreeSpecies& sp = m_species[s];
    if (lod < 0 || lod >= static_cast<int>(sp.lods.size())) return;
    TreeLOD& L = sp.lods[lod];
    if (L.vbo) glDeleteBuffers(1, &L.vbo);
    if (L.vao) glDeleteVertexArrays(1, &L.vao);
    sp.lods.erase(sp.lods.begin() + lod);
}

void VegetationSystem::setLODModel(int s, int lod, const std::string& file) {
    if (s < 0 || s >= static_cast<int>(m_species.size())) return;
    TreeSpecies& sp = m_species[s];
    if (lod < 0 || lod >= static_cast<int>(sp.lods.size())) return;
    sp.lods[lod].model = file;
    loadTreeMesh(m_modelDir + "/" + file, sp, sp.lods[lod]);
}

void VegetationSystem::setBillboard(int s, const std::string& file) {
    if (s < 0 || s >= static_cast<int>(m_species.size())) return;
    TreeSpecies& sp = m_species[s];
    sp.billboard = file;
    sp.bbTex = Texture::fromFile(m_texDir + "/" + file);
    if (sp.bbTex.height() > 0)
        sp.bbAspect = static_cast<float>(sp.bbTex.width()) / sp.bbTex.height();
}

bool VegetationSystem::initTrees(const std::string& modelDir, const std::string& texDir) {
    // Shaders are shared by every species (bound once, uniforms per draw).
    m_tree      = Shader::fromFiles("assets/shaders/tree.vert", "assets/shaders/tree.frag");
    m_treeDepth = Shader::fromFiles("assets/shaders/treedepth.vert",
                                    "assets/shaders/treedepth.frag");
    if (!m_tree.isValid() || !m_treeDepth.isValid()) {
        std::fprintf(stderr, "Failed to load tree shaders\n"); return false;
    }
    m_billboard = Shader::fromFiles("assets/shaders/billboard.vert",
                                    "assets/shaders/billboard.frag");
    if (!m_billboard.isValid()) {
        std::fprintf(stderr, "Failed to load billboard shader\n"); return false;
    }
    m_modelDir = modelDir;
    m_texDir   = texDir;
    scanTreeAssets();
    // One default species so a fresh scene reproduces the previous single-tree
    // look (tree1.glb + the classic billboard). Scenes with a saved `trees`
    // block replace this via deserializeTrees().
    addSpecies();
    return true;
}

void VegetationSystem::rebuildTreeBuffers() {
    m_treeInst.clear();
    treeCount = 0;
    for (int i = 0; i < static_cast<int>(m_species.size()); ++i) {
        TreeSpecies& sp = m_species[i];
        sp.inst.resize(sp.proceduralFloats); // keep the procedural prefix, drop old painted
        if (treePainted)
            for (std::size_t t = 0; t + 6 <= paintedTrees.size(); t += 6) {
                if (static_cast<int>(std::lround(paintedTrees[t + 5])) != i) continue;
                sp.inst.insert(sp.inst.end(), paintedTrees.begin() + t,
                                              paintedTrees.begin() + t + 5); // drop speciesIdx
            }
        glBindBuffer(GL_ARRAY_BUFFER, sp.instVBO);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(sp.inst.size() * sizeof(float)),
                     sp.inst.data(), GL_DYNAMIC_DRAW);
        sp.count = static_cast<int>(sp.inst.size() / 5);
        treeCount += sp.count;
        m_treeInst.insert(m_treeInst.end(), sp.inst.begin(), sp.inst.end());
    }
}

void VegetationSystem::regenTrees(glm::vec2 cc, const std::vector<glm::vec2>& road,
                                  float roadWidth, float waterLevel, float snowLevel) {
    for (TreeSpecies& sp : m_species) sp.inst.clear();
    // Enabled, mesh-bearing species and their cumulative distribution weights.
    std::vector<int> active;
    float totalDensity = 0.0f;
    for (int i = 0; i < static_cast<int>(m_species.size()); ++i) {
        const TreeSpecies& sp = m_species[i];
        if (!sp.enabled || sp.lods.empty() || sp.density <= 0.0f) continue;
        active.push_back(i);
        totalDensity += sp.density;
    }
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    const float spacing = 7.0f;
    // Skip the whole procedural scatter when the generated source is off -- the
    // procedural prefix stays empty and only the painted trees remain.
    if (treeProcedural && !active.empty() && totalDensity > 0.0f) {
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
                // Forests cluster in moist regions; dry biomes stay open. Overall
                // placement scales with the summed species density.
                const float moist  = terrainMoisture(m_streamer.settings(), wx, wz);
                const float forest = 0.5f + 0.35f * std::sin(wx * 0.03f)
                                          + 0.35f * std::cos(wz * 0.026f + 1.3f);
                const float prob = glm::clamp(forest, 0.0f, 1.0f)
                                 * glm::smoothstep(0.35f, 0.75f, moist)
                                 * 0.6f * totalDensity;
                if (u(m_trng) > prob) continue;
                // Pick a species weighted by its density.
                float r = u(m_trng) * totalDensity;
                int pick = active.back();
                for (int idx : active) { r -= m_species[idx].density;
                                         if (r <= 0.0f) { pick = idx; break; } }
                TreeSpecies& sp = m_species[pick];
                const float tx = wx + (u(m_trng) - 0.5f) * spacing;
                const float tz = wz + (u(m_trng) - 0.5f) * spacing;
                sp.inst.insert(sp.inst.end(), {
                    tx, m_streamer.heightAt(tx, tz) - 0.3f, tz,
                    u(m_trng) * 6.2831f,
                    glm::mix(sp.size * 0.75f, sp.size * 1.3f, u(m_trng))});
            }
        }
    }
    for (TreeSpecies& sp : m_species) sp.proceduralFloats = sp.inst.size();
    rebuildTreeBuffers();  // append the painted trees per species and upload
    treeCenter = cc;
}

void VegetationSystem::updateTrees(glm::vec2 camXZ, const std::vector<glm::vec2>& road,
                                   float roadWidth, float waterLevel, float snowLevel) {
    if (treeEnabled && glm::length(camXZ - treeCenter) > 25.0f)
        regenTrees(camXZ, road, roadWidth, waterLevel, snowLevel);
}

void VegetationSystem::stampTree(glm::vec2 c, float radius, std::mt19937& rng,
                                 float waterLevel, float snowLevel) {
    if (m_species.empty()) return;
    const int s = glm::clamp(paintSpecies, 0, static_cast<int>(m_species.size()) - 1);
    const TreeSpecies& sp = m_species[s];
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
        for (std::size_t t = 0; t + 6 <= paintedTrees.size(); t += 6) {
            const float dx = wx - paintedTrees[t], dz = wz - paintedTrees[t + 2];
            if (dx * dx + dz * dz < minSp2) { tooClose = true; break; }
        }
        if (tooClose) continue;
        const float sc = glm::mix(sp.size * 0.8f, sp.size * 1.25f, u(rng));
        paintedTrees.insert(paintedTrees.end(), {
            wx, m_streamer.heightAt(wx, wz) - 0.3f, wz, u(rng) * 6.2831853f, sc,
            static_cast<float>(s)});
    }
    rebuildTreeBuffers();
}

void VegetationSystem::eraseTree(glm::vec2 c, float radius) {
    const float r2 = radius * radius;
    std::vector<float> kept;
    kept.reserve(paintedTrees.size());
    for (std::size_t i = 0; i + 6 <= paintedTrees.size(); i += 6) {
        const float dx = paintedTrees[i] - c.x, dz = paintedTrees[i + 2] - c.y;
        if (dx * dx + dz * dz <= r2) continue; // inside brush -> remove
        kept.insert(kept.end(), paintedTrees.begin() + i, paintedTrees.begin() + i + 6);
    }
    if (kept.size() != paintedTrees.size()) {
        paintedTrees.swap(kept);
        rebuildTreeBuffers();
    }
}

void VegetationSystem::drawTreeShadow(const glm::mat4& lightSpace, double time,
                                      float weather) {
    if (!treeEnabled || treeCount == 0) return;
    glDisable(GL_CULL_FACE);
    m_treeDepth.bind();
    m_treeDepth.setMat4("uLightSpace", lightSpace);
    m_treeDepth.setFloat("uTime", static_cast<float>(time));
    m_treeDepth.setVec2("uWindDir", glm::normalize(glm::vec2(0.6f, 0.3f)));
    m_treeDepth.setFloat("uWindStrength", glm::mix(0.05f, 0.4f, weather));
    m_treeDepth.setFloat("uTreeHeight", 1.0f); // meshes normalized to unit height
    m_treeDepth.setInt("uTex", 0);
    // Shadows use LOD0 over every instance (no distance banding).
    for (const TreeSpecies& sp : m_species) {
        if (!sp.enabled || sp.count == 0 || sp.lods.empty()) continue;
        const TreeLOD& lod = sp.lods.front();
        glBindVertexArray(lod.vao);
        for (const TreeLOD::Prim& tp : lod.prims) {
            if (tp.hasTex) tp.tex.bind(0);
            m_treeDepth.setInt("uAlphaCutout", tp.cutout ? 1 : 0);
            glDrawArraysInstanced(GL_TRIANGLES, tp.first, tp.count, sp.count);
        }
    }
    glBindVertexArray(0);
    glEnable(GL_CULL_FACE);
}

void VegetationSystem::drawTrees(const VegDrawContext& c) {
    if (!treeEnabled || treeCount == 0) return;
    glDisable(GL_CULL_FACE);
    m_tree.bind();
    m_tree.setMat4("uViewProj", c.viewProj);
    m_tree.setFloat("uTime", static_cast<float>(c.time));
    m_tree.setVec2("uWindDir", glm::normalize(glm::vec2(0.6f, 0.3f)));
    m_tree.setFloat("uWindStrength", glm::mix(0.05f, 0.4f, c.weather));
    m_tree.setFloat("uTreeHeight", 1.0f);
    m_tree.setVec3("uCamPos", c.camPos);
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
    for (const TreeSpecies& sp : m_species) {
        if (!sp.enabled || sp.count == 0) continue;
        const int nl = static_cast<int>(sp.lods.size());
        for (int k = 0; k < nl; ++k) {
            const TreeLOD& lod = sp.lods[k];
            const float lo = (k == 0) ? 0.0f : sp.lods[k - 1].dist;
            // The last mesh LOD runs out to the billboard start (or the far plane
            // when the species has no billboard).
            float hi = (k + 1 == nl) ? (sp.bbEnabled ? sp.bbStart : 1e9f) : lod.dist;
            hi = std::max(hi, lo);
            m_tree.setFloat("uLodMin", lo);
            m_tree.setFloat("uLodNear", hi);
            glBindVertexArray(lod.vao);
            for (const TreeLOD::Prim& tp : lod.prims) {
                if (tp.hasTex) tp.tex.bind(0);
                m_tree.setInt("uAlphaCutout", tp.cutout ? 1 : 0);
                glDrawArraysInstanced(GL_TRIANGLES, tp.first, tp.count, sp.count);
            }
        }
    }
    glBindVertexArray(0);
    glEnable(GL_CULL_FACE);
}

void VegetationSystem::drawTreeBillboards(const VegDrawContext& c,
                                          const glm::vec3& camRight) {
    if (!treeEnabled || treeCount == 0) return;
    glDisable(GL_CULL_FACE);
    m_billboard.bind();
    m_billboard.setMat4("uViewProj", c.viewProj);
    m_billboard.setVec3("uCamRight", camRight);
    m_billboard.setVec3("uCamPos", c.camPos);
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
    for (const TreeSpecies& sp : m_species) {
        if (!sp.enabled || sp.count == 0 || !sp.bbEnabled || !sp.bbTex.isValid()) continue;
        m_billboard.setFloat("uLodNear", sp.bbStart);   // billboard only beyond this
        m_billboard.setFloat("uTreeHeight", sp.bbSize); // billboard height factor
        m_billboard.setFloat("uAspect", sp.bbAspect);
        sp.bbTex.bind(0);
        glBindVertexArray(sp.bbVAO);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, sp.count);
    }
    glBindVertexArray(0);
    glEnable(GL_CULL_FACE);
}

void VegetationSystem::panelTrees(bool& treePaintMode, bool& brushErase,
                                  const std::function<void()>& onGrabLMB) {
    // Master toggle + one-line status so the section reads at a glance.
    ImGui::Checkbox("Trees", &treeEnabled);
    ImGui::SameLine();
    ImGui::TextDisabled("%d species  -  %d drawn",
                        static_cast<int>(m_species.size()), treeCount);

    // Source toggles: use generated and/or painted trees, in any combination.
    ImGui::BeginDisabled(!treeEnabled);
    if (ImGui::Checkbox("Generated", &treeProcedural))
        treeCenter = glm::vec2(1e9f); // regen the forest with the new setting
    ImGui::SameLine();
    if (ImGui::Checkbox("Painted##src", &treePainted))
        rebuildTreeBuffers();         // just add/drop the painted instances
    ImGui::EndDisabled();

    static int sel = 0;
    sel = glm::clamp(sel, 0, std::max(0, static_cast<int>(m_species.size()) - 1));

    // === Species overview (foldable) =======================================
    if (ImGui::CollapsingHeader("Species", ImGuiTreeNodeFlags_DefaultOpen)) {
        const float rowH = ImGui::GetTextLineHeightWithSpacing();
        const int   rows = glm::clamp(static_cast<int>(m_species.size()), 1, 6);
        if (ImGui::BeginListBox("##species", ImVec2(-FLT_MIN, rows * rowH + 6.0f))) {
            for (int i = 0; i < static_cast<int>(m_species.size()); ++i) {
                TreeSpecies& sp = m_species[i];
                ImGui::PushID(i);
                bool en = sp.enabled;
                if (ImGui::Checkbox("##en", &en)) {
                    sp.enabled = en;
                    treeCenter = glm::vec2(1e9f); // redistribute
                }
                ImGui::SameLine();
                char lbl[160];
                std::snprintf(lbl, sizeof lbl, "%-12s  %dx LOD | BB %s | d%.1f",
                              sp.name.c_str(), static_cast<int>(sp.lods.size()),
                              sp.bbEnabled ? "on" : "off", sp.density);
                if (ImGui::Selectable(lbl, sel == i)) sel = i;
                ImGui::PopID();
            }
            ImGui::EndListBox();
        }
        if (ImGui::Button("+ Add species")) sel = addSpecies();
        ImGui::SameLine();
        ImGui::BeginDisabled(m_species.size() <= 1);
        if (ImGui::Button("- Remove")) {
            removeSpecies(sel);
            sel = glm::clamp(sel, 0, std::max(0, static_cast<int>(m_species.size()) - 1));
        }
        ImGui::EndDisabled();
    }

    // === Selected-species editor (foldable) ================================
    if (sel >= 0 && sel < static_cast<int>(m_species.size())) {
        TreeSpecies& sp = m_species[sel];
        // "###edit" keeps a stable id so the fold state survives renames/selection.
        char hdr[96];
        std::snprintf(hdr, sizeof hdr, "Edit: %s###editSpecies", sp.name.c_str());
        if (ImGui::CollapsingHeader(hdr, ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::PushID("editSpecies");
            ImGui::Indent();

            char nb[64];
            std::snprintf(nb, sizeof nb, "%s", sp.name.c_str());
            if (ImGui::InputText("Name", nb, sizeof nb)) sp.name = nb;
            bool retree = false;
            retree |= ImGui::SliderFloat("Density", &sp.density, 0.0f, 2.0f);
            retree |= ImGui::SliderFloat("Avg size", &sp.size, 2.0f, 25.0f, "%.1f m");
            if (retree) treeCenter = glm::vec2(1e9f);

            // --- LOD meshes (nearest first; last mesh runs out to the billboard).
            if (ImGui::TreeNodeEx("LOD meshes", ImGuiTreeNodeFlags_DefaultOpen)) {
                for (int k = 0; k < static_cast<int>(sp.lods.size()); ++k) {
                    ImGui::PushID(k);
                    TreeLOD& lod = sp.lods[k];
                    int cur = -1;
                    for (int m = 0; m < static_cast<int>(m_modelFiles.size()); ++m)
                        if (m_modelFiles[m] == lod.model) { cur = m; break; }
                    ImGui::AlignTextToFramePadding();
                    ImGui::Text("LOD%d", k);
                    ImGui::SameLine();
                    const char* preview = (cur >= 0) ? m_modelFiles[cur].c_str() : "(none)";
                    ImGui::SetNextItemWidth(150.0f);
                    if (ImGui::BeginCombo("##model", preview)) {
                        for (int m = 0; m < static_cast<int>(m_modelFiles.size()); ++m)
                            if (ImGui::Selectable(m_modelFiles[m].c_str(), m == cur))
                                setLODModel(sel, k, m_modelFiles[m]);
                        ImGui::EndCombo();
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("X")) { removeLOD(sel, k); ImGui::PopID(); break; }
                    if (k + 1 < static_cast<int>(sp.lods.size())) {
                        ImGui::SetNextItemWidth(150.0f);
                        ImGui::SliderFloat("Switch dist", &lod.dist, 5.0f, 200.0f, "%.0f m");
                    } else {
                        ImGui::TextDisabled("   -> billboard / far");
                    }
                    ImGui::PopID();
                }
                if (ImGui::SmallButton("+ Add LOD")) addLOD(sel);
                ImGui::TreePop();
            }

            // --- Billboard (far LOD).
            if (ImGui::TreeNodeEx("Billboard", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Checkbox("Enabled##bb", &sp.bbEnabled);
                int cur = -1;
                for (int m = 0; m < static_cast<int>(m_texFiles.size()); ++m)
                    if (m_texFiles[m] == sp.billboard) { cur = m; break; }
                const char* pv = (cur >= 0) ? m_texFiles[cur].c_str() : "(none)";
                if (ImGui::BeginCombo("Texture", pv)) {
                    for (int m = 0; m < static_cast<int>(m_texFiles.size()); ++m)
                        if (ImGui::Selectable(m_texFiles[m].c_str(), m == cur))
                            setBillboard(sel, m_texFiles[m]);
                    ImGui::EndCombo();
                }
                ImGui::SliderFloat("Start dist", &sp.bbStart, 20.0f, 300.0f, "%.0f m");
                ImGui::SliderFloat("Aspect", &sp.bbAspect, 0.3f, 2.0f);
                ImGui::SliderFloat("Size##bb", &sp.bbSize, 0.5f, 2.0f);
                ImGui::TreePop();
            }

            ImGui::Unindent();
            ImGui::PopID();
        }
    }

    // === Paint trees (foldable, closed by default) =========================
    if (ImGui::CollapsingHeader("Paint trees (3D brush)")) {
        ImGui::Indent();
        if (ImGui::Checkbox("Paint mode##tree", &treePaintMode) && treePaintMode) onGrabLMB();
        if (treePaintMode)
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.6f, 1.0f),
                               "Drag = plant | hold Alt = erase");
        else
            ImGui::TextDisabled("Enable to plant trees onto the terrain");
        if (!m_species.empty()) {
            paintSpecies = glm::clamp(paintSpecies, 0,
                                      static_cast<int>(m_species.size()) - 1);
            if (ImGui::BeginCombo("Paint species", m_species[paintSpecies].name.c_str())) {
                for (int i = 0; i < static_cast<int>(m_species.size()); ++i)
                    if (ImGui::Selectable(m_species[i].name.c_str(), i == paintSpecies))
                        paintSpecies = i;
                ImGui::EndCombo();
            }
        }
        ImGui::Checkbox("Erase##tree", &brushErase);
        ImGui::SliderFloat("Brush size##tree", &treeBrushRadius, 1.0f, 40.0f, "%.1f m");
        ImGui::SliderFloat("Density##tree", &treeBrushDensity, 0.1f, 4.0f);
        ImGui::SliderFloat("Min spacing", &treeMinSpacing, 1.0f, 15.0f, "%.1f m");
        ImGui::Text("Painted trees: %d", static_cast<int>(paintedTrees.size() / 6));
        ImGui::BeginDisabled(paintedTrees.empty());
        if (ImGui::Button("Clear painted##tree")) clearPaintedTrees();
        ImGui::EndDisabled();
        ImGui::Unindent();
    }
}

void VegetationSystem::serializeTrees(nlohmann::json& j) const {
    nlohmann::json arr = nlohmann::json::array();
    for (const TreeSpecies& sp : m_species) {
        nlohmann::json lods = nlohmann::json::array();
        for (const TreeLOD& lod : sp.lods)
            lods.push_back({{"model", lod.model}, {"dist", lod.dist}});
        arr.push_back({
            {"name", sp.name}, {"enabled", sp.enabled},
            {"density", sp.density}, {"size", sp.size},
            {"lods", lods},
            {"billboard", sp.billboard}, {"bbEnabled", sp.bbEnabled},
            {"bbStart", sp.bbStart}, {"bbAspect", sp.bbAspect}, {"bbSize", sp.bbSize}});
    }
    j["trees"]          = arr;
    j["treeEnabled"]    = treeEnabled;
    j["treeProcedural"] = treeProcedural;
    j["treePainted"]    = treePainted;
}

void VegetationSystem::deserializeTrees(const nlohmann::json& j) {
    if (!j.contains("trees") || !j["trees"].is_array()) return; // keep the default species
    // Free the existing species' GL resources.
    for (TreeSpecies& sp : m_species) {
        for (TreeLOD& lod : sp.lods) {
            if (lod.vbo) glDeleteBuffers(1, &lod.vbo);
            if (lod.vao) glDeleteVertexArrays(1, &lod.vao);
        }
        if (sp.instVBO) glDeleteBuffers(1, &sp.instVBO);
        if (sp.bbVAO)   glDeleteVertexArrays(1, &sp.bbVAO);
    }
    m_species.clear();
    treeEnabled    = j.value("treeEnabled", true);
    treeProcedural = j.value("treeProcedural", true);
    treePainted    = j.value("treePainted", true);
    for (const auto& sj : j["trees"]) {
        const int s = addSpecies();           // mints instVBO/bbVAO + a default LOD
        TreeSpecies& sp = m_species[s];
        for (TreeLOD& lod : sp.lods) {         // drop the default LOD; load from JSON
            if (lod.vbo) glDeleteBuffers(1, &lod.vbo);
            if (lod.vao) glDeleteVertexArrays(1, &lod.vao);
        }
        sp.lods.clear();
        sp.name    = sj.value("name", sp.name);
        sp.enabled = sj.value("enabled", true);
        sp.density = sj.value("density", 1.0f);
        sp.size    = sj.value("size", 9.0f);
        if (sj.contains("lods") && sj["lods"].is_array()) {
            for (const auto& lj : sj["lods"]) {
                TreeLOD lod;
                lod.model = lj.value("model", std::string{});
                lod.dist  = lj.value("dist", 45.0f);
                if (!lod.model.empty()) loadTreeMesh(m_modelDir + "/" + lod.model, sp, lod);
                sp.lods.push_back(std::move(lod));
            }
        }
        sp.bbEnabled = sj.value("bbEnabled", true);
        sp.bbStart   = sj.value("bbStart", 60.0f);
        sp.bbAspect  = sj.value("bbAspect", 0.93f);
        sp.bbSize    = sj.value("bbSize", 1.05f);
        const std::string bb = sj.value("billboard", std::string{});
        if (!bb.empty()) {
            sp.billboard = bb;
            sp.bbTex = Texture::fromFile(m_texDir + "/" + bb); // keep the saved aspect
        }
    }
    if (m_species.empty()) addSpecies(); // never leave the scene with zero species
    paintSpecies = glm::clamp(paintSpecies, 0,
                              static_cast<int>(m_species.size()) - 1);
    treeCenter = glm::vec2(1e9f); // force a regrow with the restored config
}

// --- Flowers ----------------------------------------------------------------

bool VegetationSystem::initFlowers() {
    m_flower = Shader::fromFiles("assets/shaders/flower.vert", "assets/shaders/flower.frag");
    if (!m_flower.isValid()) { std::fprintf(stderr, "Failed to load flower shader\n"); return false; }
    // base: pos3, normal3, tint ; instance: iPos3, iYaw, iScale, iColor3.
    const std::vector<float> mesh = makeFlowerMesh();
    m_flowerVerts = static_cast<int>(mesh.size() / 7);
    m_flowerField = InstancedMesh::create(
        mesh.data(), mesh.size(), 7 * sizeof(float),
        {{0, 3, 0}, {1, 3, 3 * sizeof(float)}, {2, 1, 6 * sizeof(float)}},
        8 * sizeof(float),
        {{3, 3, 0}, {4, 1, 3 * sizeof(float)}, {5, 1, 4 * sizeof(float)},
         {6, 3, 5 * sizeof(float)}});
    return true;
}

void VegetationSystem::rebuildFlowerBuffer() {
    m_flowerInst.resize(m_proceduralFlowerFloats);
    m_flowerInst.insert(m_flowerInst.end(),
                        paintedFlowers.begin(), paintedFlowers.end());
    m_flowerField.upload(m_flowerInst);
    flowerCount = m_flowerField.count();
}

void VegetationSystem::regenFlowers(glm::vec2 c, const std::vector<glm::vec2>& road,
                                    float roadWidth, float waterLevel, float snowLevel) {
    std::vector<float>& out = m_flowerInst;
    out.clear();
    std::mt19937 rng(4242u);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    const float R = grassRadius, spacing = 0.9f;
    const float clear = roadWidth * 0.5f + 1.5f;
    // Natural meadow palette, weighted toward buttercup yellow and white.
    const glm::vec3 palette[5] = {{0.96f, 0.78f, 0.12f},  // buttercup yellow
                                  {0.94f, 0.55f, 0.12f},  // warm orange
                                  {0.95f, 0.95f, 0.88f},  // daisy white
                                  {0.86f, 0.46f, 0.55f},  // soft pink
                                  {0.60f, 0.55f, 0.82f}}; // pale lavender
    for (float z = -R; z <= R; z += spacing) {
        for (float x = -R; x <= R; x += spacing) {
            if (x * x + z * z > R * R) continue;
            const float wx = c.x + x, wz = c.y + z;
            if (roadDistanceSq(road, wx, wz) < clear * clear) continue;
            const float h = m_streamer.heightAt(wx, wz);
            if (h < waterLevel + 0.6f || h > snowLevel - 2.0f) continue;
            const float e = 1.0f;
            const glm::vec3 n = glm::normalize(glm::vec3(
                m_streamer.heightAt(wx - e, wz) - m_streamer.heightAt(wx + e, wz), 2.0f * e,
                m_streamer.heightAt(wx, wz - e) - m_streamer.heightAt(wx, wz + e)));
            if (n.y < 0.9f) continue;
            const float moist = terrainMoisture(m_streamer.settings(), wx, wz);
            if (moist < 0.3f) continue; // flowers want greener ground

            // Clumps where a mid-frequency noise peaks; a small background chance
            // sprinkles lone flowers between the groups.
            const float clump  = valNoise2(wx * 0.16f + 50.0f, wz * 0.16f + 50.0f);
            const float groupP = glm::smoothstep(0.66f, 0.9f, clump);

            // Flowers gather in the shade around tree trunks.
            float treeP = 0.0f;
            for (int t = 0; t < treeCount; ++t) {
                const float dx = wx - m_treeInst[t * 5 + 0];
                const float dz = wz - m_treeInst[t * 5 + 2];
                const float dd = dx * dx + dz * dz;
                if (dd < 30.0f) treeP = std::max(treeP, glm::smoothstep(30.0f, 3.0f, dd));
            }

            const float prob = (0.02f + groupP * 0.9f + treeP * 0.75f)
                             * glm::smoothstep(0.3f, 0.7f, moist) * flowerDensity;
            if (u(rng) > prob) continue;
            const float fx = wx + (u(rng) - 0.5f) * spacing;
            const float fz = wz + (u(rng) - 0.5f) * spacing;
            // Weighted pick: mostly yellow/orange/white, few pink/lavender.
            const float cr = u(rng);
            const int ci = cr < 0.42f ? 0 : cr < 0.60f ? 1 : cr < 0.82f ? 2
                         : cr < 0.92f ? 3 : 4;
            const glm::vec3 col = palette[ci];
            // Meadow flowers are small; squared roll keeps most of them tiny.
            const float sr = u(rng);
            const float scale = glm::mix(0.30f, 0.60f, sr * sr);
            out.insert(out.end(), {fx, m_streamer.heightAt(fx, fz) - 0.02f, fz,
                                   u(rng) * 6.2831f, scale,
                                   col.r, col.g, col.b});
        }
    }
    m_proceduralFlowerFloats = out.size();
    rebuildFlowerBuffer();  // append the painted flowers and upload
}

void VegetationSystem::stampFlower(glm::vec2 c, float radius, std::mt19937& rng,
                                   float waterLevel, float snowLevel) {
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    const float area  = 3.14159265f * radius * radius;
    const int   tries = std::max(2, static_cast<int>(area * 0.7f * flowerBrushDensity));
    const glm::vec3 palette[5] = {{0.96f, 0.78f, 0.12f}, {0.94f, 0.55f, 0.12f},
                                  {0.95f, 0.95f, 0.88f}, {0.86f, 0.46f, 0.55f},
                                  {0.60f, 0.55f, 0.82f}};
    for (int i = 0; i < tries; ++i) {
        const float ang = u(rng) * 6.2831853f;
        const float rad = std::sqrt(u(rng)) * radius;
        const float wx  = c.x + std::cos(ang) * rad;
        const float wz  = c.y + std::sin(ang) * rad;
        const float h   = m_streamer.heightAt(wx, wz);
        if (h < waterLevel + 0.6f || h > snowLevel - 2.0f) continue;
        const float e = 1.0f;
        const glm::vec3 n = glm::normalize(glm::vec3(
            m_streamer.heightAt(wx - e, wz) - m_streamer.heightAt(wx + e, wz),
            2.0f * e,
            m_streamer.heightAt(wx, wz - e) - m_streamer.heightAt(wx, wz + e)));
        if (n.y < 0.90f) continue; // flowers want fairly flat ground
        const float cr = u(rng);
        const int ci = cr < 0.42f ? 0 : cr < 0.60f ? 1 : cr < 0.82f ? 2
                     : cr < 0.92f ? 3 : 4;
        const glm::vec3 col = palette[ci];
        const float sr = u(rng);
        const float scale = glm::mix(0.30f, 0.60f, sr * sr);
        paintedFlowers.insert(paintedFlowers.end(), {
            wx, m_streamer.heightAt(wx, wz) - 0.02f, wz,
            u(rng) * 6.2831853f, scale, col.r, col.g, col.b});
    }
    rebuildFlowerBuffer();
}

void VegetationSystem::eraseFlower(glm::vec2 c, float radius) {
    const float r2 = radius * radius;
    std::vector<float> kept;
    kept.reserve(paintedFlowers.size());
    for (std::size_t i = 0; i + 8 <= paintedFlowers.size(); i += 8) {
        const float dx = paintedFlowers[i] - c.x, dz = paintedFlowers[i + 2] - c.y;
        if (dx * dx + dz * dz <= r2) continue;
        kept.insert(kept.end(), paintedFlowers.begin() + i,
                                paintedFlowers.begin() + i + 8);
    }
    if (kept.size() != paintedFlowers.size()) {
        paintedFlowers.swap(kept);
        rebuildFlowerBuffer();
    }
}

void VegetationSystem::drawFlowers(const VegDrawContext& c) {
    if (!flowerEnabled || flowerCount <= 0) return;
    glDisable(GL_CULL_FACE);
    m_flower.bind();
    m_flower.setMat4("uViewProj", c.viewProj);
    m_flower.setFloat("uTime", static_cast<float>(c.time));
    m_flower.setVec2("uWindDir", glm::normalize(glm::vec2(0.6f, 0.3f)));
    m_flower.setFloat("uWindStrength", glm::mix(0.08f, 0.55f, c.weather));
    m_flower.setVec3("uViewPos", c.camPos);
    m_flower.setVec3("uLightDir", c.lightDir);
    m_flower.setVec3("uLightColor", c.lightColor);
    m_flower.setVec3("uAmbient", c.ambient);
    m_flower.setVec3("uFogColor", c.fogColor);
    m_flower.setVec3("uFogSunColor", c.fogSunColor);
    m_flower.setFloat("uFogDensity", c.fogDensity);
    m_flower.setFloat("uFogHeightFalloff", c.fogHeightFalloff);
    m_flower.setFloat("uFogHeight", c.fogHeight);
    m_flowerField.draw(GL_TRIANGLES, m_flowerVerts);
    glEnable(GL_CULL_FACE);
}
