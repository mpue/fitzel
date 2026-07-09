#pragma once

#include <cstdint>
#include <future>
#include <random>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/graphics/InstancedMesh.hpp>
#include <fitzel/graphics/Shader.hpp>
#include <fitzel/world/Terrain.hpp>

namespace fitzel { class Camera; }

// Per-frame lighting/fog the vegetation shaders need so grass/flowers match the
// terrain shading. Filled by the render loop and passed to the draw methods.
struct VegDrawContext {
    glm::mat4 viewProj{1.0f};
    glm::vec3 camPos{0.0f};
    double    time    = 0.0;
    float     weather = 0.0f;
    glm::vec3 lightDir{0.0f}, lightColor{0.0f}, ambient{0.0f};
    glm::vec3 fogColor{0.0f}, fogSunColor{0.0f};
    float     fogDensity = 0.0f, fogHeightFalloff = 0.0f, fogHeight = 0.0f;
};

// Owns the scene's vegetation/wildlife and its GPU resources. Migrated out of
// main.cpp one subsystem at a time; currently grass, birds and fireflies. Draw
// methods render into the caller's currently-bound framebuffer.
class VegetationSystem {
public:
    VegetationSystem(fitzel::TerrainStreamer& streamer, fitzel::Camera& camera);

    // Load shaders + build instanced meshes. Needs a live GL context; returns
    // false if a shader failed to compile.
    bool init();

    // --- Grass ---------------------------------------------------------------
    // Hand-painted blades: stamp scatters into the brush disc (dropped on the
    // terrain, rejecting steep/underwater/above-snow spots), erase removes those
    // in the disc. `rng` is shared with the other brushes (kept in main).
    void stampGrass(glm::vec2 c, float radius, std::mt19937& rng, float brushDensity,
                    float waterLevel, float snowLevel);
    void eraseGrass(glm::vec2 c, float radius);
    // Async pump: kick off / finish the procedural field regen around camXZ and
    // push any painted-blade edits. Returns true the frame the procedural field
    // just (re)generated -- grassCenter() then holds the center it used, so the
    // caller can regrow flowers to match.
    bool updateGrass(glm::vec2 camXZ, const std::vector<glm::vec2>& road,
                     float roadClear, float waterLevel, float snowLevel);
    void drawGrass(const VegDrawContext& ctx);
    glm::vec2 grassCenter() const { return m_grassCenter; }

    // --- Birds + fireflies ---------------------------------------------------
    void drawBirds(const glm::mat4& viewProj, double time, const glm::vec3& camPos);
    void drawFireflies(const glm::mat4& viewProj, double time, float night,
                       const glm::vec3& camPos);
    void panelBirdsFireflies();

    // Tunables the editor panel / scene setup drive directly.
    bool  grassEnabled = true;
    float grassHeight  = 0.35f;
    float grassDensity = 1.0f;
    float grassRadius  = 46.0f;
    glm::vec3 grassTint{1.0f, 1.0f, 1.0f};
    int   grassCount   = 0;
    bool  grassDirty   = true;         // request a (re)grow of the procedural field
    std::vector<float> paintedBlades;  // 7 floats/blade, world space (saved)
    bool  paintedDirty = false;        // GPU re-upload of painted blades pending

    bool  birdsEnabled   = true;
    bool  fireflyEnabled = true;

private:
    static std::vector<float> computeGrass(
        fitzel::TerrainSettings s, glm::vec2 c, float waterLvl, float snowLvl,
        float gHeight, float gDensity, float R, std::uint32_t seed,
        std::vector<glm::vec2> road, float roadClear);

    fitzel::TerrainStreamer& m_streamer;
    fitzel::Camera&          m_camera;

    // Grass.
    fitzel::Shader        m_grass;
    fitzel::InstancedMesh m_grassField, m_paintedGrass;
    glm::vec2                       m_grassCenter{1e9f}; // forces the first regen
    std::future<std::vector<float>> m_grassFuture;
    bool          m_grassPending = false;
    glm::vec2     m_grassPendingCenter{0.0f};
    std::uint32_t m_grassSeed = 1234u;

    // Birds.
    fitzel::Shader        m_bird;
    fitzel::InstancedMesh m_birdField;
    int   m_birdCount = 18;
    float m_birdSize  = 2.2f;

    // Fireflies.
    fitzel::Shader        m_firefly;
    fitzel::InstancedMesh m_fireflyField;
    int         m_fireflyCount  = 70;
    float       m_fireflySize   = 0.09f;
    const float m_fireflyRadius = 34.0f;
    std::mt19937                          m_flyRng{9001u};
    std::uniform_real_distribution<float> m_flyU{0.0f, 1.0f};
    std::vector<glm::vec3>                m_fireflies; // home xz + blink phase
};
