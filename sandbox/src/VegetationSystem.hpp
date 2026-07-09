#pragma once

#include <cstdint>
#include <future>
#include <random>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/graphics/InstancedMesh.hpp>
#include <fitzel/graphics/Shader.hpp>
#include <fitzel/graphics/Texture.hpp>
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
    ~VegetationSystem(); // frees the tree GL buffers

    // Load grass/bird/firefly shaders + build instanced meshes. Needs a live GL
    // context; returns false if a shader failed to compile.
    bool init();
    // Load the tree model + build its instanced/billboard GL buffers. Separate so
    // the caller can supply the content directories. Returns false on shader fail.
    bool initTrees(const std::string& modelDir, const std::string& texDir);
    // Load the flower shader + instanced mesh. Returns false on shader fail.
    bool initFlowers();

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

    // --- Trees ---------------------------------------------------------------
    // Regrow the procedural forest when the camera has strayed far enough from
    // its last center (painted trees always ride along). Paint/erase place or
    // remove hand-placed trees (world space). Draw methods cover the shadow pass,
    // the lit 3D pass and the distant-billboard LOD pass.
    void updateTrees(glm::vec2 camXZ, const std::vector<glm::vec2>& road,
                     float roadWidth, float waterLevel, float snowLevel);
    void stampTree(glm::vec2 c, float radius, std::mt19937& rng,
                   float waterLevel, float snowLevel);
    void eraseTree(glm::vec2 c, float radius);
    void clearPaintedTrees() { paintedTrees.clear(); rebuildTreeBuffer(); }
    void drawTreeShadow(const glm::mat4& lightSpace, double time, float weather);
    void drawTrees(const VegDrawContext& ctx);
    void drawTreeBillboards(const VegDrawContext& ctx, const glm::vec3& camRight);
    // Tree positions (5 floats/tree: pos3, yaw, scale) so flowers can cluster.
    const std::vector<float>& treeInstances() const { return m_treeInst; }

    // --- Flowers -------------------------------------------------------------
    // Procedural blooms regrow with the grass pass (clustering in moist ground
    // and around tree trunks); paint/erase place hand-flowers (world space).
    void regenFlowers(glm::vec2 c, const std::vector<glm::vec2>& road, float roadWidth,
                      float waterLevel, float snowLevel);
    void stampFlower(glm::vec2 c, float radius, std::mt19937& rng,
                     float waterLevel, float snowLevel);
    void eraseFlower(glm::vec2 c, float radius);
    void clearPaintedFlowers() { paintedFlowers.clear(); rebuildFlowerBuffer(); }
    void drawFlowers(const VegDrawContext& ctx);

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

    bool      treeEnabled = true;
    float     treeDensity = 1.0f;
    float     treeSize    = 9.0f;      // average tree height (world units)
    float     lodNear     = 45.0f;     // 3D trees within this range, billboards beyond
    glm::vec2 treeCenter{1e9f};        // last procedural-forest center; 1e9 forces regen
    int       treeCount   = 0;         // total drawn (procedural + painted)
    std::vector<float> paintedTrees;   // 5 floats/tree, world space (saved)
    float     treeBrushRadius  = 8.0f;
    float     treeBrushDensity = 1.0f; // attempts per m^2 (trees stay sparse)
    float     treeMinSpacing   = 4.0f; // reject placements closer than this
    float     treePaintScale   = 9.0f; // painted-tree height (m)

    bool  flowerEnabled = true;
    float flowerDensity = 1.0f;
    int   flowerCount   = 0;
    std::vector<float> paintedFlowers;    // 8 floats/flower, world space (saved)
    float flowerBrushRadius  = 6.0f;
    float flowerBrushDensity = 1.0f;

    bool  birdsEnabled   = true;
    bool  fireflyEnabled = true;

private:
    static std::vector<float> computeGrass(
        fitzel::TerrainSettings s, glm::vec2 c, float waterLvl, float snowLvl,
        float gHeight, float gDensity, float R, std::uint32_t seed,
        std::vector<glm::vec2> road, float roadClear);

    void regenTrees(glm::vec2 cc, const std::vector<glm::vec2>& road, float roadWidth,
                    float waterLevel, float snowLevel);
    void rebuildTreeBuffer();   // re-upload procedural prefix + painted trees
    void rebuildFlowerBuffer(); // re-upload procedural prefix + painted flowers

    // One draw group of the tree model (a material/texture slice of the mesh).
    struct TreePrim {
        fitzel::Texture tex;
        bool hasTex = false;
        int  first = 0, count = 0;
        bool cutout = false;
    };

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

    // Trees.
    fitzel::Shader        m_tree, m_treeDepth, m_billboard;
    fitzel::Texture       m_billboardTex;
    std::vector<TreePrim> m_treePrims;
    std::uint32_t         m_treeVAO = 0, m_treeVBO = 0, m_treeInstVBO = 0, m_bbVAO = 0;
    const float           m_treeLocalHeight = 1.0f;
    float                 m_bbAspect = 0.93f;
    const float           m_treeRadius = 120.0f;
    std::mt19937          m_trng{555u};
    std::vector<float>    m_treeInst;              // procedural prefix + painted trees
    std::size_t           m_proceduralFloats = 0;  // size of the procedural prefix

    // Flowers.
    fitzel::Shader        m_flower;
    fitzel::InstancedMesh m_flowerField;
    int                   m_flowerVerts = 0;
    std::vector<float>    m_flowerInst;
    std::size_t           m_proceduralFlowerFloats = 0;

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
