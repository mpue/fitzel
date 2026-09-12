#include "VegetationSystem.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glad/gl.h>
#include <imgui.h>
#include <nlohmann/json.hpp>

#include <fitzel/scene/Camera.hpp>
#include <fitzel/asset/Vfs.hpp>
#include <fitzel/world/Model.hpp>
#include <fitzel/world/Terrain.hpp>

#include "GrassTrace.hpp"
#include "MeshSimplify.hpp"
#include "Primitives.hpp"
#include "SandboxMath.hpp"
#include "UiStyle.hpp"

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
    m_paintedGrass = makeBladeField();
    // Base VAO for the streamed procedural field: just the blade strip (attrib 0).
    // Each tile's instance VBO is bound into attribs 1..5 at draw time.
    glGenVertexArrays(1, &m_grassBaseVAO);
    glGenBuffers(1, &m_grassBaseVBO);
    glBindVertexArray(m_grassBaseVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_grassBaseVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kBlade), kBlade, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glBindVertexArray(0);
    // Tile the field ~48 m each way (radius 4 * 12 m tiles) -- a touch wider than
    // the old 46 m disc, but now streamed on a worker pool so the camera never
    // triggers a field-wide regen. The real generator is wired on the first
    // updateGrass; this call just starts the pool with a stable config.
    TiledScatter::Config gc;
    gc.tileSize = 12.0f; gc.radius = 4; gc.floatsPerInstance = 7;
    gc.maxUploadsPerFrame = 2; gc.workerThreads = 0; // auto pool size
    m_grassTiles.configure(gc, {});

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
    ui::sectionText("Birds");
    ImGui::Checkbox("Birds", &birdsEnabled);
    ImGui::SliderInt("Flock size", &m_birdCount, 0, 60);
    ImGui::SliderFloat("Bird size", &m_birdSize, 0.8f, 5.0f);

    ui::sectionText("Fireflies (night)");
    ImGui::Checkbox("Fireflies", &fireflyEnabled);
    ImGui::SliderInt("Count", &m_fireflyCount, 0, 256);
    ImGui::SliderFloat("Firefly size", &m_fireflySize, 0.03f, 0.25f, "%.2f");
}

// --- Grass ------------------------------------------------------------------

// Cheap stable hash of a road centerline, so the grass field only re-places when
// the road actually moved (not every frame the polyline is passed in).
static std::uint32_t wetHashOf(const std::vector<glm::vec3>& w) {
    std::uint32_t h = 2166136261u ^ static_cast<std::uint32_t>(w.size());
    auto mix = [&](float f) {
        std::uint32_t b;
        std::memcpy(&b, &f, sizeof(b));
        h = (h ^ b) * 16777619u;
    };
    for (const glm::vec3& d : w) { mix(d.x); mix(d.y); mix(d.z); }
    return h;
}

static std::uint32_t roadHashOf(const std::vector<glm::vec2>& r) {
    std::uint32_t h = 2166136261u ^ static_cast<std::uint32_t>(r.size());
    auto mix = [&](float f) {
        std::uint32_t b;
        std::memcpy(&b, &f, sizeof(b));
        h = (h ^ b) * 16777619u;
    };
    if (!r.empty()) {
        mix(r.front().x); mix(r.front().y);
        mix(r.back().x);  mix(r.back().y);
        const glm::vec2 m = r[r.size() / 2];
        mix(m.x); mix(m.y);
    }
    return h;
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
        if (inDiscs(wet, wx, wz)) continue;
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
    if (!terrainPresent) return false; // nothing to grow on
    bool regenerated = false;

    // (Re)wire the tile generator whenever an input that changes placement moves
    // (sliders, water/snow line, terrain regen via grassDirty, or the road). Each
    // rebuild captures fresh copies so the workers never read live state, then
    // invalidates the resident tiles so the field re-streams with the new look.
    const std::uint32_t rh = roadHashOf(road);
    const auto ecoDiffers = [](const ecology::Params& a, const ecology::Params& b) {
        return a.enabled != b.enabled || a.cover != b.cover || a.standSize != b.standSize ||
               a.treeLine != b.treeLine || a.waterLevel != b.waterLevel ||
               a.solitary != b.solitary || a.slopeLove != b.slopeLove;
    };
    if (grassDirty || ecoDiffers(eco, m_gEco) || grassDryGrowth != m_gDry ||
        grassDensity != m_gDensity || grassChaos != m_gChaos ||
        grassHeight != m_gHeight || grassRadius != m_gRadius ||
        waterLevel != m_gWater || snowLevel != m_gSnow ||
        roadClear != m_gRoadClear || rh != m_gRoadHash ||
        wetHashOf(wet) != m_gWetHash) {
        const float water = waterLevel, snow = snowLevel, gh = grassHeight;
        const float gd = grassDensity, gc = grassChaos, rc = roadClear;
        // The field, as one value. Kept as a member so the path tracer can ask
        // what the viewport is showing (see traceField) instead of a second
        // place assembling the same parameters and getting one of them wrong.
        m_field.terrain    = m_streamer.settings();
        m_field.waterLevel = water;
        m_field.snowLevel  = snow;
        m_field.height     = gh;
        m_field.density    = gd;
        m_field.chaos      = gc;
        m_field.road       = road;
        m_field.roadClear  = rc;
        m_field.wet        = wet;
        m_field.eco        = eco;
        m_gEco             = eco;
        m_field.dryGrowth  = grassDryGrowth;
        m_gDry             = grassDryGrowth;
        // The "Grass range" slider (m) maps to a tile radius over the 12 m grid.
        const int tileR = std::clamp(
            static_cast<int>(std::lround(grassRadius / grassfield::Field::kTileSize)),
            1, 12);
        // Copied into the lambda, not captured by reference: the generator runs
        // on TiledScatter's workers, which outlive no particular frame.
        const grassfield::Field fieldCopy = m_field;
        m_grassTiles.configure(
            {grassfield::Field::kTileSize, tileR, 7, 2, 0},
            [fieldCopy](std::int32_t tx, std::int32_t tz, glm::vec2 origin,
                        float size, std::vector<float>& out) {
                grassfield::generateTile(tx, tz, origin, size, fieldCopy, out);
            });
        m_grassTiles.invalidate();
        m_gDensity = gd; m_gChaos = gc; m_gHeight = gh; m_gRadius = grassRadius;
        m_gWater = water; m_gSnow = snow; m_gRoadClear = rc; m_gRoadHash = rh;
        m_gWetHash = wetHashOf(wet);
        grassDirty = false;
    }

    // The tint is a shader uniform, not a placement input: changing it moves no
    // blade, so it is not in the dirty test above and is refreshed here instead.
    m_field.tint = grassTint;

    if (grassEnabled) m_grassTiles.update(camXZ);
    grassCount = m_grassTiles.instanceCount();

    // The old whole-field pass reported a regen (and its centre) so the caller
    // could regrow flowers to match. Streaming has no single regen event, so fire
    // the same signal on the same ~10 m camera drift the field used to rebuild on.
    if (glm::length(camXZ - m_grassCenter) > 10.0f) {
        m_grassCenter = camXZ;
        regenerated   = true;
    }

    if (paintedDirty) { // painted blades changed -> push to the GPU
        m_paintedGrass.upload(paintedBlades);
        paintedDirty = false;
    }
    return regenerated;
}

// --- Instance culling -------------------------------------------------------
// Why a tree needs this and a blade of grass does not.
//
// grass.vert rejects an off-screen blade in its first four lines, and that is
// enough there: a blade is seven vertices, so the shader that throws it away is
// most of what it would have cost anyway. A tree mesh is not seven vertices --
// the models people actually use run to hundreds of thousands each -- and a
// vertex shader that discards the tree still had to fetch and transform every
// one of them. The only way not to pay for a tree that is behind the camera is
// to not put it in the draw call.
//
// So each pass builds its own visible list. Per pass, not per frame: the water
// reflection looks the other way up and every shadow cascade looks from the sun,
// and a list culled against the camera would put holes in both.

// The six world-space frustum planes of a view-projection matrix (Gribb-Hartmann),
// normalized so a plane test yields a real distance.
static std::array<glm::vec4, 6> frustumPlanesOf(const glm::mat4& m) {
    std::array<glm::vec4, 6> p{};
    for (int i = 0; i < 3; ++i) {
        p[i * 2 + 0] = glm::vec4(m[0][3] + m[0][i], m[1][3] + m[1][i],
                                 m[2][3] + m[2][i], m[3][3] + m[3][i]);
        p[i * 2 + 1] = glm::vec4(m[0][3] - m[0][i], m[1][3] - m[1][i],
                                 m[2][3] - m[2][i], m[3][3] - m[3][i]);
    }
    for (glm::vec4& pl : p) {
        const float len = glm::length(glm::vec3(pl));
        if (len > 1e-6f) pl /= len;
    }
    return p;
}

