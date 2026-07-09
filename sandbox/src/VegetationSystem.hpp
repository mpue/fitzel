#pragma once

#include <random>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/graphics/InstancedMesh.hpp>
#include <fitzel/graphics/Shader.hpp>

namespace fitzel { class TerrainStreamer; class Camera; }

// Owns the scene's ambient vegetation/wildlife and its GPU resources. Currently
// covers birds + fireflies; the rest of the vegetation (grass, trees, flowers)
// is being migrated here out of main.cpp step by step. Draw methods render into
// the caller's currently-bound framebuffer.
class VegetationSystem {
public:
    VegetationSystem(fitzel::TerrainStreamer& streamer, fitzel::Camera& camera);

    // Load shaders + build the instanced meshes. Needs a live GL context; returns
    // false if a shader failed to compile.
    bool init();

    // Birds: a flock wheeling above the camera, drawn two-sided. Fireflies:
    // additive, night-only glowing wanderers. Both draw into the bound (HDR)
    // buffer; `camPos` is the render eye, `night` is 1-daylight (0..1).
    void drawBirds(const glm::mat4& viewProj, double time, const glm::vec3& camPos);
    void drawFireflies(const glm::mat4& viewProj, double time, float night,
                       const glm::vec3& camPos);

    // The "Birds" + "Fireflies" sections of the Vegetation panel.
    void panelBirdsFireflies();

    // Scene setup flips these on/off (Nature vs Empty).
    bool birdsEnabled   = true;
    bool fireflyEnabled = true;

private:
    fitzel::TerrainStreamer& m_streamer; // terrain height for grounding
    fitzel::Camera&          m_camera;   // billboard basis for fireflies

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