// Is the sphere at least partly on the inside of the first `count` planes?
//
// `count` is 4 for a shadow cascade: the near and far planes of a light's box
// cut along the direction the light looks, and a caster standing in front of the
// slice -- outside it, but between it and the sun -- still throws its shadow
// into it. Culling on those two planes deletes exactly those shadows. Sideways
// is safe, which is where the saving is anyway.
static bool sphereVisible(const std::array<glm::vec4, 6>& planes, const glm::vec3& c,
                   float r, int count = 6) {
    for (int i = 0; i < count; ++i)
        if (glm::dot(glm::vec3(planes[i]), c) + planes[i].w < -r) return false;
    return true;
}

// Point the currently-bound VAO's per-instance attributes (iPos3, iRot, iHeight,
// iPhase, iLush) at one streamed grass tile's instance buffer. Mirrors the blade
// instance layout makeBladeField() sets up, but rebound per tile at draw time.
static void bindGrassInstanceAttribs(std::uint32_t instVBO) {
    glBindBuffer(GL_ARRAY_BUFFER, instVBO);
    const GLsizei is = 7 * sizeof(float);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, is, (void*)0);
    glVertexAttribDivisor(1, 1);
    for (int loc = 2; loc <= 5; ++loc) {
        glEnableVertexAttribArray(loc);
        glVertexAttribPointer(loc, 1, GL_FLOAT, GL_FALSE, is,
                              (void*)((3 + (loc - 2)) * sizeof(float)));
        glVertexAttribDivisor(loc, 1);
    }
}

void VegetationSystem::drawGrass(const FrameContext& c) {
    if (!terrainPresent) return;
    const bool drawProc    = grassEnabled && m_grassTiles.instanceCount() > 0;
    const bool drawPainted = m_paintedGrass.count() > 0;
    if (!drawProc && !drawPainted) return;
    glDisable(GL_CULL_FACE);
    m_grass.bind();
    m_grass.setMat4("uViewProj", c.viewProj);
    wind::apply(m_grass, wind, 0.5f);   // a blade goes further than a trunk
    {
        const int n = static_cast<int>(std::min<std::size_t>(grassPushers.size(), 8));
        m_grass.setInt("uPushCount", n);
        static const char* const kPush[8] = {"uPush[0]", "uPush[1]", "uPush[2]", "uPush[3]",
                                             "uPush[4]", "uPush[5]", "uPush[6]", "uPush[7]"};
        for (int i = 0; i < n; ++i) m_grass.setVec4(kPush[i], grassPushers[i]);
    }
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
    applySunShadows(m_grass, c);   // receives the sun's shadow (sunshadow.glsl)
    // Procedural blades bake absolute height (scale 1); painted blades store a
    // relative height and take the live "Blade height" slider.
    if (drawProc) {
        // Fade the field out over the outermost ~1.5 tiles so streamed tiles grow
        // in from the horizon rather than popping in at full height.
        const float ringEnd = m_grassTiles.radius() * m_grassTiles.tileSize();
        m_grass.setFloat("uFadeEnd", ringEnd);
        m_grass.setFloat("uFadeStart", glm::max(0.0f, ringEnd - m_grassTiles.tileSize() * 1.5f));
        m_grass.setFloat("uHeightScale", 1.0f);
        glBindVertexArray(m_grassBaseVAO);
        m_grassTiles.draw([](std::uint32_t vbo, int count, glm::vec2, float) {
            bindGrassInstanceAttribs(vbo);
            glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 7, count);
        });
        glBindVertexArray(0);
    }
    if (drawPainted) {
        m_grass.setFloat("uFadeStart", 1e9f); // painted layer: no distance fade
        m_grass.setFloat("uFadeEnd", 1e9f);
        m_grass.setFloat("uHeightScale", grassHeight);
        m_paintedGrass.draw(GL_TRIANGLE_STRIP, 7);
    }
    glEnable(GL_CULL_FACE);
}

// --- Trees ------------------------------------------------------------------

VegetationSystem::~VegetationSystem() {
    if (m_cullVBO)      glDeleteBuffers(1, &m_cullVBO);
    if (m_grassBaseVBO) glDeleteBuffers(1, &m_grassBaseVBO);
    if (m_grassBaseVAO) glDeleteVertexArrays(1, &m_grassBaseVAO);
    for (TreeSpecies& sp : m_species) {
        for (TreeLOD& lod : sp.lods) {
            if (lod.vbo) glDeleteBuffers(1, &lod.vbo);
            if (lod.ibo) glDeleteBuffers(1, &lod.ibo);
            if (lod.vao) glDeleteVertexArrays(1, &lod.vao);
        }
        if (sp.instVBO) glDeleteBuffers(1, &sp.instVBO);
        if (sp.bbVAO)   glDeleteVertexArrays(1, &sp.bbVAO);
        if (sp.farVBO)  glDeleteBuffers(1, &sp.farVBO);
        if (sp.farVAO)  glDeleteVertexArrays(1, &sp.farVAO);
        if (sp.impAlbedo) glDeleteTextures(1, &sp.impAlbedo);
        if (sp.impNormal) glDeleteTextures(1, &sp.impNormal);
        freeAutoLod(sp.mid);
        freeAutoLod(sp.shadow);
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
    m_modelPaths.clear();
    m_texPaths.clear();
    std::unordered_set<std::string> seenModel, seenTex; // dedupe by display name

    auto ext = [](const std::filesystem::path& p) {
        std::string e = p.extension().string();
        std::transform(e.begin(), e.end(), e.begin(),
                       [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        return e;
    };
    auto addModel = [&](const std::filesystem::path& p) {
        const std::string e = ext(p);
        if (e != ".glb" && e != ".gltf") return;
        const std::string name = p.filename().string();
        if (!seenModel.insert(name).second) return;
        m_modelFiles.push_back(name);
        m_modelPaths.push_back(p.generic_string());
    };
    auto addTex = [&](const std::filesystem::path& p) {
        if (ext(p) != ".png") return;
        const std::string name = p.filename().string();
        if (!seenTex.insert(name).second) return;
        m_texFiles.push_back(name);
        m_texPaths.push_back(p.generic_string());
    };

    // Built-in content dirs (flat scan, as before). Through the VFS: a species
    // names its model and texture, and in an exported game those names have to
    // resolve against the archive.
    for (const std::string& f : fitzel::vfs::listFiles(m_modelDir, false))
        addModel(std::filesystem::path(f));
    for (const std::string& f : fitzel::vfs::listFiles(m_texDir, false))
        addTex(std::filesystem::path(f));
    // Project-local assets (recursive), so a model dropped into the open project
    // is selectable as a species. Names already in content are kept (not shadowed).
    if (!m_projectDir.empty())
        for (const std::string& f : fitzel::vfs::listFiles(m_projectDir, true)) {
            addModel(std::filesystem::path(f));
            addTex(std::filesystem::path(f));
        }

    // Sort display names, keeping the parallel path lists aligned.
    auto sortPair = [](std::vector<std::string>& names, std::vector<std::string>& paths) {
        std::vector<int> order(names.size());
        for (int i = 0; i < static_cast<int>(order.size()); ++i) order[i] = i;
        std::sort(order.begin(), order.end(),
                  [&](int a, int b) { return names[a] < names[b]; });
        std::vector<std::string> sn, sp;
        sn.reserve(order.size()); sp.reserve(order.size());
        for (int i : order) { sn.push_back(names[i]); sp.push_back(paths[i]); }
        names.swap(sn);
        paths.swap(sp);
    };
    sortPair(m_modelFiles, m_modelPaths);
    sortPair(m_texFiles, m_texPaths);
}

std::string VegetationSystem::modelPath(const std::string& file) const {
    for (int i = 0; i < static_cast<int>(m_modelFiles.size()); ++i)
        if (m_modelFiles[i] == file) return m_modelPaths[i];
    return m_modelDir + "/" + file;
}

std::string VegetationSystem::texPath(const std::string& file) const {
    for (int i = 0; i < static_cast<int>(m_texFiles.size()); ++i)
        if (m_texFiles[i] == file) return m_texPaths[i];
    return m_texDir + "/" + file;
}

void VegetationSystem::refreshTreeAssets(const std::string& projectDir) {
    m_projectDir = projectDir;
    scanTreeAssets();
    // Re-resolve what each species already references: a name that used to fall
    // back to content may now live in the project (or the other way round).
    for (int s = 0; s < static_cast<int>(m_species.size()); ++s) {
        TreeSpecies& sp = m_species[s];
        for (TreeLOD& lod : sp.lods)
            if (!lod.model.empty()) loadTreeMesh(modelPath(lod.model), sp, lod);
        if (!sp.billboard.empty()) setBillboard(s, sp.billboard);
    }
}

namespace {
// One tree vertex as a hashable key: the eight floats exactly as they will be
// uploaded, compared bit for bit. See loadTreeMesh.
struct VertexKey {
    float v[8];
    bool operator==(const VertexKey& o) const {
        return std::memcmp(v, o.v, sizeof v) == 0;
    }
};
struct VertexKeyHash {
    std::size_t operator()(const VertexKey& k) const noexcept {
        std::size_t h = 1469598103934665603ull; // FNV-1a over the bit pattern
        std::uint32_t bits[8];
        std::memcpy(bits, k.v, sizeof bits);
        for (std::uint32_t b : bits) { h ^= b; h *= 1099511628211ull; }
        return h;
    }
};
} // namespace

bool VegetationSystem::loadTreeMesh(const std::string& path, TreeSpecies& sp, TreeLOD& lod) {
    lod.prims.clear();
    if (lod.vbo) { glDeleteBuffers(1, &lod.vbo); lod.vbo = 0; }
    if (lod.ibo) { glDeleteBuffers(1, &lod.ibo); lod.ibo = 0; }
    if (lod.vao) { glDeleteVertexArrays(1, &lod.vao); lod.vao = 0; }

    // Instanced mesh geometry normalized to unit height (instance scale = size),
    // split into per-material draw groups (combined layout: pos3 normal3 uv2).
    fitzel::ModelData md = fitzel::loadGltf(path);
    if (md.empty() || md.height() < 0.01f) {
        std::fprintf(stderr, "Tree model failed to load: %s\n", path.c_str());
        return false;
    }
    std::vector<float>         verts;
    std::vector<std::uint32_t> indices;
    // Vertex -> its slot in `verts`, so the second triangle to use a corner
    // reuses it instead of appending it again. Keyed on the eight floats' exact
    // bits: two corners that a modelling tool wrote as the same numbers ARE the
    // same corner, and two that differ in the last bit have different normals or
    // UVs and must stay apart -- welding those would visibly seam the leaves.
    std::unordered_map<VertexKey, std::uint32_t, VertexKeyHash> unique;
    const float scale = 1.0f / md.height();
    // The trunk goes on the origin. A model exported where it stood in its
    // artist's scene (tree2.glb sits nine of its own heights off its origin)
    // would otherwise be drawn that far from where the tree was planted --
    // in the river -- and the wind, which bends a tree by the distance from
    // its trunk axis, would fling the whole crown about. The foot of the
    // trunk is the centre of what lies within the lowest few percent.
    float baseX = 0.0f, baseZ = 0.0f;
    {
        const float cut = md.minY + 0.04f * md.height();
        double sx = 0.0, sz = 0.0;
        long long n = 0;
        float bx0 = 1e30f, bx1 = -1e30f, bz0 = 1e30f, bz1 = -1e30f;
        for (const fitzel::ModelPrimitive& p : md.primitives)
            for (std::size_t i = 0; i + 7 < p.vertices.size(); i += 8) {
                const float x = p.vertices[i], y = p.vertices[i + 1], z = p.vertices[i + 2];
                bx0 = std::min(bx0, x); bx1 = std::max(bx1, x);
                bz0 = std::min(bz0, z); bz1 = std::max(bz1, z);
                if (y <= cut) { sx += x; sz += z; ++n; }
            }
        baseX = n > 0 ? static_cast<float>(sx / n) : 0.5f * (bx0 + bx1);
        baseZ = n > 0 ? static_cast<float>(sz / n) : 0.5f * (bz0 + bz1);
    }
    // ...and the sphere that holds the normalized mesh, for the instance culling.
    float boundR2 = 0.0f;
    for (fitzel::ModelPrimitive& p : md.primitives) {
        TreeLOD::Prim tp;
        tp.first  = static_cast<int>(indices.size());
        tp.count  = p.vertexCount();
        tp.cutout = p.alphaCutout;
        tp.hasTex = !p.texPixels.empty();
        if (tp.hasTex)
            tp.tex = Texture::fromPixels(p.texPixels.data(), p.texWidth, p.texHeight, 4);
        for (std::size_t i = 0; i + 7 < p.vertices.size(); i += 8) {
            VertexKey key{};
            key.v[0] = (p.vertices[i + 0] - baseX) * scale;
            key.v[1] = (p.vertices[i + 1] - md.minY) * scale;
            key.v[2] = (p.vertices[i + 2] - baseZ) * scale;
            for (int k = 3; k < 8; ++k) key.v[k] = p.vertices[i + k];
            const auto it = unique.find(key);
            if (it != unique.end()) { indices.push_back(it->second); continue; }
            const std::uint32_t slot =
                static_cast<std::uint32_t>(verts.size() / 8);
            unique.emplace(key, slot);
            indices.push_back(slot);
            // Around the mesh's mid-height, which is where the instance sphere
            // is centred.
            const float dy = key.v[1] - 0.5f;
            boundR2 = std::max(boundR2,
                               key.v[0] * key.v[0] + dy * dy + key.v[2] * key.v[2]);
            verts.insert(verts.end(), key.v, key.v + 8);
        }
        lod.prims.push_back(std::move(tp));
    }
    lod.boundR = std::max(0.5f, std::sqrt(boundR2));
    glGenVertexArrays(1, &lod.vao);
    glBindVertexArray(lod.vao);
    glGenBuffers(1, &lod.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, lod.vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_STATIC_DRAW);
    // The element buffer belongs to the VAO, so it is bound here and never
    // again -- binding the VAO at draw time brings it along.
    glGenBuffers(1, &lod.ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, lod.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(indices.size() * sizeof(std::uint32_t)),
                 indices.data(), GL_STATIC_DRAW);
    const GLsizei ms = 8 * sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, ms, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, ms, (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, ms, (void*)(6 * sizeof(float)));
    bindTreeInstanceAttribs(sp.instVBO);
    glBindVertexArray(0);
    lod.cpuVerts = std::move(verts);
    lod.cpuIdx   = std::move(indices);
    sp.autoDirty = true;   // the generated levels derive from this
    sp.impDirty  = true;   // ...and so does the impostor
    return true;
}

void VegetationSystem::freeAutoLod(AutoLod& a) {
    if (a.vbo) glDeleteBuffers(1, &a.vbo);
    if (a.ibo) glDeleteBuffers(1, &a.ibo);
    if (a.vao) glDeleteVertexArrays(1, &a.vao);
    a = AutoLod{};
}

void VegetationSystem::buildAutoLods(TreeSpecies& sp) {
    sp.autoDirty = false;
    freeAutoLod(sp.mid);
    freeAutoLod(sp.shadow);
    if (sp.lods.empty()) return;
    const TreeLOD& src = sp.lods.back();
    if (src.cpuVerts.empty() || src.cpuIdx.empty()) return;
    const std::size_t nV = src.cpuVerts.size() / 8;

    // One level: solid prims collapsed to `barkRatio` of their triangles, leaf
    // prims thinned to `leafKeep` of their cards (grown to keep the area).
    const auto make = [&](AutoLod& out, float barkRatio, float leafKeep,
                          std::uint32_t seed) {
        std::vector<float>         v = src.cpuVerts;   // pruning moves leaf corners
        std::vector<std::uint32_t> ix;
        for (int p = 0; p < static_cast<int>(src.prims.size()); ++p) {
            const TreeLOD::Prim& pr = src.prims[p];
            std::vector<std::uint32_t> sub(src.cpuIdx.begin() + pr.first,
                                           src.cpuIdx.begin() + pr.first + pr.count);
            if (pr.cutout) {
                meshsimplify::pruneCards(v, 8, sub, leafKeep, seed + p * 7919u);
            } else {
                const std::size_t target = std::max<std::size_t>(
                    12, static_cast<std::size_t>(sub.size() / 3 * barkRatio));
                sub = meshsimplify::simplify(src.cpuVerts.data(), 8, nV, sub.data(),
                                             sub.size(), target);
            }
            if (sub.empty()) continue;
            AutoLod::Range r;
            r.first  = static_cast<int>(ix.size());
            r.count  = static_cast<int>(sub.size());
            r.prim   = p;
            r.cutout = pr.cutout;
            out.ranges.push_back(r);
            out.tris += static_cast<long long>(sub.size() / 3);
            ix.insert(ix.end(), sub.begin(), sub.end());
        }
        if (ix.empty()) return;
        glGenVertexArrays(1, &out.vao);
        glBindVertexArray(out.vao);
        glGenBuffers(1, &out.vbo);
        glBindBuffer(GL_ARRAY_BUFFER, out.vbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(v.size() * sizeof(float)),
                     v.data(), GL_STATIC_DRAW);
        glGenBuffers(1, &out.ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, out.ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(ix.size() * sizeof(std::uint32_t)),
                     ix.data(), GL_STATIC_DRAW);
        const GLsizei ms = 8 * sizeof(float);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, ms, (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, ms, (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, ms, (void*)(6 * sizeof(float)));
        bindTreeInstanceAttribs(sp.instVBO);
        glBindVertexArray(0);
    };
    const auto t0 = std::chrono::steady_clock::now();
    make(sp.mid,    0.16f, 0.5f,  0x5eedu);
    make(sp.shadow, 0.05f, 0.25f, 0xbeefu);
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();
    long long srcTris = static_cast<long long>(src.cpuIdx.size() / 3);
    std::fprintf(stderr, "tree LODs %s: %lld -> mid %lld, shadow %lld tris (%.0f ms)\n",
                 sp.name.c_str(), srcTris, sp.mid.tris, sp.shadow.tris, ms);
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
    // The impostor pass's own instances: the whole forest field, not the near
    // set the meshes draw.
    glGenBuffers(1, &sp.farVBO);
    glGenVertexArrays(1, &sp.farVAO);
    glBindVertexArray(sp.farVAO);
    bindTreeInstanceAttribs(sp.farVBO);
    glBindVertexArray(0);
    // Default LOD0 = first available model, so a fresh species is visible at once.
    TreeLOD lod;
    lod.dist = 45.0f;
    if (!m_modelFiles.empty()) {
        lod.model = m_modelFiles.front();
        loadTreeMesh(modelPath(lod.model), sp, lod);
    }
    sp.lods.push_back(std::move(lod));
    // Default billboard: the classic tree impostor if present, else the first PNG.
    std::string bb;
    for (const std::string& f : m_texFiles)
        if (f == "billboard_tree_bled.png") { bb = f; break; }
    if (bb.empty() && !m_texFiles.empty()) bb = m_texFiles.front();
    if (!bb.empty()) {
        sp.billboard = bb;
        sp.bbTex = Texture::fromFile(texPath(bb));
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
            if (lod.ibo) glDeleteBuffers(1, &lod.ibo);
        if (lod.vao) glDeleteVertexArrays(1, &lod.vao);
    }
    if (sp.instVBO) glDeleteBuffers(1, &sp.instVBO);
    if (sp.bbVAO)   glDeleteVertexArrays(1, &sp.bbVAO);
    if (sp.farVBO)  glDeleteBuffers(1, &sp.farVBO);
    if (sp.farVAO)  glDeleteVertexArrays(1, &sp.farVAO);
    if (sp.impAlbedo) glDeleteTextures(1, &sp.impAlbedo);
    if (sp.impNormal) glDeleteTextures(1, &sp.impNormal);
    freeAutoLod(sp.mid);
    freeAutoLod(sp.shadow);
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
    if (!lod.model.empty()) loadTreeMesh(modelPath(lod.model), sp, lod);
    sp.lods.push_back(std::move(lod));
}

void VegetationSystem::removeLOD(int s, int lod) {
    if (s < 0 || s >= static_cast<int>(m_species.size())) return;
    TreeSpecies& sp = m_species[s];
    if (lod < 0 || lod >= static_cast<int>(sp.lods.size())) return;
    TreeLOD& L = sp.lods[lod];
    if (L.vbo) glDeleteBuffers(1, &L.vbo);
    if (L.ibo) glDeleteBuffers(1, &L.ibo);
    if (L.vao) glDeleteVertexArrays(1, &L.vao);
    sp.lods.erase(sp.lods.begin() + lod);
    sp.autoDirty = true;   // the coarsest level may have changed
}

void VegetationSystem::setLODModel(int s, int lod, const std::string& file) {
    if (s < 0 || s >= static_cast<int>(m_species.size())) return;
    TreeSpecies& sp = m_species[s];
    if (lod < 0 || lod >= static_cast<int>(sp.lods.size())) return;
    sp.lods[lod].model = file;
    loadTreeMesh(modelPath(file), sp, sp.lods[lod]);
}

void VegetationSystem::setBillboard(int s, const std::string& file) {
    if (s < 0 || s >= static_cast<int>(m_species.size())) return;
    TreeSpecies& sp = m_species[s];
    sp.billboard = file;
    sp.bbTex = Texture::fromFile(texPath(file));
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
    // Optional: a failed impostor shader costs the distant forest, not the trees.
    m_impostor     = Shader::fromFiles("assets/shaders/impostor.vert",
                                       "assets/shaders/impostor.frag");
    m_impostorBake = Shader::fromFiles("assets/shaders/impostorbake.vert",
                                       "assets/shaders/impostorbake.frag");
    if (!m_impostor.isValid() || !m_impostorBake.isValid())
        std::fprintf(stderr, "Failed to load impostor shaders\n");
    m_treeMotion = Shader::fromFiles("assets/shaders/treemotion.vert",
                                     "assets/shaders/treemotion.frag");
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

// Stable per-cell seed from a lattice cell's integer coords. Same cell -> same
// seed -> same tree, so regrowing the forest never reshuffles what's on screen.
static std::uint32_t treeCellHash(int gx, int gz) {
    std::uint32_t h = static_cast<std::uint32_t>(gx) * 73856093u ^
                      static_cast<std::uint32_t>(gz) * 19349663u;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return h ? h : 1u;
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
        // World-aligned lattice: sample points sit at fixed world coords
        // (gx*spacing) and every random decision in a cell is seeded from its
        // integer coords -- so a cell always drops the SAME tree, regardless of
        // when or from where the forest regrows. Drifting the camera then only
        // adds/removes rim cells; the trees already on screen never move. (The old
        // code sampled a cc-relative grid with a running RNG, so every 25 m the
        // whole forest jumped to new positions.)
        const int gx0 = static_cast<int>(std::floor((cc.x - m_treeRadius) / spacing));
        const int gx1 = static_cast<int>(std::ceil ((cc.x + m_treeRadius) / spacing));
        const int gz0 = static_cast<int>(std::floor((cc.y - m_treeRadius) / spacing));
        const int gz1 = static_cast<int>(std::ceil ((cc.y + m_treeRadius) / spacing));
        const float roadClear = roadWidth * 0.5f + 3.0f; // keep trees clear
        const float R2 = m_treeRadius * m_treeRadius;
        for (int gz = gz0; gz <= gz1; ++gz) {
            for (int gx = gx0; gx <= gx1; ++gx) {
                const float wx = gx * spacing, wz = gz * spacing;
                const float dx = wx - cc.x, dz = wz - cc.y;
                if (dx * dx + dz * dz > R2) continue;
                if (roadDistanceSq(road, wx, wz) < roadClear * roadClear) continue;
                if (inDiscs(wet, wx, wz)) continue;
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
                std::mt19937 crng(treeCellHash(gx, gz)); // stable per-cell randomness
                if (u(crng) > prob) continue;
                // Pick a species weighted by its density.
                float r = u(crng) * totalDensity;
                int pick = active.back();
                for (int idx : active) { r -= m_species[idx].density;
                                         if (r <= 0.0f) { pick = idx; break; } }
                TreeSpecies& sp = m_species[pick];
                const float tx = wx + (u(crng) - 0.5f) * spacing;
                const float tz = wz + (u(crng) - 0.5f) * spacing;
                sp.inst.insert(sp.inst.end(), {
                    tx, m_streamer.heightAt(tx, tz) - 0.3f, tz,
                    u(crng) * 6.2831f,
                    glm::mix(sp.size * 0.75f, sp.size * 1.3f, u(crng))});
            }
        }
    }
    for (TreeSpecies& sp : m_species) sp.proceduralFloats = sp.inst.size();
    rebuildTreeBuffers();  // append the painted trees per species and upload
    treeCenter = cc;
}

void VegetationSystem::updateTrees(glm::vec2 camXZ, const std::vector<glm::vec2>& road,
                                   float roadWidth, float waterLevel, float snowLevel) {
    if (!terrainPresent) return; // nothing to plant on
    if (!eco.enabled) {
        if (treeEnabled && glm::length(camXZ - treeCenter) > 25.0f)
            regenTrees(camXZ, road, roadWidth, waterLevel, snowLevel);
        return;
    }
    if (!treeEnabled) return;

    // --- The forest field ------------------------------------------------------
    // Everything the placement reads, as one value: a change anywhere in it
    // regrows the field (TreeField::configure is a no-op when nothing moved).
    TreeField::Inputs in;
    in.terrain    = m_streamer.settings();
    in.eco        = eco;
    in.waterLevel = waterLevel;
    in.snowLevel  = snowLevel;
    in.road       = road;
    in.roadClear  = roadWidth * 0.5f + 3.0f;
    in.wet        = wet;
    in.wet.insert(in.wet.end(), treeClearings.begin(), treeClearings.end());
    for (const TreeSpecies& sp : m_species) {
        TreeField::Species s;
        s.density = (treeProcedural && sp.enabled && !sp.lods.empty()) ? sp.density : 0.0f;
        s.size    = sp.size;
        s.shrub   = sp.size < 4.0f;    // a bush, by what it is rather than a flag
        in.species.push_back(s);
    }
    m_treeField.radius = forestRadius;
    m_treeField.configure(in);
    // treeCenter reset to "far away" is how the host says the ground itself
    // changed (a sculpt, a road re-graded) -- which no input above records.
    if (treeCenter.x > 1e8f) {
        m_treeField.invalidate();
        treeCenter = camXZ;
    }
    const bool fieldChanged = m_treeField.update(camXZ);

    // The mesh set: the field's trees near enough to be meshes, plus every
    // painted one (rebuildTreeBuffers appends those). Regathered when the field
    // changed or the eye has walked far enough to bring new ones into range.
    if (fieldChanged || glm::length(camXZ - m_nearCenter) > 8.0f) {
        std::vector<std::vector<float>> near(m_species.size());
        m_treeField.gather(camXZ, impostorStart + 20.0f, near);
        for (std::size_t s = 0; s < m_species.size(); ++s) {
            m_species[s].inst = std::move(near[s]);
            m_species[s].proceduralFloats = m_species[s].inst.size();
        }
        rebuildTreeBuffers();
        m_nearCenter = camXZ;
    }
    // The impostors: the whole field, and the painted trees with it (a brush
    // stroke changes their count, which is all this needs to notice).
    if (fieldChanged || paintedTrees.size() != m_farPainted || treePainted != m_farPaintedOn)
        uploadFar();
}

void VegetationSystem::uploadFar() {
    m_farPainted   = paintedTrees.size();
    m_farPaintedOn = treePainted;
    std::vector<std::vector<float>> all(m_species.size());
    m_treeField.gather(glm::vec2(0.0f), -1.0f, all);
    for (std::size_t s = 0; s < m_species.size(); ++s) {
        TreeSpecies& sp = m_species[s];
        std::vector<float>& v = all[s];
        if (treePainted)
            for (std::size_t t = 0; t + 6 <= paintedTrees.size(); t += 6)
                if (static_cast<std::size_t>(std::lround(paintedTrees[t + 5])) == s)
                    v.insert(v.end(), paintedTrees.begin() + t, paintedTrees.begin() + t + 5);
        glBindBuffer(GL_ARRAY_BUFFER, sp.farVBO);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(v.size() * sizeof(float)),
                     v.data(), GL_DYNAMIC_DRAW);
        sp.farCount = static_cast<int>(v.size() / 5);
    }
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

int VegetationSystem::cullInstances(const TreeSpecies& sp, float boundR,
                                    const glm::mat4& viewProj, int planeCount,
                                    const glm::vec2& camXZ, float lodMin,
                                    float lodMax) {
    const std::array<glm::vec4, 6> planes = frustumPlanesOf(viewProj);
    m_visInst.clear();
    m_visInst.reserve(sp.inst.size());
    for (std::size_t i = 0; i + 5 <= sp.inst.size(); i += 5) {
        const float scale = sp.inst[i + 4];
        // The LOD band, decided here as well as in the vertex shader. The shader
        // still has to do it (a band edge must not depend on who asked), but a
        // tree rejected there has already been fetched and transformed -- which
        // for a mesh of this size is the entire cost. Rejecting it here is what
        // makes a LOD chain worth having.
        const float d = glm::length(glm::vec2(sp.inst[i], sp.inst[i + 2]) - camXZ);
        if (d < lodMin || d > lodMax) continue;
        const glm::vec3 centre(sp.inst[i], sp.inst[i + 1] + 0.5f * scale,
                               sp.inst[i + 2]);
        // A margin on top of the mesh's own radius: the crown sways in the wind,
        // and a tree that pops out at the edge of the screen is a worse bug than
        // the handful of instances this keeps.
        const float r = boundR * scale + 0.5f;
        if (!sphereVisible(planes, centre, r, planeCount)) continue;
        m_visInst.insert(m_visInst.end(), sp.inst.begin() + i,
                                          sp.inst.begin() + i + 5);
    }
    const int count = static_cast<int>(m_visInst.size() / 5);
    m_drawnInstances += count;
    if (count == 0) return 0;
    if (!m_cullVBO) glGenBuffers(1, &m_cullVBO);
    glBindBuffer(GL_ARRAY_BUFFER, m_cullVBO);
    // Orphan first: the buffer was read by the pass before this one, and
    // overwriting it without saying so makes the driver wait for that draw.
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(m_visInst.size() * sizeof(float)),
                 nullptr, GL_STREAM_DRAW);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(m_visInst.size() * sizeof(float)),
                 m_visInst.data(), GL_STREAM_DRAW);
    return count;
}

void VegetationSystem::drawTreeShadow(const glm::mat4& lightSpace, double time,
                                      float weather, glm::vec2 camXZ,
                                      float maxDist) {
    if (!terrainPresent || !treeEnabled || treeCount == 0) return;
    const float shadowDistance = maxDist > 0.0f ? maxDist : 1e9f;
    // Backfaces are dropped here even though the lit pass keeps them. A leaf
    // card is two quads' worth of fragments and only the near one decides the
    // depth the sun sees; the far one writes the same shadow a hair later and is
    // discarded by the depth test anyway. With an alpha-cutout shader that costs
    // a texture fetch and a discard per fragment -- which is the pass, not a
    // detail of it. Trunks are closed, so nothing that was solid becomes hollow.
    // Saved and put back: this runs INSIDE the renderer's cascade loop, between
    // that pass's own draws, and the cascade pass inherits a cull face from
    // whoever ran last (the point-shadow pass leaves GL_FRONT behind). Leaving
    // ours set would change how the terrain self-shadows -- an acne bug two
    // files away from anything about trees.
    const GLboolean prevCull = glIsEnabled(GL_CULL_FACE);
    GLint prevFace = GL_BACK;
    glGetIntegerv(GL_CULL_FACE_MODE, &prevFace);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    m_treeDepth.bind();
    m_treeDepth.setMat4("uLightSpace", lightSpace);
    wind::apply(m_treeDepth, wind, 1.0f);   // the same air as the lit pass
    m_treeDepth.setFloat("uTreeHeight", 1.0f); // meshes normalized to unit height
    m_treeDepth.setInt("uTex", 0);
    // Shadows use LOD0 -- but only the instances THIS cascade can see, and only
    // those within treeShadowDistance of the camera. A cascade is a slice of the
    // view, and drawing the whole forest into each of them was the same trees
    // five times over: it is the single most expensive thing a dense forest did.
    for (TreeSpecies& sp : m_species) {
        if (!sp.enabled || sp.count == 0 || sp.lods.empty()) continue;
        if (sp.autoDirty) buildAutoLods(sp);
        // The COARSEST level, not the finest. A shadow is a silhouette: the leaf
        // a lower LOD dropped is worth a texel of the map at best, and this pass
        // draws more tree geometry than the visible frame does -- so it draws the
        // generated shadow level (a twentieth of the bark, a quarter of the
        // leaf cards, grown to cast the same shade) when there is one.
        const TreeLOD& lod = sp.lods.back();
        // Four planes, not six: a tree standing between the sun and the slice is
        // outside the box and still casts into it (see sphereVisible).
        const int vis = cullInstances(sp, lod.boundR, lightSpace, 4,
                                      camXZ, 0.0f, shadowDistance);
        if (vis == 0) continue;
        m_shadowInst += vis;
        if (sp.shadow.valid()) {
            glBindVertexArray(sp.shadow.vao);
            bindTreeInstanceAttribs(m_cullVBO);
            for (const AutoLod::Range& r : sp.shadow.ranges) {
                const TreeLOD::Prim& tp = lod.prims[r.prim];
                if (tp.hasTex) tp.tex.bind(0);
                m_treeDepth.setInt("uAlphaCutout", r.cutout ? 1 : 0);
                glDrawElementsInstanced(
                    GL_TRIANGLES, r.count, GL_UNSIGNED_INT,
                    reinterpret_cast<const void*>(
                        static_cast<std::uintptr_t>(r.first) * sizeof(std::uint32_t)),
                    vis);
                m_shadowTris += static_cast<long long>(r.count / 3) * vis;
            }
            continue;
        }
        glBindVertexArray(lod.vao);
        bindTreeInstanceAttribs(m_cullVBO);
        for (const TreeLOD::Prim& tp : lod.prims) {
            if (tp.hasTex) tp.tex.bind(0);
            m_treeDepth.setInt("uAlphaCutout", tp.cutout ? 1 : 0);
            glDrawElementsInstanced(
                GL_TRIANGLES, tp.count, GL_UNSIGNED_INT,
                reinterpret_cast<const void*>(
                    static_cast<std::uintptr_t>(tp.first) * sizeof(std::uint32_t)),
                vis);
            m_shadowTris += static_cast<long long>(tp.count / 3) * vis;
        }
    }
    glBindVertexArray(0);
    glCullFace(prevFace);
    if (!prevCull) glDisable(GL_CULL_FACE);
}

void VegetationSystem::drawTrees(const FrameContext& c) {
    if (!terrainPresent || !treeEnabled || treeCount == 0) return;
    glDisable(GL_CULL_FACE);
    m_tree.bind();
    m_tree.setMat4("uViewProj", c.viewProj);
    wind::apply(m_tree, wind, 1.0f);
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
    applySunShadows(m_tree, c);   // receives the sun's shadow (sunshadow.glsl)
    m_tree.setFloat("uBrightness", treeBrightness);
    m_tree.setFloat("uContrast", treeContrast);
    m_tree.setFloat("uHue", glm::radians(treeHue));
    m_tree.setInt("uTex", 0);
    for (TreeSpecies& sp : m_species) {
        if (!sp.enabled || sp.count == 0) continue;
        if (sp.autoDirty) buildAutoLods(sp);
        const int nl = static_cast<int>(sp.lods.size());
        // Past the one authored level, the generated one: an author who gave a
        // single mesh gets it up close and a sixth of it beyond its distance,
        // which is where the silhouette is all that is left to see.
        const bool useMid = (nl == 1) && sp.mid.valid();
        // With the forest field the meshes hand over to the impostors (the
        // same trees -- see drawImpostors), dissolving across the last 15 m.
        const bool imp = eco.enabled && m_impostor.isValid() && sp.impAlbedo != 0;
        const float farEnd = imp ? impostorStart : (sp.bbEnabled ? sp.bbStart : 1e9f);
        m_tree.setFloat("uHandover", impostorStart);
        m_tree.setFloat("uHandoverWidth", imp ? 15.0f : 0.0f);
        for (int k = 0; k < nl; ++k) {
            const TreeLOD& lod = sp.lods[k];
            const float lo = (k == 0) ? 0.0f : sp.lods[k - 1].dist;
            // The last mesh LOD runs out to the billboard start (or the far plane
            // when the species has no billboard).
            float hi = (k + 1 == nl) ? farEnd : lod.dist;
            if (useMid) hi = std::min(lod.dist, farEnd);
            hi = std::max(hi, lo);
            m_tree.setFloat("uLodMin", lo);
            m_tree.setFloat("uLodNear", hi);
            // Only the trees this view can see, and only the ones this LOD is
            // responsible for. Per pass, because the water reflection asks with
            // a different matrix (see cullInstances).
            const int vis = cullInstances(sp, lod.boundR, c.viewProj, 6,
                                          glm::vec2(c.camPos.x, c.camPos.z), lo, hi);
            if (vis == 0) continue;
            glBindVertexArray(lod.vao);
            bindTreeInstanceAttribs(m_cullVBO);
            for (const TreeLOD::Prim& tp : lod.prims) {
                if (tp.hasTex) tp.tex.bind(0);
                m_tree.setInt("uAlphaCutout", tp.cutout ? 1 : 0);
                glDrawElementsInstanced(
                    GL_TRIANGLES, tp.count, GL_UNSIGNED_INT,
                    reinterpret_cast<const void*>(
                        static_cast<std::uintptr_t>(tp.first) * sizeof(std::uint32_t)),
                    vis);
            }
        }
        if (useMid) {
            const TreeLOD& src = sp.lods.back();
            const float lo = std::min(src.dist, farEnd);
            if (farEnd > lo) {
                m_tree.setFloat("uLodMin", lo);
                m_tree.setFloat("uLodNear", farEnd);
                const int vis = cullInstances(sp, src.boundR, c.viewProj, 6,
                                              glm::vec2(c.camPos.x, c.camPos.z), lo,
                                              farEnd);
                if (vis > 0) {
                    glBindVertexArray(sp.mid.vao);
                    bindTreeInstanceAttribs(m_cullVBO);
                    for (const AutoLod::Range& r : sp.mid.ranges) {
                        const TreeLOD::Prim& tp = src.prims[r.prim];
                        if (tp.hasTex) tp.tex.bind(0);
                        m_tree.setInt("uAlphaCutout", r.cutout ? 1 : 0);
                        glDrawElementsInstanced(
                            GL_TRIANGLES, r.count, GL_UNSIGNED_INT,
                            reinterpret_cast<const void*>(
                                static_cast<std::uintptr_t>(r.first) * sizeof(std::uint32_t)),
                            vis);
                    }
                }
            }
        }
    }
    glBindVertexArray(0);
    glEnable(GL_CULL_FACE);
}

void VegetationSystem::drawTreeMotion(const glm::mat4& viewProj, const glm::mat4& curVP,
                                      const glm::mat4& prevVP, const glm::vec3& camPos) {
    const float prevT = (m_prevWindTime < 0.0f) ? wind.time : m_prevWindTime;
    m_prevWindTime = wind.time;
    if (!terrainPresent || !treeEnabled || treeCount == 0 || !m_treeMotion.isValid()) return;
    // Depth-tested against the finished opaque scene, like Renderer::renderMotion.
    GLint prevFunc = GL_LESS;
    glGetIntegerv(GL_DEPTH_FUNC, &prevFunc);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    m_treeMotion.bind();
    m_treeMotion.setMat4("uViewProj", viewProj);
    m_treeMotion.setMat4("uCurVP", curVP);
    m_treeMotion.setMat4("uPrevVP", prevVP);
    m_treeMotion.setFloat("uPrevWindTime", prevT);
    m_treeMotion.setFloat("uTreeHeight", 1.0f);
    m_treeMotion.setVec3("uCamPos", camPos);
    m_treeMotion.setInt("uTex", 0);
    wind::apply(m_treeMotion, wind, 1.0f);
    const glm::vec2 camXZ(camPos.x, camPos.z);
    // The same bands drawTrees drew, so every lit fragment finds its vector.
    const auto drawRanges = [&](std::uint32_t vao, const TreeLOD& texLod,
                                const auto& ranges) {
        glBindVertexArray(vao);
        bindTreeInstanceAttribs(m_cullVBO);
        for (const auto& r : ranges) {
            const TreeLOD::Prim& tp = texLod.prims[r.prim];
            if (tp.hasTex) tp.tex.bind(0);
            m_treeMotion.setInt("uAlphaCutout", r.cutout ? 1 : 0);
            glDrawElementsInstanced(GL_TRIANGLES, r.count, GL_UNSIGNED_INT,
                reinterpret_cast<const void*>(static_cast<std::uintptr_t>(r.first) *
                                              sizeof(std::uint32_t)),
                r.vis);
        }
    };
    struct R { int first, count, prim; bool cutout; int vis; };
    for (TreeSpecies& sp : m_species) {
        if (!sp.enabled || sp.count == 0 || sp.lods.empty()) continue;
        const int nl = static_cast<int>(sp.lods.size());
        const bool useMid = (nl == 1) && sp.mid.valid();
        const bool imp = eco.enabled && m_impostor.isValid() && sp.impAlbedo != 0;
        const float farEnd = imp ? impostorStart : (sp.bbEnabled ? sp.bbStart : 1e9f);
        for (int k = 0; k < nl; ++k) {
            const TreeLOD& lod = sp.lods[k];
            const float lo = (k == 0) ? 0.0f : sp.lods[k - 1].dist;
            float hi = (k + 1 == nl) ? farEnd : lod.dist;
            if (useMid) hi = std::min(lod.dist, farEnd);
            hi = std::max(hi, lo);
            m_treeMotion.setFloat("uLodMin", lo);
            m_treeMotion.setFloat("uLodNear", hi);
            const int vis = cullInstances(sp, lod.boundR, viewProj, 6, camXZ, lo, hi);
            if (vis == 0) continue;
            std::vector<R> rs;
            for (int p = 0; p < static_cast<int>(lod.prims.size()); ++p)
                rs.push_back({lod.prims[p].first, lod.prims[p].count, p,
                              lod.prims[p].cutout, vis});
            drawRanges(lod.vao, lod, rs);
        }
        if (useMid) {
            const TreeLOD& src = sp.lods.back();
            const float lo = std::min(src.dist, farEnd);
            if (farEnd <= lo) continue;
            m_treeMotion.setFloat("uLodMin", lo);
            m_treeMotion.setFloat("uLodNear", farEnd);
            const int vis = cullInstances(sp, src.boundR, viewProj, 6, camXZ, lo, farEnd);
            if (vis == 0) continue;
            std::vector<R> rs;
            for (const AutoLod::Range& r : sp.mid.ranges)
                rs.push_back({r.first, r.count, r.prim, r.cutout, vis});
            drawRanges(sp.mid.vao, src, rs);
        }
    }
    glBindVertexArray(0);
    glEnable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glDepthFunc(static_cast<GLenum>(prevFunc));
}

void VegetationSystem::drawTreeBillboards(const FrameContext& c,
                                          const glm::vec3& camRight) {
    if (eco.enabled) { drawImpostors(c); return; }
    if (!terrainPresent || !treeEnabled || treeCount == 0) return;
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
    applySunShadows(m_billboard, c);   // receives the sun's shadow (sunshadow.glsl)
    m_billboard.setFloat("uBrightness", treeBrightness);
    m_billboard.setFloat("uContrast", treeContrast);
    m_billboard.setFloat("uHue", glm::radians(treeHue));
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
    ImGui::TextDisabled("%d species  -  %d in range",
                        static_cast<int>(m_species.size()), treeCount);
    // What the frame actually submitted, summed over the main view, the water
    // reflection and every shadow cascade. This is the number that decides the
    // cost -- a forest standing in four cascades is drawn five times over -- and
    // it is invisible everywhere else in the editor.
    if (treeEnabled && treeCount > 0) {
        int tris = 0;
        for (const TreeSpecies& sp : m_species)
            if (!sp.lods.empty())
                for (const TreeLOD::Prim& tp : sp.lods.front().prims)
                    tris = std::max(tris, (tp.first + tp.count) / 3);
        ImGui::TextDisabled("%d instances submitted last frame (%d k triangles each)",
                            m_drawnLast, tris / 1000);
        if (tris > 20000)
            ui::hint("That mesh is %d triangles, and every instance pays all of "
                     "them in every pass it survives. At this size a billboard "
                     "past 60 m and one coarse LOD are worth more than every "
                     "other setting in this panel put together.", tris);
    }

    // The one setting in this panel that costs nothing to look at and everything
    // to leave alone: the cascades cover the whole streamed terrain, so a forest
    // with no limit here is redrawn once per cascade out to the horizon.
    ImGui::BeginDisabled(!treeEnabled);
    ImGui::SliderFloat("Shadow distance", &treeShadowDistance, 0.0f, 600.0f,
                       treeShadowDistance <= 0.0f ? "no limit" : "%.0f m");
    ui::hint("How far a tree still casts a sun shadow. Past it the tree is still "
             "drawn -- it simply stops being redrawn into every cascade, which "
             "at this mesh size is most of the frame. 0 means no limit.");
    ImGui::TextDisabled("%d instances into the cascades last frame (%lld k triangles)",
                        m_shadowInstLast,
                        static_cast<long long>(m_shadowTrisLast / 1000));
    ImGui::EndDisabled();

    // Source toggles: use generated and/or painted trees, in any combination.
    ImGui::BeginDisabled(!treeEnabled);
    if (ImGui::Checkbox("Generated", &treeProcedural))
        treeCenter = glm::vec2(1e9f); // regen the forest with the new setting
    ImGui::SameLine();
    if (ImGui::Checkbox("Painted##src", &treePainted))
        rebuildTreeBuffers();         // just add/drop the painted instances
    ImGui::EndDisabled();

    // === Colour correction (applies to every species' mesh + billboard) =====
    if (ui::header("Color correction")) {
        ImGui::Indent();
        ImGui::SliderFloat("Brightness", &treeBrightness, 0.0f, 2.0f);
        ImGui::SliderFloat("Contrast",   &treeContrast,   0.0f, 2.0f);
        ImGui::SliderFloat("Hue",        &treeHue,     -180.0f, 180.0f, "%.0f deg");
        if (ImGui::SmallButton("Reset##treecc")) {
            treeBrightness = 1.0f; treeContrast = 1.0f; treeHue = 0.0f;
        }
        ImGui::Unindent();
    }

    static int sel = 0;
    sel = glm::clamp(sel, 0, std::max(0, static_cast<int>(m_species.size()) - 1));

    // === Species overview (foldable) =======================================
    if (ui::header("Species", ImGuiTreeNodeFlags_DefaultOpen)) {
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
        ImGui::SameLine();
        // Assets are scanned on project switch; this picks up files dropped into
        // the project folder while it is open, without reopening it.
        if (ImGui::Button("Rescan assets")) refreshTreeAssets(m_projectDir);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Re-read models/billboards from content + the open project");
    }

    // === Selected-species editor (foldable) ================================
    if (sel >= 0 && sel < static_cast<int>(m_species.size())) {
        TreeSpecies& sp = m_species[sel];
        // "###edit" keeps a stable id so the fold state survives renames/selection.
        char hdr[96];
        std::snprintf(hdr, sizeof hdr, "Edit: %s###editSpecies", sp.name.c_str());
        if (ui::header(hdr, ImGuiTreeNodeFlags_DefaultOpen)) {
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
    if (ui::header("Paint trees (3D brush)")) {
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
    j["treeBrightness"] = treeBrightness;
    j["treeContrast"]   = treeContrast;
    j["treeHue"]        = treeHue;
    j["treeShadowDist"] = treeShadowDistance;
}

void VegetationSystem::deserializeTrees(const nlohmann::json& j) {
    if (!j.contains("trees") || !j["trees"].is_array()) return; // keep the default species
    // Free the existing species' GL resources.
    for (TreeSpecies& sp : m_species) {
        for (TreeLOD& lod : sp.lods) {
            if (lod.vbo) glDeleteBuffers(1, &lod.vbo);
            if (lod.ibo) glDeleteBuffers(1, &lod.ibo);
            if (lod.vao) glDeleteVertexArrays(1, &lod.vao);
        }
        if (sp.instVBO) glDeleteBuffers(1, &sp.instVBO);
        if (sp.bbVAO)   glDeleteVertexArrays(1, &sp.bbVAO);
    }
    m_species.clear();
    treeEnabled    = j.value("treeEnabled", true);
    treeProcedural = j.value("treeProcedural", true);
    treePainted    = j.value("treePainted", true);
    treeBrightness = j.value("treeBrightness", 1.0f);
    treeContrast   = j.value("treeContrast", 1.0f);
    treeHue        = j.value("treeHue", 0.0f);
    treeShadowDistance = j.value("treeShadowDist", 120.0f);
    for (const auto& sj : j["trees"]) {
        const int s = addSpecies();           // mints instVBO/bbVAO + a default LOD
        TreeSpecies& sp = m_species[s];
        for (TreeLOD& lod : sp.lods) {         // drop the default LOD; load from JSON
            if (lod.vbo) glDeleteBuffers(1, &lod.vbo);
            if (lod.ibo) glDeleteBuffers(1, &lod.ibo);
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
                if (!lod.model.empty()) loadTreeMesh(modelPath(lod.model), sp, lod);
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
            sp.bbTex = Texture::fromFile(texPath(bb)); // keep the saved aspect
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
    // base: pos3, normal3, tint, petalIndex ; instance: iPos3, iYaw, iScale, iColor3.
    const std::vector<float> mesh = makeFlowerMesh();
    m_flowerVerts = static_cast<int>(mesh.size() / 8);
    m_flowerField = InstancedMesh::create(
        mesh.data(), mesh.size(), 8 * sizeof(float),
        {{0, 3, 0}, {1, 3, 3 * sizeof(float)}, {2, 1, 6 * sizeof(float)},
         {7, 1, 7 * sizeof(float)}},
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
    // Where the blooms sit (the stem top, half a flower's scale up), for the
    // butterflies to visit.
    m_flowerHeads.clear();
    for (std::size_t i = 0; i + 8 <= m_flowerInst.size(); i += 8)
        m_flowerHeads.push_back({m_flowerInst[i], m_flowerInst[i + 1] + 0.5f * m_flowerInst[i + 4],
                                 m_flowerInst[i + 2]});
}

// Weighted palette pick plus a small per-bloom colour jitter, so no two flowers
// come out exactly the same shade. Shared by the procedural pass and the brush.
static glm::vec3 flowerColor(std::mt19937& rng) {
    // Natural meadow palette, weighted toward buttercup yellow and white.
    static const glm::vec3 palette[5] = {{0.96f, 0.78f, 0.12f},  // buttercup yellow
                                         {0.94f, 0.55f, 0.12f},  // warm orange
                                         {0.95f, 0.95f, 0.88f},  // daisy white
                                         {0.86f, 0.46f, 0.55f},  // soft pink
                                         {0.60f, 0.55f, 0.82f}}; // pale lavender
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    const float cr = u(rng);
    const int   ci = cr < 0.42f ? 0 : cr < 0.60f ? 1 : cr < 0.82f ? 2
                   : cr < 0.92f ? 3 : 4;
    glm::vec3 col = palette[ci] * glm::mix(0.86f, 1.10f, u(rng)); // brightness
    col.r *= glm::mix(0.93f, 1.07f, u(rng));                      // slight hue drift
    col.b *= glm::mix(0.90f, 1.10f, u(rng));
    return glm::clamp(col, glm::vec3(0.0f), glm::vec3(1.0f));
}

// Procedural bloom placement over a disc around `c`. Pure + thread-safe (reads
// only its by-value inputs incl. a snapshot of the tree positions), so it runs
// on a worker off the render thread. Returns the procedural flower floats.
//
// Like the forest, the samples sit on a WORLD-aligned lattice and every random
// decision in a cell is seeded from its integer coords -- so a cell always grows
// the same bloom at the same spot no matter where the pass was centred. (The old
// pass swept a c-relative grid with one running RNG, so every camera-follow
// regen re-placed the whole field and the flowers appeared to jump around.)
static std::vector<float> computeFlowers(
    fitzel::TerrainSettings s, glm::vec2 c, std::vector<glm::vec2> road,
    float roadWidth, float waterLevel, float snowLevel, float R, float flowerDensity,
    std::vector<float> treeInst) {
    std::vector<float> out;
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    const float spacing = 0.9f;
    const float clear = roadWidth * 0.5f + 1.5f;
    const int treeCount = static_cast<int>(treeInst.size() / 5);
    const int gx0 = static_cast<int>(std::floor((c.x - R) / spacing));
    const int gx1 = static_cast<int>(std::ceil ((c.x + R) / spacing));
    const int gz0 = static_cast<int>(std::floor((c.y - R) / spacing));
    const int gz1 = static_cast<int>(std::ceil ((c.y + R) / spacing));
    for (int gz = gz0; gz <= gz1; ++gz) {
        for (int gx = gx0; gx <= gx1; ++gx) {
            const float wx = gx * spacing, wz = gz * spacing;
            const float dx = wx - c.x, dz = wz - c.y;
            if (dx * dx + dz * dz > R * R) continue;
            if (roadDistanceSq(road, wx, wz) < clear * clear) continue;
            const float h = terrainHeight(s, wx, wz);
            if (h < waterLevel + 0.6f || h > snowLevel - 2.0f) continue;
            const float e = 1.0f;
            const glm::vec3 n = glm::normalize(glm::vec3(
                terrainHeight(s, wx - e, wz) - terrainHeight(s, wx + e, wz), 2.0f * e,
                terrainHeight(s, wx, wz - e) - terrainHeight(s, wx, wz + e)));
            if (n.y < 0.9f) continue;
            const float moist = terrainMoisture(s, wx, wz);
            if (moist < 0.3f) continue; // flowers want greener ground

            // Clumps where a mid-frequency noise peaks; a small background chance
            // sprinkles lone flowers between the groups.
            const float clump  = valNoise2(wx * 0.16f + 50.0f, wz * 0.16f + 50.0f);
            const float groupP = glm::smoothstep(0.66f, 0.9f, clump);

            // Flowers gather in the shade around tree trunks.
            float treeP = 0.0f;
            for (int t = 0; t < treeCount; ++t) {
                const float tdx = wx - treeInst[t * 5 + 0];
                const float tdz = wz - treeInst[t * 5 + 2];
                const float dd = tdx * tdx + tdz * tdz;
                if (dd < 30.0f) treeP = std::max(treeP, glm::smoothstep(30.0f, 3.0f, dd));
            }

            const float prob = (0.02f + groupP * 0.9f + treeP * 0.75f)
                             * glm::smoothstep(0.3f, 0.7f, moist) * flowerDensity;
            std::mt19937 crng(treeCellHash(gx, gz) ^ 0x510E5Bu); // stable per cell
            if (u(crng) > prob) continue;
            const float fx = wx + (u(crng) - 0.5f) * spacing;
            const float fz = wz + (u(crng) - 0.5f) * spacing;
            const glm::vec3 col = flowerColor(crng);
            // Meadow flowers are small; squared roll keeps most of them tiny.
            const float sr = u(crng);
            const float scale = glm::mix(0.30f, 0.60f, sr * sr);
            out.insert(out.end(), {fx, terrainHeight(s, fx, fz) - 0.02f, fz,
                                   u(crng) * 6.2831f, scale,
                                   col.r, col.g, col.b});
        }
    }
    return out;
}

void VegetationSystem::regenFlowers(glm::vec2 c, const std::vector<glm::vec2>& road,
                                    float roadWidth, float waterLevel, float snowLevel) {
    if (m_flowerPending) return; // one regen at a time; the next drift retriggers
    m_flowerPending = true;
    m_flowerFuture = std::async(std::launch::async, &computeFlowers,
                                m_streamer.settings(), c, road, roadWidth, waterLevel,
                                snowLevel, grassRadius, flowerDensity, m_treeInst);
}

void VegetationSystem::updateFlowers() {
    if (!m_flowerPending || !m_flowerFuture.valid() ||
        m_flowerFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    m_flowerInst = m_flowerFuture.get();
    m_proceduralFlowerFloats = m_flowerInst.size();
    m_flowerPending = false;
    rebuildFlowerBuffer(); // append the painted flowers and upload
}

void VegetationSystem::stampFlower(glm::vec2 c, float radius, std::mt19937& rng,
                                   float waterLevel, float snowLevel) {
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    const float area  = 3.14159265f * radius * radius;
    const int   tries = std::max(2, static_cast<int>(area * 0.7f * flowerBrushDensity));
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
        const glm::vec3 col = flowerColor(rng);
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

void VegetationSystem::drawFlowers(const FrameContext& c) {
    if (!terrainPresent || !flowerEnabled || flowerCount <= 0) return;
    glDisable(GL_CULL_FACE);
    m_flower.bind();
    m_flower.setMat4("uViewProj", c.viewProj);
    wind::apply(m_flower, wind, 0.5f);
    m_flower.setVec3("uViewPos", c.camPos);
    m_flower.setVec3("uLightDir", c.lightDir);
    m_flower.setVec3("uLightColor", c.lightColor);
    m_flower.setVec3("uAmbient", c.ambient);
    m_flower.setVec3("uFogColor", c.fogColor);
    m_flower.setVec3("uFogSunColor", c.fogSunColor);
    m_flower.setFloat("uFogDensity", c.fogDensity);
    m_flower.setFloat("uFogHeightFalloff", c.fogHeightFalloff);
    m_flower.setFloat("uFogHeight", c.fogHeight);
    applySunShadows(m_flower, c);   // receives the sun's shadow (sunshadow.glsl)
    m_flowerField.draw(GL_TRIANGLES, m_flowerVerts);
    glEnable(GL_CULL_FACE);
}
